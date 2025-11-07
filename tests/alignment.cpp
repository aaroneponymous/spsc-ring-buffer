#include <cstdint>
#include <memory>
#include <type_traits>
#include <utility>
#include <cassert>
#include <bit>


template <typename T>
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

int main() {

    // Stack Node
    NodeT<uint64_t> node;

    // ------------------------ Size Agreements ------------------------
    assert(sizeof(uint64_t) == sizeof(node));
    assert(sizeof(uint64_t) == sizeof(node.raw_bytes));
    assert(sizeof(node) == sizeof(node.raw_bytes));


    // ------------------------ Addresses Agreement --------------------
    /** @uintptr_t: To compare pointers to arbitrary locations in memory in a well-defined manner in C++ */
    /** @behavior:  Compare the integral values associated with pointers in a well-defined manner 
     *  - Form below checks physical address equality, not object identity
    */

    auto node_addr = reinterpret_cast<std::uintptr_t>(std::addressof(node));
    auto obj_addr = reinterpret_cast<std::uintptr_t>(std::addressof(*node.get()));
    assert(node_addr == obj_addr);

    // Equivalently, C++20
    assert(std::bit_cast<std::uintptr_t>(std::addressof(node))
        == std::bit_cast<std::uintptr_t>(std::addressof(*node.get())));

    // Heap Node
    
    NodeT<uint64_t> *node_heap = new NodeT<uint64_t>();

    // Size Agreements
    assert(sizeof(std::uint64_t) == sizeof(*node_heap));
    assert(sizeof(std::uint64_t) == sizeof(node_heap->raw_bytes));
    assert(sizeof(*node_heap) == sizeof(node_heap->raw_bytes));

    // Address Agreements
    node_addr = reinterpret_cast<std::uintptr_t>(std::addressof(*node_heap));
    obj_addr = reinterpret_cast<std::uintptr_t>(std::addressof((*node_heap->get())));

    assert(node_addr == obj_addr);

    assert(std::bit_cast<std::uintptr_t>(std::addressof(*node_heap))
        == std::bit_cast<std::uintptr_t>(std::addressof((*node_heap->get()))));

    delete node_heap;
    node_heap = nullptr;
    return 0;
}

// template<typename T>
// class DumbQueue final
// {
//     struct NodeT
//     {
//         alignas(T) std::byte raw_bytes[sizeof(T)];
        
//         const T* get() const noexcept {
//             return std::launder(reinterpret_cast<const T*>(raw_bytes));
//         }

//         T* get() noexcept {
//             return std::launder(reinterpret_cast<T*>(raw_bytes));
//         }
//     }


// public:
//     explicit DumbQueue() : DumbQueue()
// };


