// this file only includes LSEQ specifics - how the started document is laid
// out. everything else is shared with the Logoot algorithm, goto position_list.cpp.
#include "crdt/algorithms/lseq.hpp"

#include <stdexcept>

namespace crdt {

// spreads the starting elements evenly over the smallest number of levels that leaves min
// one free slot between neighbours. levels together act like one big number
// with a different base per digit (level d has 2^(base_bits + d) digits).
template <>
void LSEQ::bulk_load(const std::vector<Value>& initial) {
    const uint64_t element_count = initial.size();

    // add levels until the combined space has room for twice the elements
    std::vector<uint64_t> bases;
    unsigned total_bits = 0;
    uint64_t space = 1;
    for (size_t depth = 0;; ++depth) {
        uint64_t base = rules_.base(depth);
        unsigned level_bits = 0;
        while ((1ull << level_bits) < base) ++level_bits;
        if (total_bits + level_bits > 63) throw std::runtime_error("bulk_load: document too large");

        total_bits += level_bits;
        space <<= level_bits;
        bases.push_back(base);
        if (space / 2 >= element_count + 1) break;
    }

    const uint64_t spacing = space / (element_count + 1);
    std::vector<std::pair<Entry, bool>> items;
    items.reserve(initial.size());
    for (uint64_t index = 0; index < element_count; ++index) {
        uint64_t number = (index + 1) * spacing;
        if (number % bases.back() == 0) ++number; // the last digit must be at least 1 (spacing >= 2 keeps order)

        // split the number into one digit per level, last level first
        Entry entry;
        entry.position.resize(bases.size());
        for (size_t depth = bases.size(); depth-- > 0;) {
            entry.position[depth].digit = number % bases[depth];
            number /= bases[depth];
            entry.position[depth].site = kInitialSite;
            entry.position[depth].clock = 0;
        }
        entry.position.back().clock = static_cast<uint32_t>(index + 1);
        entry.value = initial[index];
        items.push_back({std::move(entry), true});
    }

    sequence_.build(std::move(items));
    clock_ = 0;
}

}  // namespace crdt
