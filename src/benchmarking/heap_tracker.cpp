#include "heap_tracker.hpp"

#if !defined(CRDT_HEAP_TRACKING) // heap counting adds around 12% latency

namespace heap_tracker {
bool enabled() { return false; }
size_t current_bytes() { return 0; }
size_t peak_bytes() { return 0; }
void reset_peak() {}
const char* hook_description() { return "off"; }
}  // namespace heap_tracker

#else

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <new>

#if defined(_WIN32)
#include <malloc.h>
#define CRDT_USABLE_SIZE(pointer) _msize(pointer)
#elif defined(__APPLE__)
#include <malloc/malloc.h>
#define CRDT_USABLE_SIZE(pointer) malloc_size(pointer)
#elif defined(__linux__) || defined(__GLIBC__)
#include <malloc.h>
#define CRDT_USABLE_SIZE(pointer) malloc_usable_size(pointer)
#endif

namespace {

std::atomic<size_t> current_heap{0};
std::atomic<size_t> peak_heap{0};
std::atomic<uint64_t> allocation_count{0};

void count_allocation(void* pointer) {
#ifdef CRDT_USABLE_SIZE
    size_t size = CRDT_USABLE_SIZE(pointer);
    size_t current = current_heap.fetch_add(size, std::memory_order_relaxed) + size;
    allocation_count.fetch_add(1, std::memory_order_relaxed);

    // raise the peak if we are above it (retry if the value changed in between)
    size_t peak = peak_heap.load(std::memory_order_relaxed);
    while (current > peak && !peak_heap.compare_exchange_weak(peak, current, std::memory_order_relaxed)) {
    }
#else
    (void)pointer;
#endif
}

void count_release(void* pointer) {
#ifdef CRDT_USABLE_SIZE
    current_heap.fetch_sub(CRDT_USABLE_SIZE(pointer), std::memory_order_relaxed);
#else
    (void)pointer;
#endif
}

// what does the new operator have to do?
// allocate or ask the "new handler" to free memory and retry,
// or throw std::bad_alloc.
void* tracked_allocate(std::size_t size) {
    if (size == 0) size = 1;

    while (true) {
        void* pointer = std::malloc(size);
        if (pointer) {
            count_allocation(pointer);
            return pointer;
        }

        std::new_handler handler = std::get_new_handler();
        if (!handler) 
            throw std::bad_alloc();
        handler();
    }
}

void* tracked_allocate_or_null(std::size_t size) noexcept {
    try {
        return tracked_allocate(size);
    } catch (...) {
        return nullptr;
    }
}

void tracked_free(void* pointer) noexcept {
    if (!pointer) return;

    count_release(pointer);
    std::free(pointer);
}

}  // namespace

namespace heap_tracker {

bool enabled() {
#ifdef CRDT_USABLE_SIZE
    return true;
#else
    return false;
#endif
}

size_t current_bytes() { return current_heap.load(std::memory_order_relaxed); }
size_t peak_bytes() { return peak_heap.load(std::memory_order_relaxed); }
void reset_peak() { peak_heap.store(current_heap.load(std::memory_order_relaxed), std::memory_order_relaxed); }

const char* hook_description() {
    return enabled() ? "on (operator new/delete counts usable size)" : "on, but usable size unavailable on this platform";
}

}  // namespace heap_tracker

// replace every form of the global allocation functions, so nothing slips past the counters
void* operator new(std::size_t size) { return tracked_allocate(size); }
void* operator new[](std::size_t size) { return tracked_allocate(size); }
void* operator new(std::size_t size, const std::nothrow_t&) noexcept { return tracked_allocate_or_null(size); }
void* operator new[](std::size_t size, const std::nothrow_t&) noexcept { return tracked_allocate_or_null(size); }

void operator delete(void* pointer) noexcept { tracked_free(pointer); }
void operator delete[](void* pointer) noexcept { tracked_free(pointer); }
void operator delete(void* pointer, std::size_t) noexcept { tracked_free(pointer); }
void operator delete[](void* pointer, std::size_t) noexcept { tracked_free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { tracked_free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { tracked_free(pointer); }

#endif  // CRDT_HEAP_TRACKING
