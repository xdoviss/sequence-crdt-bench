
//
// a position-identifier list (see position_list.hpp) with these rules for new digits:
//   * level d has 2^(base_bits + d) digits, so deeper levels have more room
//     (64-bit digits, because the base keeps growing);
//   * at every depth the new digit is placed either close to the left neighbour
//     ("boundary+") or close to the right one ("boundary-"). which one is decided by a
//     pseudo-random coin per depth that is the same on every replica. this keeps
//     identifiers short whether people type at the front or at the end.

// Nédelec, Molli, Mostéfaoui, Desmontils 2013
// the LSEQ algorithm
#pragma once

#include "crdt/algorithms/position_list.hpp"

namespace crdt {

struct LseqRules {
    using Digit = uint64_t;
    static constexpr Algorithm kAlgorithm = Algorithm::LSEQ;
    static constexpr uint8_t kSnapshotTag = 'Q';

    unsigned base_bits = 4;
    uint64_t boundary = 10;
    uint64_t strategy_seed = 0x5EEDull;  // must be the same on all replicas

    // 2^(base_bits + depth) digits, capped at 2^63.
    uint64_t base(size_t depth) const {
        size_t bits = base_bits + depth;
        return bits >= 63 ? (1ull << 63) : (1ull << bits);
    }

    bool near_left(size_t depth) const { return (mix64(strategy_seed ^ (depth * 0x9E37ull)) & 1) == 0; }
};

using LSEQ = PositionList<LseqRules>;

template <>
void LSEQ::bulk_load(const std::vector<Value>& initial);

}  // namespace crdt
