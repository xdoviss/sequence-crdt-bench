// this file only includes Logoot specifics - how the started document is laid out.
// everything else is shared with the LSEQ algorithm, goto position_list.cpp.
#include "crdt/algorithms/logoot.hpp"

#include <stdexcept>

namespace crdt {

// spreads the starting elements evenly over a single level of 2^32 digits, with at least
// one free digit between neighbours. one level holds up to 2^31 - 1 elements.
template <>
void Logoot::bulk_load(const std::vector<Value>& initial) {
    const uint64_t base = rules_.base(0);
    const uint64_t element_count = initial.size();
    if (element_count + 1 > base / 2) throw std::runtime_error("bulk_load: document too large");

    const uint64_t spacing = base / (element_count + 1);  // minimum 2, so every digit is at least 1
    std::vector<std::pair<Entry, bool>> items;
    items.reserve(initial.size());

    for (uint64_t index = 0; index < element_count; ++index) {
        Entry entry;
        entry.position.push_back(
            Level{static_cast<uint32_t>((index + 1) * spacing), kInitialSite, static_cast<uint32_t>(index + 1)});
        entry.value = initial[index];
        items.push_back({std::move(entry), true});
    }

    sequence_.build(std::move(items));
    clock_ = 0;
}

}  // namespace crdt
