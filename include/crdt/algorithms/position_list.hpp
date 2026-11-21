// shared core for Logoot and LSEQ
// both algorithms give each element a position identifier, which is a list of levels each level looking like (digit, site, clock).
// identifiers are compared level by level, a shorter prefix comes first and document consists of all elements sorted by identifier.
// between each indentifier there is always left a space for another one (if necessary adds another level).
// no tombstones.
#pragma once

#include <vector>

#include "crdt/common/ordered_sequence.hpp"
#include "crdt/common/random_generator.hpp"
#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

template <class Digit>
struct PositionLevel {
    Digit digit = 0;
    uint32_t site = 0; // replica that created this level
    uint32_t clock = 0;  // ^ replica's counter at the time (makes it unique)
};

template <class Digit>
bool operator<(const PositionLevel<Digit>& left, const PositionLevel<Digit>& right) {
    if (left.digit != right.digit) 
        return left.digit < right.digit;

    if (left.site != right.site) 
        return left.site < right.site;

    return left.clock < right.clock;
}

// comparison like in a dictionary
// -1 if left comes first, 1 if right comes first, 0 if equal.
template <class Level>
int compare_positions(const std::vector<Level>& left, const std::vector<Level>& right) {
    size_t common = left.size() < right.size() ? left.size() : right.size();
    for (size_t depth = 0; depth < common; ++depth) {
        if (left[depth] < right[depth]) 
            return -1;
        if (right[depth] < left[depth])
             return 1;
    }

    if (left.size() == right.size())
         return 0;

    return left.size() < right.size() ? -1 : 1;
}

template <class Rules>
struct PositionInsert final : OperationBody {
    std::vector<PositionLevel<typename Rules::Digit>> position;
    Value value = 0;
};

template <class Rules>
struct PositionDelete final : OperationBody {
    std::vector<PositionLevel<typename Rules::Digit>> position;
};

template <class Rules>
class PositionList final : public SequenceCRDT {
public:
    using Level = PositionLevel<typename Rules::Digit>;
    using Position = std::vector<Level>;

    PositionList(ReplicaId site, Rules rules, uint64_t seed);

    Algorithm algorithm() const override { 
        return Rules::kAlgorithm; 
    }

    Operation insert(size_t position, Value value) override;
    Operation erase(size_t position) override;

    std::optional<Value> lookup(size_t position) const override;
    std::optional<Value> lookup_id(const ElementId& id) const override;
    ElementId id_at(size_t position) const override;
    std::vector<Value> traverse() const override;

    size_t size() const override { 
        return sequence_.size(); 
    }

    ApplyStatus apply(const Operation& operation) override;
    void encode_body(const Operation& operation, ByteWriter& writer) const override;
    std::shared_ptr<const OperationBody> decode_body(OperationKind kind, ByteReader& reader) const override;
    bool requires_causal_delivery() const override { 
        return true; 
    }

    Bytes serialize() const override;
    void deserialize(const Bytes& data) override;
    // written separately for Logoot and LSEQ
    void bulk_load(const std::vector<Value>& initial) override;

    size_t get_memory_usage() const override;
    StructureStats get_structure_statistics() const override;
    std::vector<std::string> check_invariants() const override;

private:
    struct Entry {
        Position position;
        Value value = 0;
    };
    
    using Sequence = OrderedSequence<Entry>;
    using Node = typename Sequence::Node;

    ReplicaId site_;
    Rules rules_;
    RandomGenerator random_;
    uint32_t clock_ = 0;
    Sequence sequence_;

    Position generate(const Position* left, const Position* right);
    Node* first_not_before(const Position& position) const;
    Node* find(const Position& position) const;
    bool is_valid(const Position& position) const;
    void write_position(ByteWriter& writer, const Position& position) const;
    Position read_position(ByteReader& reader) const;
};

}  // namespace crdt
