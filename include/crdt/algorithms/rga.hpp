// the RGA algorithm (Replicated Growable Array) — .
//
// the elements form one list. every element gets a Lamport timestamp (counter, site) as
// its unique id, and an insertion names the element it goes after (its "reference").
// when several replicas insert after the same reference at the same time, each new
// element skips over the following elements that have a newer timestamp, so the newest
// insertion always ends up first and every replica gets the same order. deleted elements
// stay as hidden tombstones, so later insertions can still refer to them.

// Roh, Jeon, Kim, Lee 2011
// RGA - Replicated Growable Array
// all elements create a single list.
// each elements gets a Lamport timestamp (counter, site), which is its unique ID.
// insertion names the element it goes after (aka a reference).
// when multiple replicas insert after the same reference at same time each new element.
// skips over the following elements that have newer timestamp, so the newest insertion
// always ends up first and every replica gets the same order.
// has tombstones (hidden elements), which insertions can refer to
#pragma once

#include <unordered_map>

#include "crdt/common/ordered_sequence.hpp"
#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

struct RgaId {
    uint64_t counter = 0; // Lamport clock, which is always bigger than every counter this replica has encountered
    uint32_t site = 0;
};
inline bool operator==(RgaId left, RgaId right) { 
    return left.counter == right.counter && left.site == right.site; 
}

// newer === larger counter, equal counters are ordered by site
inline bool operator<(RgaId left, RgaId right) {
    return left.counter < right.counter || (left.counter == right.counter && left.site < right.site);
}

struct RgaElement {
    RgaId id;
    Value value = 0;
};

struct RgaInsert final : OperationBody {
    RgaId id;
    RgaId reference; // element to insert after ({0, 0} is the list head)
    Value value = 0;
};

struct RgaDelete final : OperationBody {
    RgaId id;
};

class Rga final : public SequenceCRDT {
public:
    explicit Rga(ReplicaId site);

    Algorithm algorithm() const override { return Algorithm::RGA; }

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

    Bytes serialize() const override;
    void deserialize(const Bytes& data) override;
    void bulk_load(const std::vector<Value>& initial) override;

    size_t get_memory_usage() const override;
    StructureStats get_structure_statistics() const override;
    std::vector<std::string> check_invariants() const override;

private:
    using Sequence = OrderedSequence<RgaElement>;
    using Node = Sequence::Node;

    ReplicaId site_;
    uint64_t clock_ = 0;
    Sequence sequence_;
    std::unordered_map<uint64_t, Node*> id_index_; // finds element by its id
    Node* head_ = nullptr; // first element, id {0, 0} (invisible)
    std::vector<Operation> waiting_; // operations whose reference has not yet arrived

    Node* find(RgaId id) const;
    ApplyStatus try_apply(const Operation& operation);
    Operation hide(Node* node);
    void load_elements(std::vector<std::pair<RgaElement, bool>>&& items);
};

}  // namespace crdt
