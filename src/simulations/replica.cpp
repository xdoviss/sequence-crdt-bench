#include "crdt/simulations/replica.hpp"

#include <algorithm>
#include <string>

namespace crdt {

Replica::Replica(ReplicaId id, std::unique_ptr<SequenceCRDT> crdt, DeliveryMode mode, bool keep_history)
    : id_(id), crdt_(std::move(crdt)), mode_(mode), keep_history_(keep_history) {
    make_room_for(id_);
}

// per replica tables are indexed by replica ID and grow when a new id comes in
void Replica::make_room_for(ReplicaId replica) {
    if (replica < delivered_.size()) return;

    delivered_.resize(replica + 1, 0);
    early_.resize(replica + 1);
    out_of_order_seen_.resize(replica + 1);
}

// fills in the envelope of a local edit w/ who made it, its number and what it
// depends on (aka everything we have applied from other replicas so far)
Operation Replica::stamp(Operation operation) {
    operation.origin = id_;
    operation.dependencies.clear();
    for (ReplicaId replica = 0; replica < delivered_.size(); ++replica) {
        if (replica != id_ && delivered_[replica] > 0) operation.dependencies.push_back(Dependency{replica, delivered_[replica]});
    }

    operation.sequence_number = ++delivered_[id_];
    if (keep_history_) history_.push_back(operation);
    return operation;
}

Operation Replica::insert(size_t position, Value value) { return stamp(crdt_->insert(position, value)); }

Operation Replica::erase(size_t position) { return stamp(crdt_->erase(position)); }

// ready === its the next operation from its origin and everything it saw has been applied too
bool Replica::is_ready(const Operation& operation) const {
    if (operation.sequence_number != delivered_from(operation.origin) + 1) return false;

    for (const auto& dependency : operation.dependencies) {
        if (delivered_from(dependency.replica) < dependency.count) return false;
    }

    return true;
}

void Replica::deliver(const Operation& operation, ReceiveOutcome& outcome) {
    if (crdt_->apply(operation) == ApplyStatus::Rejected) outcome.rejected++;

    delivered_[operation.origin] = std::max(delivered_[operation.origin], operation.sequence_number);
    outcome.applied.push_back({operation.origin, operation.sequence_number});
}

// keeps delivering buffered operations when they become ready
void Replica::deliver_buffered(ReceiveOutcome& outcome) {
    bool progress = true;
    while (progress && buffered_count_ > 0) {
        progress = false;
        for (ReplicaId origin = 0; origin < early_.size(); ++origin) {
            auto& queue = early_[origin];
            while (!queue.empty()) {
                auto earliest = queue.begin();
                if (earliest->first <= delivered_[origin]) {  // already applied -> old duplicate
                    queue.erase(earliest);
                    --buffered_count_;
                    continue;
                }
                if (!is_ready(earliest->second)) break;

                Operation operation = std::move(earliest->second);
                queue.erase(earliest);
                --buffered_count_;
                deliver(operation, outcome);
                progress = true;
            }
        }
    }
}

// direct === apply instantly.
// version vector still moves forward over every sequence number that has been applied without gaps
ReceiveOutcome Replica::receive_direct(const Operation& operation) {
    ReceiveOutcome outcome;
    uint64_t& delivered = delivered_[operation.origin];
    auto& seen = out_of_order_seen_[operation.origin];
    if (operation.sequence_number <= delivered || seen.count(operation.sequence_number)) {
        outcome.duplicates++;
        return outcome;
    }

    if (crdt_->apply(operation) == ApplyStatus::Rejected) outcome.rejected++;

    seen.insert(operation.sequence_number);
    while (seen.count(delivered + 1)) {
        seen.erase(delivered + 1);
        delivered++;
    }
    outcome.applied.push_back({operation.origin, operation.sequence_number});
    return outcome;
}

ReceiveOutcome Replica::receive(const Operation& operation) {
    ReceiveOutcome outcome;
    if (operation.origin == id_ || operation.sequence_number == 0) {
        outcome.duplicates++;  // own operations coming back should be ignored
        return outcome;
    }

    make_room_for(operation.origin);
    for (const auto& dependency : operation.dependencies) make_room_for(dependency.replica);

    if (mode_ == DeliveryMode::Direct) return receive_direct(operation);

    if (operation.sequence_number <= delivered_[operation.origin] ||
        early_[operation.origin].count(operation.sequence_number)) {
        outcome.duplicates++;
        return outcome;
    }

    if (is_ready(operation)) {
        deliver(operation, outcome);
        if (buffered_count_ > 0) deliver_buffered(outcome);
    } else {
        early_[operation.origin].emplace(operation.sequence_number, operation);
        ++buffered_count_;
        outcome.buffered++;
    }
    return outcome;
}

std::vector<std::string> Replica::check_causal_invariants() const {
    std::vector<std::string> errors;
    if (keep_history_ && history_.size() != delivered_[id_]) errors.push_back("own history length != own vv entry");

    size_t buffered = 0;
    for (ReplicaId origin = 0; origin < early_.size(); ++origin) {
        for (const auto& [sequence_number, operation] : early_[origin]) {
            ++buffered;
            if (sequence_number <= delivered_[origin]) errors.push_back("causal buffer holds an already delivered op");
            if (mode_ == DeliveryMode::Causal && is_ready(operation)) errors.push_back("causal buffer holds a deliverable op");
        }
    }
    if (buffered != buffered_count_) errors.push_back("causal buffer counter mismatch");

    for (size_t index = 0; index < history_.size(); ++index) {
        if (history_[index].sequence_number != index + 1) {
            errors.push_back("own history sequence numbers not contiguous");
            break;
        }
    }
    return errors;
}

}  // namespace crdt
