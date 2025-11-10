#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>   // std::hardware_destructive_interfence_size
#include <concepts>
#include <type_traits>
#include <utility>
#include <vector>

// -- For Log Table --
#include <array>
#include <cstdint>
// #include <type_traits>
#if __has_include(<bit>)
    #include <bit>  // C++ 20: countl_zero, bit_cell (optional fast path)
#endif

namespace SPSC {
    namespace BitOps {
        // -----------  1) Byte log2 table (constexpr, header-safe) -----------
        constexpr std::array<int8_t, 256> makeLog2Byte()
        {
            std::array<int8_t, 256> table{};
            table[0] = -1;  // log2(2) undefined
            
            for (unsigned i = 1; i < 256; ++i) {
                unsigned k = 0, x = i;
                while (x >>= 1) ++k;    // k = floor(log2(i))
                table[i] = static_cast<int8_t>(k);
            }
            return table;
        }

        // C++17+: Single ODR entity across TUs
        inline constexpr auto klog2_table = makeLog2Byte();

        // ----------- 2) floor_log2 for 32/64-bit unsigned -----------
        constexpr int floorLog2u64(std::uint64_t v) noexcept
        {
            #if defined(__cpp_lib_bitops) && __cpp_lib_bitops >= 201907L
                return v ? (63 - std::countl_zero(v)) : -1; // C++ 20: Fast Path
            #else
                if (auto b = (v >> 56) & 0xFFu) return 56 + klog2_table[b];
                if (auto b = (v >> 48) & 0xFFu) return 48 + klog2_table[b];
                if (auto b = (v >> 40) & 0xFFu) return 40 + klog2_table[b];
                if (auto b = (v >> 32) & 0xFFu) return 32 + klog2_table[b];
                if (auto b = (v >> 24) & 0xFFu) return 24 + klog2_table[b];
                if (auto b = (v >> 16) & 0xFFu) return 16 + klog2_table[b];
                if (auto b = (v >>  8) & 0xFFu) return  8 + klog2_table[b];
                return klog2_table[v & 0xFFu];
            #endif
        }

        constexpr int floorLog2u32(std::uint32_t v) noexcept
        {
            #if defined(__cpp_lib_bitops_) && __cpp_lib_bitops >= 201907L
                return v ? (31 - std::countl_zero(v)) : -1;
            #else
                if (!v) return -1;
                if (auto b = (v >> 24) & 0xFFu) return 24 + klog2_table[b];
                if (auto b = (v >> 16) & 0xFFu) return 16 + klog2_table[b];
                if (auto b = (v >>  8) & 0xFFu) return  8 + klog2_table[b];
                return klog2_table[v & 0xFFu];
            #endif
        }

        // Power of Two
        constexpr bool isPow2(std::uint64_t v) noexcept {
            return v && ((v & (v - 1)) == 0);
        }

        constexpr std::uint64_t ceilPow2(std::uint64_t v) noexcept
        {
            #if defined(__cpp_lib_bitops_) && __cpp_lib_bitops >= 201907L
                return std::bit_ceil(v);    // C++20: returns 1 for v == 0, too
            #else
                if (v <= 1) return 1;
                --v;
                v |= v >> 1;
                v |= v >> 2;
                v |= v >> 4;
                v |= v >> 8;
                v |= v >> 16;
                if constexpr (sizeof(v) == 8) v |= v >> 32;
                return v + 1;
            #endif
        }
    } // namespace BitOps

    /**
     * @storage:    raw byte array (for in-place construction)
     * @alignment:  alignas(T) std::byte storage_[sizeof(T) * capacity]
     * @ctor:       
     * - cap (round to next power of 2 if not power of 2)
     * - set capacity after preallocated successfully? But noexcept so?
     * @size:
     * - consistent view of the number of current objects in the SPSC Queue
     * @atomicity:  memory_order_seq_cst, memory_order_relaxed, acquire/release
     * @private:
     * - do we implement behavior that shouldn't be exposed?
     * 
     * 
     * @allocate_object:
     * - head_ index to location in queue to construct object at
     * - need to access object: indexed-based - operator[]
     * 
     * @free_object:
     * - tail_ index to location in queue to destruct/deallocate at
     * - need to access object: indexed-based - operator[]
     * - 
     * 
     *
    */

    template <class T>
    class SpscRing final
    {
        struct Slot final
        {
            alignas(T) std::byte obj_buf[sizeof(T)];

            // tail_ writes (pre-condition: no object exists/has been destroyed) - no launder
            T* raw() noexcept {
                return reinterpret_cast<T*>(obj_buf);
            }

