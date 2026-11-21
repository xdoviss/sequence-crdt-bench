// Weiss, Urso, Molli 2009
// Logoot algorithm
#pragma once

#include "crdt/algorithms/position_list.hpp"

namespace crdt {

struct LogootRules {
    using Digit = uint32_t;
    static constexpr Algorithm kAlgorithm = Algorithm::Logoot;
    static constexpr uint8_t kSnapshotTag = 'L';

    uint64_t boundary = 1ull << 16;

    uint64_t base(size_t /*depth*/) const { return 1ull << 32; }
    bool near_left(size_t /*depth*/) const { return true; }
};

using Logoot = PositionList<LogootRules>;

template <>
void Logoot::bulk_load(const std::vector<Value>& initial);

}  // namespace crdt
