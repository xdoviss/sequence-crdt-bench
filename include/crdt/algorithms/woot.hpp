// the WOOT algorithm ("WithOut Operational Transformation") — .
//
// every character gets a unique id (site, clock) and remembers the two characters it
// was typed between: the one before it and the one after it at that moment. deleted
// characters are only hidden (tombstones), so they can still be referred to later.
// an insertion from another replica can be applied once both of its neighbours exist;
// until then it waits.

// Oster, Urso, Molli, Imine 2006
// WOOT (WithOut Operational Transformation)
// every character gets a Lamport clock id (site, clock) and stores
// characters it was entered between.
//
// insertions from other replicas can be applied only once its neighbors exist, otherwise - waits.
//
// uses tombstones
#pragma once

#include <unordered_map>

#include "crdt/common/ordered_sequence.hpp"
#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

struct WootId {
    uint32_t site = 0;
    uint32_t clock = 0;
};

inline bool operator==(WootId left, WootId right) {
    return left.site == right.site && left.clock == right.clock;
}

// ids are ordered by site first, then by clock
inline bool operator<(WootId left, WootId right) {
    return left.site < right.site || (left.site == right.site && left.clock < right.clock);
}

struct WootChar {
    WootId id;
    WootId previous_id; // previous character
    WootId next_id; // next character
    Value value = 0;
};

struct WootInsert final : OperationBody {
    WootChar character;
};

struct WootDelete final : OperationBody {
    WootId id;
};

class Woot final : public SequenceCRDT {
public:
    explicit Woot(ReplicaId site);

    Algorithm algorithm() const override {
        return Algorithm::WOOT; 
    }

    Operation insert(size_t position, Value value) override;
    Operation erase(size_t position) override;

    std::optional<Value> lookup(size_t position) const override;
    std::optional<Value> lookup_id(const ElementId& id) const override;
    ElementId id_at(size_t position) const override;
    std::vector<Value> traverse() const override;

    size_t size() const override { 
        return sequence_.visible_size(); 
    }

    ApplyStatus apply(const Operation& operation) override;
    void encode_body(const Operation& operation, ByteWriter& writer) const override;
    std::shared_ptr<const OperationBody> decode_body(OperationKind kind, ByteReader& reader) const override;

    bool requires_causal_delivery() const override {
        return false;
    }

    std::vector<std::pair<ReplicaId, uint64_t>> drain_released() override {
        return std::exchange(released_, {});
    }

    Bytes serialize() const override;
    void deserialize(const Bytes& data) override;
    void bulk_load(const std::vector<Value>& initial) override;

    size_t get_memory_usage() const override;
    StructureStats get_structure_statistics() const override;
    std::vector<std::string> check_invariants() const override;

private:
    using Sequence = OrderedSequence<WootChar>;
    using Node = Sequence::Node;

    ReplicaId site_;
    uint32_t clock_ = 0;
    Sequence sequence_;
    std::unordered_map<uint64_t, Node*> id_index_; // finds character by id
    Node* begin_ = nullptr; // invisible marker before the first character
    Node* end_ = nullptr; // invisible marker after the last character
    std::vector<Operation> waiting_; // ops whose neighbours have not yet arrived
    std::vector<std::pair<ReplicaId, uint64_t>> released_; // waiting ops processed since the last drain_released()

    Node* find(WootId id) const;
    void integrate(const WootChar& character, Node* previous, Node* next);
    ApplyStatus try_apply(const Operation& operation);
    Operation hide(Node* node);
    void load_characters(std::vector<std::pair<WootChar, bool>>&& items);
};

}  // namespace crdt