            const T* obj() noexcept const {
                return std::launder(reinterpret_cast<const T*>(obj_buf));
            }

            // head_ reads (pre-condition: object exists/prevent a stale version) - launder
            T* obj() noexcept {
                return std::launder(reinterpret_cast<T*>(obj_buf));
            }

        };

    public:
        explicit SpscRing() noexcept : SpscRing(1) {}
        explicit SpscRing(std::size_t cap) noexcept
        {
            std::size_t cap_checked = (BitOps::isPow2(static_cast<uint64_t>(cap)) ? cap : 
                static_cast<std::size_t>(BitOps::ceilPow2(static_cast<uint64_t>(cap))));
            buffer_ = new Slot[cap_checked];
            cap_ = cap_checked;

        } 

        ~SpscRing() noexcept {
            if (!buffer_) return;
            auto h = head_.load(std::memory_order_relaxed);
            auto t = tail_.load(std::memory_order_relaxed);
            while (h != t) { std::destroy_at(buffer_[h].obj()); h = (h + 1) & (cap_ - 1); }
            delete[] buffer_;
        }


        SpscRing(SpscRing&& rhs) noexcept = delete;
        SpscRing& operator=(SpscRing&& rhs) noexcept = delete;

        SpscRing(const SpscRing&) = delete;
        SpscRing& operator=(const SpscRing&) = delete;

        std::size_t capacity() const noexcept { return cap_; }

        std::size_t size() const noexcept
        {
            std::size_t head = head_.load(std::memory_order_acquire);
            std::size_t tail = tail_.load(std::memory_order_acquire);
            return (tail - head) & (cap_ - 1);
        }

        bool empty() const noexcept { return size() == 0; }
        bool full() const noexcept 
        {
            auto t = tail_.load(std::memory_order_relaxed);
            auto n = (t + 1) & (cap_ - 1);
            return n == head_.load(std::memory_order_acquire);
        }

        // Producer Thread: tail_ (can see previous writes to tail)
        // std::memory_order_relaxed
        bool try_push(const T& v) noexcept(noexcept(T(v)))
        {
            std::size_t tail = tail_.load(std::memory_order_relaxed);
            std::size_t tail_next = (tail + 1) & (cap_ - 1);

            if (tail_next == head_.load(std::memory_order_acquire))  return false;
            std::construct_at(buffer_[tail].raw(), v);
            tail_.store(tail_next, std::memory_order_release);
            return true;
        }

        bool try_push(T&& v) noexcept(noexcept(T(std::move(v)))) 
        {
            std::size_t tail = tail_.load(std::memory_order_relaxed);
            std::size_t tail_next = (tail + 1) & (cap_ - 1);

            if (tail_next == head_.load(std::memory_order_acquire))  return false;
            std::construct_at(buffer_[tail].raw(), std::move(v));
            tail_.store(tail_next, std::memory_order_release);
            return true;
        }

        template <class... Args>
            requires std::constructible_from<T, Args...>
        bool try_emplace(Args&&... args) noexcept
        {
            std::size_t tail = tail_.load(std::memory_order_relaxed);
            std::size_t tail_next = (tail + 1) & (cap_ - 1);
            if (tail_next == head_.load(std::memory_order_acquire)) return false;

            std::construct_at(buffer_[tail].raw(), std::forward<Args>(args)...);
            tail_.store(tail_next, std::memory_order_release);
            return true;
        }

        bool try_pop(T& out) noexcept 
        {
            const std::size_t head = head_.load(std::memory_order_relaxed);
            if (head == tail_.load(std::memory_order_acquire)) return false;

            T* object = buffer_[head].obj();
            out = std::move(*object);
            std::destroy_at(object);

            head_.store((head + 1) & (cap_ - 1), std::memory_order_release);
            return true;
        }

    private:

        #ifdef __cpp_lib_hardware_interference_size
            inline static constexpr std::size_t cache_align = std::hardware_destructive_interference_size;
        #else
            // 64 bytes on x86-64 │ L1_CACHE_BYTES │ L1_CACHE_SHIFT │ __cacheline_aligned │ ...
            inline static constexpr std::size_t cache_align = 64;
        #endif


        std::size_t cap_;
        alignas(cache_align) std::atomic<std::size_t> head_{ 0 };
        alignas(cache_align) std::atomic<std::size_t> tail_{ 0 };
        Slot *buffer_{ nullptr };

    };

} // namespace SpscRing