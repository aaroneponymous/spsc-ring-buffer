#pragma once
#include <atomic>
#include <cstddef>
#include <memory>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>
#include <mutex>

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
        struct NodeT
        {
            alignas(T) std::byte raw_bytes[sizeof(T)];
            
            const T* get() const noexcept {
                return std::launder(reinterpret_cast<const T*>(raw_bytes));
            }

            T* get() noexcept {
                return std::launder(reinterpret_cast<T*>(raw_bytes));
            }
        };


    public:
        
        explicit SpscRing() noexcept : SpscRing(0) {}
        explicit SpscRing(std::size_t capacity) noexcept
            : cap_(0), buf_(nullptr), head_(0), tail_(0)
        { 
            init(capacity);
        }

        ~SpscRing() noexcept {
            for (auto& x : buf_) {
                x.~T();
            }
        }

        SpscRing(SpscRing&& rhs) noexcept
            : cap_(rhs.cap_)
            , buf_(std::move(rhs.buf_))
            , head_(rhs.head_.load())
            , tail_(rhs.tail_.load())
        {
            rhs.cap_ = 0;
            rhs.head_.store(0);
            rhs.tail_.store(0);
        }

        SpscRing& operator=(SpscRing&& rhs) noexcept {
            if (this != &rhs) {
                cap_ = rhs.cap_;
                buf_ = std::move(rhs.buf_);
                head_.store(rhs.head_.load(std::memory_order_relaxed));
                tail_.store(rhs.tail_.load(std::memory_order_relaxed));
                rhs.cap_ = 0;
                rhs.head_.store(0);
                rhs.tail_.store(0);
            }
            return *this;
        }

        SpscRing(const SpscRing&) = delete;
        SpscRing& operator=(const SpscRing&) = delete;

        std::size_t capacity() const noexcept { return cap_; }

        std::size_t size() const noexcept {
            return tail_.load() - head_.load();
        }

        bool empty() const noexcept { return size() == 0; }
        bool full()  const noexcept { return size() == cap_; }

        bool try_push(const T& v) noexcept {
            if (full()) return false;
            auto t = tail_.load(std::memory_order_relaxed);
            buf_[t % cap_] = v;
            tail_.store(t + 1, std::memory_order_relaxed);
            return true;
        }

        bool try_push(T&& v) noexcept {
            if (full()) return false;
            auto t = tail_.load();
            buf_[t % cap_] = std::move(v);
            tail_.store(t + 1);
            return true;
        }

        template <class... Args>
        bool try_emplace(Args&&... args) noexcept {
            if (full()) return false;
            auto t = tail_.load();
            new (&buf_[t % cap_]) T(std::forward<Args>(args)...);
            tail_.store(t + 1, std::memory_order_relaxed);
            return true;
        }

        bool try_pop(T& out) noexcept {
            if (empty()) return false;
            auto h = head_.load(std::memory_order_relaxed);
            out = std::move(buf_[h % cap_]);
            head_.store(h + 1, std::memory_order_relaxed);
            return true;
        }

    private:
        
        static constexpr std::size_t alignment_T = alignof(T);
        static constexpr std::size_t size_T = sizeof(T);
        static constexpr std::size_t size_NodeT = sizeof(NodeT);

        // @init: Ensure only one thread inits and no other thread initializes again 
        void init(std::size_t capacity) noexcept
        {
            const uint64_t cap_uint64 = BitOps::ceilPow2(static_cast<uint64_t>(capacity));
            const auto cap = static_cast<std::size_t>(cap_uint64);
            buffer = new std::byte[size_NodeT * cap];
            cap_ = cap;
        }

        // implement pointer arithmetic to access objects from buffer with support of operator[]
    
    private:


        std::size_t                cap_;
        std::byte                 *buffer_;
        std::vector<T>             buf_;
        std::atomic<std::size_t>   head_;
        std::atomic<std::size_t>   tail_;
    };

} // namespace SPSC
