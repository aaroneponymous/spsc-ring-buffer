# `SPSC::SpscRing<T>` — Lock-Free Single-Producer/Single-Consumer Ring

A bounded, lock-free queue specialized for **exactly one producer thread** and **exactly one consumer thread**.
Elements are **constructed in-place** on push and **destroyed** on pop; storage is a ring of raw, `alignas(T)` bytes.

---

## Key properties

* **Concurrency model:** SPSC (one producer, one consumer). No locks.
* **Capacity:** Internal ring size is a power of two; if the constructor receives a non-power-of-two, it is rounded **up** via `ceilPow2`.
  The design keeps **one slot empty**, so the **max storable items = `cap_ - 1`**.
* **Progress & complexity:** All ops are `O(1)`.
* **Memory model:** Producer publishes with **release**; consumer observes with **acquire** (and vice-versa for the opposite index). Own-index loads/stores are **relaxed**.
* **Object lifetime:** Uses `std::construct_at` / `std::destroy_at` per slot; supports non-default-constructible and non-trivially destructible `T`.
* **RAII:** Destructor drains remaining elements (destroys live objects) and frees the ring.
* **Non-movable / non-copyable:** All copy/move special members are deleted.

---

## Public API

```cpp
explicit SpscRing();
explicit SpscRing(std::size_t cap);

~SpscRing();

SpscRing(const SpscRing&)            = delete;
SpscRing& operator=(const SpscRing&) = delete;
SpscRing(SpscRing&&)                 = delete;
SpscRing& operator=(SpscRing&&)      = delete;

std::size_t capacity() const noexcept; // returns internal ring size (usable = capacity()-1)
std::size_t size()     const noexcept; // snapshot: (tail - head) & (cap_-1)
bool        empty()    const noexcept; // size() == 0
bool        full()     const noexcept; // next(tail) == head

bool try_push(const T& v)  noexcept(noexcept(T(v)));
bool try_push(T&& v)       noexcept(noexcept(T(std::move(v))));
template<class... Args>
bool try_emplace(Args&&... args);      // constructs T in-place

bool try_pop(T& out)       noexcept;    // moves out, destroys slot
```

### Semantics

* **`try_push` / `try_emplace`**

  * Fail (return `false`) if the queue is full.
  * On success: place-construct `T` into the current tail slot and publish the new tail index with **release**.
  * If construction throws, indices are unchanged and the slot remains empty.

* **`try_pop`**

  * Fail (return `false`) if the queue is empty.
  * On success: move from the head slot, destroy the object, and publish the new head index with **release**.

* **`size()`**

  * Snapshot under concurrency; treat as informational (may be slightly stale).

---

## Implementation notes

* **Slot layout:**

  ```cpp
  struct Slot {
    alignas(T) std::byte obj_buf[sizeof(T)];
    T* raw() noexcept { return reinterpret_cast<T*>(obj_buf); }                // before construction
    T* obj() noexcept { return std::launder(reinterpret_cast<T*>(obj_buf)); }  // after construction
  };
  ```

* **Indexing:** wrap with `(i + 1) & (cap_ - 1)` (requires power-of-two `cap_`).

* **Cache alignment:** Head/tail atomics are aligned to
  `std::hardware_destructive_interference_size` (or 64 as fallback) to reduce false sharing.

---

## Example

```cpp
SPSC::SpscRing<int> q(8);   // internal ring size power-of-two; usable capacity = 7

int x = 42;
q.try_push(x);              // copies into the queue
q.try_push(7);              // move-constructs (from temporary)

int out;
while (!q.try_pop(out)) { /* spin or backoff */ }
// use out
```

---

## Guarantees & constraints

* **Only** one thread may call `try_push/try_emplace` and **only** one thread may call `try_pop`.
* No `std::memory_order_seq_cst` is used.
* All operations are constant time with no dynamic allocation after construction.
