// Preguiça, Marquès, Shapiro, Leția 2009
// Treedoc
//
// document is a binary tree read in order (subtree left, node, subtree right).
// if multiple replicas insert concurrently at the same spot - their nodes are
// split into "mini-nodes" (quoting the paper) of one "major node". those nodes
// are sorted then by the "disambiguator", which is just (site, counter).//
// element identifiers (PosIDs) are paths from the root.
//
// if a replica gets an insertion and doesn't have its ancestors - it recreates
// missing ancestors as invisible placeholders.
// has tombstones, deleted leaf mini-nodes are collected immediately
//
// NOT COVERED:
// flatten operation rebalances the tree, but requires all replicas to agree
// the tree-growing heuristic for appendss not implemented either
// because of this typing towards the end makes the tree and identifiers grow
#pragma once

#include <memory>
#include <vector>

#include "crdt/common/sequence_crdt.hpp"

namespace crdt {

// UDIS: (counter, site). {0, 0} means "no disambiguator" (atoms of the exploded starting tree)
struct Disambiguator {
    uint32_t counter = 0;
    uint32_t site = 0;
};

inline bool operator<(Disambiguator left, Disambiguator right) {
    return left.counter < right.counter || (left.counter == right.counter && left.site < right.site);
}

inline bool operator==(Disambiguator left, Disambiguator right) {
    return left.counter == right.counter && left.site == right.site;
}

struct PathStep {
    uint8_t side = 0; // 0 === left child, 1 === right child, 2 === root (first step)
    bool has_disambiguator = false;
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

    struct Major;

    struct Mini {
        Disambiguator disambiguator;
        Value value = 0;
        uint8_t state = Placeholder;
    };

    // child of one specific mini-node (only created when inserting between mini-siblings)
    struct MiniChild {
        Disambiguator owner;
        uint8_t side = 0;
        Major* major = nullptr;
    };

    // rarely used parts of a major node, allocated only when needed
    struct Extra {
        std::vector<Mini> minis; // all mini-nodes when there are 2 or more
        std::vector<MiniChild> mini_children;
    };

    struct Major {
        Major* parent = nullptr;
        Disambiguator via; // the parent's mini-node this major node hangs off
        Major* children[2] = {nullptr, nullptr}; // children of the major node itself
        uint32_t visible_count = 0; // visible atoms in the subtree
        uint32_t mini_count = 0;
        uint8_t side = kRootSide; // which child slot of the parent
        bool via_mini = false;
        Mini single; // the mini-node when mini_count == 1
        std::unique_ptr<Extra> extra;
    };

    // mini-node, its major node and index among that major node's mini-nodes
    struct Spot {
        Major* major = nullptr;
        uint32_t index = 0;
        explicit operator bool() const { return major != nullptr; }
    };

    ReplicaId site_;
    bool collect_garbage_;
    uint32_t counter_ = 0;
    Major* root_ = nullptr;
    size_t mini_total_ = 0; // mini-nodes (visible, tombstone, placeholder)
    size_t major_total_ = 0;

    void clear();

    static Mini* minis(Major* major) { return major->mini_count >= 2 ? major->extra->minis.data() : &major->single; }
    static const Mini* minis(const Major* major) {
        return major->mini_count >= 2 ? major->extra->minis.data() : &major->single;
    }
    static Mini& mini_at(Spot spot) { return minis(spot.major)[spot.index]; }
    static bool find_mini(const Major* major, Disambiguator disambiguator, uint32_t* index);
    uint32_t add_mini(Major* major, Disambiguator disambiguator);
    void remove_mini(Major* major, uint32_t index);
    static Major* mini_child(const Major* major, Disambiguator owner, uint8_t side);
    static bool has_mini_children(const Major* major, Disambiguator owner);

    Major* new_major(Major* parent, uint8_t side, bool via_mini, Disambiguator via);
    void detach(Major* major);

    static Spot leftmost(Major* major);
    static Spot leftmost_from_mini(Major* major, uint32_t index);
    static Spot after_mini(Major* major, uint32_t index);
    Spot first() const;
    Spot successor(Spot spot) const;
    Spot at_visible(size_t rank) const;

    Spot find(const TreedocPath& path) const;
    TreedocPath path_of(Spot spot) const;
    void add_to_visible_counts(Major* major, int delta);
    void make_tombstone(Spot spot);
    void collect(Major* major);

    Major* build_balanced(const std::vector<Value>& values, size_t low, size_t high, Major* parent, uint8_t side);
    void write_path(ByteWriter& writer, const TreedocPath& path) const;
    TreedocPath read_path(ByteReader& reader) const;

    template <class Visitor>
    void for_each_major(Visitor visit) const;
};

}  // namespace crdt
