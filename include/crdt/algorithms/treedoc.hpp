// Preguiça, Marquès, Shapiro, Leția 2009
// Treedoc
//
// document is a binary tree read in order (subtree left, node, subtree right).
// if multiple replicas insert concurrently at the same spot - their nodes are split into
// "mini-nodes" (quoting the paper) of one "major node".
// those nodes are sorted then by the "disambiguator", which is just (site, counter).
//
// element identifiers are paths from root [(side, disambiguator), ..].
// paths contain info about the location of the element.
// if a replica gets an insertion and doesn't have its ancestors - it recreates
// missing ancestors as invisible placeholders.
// has tombstone, delete leaves are collected by the garbage collector.
//
// NOT COVERED:
// flatten operation rebalances the tree, but requires all replicas to agree
// because this isn't covered - typing towards the end makes the tree and identifiers grow
#pragma once

#include <vector>

#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

struct Disambiguator {
    uint32_t site = 0;
    uint32_t counter = 0;
};
inline bool operator<(Disambiguator left, Disambiguator right) {
    return left.site < right.site || (left.site == right.site && left.counter < right.counter);
}
inline bool operator==(Disambiguator left, Disambiguator right) {
    return left.site == right.site && left.counter == right.counter;
}

struct PathStep {
    uint8_t side = 0; // 0 === left slot, 1 === right slot of the previous step's node, 2 === root (first step)
    Disambiguator disambiguator;
};
using TreedocPath = std::vector<PathStep>;

struct TreedocInsert final : OperationBody {
    TreedocPath path;
    Value value = 0;
};

struct TreedocDelete final : OperationBody {
    TreedocPath path;
};

class Treedoc final : public SequenceCRDT {
public:
    Treedoc(ReplicaId site, bool collect_garbage);
    ~Treedoc() override;
    Treedoc(const Treedoc&) = delete;
    Treedoc& operator=(const Treedoc&) = delete;

    Algorithm algorithm() const override { 
        return Algorithm::Treedoc; 
    }

    Operation insert(size_t position, Value value) override;
    Operation erase(size_t position) override;

    std::optional<Value> lookup(size_t position) const override;
    std::optional<Value> lookup_id(const ElementId& id) const override;
    ElementId id_at(size_t position) const override;
    std::vector<Value> traverse() const override;
    size_t size() const override;

    ApplyStatus apply(const Operation& operation) override;
    void encode_body(const Operation& operation, ByteWriter& writer) const override;
    std::shared_ptr<const OperationBody> decode_body(OperationKind kind, ByteReader& reader) const override;

    bool requires_causal_delivery() const override { 
        return true; 
    }

    Bytes serialize() const override;
    void deserialize(const Bytes& data) override;
    void bulk_load(const std::vector<Value>& initial) override;

    size_t get_memory_usage() const override;
    StructureStats get_structure_statistics() const override;
    std::vector<std::string> check_invariants() const override;

private:
    enum State : uint8_t { Placeholder = 0, Visible = 1, Tombstone = 2 };
    static constexpr uint8_t kRootSide = 2;

    struct TreeNode {
        Disambiguator disambiguator;
        Value value = 0;
        uint8_t state = Placeholder;
        uint8_t side = kRootSide; // which slot of the parent this node sits in
        TreeNode* parent = nullptr;
        std::vector<TreeNode*> children[2]; // left and right slots
        uint32_t visible_count = 0; // visible elements in the subtree (includes itself)
    };
    using Slot = std::vector<TreeNode*>;

    ReplicaId site_;
    bool collect_garbage_;
    uint32_t counter_ = 0;
    Slot root_;
    size_t node_count_ = 0;

    void clear();

    Slot& slot_of(TreeNode* node) { 
        return node->parent ? node->parent->children[node->side] : root_; 
    }

    const Slot& slot_of(const TreeNode* node) const { 
        return node->parent ? node->parent->children[node->side] : root_; 
    }
    
    static size_t index_in_slot(const Slot& slot, Disambiguator disambiguator, bool* found);
    static size_t visible_in_slot(const Slot& slot);
    static TreeNode* leftmost(TreeNode* node);
    TreeNode* first() const;
    TreeNode* successor(TreeNode* node) const;
    TreeNode* at_visible(size_t rank) const;
    TreeNode* find(const TreedocPath& path) const;
    TreedocPath path_of(const TreeNode* node) const;
    void add_to_visible_counts(TreeNode* node, int delta);
    TreeNode* find_or_create_child(TreeNode* parent, uint8_t side, Disambiguator disambiguator);
    void make_tombstone(TreeNode* node);
    void remove_dead_leaves(TreeNode* node);
    uint32_t build_balanced(const std::vector<Value>& values, size_t low, size_t high, TreeNode* parent, uint8_t side);
    void write_path(ByteWriter& writer, const TreedocPath& path) const;
    TreedocPath read_path(ByteReader& reader) const;
    template <class Visitor>
    void for_each_node(Visitor visit) const;
};

}  // namespace crdt
