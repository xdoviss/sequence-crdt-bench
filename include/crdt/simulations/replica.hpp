// a replica is one simulated user's copy of the document.
//
// it owns a CRDT, a version vector (how many operations from each replica it has
// applied), the history of its own operations and a buffer for operations that arrived
// too early. local edits are stamped with (origin, sequence number, dependencies).
// an operation from another replica is handed to the CRDT only once everything it
// depends on has been applied ("causal delivery"); duplicates are dropped.
// in "direct" mode causal ordering is skipped (duplicates are still dropped), to test
// how the algorithms themselves cope with operations that arrive too early.
#pragma once

#include <map>
#include <memory>
#include <set>
#include <utility>
#include <vector>

#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

enum class DeliveryMode { Causal, Direct };

// what happened when one operation was received.
struct ReceiveOutcome {
    std::vector<std::pair<ReplicaId, uint64_t>> applied;  // (origin, sequence number) handed to the CRDT
    size_t duplicates = 0;
    size_t buffered = 0;  // put into the waiting buffer
    size_t rejected = 0;
};

class Replica {
public:
    Replica(ReplicaId id, std::unique_ptr<SequenceCRDT> crdt, DeliveryMode mode = DeliveryMode::Causal,
            bool keep_history = true);

    ReplicaId id() const { return id_; }
    SequenceCRDT& crdt() { return *crdt_; }
    const SequenceCRDT& crdt() const { return *crdt_; }

    // local edits: change our own CRDT and return the stamped operation to send.
    Operation insert(size_t position, Value value);
    Operation erase(size_t position);

    // an operation from another replica (it may be a duplicate or arrive too early).
    ReceiveOutcome receive(const Operation& operation);

    uint64_t delivered_from(ReplicaId replica) const { return replica < delivered_.size() ? delivered_[replica] : 0; }
    size_t causal_buffer_size() const { return buffered_count_; }

    // checks the bookkeeping; returns every problem found (empty means all good).
    std::vector<std::string> check_causal_invariants() const;

private:
    ReplicaId id_;
    std::unique_ptr<SequenceCRDT> crdt_;
    DeliveryMode mode_;
    bool keep_history_;
    std::vector<uint64_t> delivered_;                    // version vector: operations applied per replica
    std::vector<std::map<uint64_t, Operation>> early_;   // per origin, by sequence number
    std::vector<std::set<uint64_t>> out_of_order_seen_;  // direct mode: sequence numbers applied ahead of order
    size_t buffered_count_ = 0;
    std::vector<Operation> history_;

    void make_room_for(ReplicaId replica);
    Operation stamp(Operation operation);
    bool is_ready(const Operation& operation) const;
    void deliver(const Operation& operation, ReceiveOutcome& outcome);
    void deliver_buffered(ReceiveOutcome& outcome);
    ReceiveOutcome receive_direct(const Operation& operation);
};

}  // namespace crdt
