// shared by WOOT and RGA
// waiting list for operations that arrive too early
//
// both algorithms ahve tombstones, therefore each operation can be easily applied
// as soon as the element it refers to arrives.
// while it waits every succesful apply retries ones in this list.
//
// makes WOOT and RGA work without a causal delivery layer.
#pragma once

#include <vector>

#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

// applies `operation` with `try_apply`
// too early -> stored in `waiting`
// success -> waiting operations are retried until all fail
// every waiting operation that leaves the list is reported in `released` as (origin, sequence number)
template <class TryApply>
ApplyStatus apply_or_wait(std::vector<Operation>& waiting, std::vector<std::pair<ReplicaId, uint64_t>>& released,
                          const Operation& operation, TryApply try_apply) {
    if (!operation.body) 
        return ApplyStatus::Rejected;

    ApplyStatus status = try_apply(operation);
    if (status == ApplyStatus::Buffered) {
        waiting.push_back(operation);
        return status;
    }

    if (status != ApplyStatus::Applied) 
    return status;

    bool progress = true;
    while (progress && !waiting.empty()) {
        progress = false;

        for (size_t index = 0; index < waiting.size();) {
            if (try_apply(waiting[index]) == ApplyStatus::Buffered) {
                ++index;
                continue;
            }

            released.push_back({waiting[index].origin, waiting[index].sequence_number});
            waiting.erase(waiting.begin() + static_cast<std::ptrdiff_t>(index));
            progress = true;
        }
    }

    return status;
}

// waiting ops have a snapshot, consisting of count, then (kind, body) for each operation
inline void write_waiting(const SequenceCRDT& crdt, const std::vector<Operation>& waiting, ByteWriter& writer) {
    writer.write_varint(waiting.size());
    for (const auto& operation : waiting) {
        writer.write_byte(static_cast<uint8_t>(operation.kind));
        crdt.encode_body(operation, writer);
    }
}

inline std::vector<Operation> read_waiting(const SequenceCRDT& crdt, ByteReader& reader) {
    std::vector<Operation> waiting;
    size_t count = reader.read_count(2);
    
    for (size_t index = 0; index < count; ++index) {
        Operation operation;
        operation.kind = static_cast<OperationKind>(reader.read_byte());
        operation.body = crdt.decode_body(operation.kind, reader);
        waiting.push_back(std::move(operation));
    }

    return waiting;
}

}  // namespace crdt
