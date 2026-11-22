// the operation message that replicas send each other.
//
// an operation has two parts:
//   * an envelope that is the same for every algorithm: what kind of edit it is, which
//     replica made it, its sequence number at that replica and which other operations
//     must be applied before it (filled in by the Replica class);
//   * a body that only the algorithm understands (identifier or position + value).
// in memory the body stays decoded; it is turned into bytes only when it is sent,
// so benchmarks can time creating, encoding, decoding and applying separately.
#pragma once

#include <memory>
#include <vector>

#include "crdt/common/codec.hpp"
#include "crdt/common/types.hpp"

namespace crdt {

// base for algorithm specific part of an operation
struct OperationBody {
    virtual ~OperationBody() = default;
};

// first `count` operations of `replica` have to be applied and only then can continue
struct Dependency {
    ReplicaId replica = 0;
    uint64_t count = 0;
};

struct Operation {
    OperationKind kind = OperationKind::Insert;
    ReplicaId origin = 0; // replica that originated the edit
    uint64_t sequence_number = 0; // how many edits has this replica made
    std::vector<Dependency> dependencies; // what it saw from other replicas since the origin's previous operation
    std::shared_ptr<const OperationBody> body;
};

inline Operation make_operation(OperationKind kind, ReplicaId origin, std::shared_ptr<const OperationBody> body) {
    Operation operation;
    operation.kind = kind;
    operation.origin = origin;
    operation.body = std::move(body);

    return operation;
}

class SequenceCRDT;

// how many bytes of an encoded message are envelope and how many are body
struct WireStats {
    size_t envelope_bytes = 0; // kind, origin, sequence number, dependencies, counts
    size_t body_bytes = 0; // identifiers and the value

    size_t total() const { 
        return envelope_bytes + body_bytes; 
    }
};

void encode_operation(const SequenceCRDT& crdt, const Operation& operation, ByteWriter& writer,
                      WireStats* stats = nullptr);

Operation decode_operation(const SequenceCRDT& crdt, ByteReader& reader);

// batch message that includes the number of operations and the operations
Bytes encode_batch(const SequenceCRDT& crdt, const std::vector<Operation>& operations, WireStats* stats = nullptr);
std::vector<Operation> decode_batch(const SequenceCRDT& crdt, const Bytes& data);

} // namespace crdt
