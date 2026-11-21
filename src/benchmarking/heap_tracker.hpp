// measures heap memory by replacing global op new/delete
//
// for each allocation we count the size the allocator reserved.
// includes allocators' rounding up
#pragma once

#include <cstddef>

namespace heap_tracker {

bool enabled(); // false on platforms where the real allocation size cannot be asked for
size_t current_bytes();
size_t peak_bytes();
void reset_peak();

// measures how much the heap grew since this object was created
class HeapScope {
public:
    HeapScope() : start_(current_bytes()) { reset_peak(); }

    // bytes still allocated now that were not allocated at the start
    size_t delta() const {
        size_t current = current_bytes();
        return current > start_ ? current - start_ : 0;
    }

    // the highest heap usage since the start, above the starting usage
    size_t peak_delta() const {
        size_t peak = peak_bytes();
        return peak > start_ ? peak - start_ : 0;
    }

private:
    size_t start_;
};

}  // namespace heap_tracker
