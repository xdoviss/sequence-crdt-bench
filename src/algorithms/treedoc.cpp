#include "crdt/algorithms/treedoc.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace crdt {

namespace {
constexpr uint8_t kSnapshotTag = 'T';
}

Treedoc::Treedoc(ReplicaId site, bool collect_garbage) : site_(site), collect_garbage_(collect_garbage) {}

Treedoc::~Treedoc() { clear(); }

// deletes all nodes
// no recursion, as tree can be many many many levels deep
void Treedoc::clear() {
    std::vector<TreeNode*> pending(root_.begin(), root_.end());
    while (!pending.empty()) {
        TreeNode* node = pending.back();
        pending.pop_back();
        for (auto& slot : node->children) {
            for (TreeNode* child : slot) 
                pending.push_back(child);
        }

        delete node;
    }

    root_.clear();
    root_.shrink_to_fit();
    node_count_ = 0;
}

// binary search for a disambiguator in a sorted slot, returns where it is
size_t Treedoc::index_in_slot(const Slot& slot, Disambiguator disambiguator, bool* found) {
    auto position = std::lower_bound(slot.begin(), slot.end(), disambiguator,
                                     [](const TreeNode* node, Disambiguator key) { return node->disambiguator < key; });
    *found = position != slot.end() && (*position)->disambiguator == disambiguator;

    return static_cast<size_t>(position - slot.begin());
}

size_t Treedoc::visible_in_slot(const Slot& slot) {
    size_t visible = 0;
    for (const TreeNode* node : slot) 
        visible += node->visible_count;

    return visible;
}

size_t Treedoc::size() const { return visible_in_slot(root_); }

Treedoc::TreeNode* Treedoc::leftmost(TreeNode* node) {
    while (!node->children[0].empty()) 
        node = node->children[0].front();

    return node;
}

Treedoc::TreeNode* Treedoc::first() const { return root_.empty() ? nullptr : leftmost(root_.front()); }

// next node, counting every node (visible, tombstone/placeholder)
// nodes are sorted by the disambiguator
Treedoc::TreeNode* Treedoc::successor(TreeNode* node) const {
    if (!node->children[1].empty()) 
        return leftmost(node->children[1].front());

    TreeNode* current = node;
    while (true) {
        const Slot& slot = slot_of(current);
        bool found;
        size_t index = index_in_slot(slot, current->disambiguator, &found);
        if (index + 1 < slot.size()) 
            return leftmost(slot[index + 1]);

        if (!current->parent) 
            return nullptr;

        if (current->side == 0) 
            return current->parent; // finished the parent's left slot
        current = current->parent;
    }
}

// visible node with `rank` visible nodes before it
Treedoc::TreeNode* Treedoc::at_visible(size_t rank) const {
    const Slot* slot = &root_;
    while (true) {
        bool descended = false;
        for (TreeNode* node : *slot) {
            if (rank >= node->visible_count) {
                rank -= node->visible_count;
                continue;
            }

            size_t left_visible = visible_in_slot(node->children[0]);
            if (rank < left_visible) {
                slot = &node->children[0];
            } else {
                rank -= left_visible;
                if (node->state == Visible) {
                    if (rank == 0) return node;
                    --rank;
                }
                slot = &node->children[1];
            }
            descended = true;
            break;
        }

        if (!descended) 
            return nullptr;
    }
}

void Treedoc::add_to_visible_counts(TreeNode* node, int delta) {
    for (TreeNode* ancestor = node; ancestor; ancestor = ancestor->parent) {
        ancestor->visible_count = static_cast<uint32_t>(static_cast<int64_t>(ancestor->visible_count) + delta);
    }
}

// returns the child with this disambiguator in the given slot of `parent`
// (the root slot if parent is nullptr), creating an empty placeholder if it is not there
Treedoc::TreeNode* Treedoc::find_or_create_child(TreeNode* parent, uint8_t side, Disambiguator disambiguator) {
    Slot& slot = parent ? parent->children[side] : root_;
    bool found;
    size_t index = index_in_slot(slot, disambiguator, &found);
    if (found)
        return slot[index];

    TreeNode* node = new TreeNode();
    node->disambiguator = disambiguator;
    node->parent = parent;
    node->side = parent ? side : kRootSide;
    slot.insert(slot.begin() + static_cast<std::ptrdiff_t>(index), node);
    ++node_count_;
    return node;
}

TreedocPath Treedoc::path_of(const TreeNode* node) const {
    TreedocPath path;
    for (const TreeNode* ancestor = node; ancestor; ancestor = ancestor->parent) {
        path.push_back(PathStep{ancestor->side, ancestor->disambiguator});
    }

    std::reverse(path.begin(), path.end());

    return path;
}

Treedoc::TreeNode* Treedoc::find(const TreedocPath& path) const {
    if (path.empty() || path[0].side != kRootSide) 
        return nullptr;

    const Slot* slot = &root_;
    TreeNode* node = nullptr;
    for (size_t step = 0; step < path.size(); ++step) {
        if (step > 0) {
            if (path[step].side > 1) 
                return nullptr;

            slot = &node->children[path[step].side];
        }

        bool found;
        size_t index = index_in_slot(*slot, path[step].disambiguator, &found);
        if (!found)
            return nullptr;

        node = (*slot)[index];
    }

    return node;
}

// removes `node` if it is an invisible leaf
// does the same for its parent, and its parent, and its parent..... 
// visible nodes and nodes with children stay
void Treedoc::remove_dead_leaves(TreeNode* node) {
    while (node && node->state != Visible && node->children[0].empty() && node->children[1].empty()) {
        TreeNode* parent = node->parent;
        Slot& slot = slot_of(node);

        bool found;
        size_t index = index_in_slot(slot, node->disambiguator, &found);
        slot.erase(slot.begin() + static_cast<std::ptrdiff_t>(index));
        if (slot.empty()) 
            slot.shrink_to_fit();

        delete node;
        --node_count_;
        node = parent;
    }
}

void Treedoc::make_tombstone(TreeNode* node) {
    node->state = Tombstone;
    add_to_visible_counts(node, -1);
    if (collect_garbage_) 
        remove_dead_leaves(node);
}

Operation Treedoc::insert(size_t position, Value value) {
    if (position > size()) throw std::out_of_range("Treedoc insert position out of range");

    // `previous` is the visible element before the new one and `following` the very next
    // node after it
    // those nodes are neighbours, so either the right slot of
    // `previous` or the left slot of `following` is empty
    // new node goes in that empty slot
    TreeNode* previous = position ? at_visible(position - 1) : nullptr;
    TreeNode* following = previous ? successor(previous) : first();

    TreeNode* parent = nullptr;
    uint8_t side = kRootSide;
    if (previous && previous->children[1].empty()) {
        parent = previous;
        side = 1;
    } else if (following) {
        parent = following;
        side = 0;
    }

    TreeNode* node = find_or_create_child(parent, side, Disambiguator{site_, ++counter_});
    node->state = Visible;
    node->value = value;
    add_to_visible_counts(node, +1);

    auto body = std::make_shared<TreedocInsert>();
    body->path = path_of(node);
    body->value = value;

    return make_operation(OperationKind::Insert, site_, std::move(body));
}

Operation Treedoc::erase(size_t position) {
    TreeNode* node = at_visible(position);
    if (!node) 
        throw std::out_of_range("Treedoc erase position out of range");

    auto body = std::make_shared<TreedocDelete>();
    body->path = path_of(node);
    make_tombstone(node);

    return make_operation(OperationKind::Delete, site_, std::move(body));
}

std::optional<Value> Treedoc::lookup(size_t position) const {
    TreeNode* node = at_visible(position);
    if (!node) 
        return std::nullopt;

    return node->value;
}

std::optional<Value> Treedoc::lookup_id(const ElementId& id) const {
    try {
        ByteReader reader(id);
        TreeNode* node = find(read_path(reader));

        if (!node || node->state != Visible) 
            return std::nullopt;

        return node->value;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

ElementId Treedoc::id_at(size_t position) const {
    TreeNode* node = at_visible(position);
    if (!node) 
        throw std::out_of_range("Treedoc id_at out of range");

    Bytes bytes;
    ByteWriter writer(bytes);
    write_path(writer, path_of(node));
    return bytes_to_id(bytes);
}

template <class Visitor>
void Treedoc::for_each_node(Visitor visit) const {
    for (TreeNode* node = first(); node; node = successor(node)) 
        visit(node);
}

std::vector<Value> Treedoc::traverse() const {
    std::vector<Value> document;
    document.reserve(size());
    for_each_node([&](const TreeNode* node) {
        if (node->state == Visible) 
            document.push_back(node->value);
    });

    return document;
}

ApplyStatus Treedoc::apply(const Operation& operation) {
    if (!operation.body) 
        return ApplyStatus::Rejected;

    if (operation.kind == OperationKind::Insert) {
        auto* body = dynamic_cast<const TreedocInsert*>(operation.body.get());
        if (!body || body->path.empty() || body->path[0].side != kRootSide) return ApplyStatus::Rejected;
        for (size_t step = 1; step < body->path.size(); ++step) {
            if (body->path[step].side > 1) return ApplyStatus::Rejected;
        }

        // follow the path, create missing ancestors as placeholders
        TreeNode* node = nullptr;
        for (const PathStep& step : body->path) 
            node = find_or_create_child(node, step.side, step.disambiguator);

        if (node->state != Placeholder) 
            return ApplyStatus::Duplicate;

        node->state = Visible;
        node->value = body->value;
        add_to_visible_counts(node, +1);

        if (node->disambiguator.site == site_) 
            counter_ = std::max(counter_, node->disambiguator.counter);

        return ApplyStatus::Applied;
    }

    auto* body = dynamic_cast<const TreedocDelete*>(operation.body.get());
    if (!body) 
        return ApplyStatus::Rejected;

    TreeNode* node = find(body->path);
    if (!node || node->state != Visible)
        return ApplyStatus::NoOp;

    make_tombstone(node);

    return ApplyStatus::Applied;
}

void Treedoc::write_path(ByteWriter& writer, const TreedocPath& path) const {
    writer.write_varint(path.size());
    for (const PathStep& step : path) {
        writer.write_varint((static_cast<uint64_t>(step.disambiguator.site) << 2) | step.side);
        writer.write_varint(step.disambiguator.counter);
    }
}

TreedocPath Treedoc::read_path(ByteReader& reader) const {
    TreedocPath path(reader.read_count(2));
    for (PathStep& step : path) {
        uint64_t site_and_side = reader.read_varint();
        if ((site_and_side >> 2) > 0xFFFFFFFFull) 
            throw DecodeError("site out of range");

        step.side = static_cast<uint8_t>(site_and_side & 3);
        step.disambiguator.site = static_cast<uint32_t>(site_and_side >> 2);
        step.disambiguator.counter = reader.read_u32();
    }

    return path;
}

void Treedoc::encode_body(const Operation& operation, ByteWriter& writer) const {
    if (operation.kind == OperationKind::Insert) {
        const auto& body = static_cast<const TreedocInsert&>(*operation.body);
        write_path(writer, body.path);
        writer.write_varint(body.value);
    } else {
        write_path(writer, static_cast<const TreedocDelete&>(*operation.body).path);
    }
}

std::shared_ptr<const OperationBody> Treedoc::decode_body(OperationKind kind, ByteReader& reader) const {
    if (kind == OperationKind::Insert) {
        auto body = std::make_shared<TreedocInsert>();
        body->path = read_path(reader);
        body->value = reader.read_u32();

        return body;
    }

    auto body = std::make_shared<TreedocDelete>();
    body->path = read_path(reader);

    return body;
}

// snapshot = tag, number of root nodes, then every node top-down
Bytes Treedoc::serialize() const {
    Bytes output;
    ByteWriter writer(output);
    writer.write_byte(kSnapshotTag);
    writer.write_varint(root_.size());

    std::vector<const TreeNode*> pending(root_.rbegin(), root_.rend());
    while (!pending.empty()) {
        const TreeNode* node = pending.back();
        pending.pop_back();

        writer.write_varint(node->disambiguator.site);
        writer.write_varint(node->disambiguator.counter);
        writer.write_byte(node->state);

        if (node->state != Placeholder) 
            writer.write_varint(node->value);

        writer.write_varint(node->children[0].size());
        writer.write_varint(node->children[1].size());

        // the waiting list is a stack, therefore need to push right children first so left ones come out first
        for (auto child = node->children[1].rbegin(); child != node->children[1].rend(); ++child) 
            pending.push_back(*child);

        for (auto child = node->children[0].rbegin(); child != node->children[0].rend(); ++child) 
            pending.push_back(*child);
    }
    return output;
}

void Treedoc::deserialize(const Bytes& data) {
    clear();
    counter_ = 0;
    try {
        ByteReader reader(data);
        if (reader.read_byte() != kSnapshotTag) 
            throw DecodeError("not a Treedoc snapshot");

        struct Frame {
            TreeNode* parent;
            uint8_t side;
            size_t remaining;
        };

        std::vector<Frame> frames{{nullptr, kRootSide, reader.read_count(5)}};
        std::vector<TreeNode*> top_down;

        while (!frames.empty()) {
            if (frames.back().remaining == 0) {
                frames.pop_back();
                continue;
            }
            frames.back().remaining--;
            TreeNode* parent = frames.back().parent;
            uint8_t side = frames.back().side;

            TreeNode* node = new TreeNode();
            Slot& slot = parent ? parent->children[side] : root_;
            slot.push_back(node);
            ++node_count_;
            node->parent = parent;
            node->side = parent ? side : kRootSide;

            node->disambiguator.site = reader.read_u32();
            node->disambiguator.counter = reader.read_u32();
            if (slot.size() > 1 && !(slot[slot.size() - 2]->disambiguator < node->disambiguator)) {
                throw DecodeError("slot not sorted");
            }

            node->state = reader.read_byte();

            if (node->state > Tombstone) 
                throw DecodeError("bad node state");

            if (node->state != Placeholder) 
                node->value = reader.read_u32();

            size_t left_count = reader.read_count(5);
            size_t right_count = reader.read_count(5);
            if (node->disambiguator.site == site_) 
                counter_ = std::max(counter_, node->disambiguator.counter);

            top_down.push_back(node);
            frames.push_back({node, 1, right_count});
            frames.push_back({node, 0, left_count});
        }

        // children come after their parents in `top_down`, so walking it backwards
        // lets every node add its finished count to its parent
        for (auto iterator = top_down.rbegin(); iterator != top_down.rend(); ++iterator) {
            TreeNode* node = *iterator;
            node->visible_count += node->state == Visible ? 1 : 0;
            if (node->parent) 
                node->parent->visible_count += node->visible_count;
        }
    } catch (...) {
        clear();
        throw;
    }
}

// the middle element at the root, the halves below it
// (the paper calls this "exploding" a flat text)
uint32_t Treedoc::build_balanced(const std::vector<Value>& values, size_t low, size_t high, TreeNode* parent,
                                 uint8_t side) {
    if (low >= high) 
        return 0;

    size_t middle = low + (high - low) / 2;
    TreeNode* node = new TreeNode();
    node->disambiguator = Disambiguator{kInitialSite, static_cast<uint32_t>(middle + 1)};
    node->state = Visible;
    node->value = values[middle];
    node->parent = parent;
    node->side = parent ? side : kRootSide;
    (parent ? parent->children[side] : root_).push_back(node);
    ++node_count_;

    node->visible_count =
        1 + build_balanced(values, low, middle, node, 0) + build_balanced(values, middle + 1, high, node, 1);\

    return node->visible_count;
}

void Treedoc::bulk_load(const std::vector<Value>& initial) {
    clear();
    counter_ = 0;
    build_balanced(initial, 0, initial.size(), nullptr, kRootSide);
}

size_t Treedoc::get_memory_usage() const {
    size_t bytes = sizeof(*this) + root_.capacity() * sizeof(TreeNode*);
    for_each_node([&](const TreeNode* node) {
        bytes += sizeof(TreeNode) + (node->children[0].capacity() + node->children[1].capacity()) * sizeof(TreeNode*);
    });

    return bytes;
}

StructureStats Treedoc::get_structure_statistics() const {
    size_t visible = 0, tombstones = 0, placeholders = 0;
    size_t depth_sum = 0, max_depth = 0, slot_bytes = 0;
    for_each_node([&](const TreeNode* node) {
        size_t depth = 0;

        for (const TreeNode* ancestor = node; ancestor; ancestor = ancestor->parent)
            ++depth;

        max_depth = std::max(max_depth, depth);

        if (node->state == Visible) {
            ++visible;
            depth_sum += depth;
        } else if (node->state == Tombstone) {
            ++tombstones;
        } else {
            ++placeholders;
        }
        slot_bytes += (node->children[0].capacity() + node->children[1].capacity()) * sizeof(TreeNode*);
    });

    const size_t nodes = node_count_;
    const size_t bytes_per_node = sizeof(TreeNode);
    // in memory a node stores only its disambiguator and a parent link
    // full path is implied by where the node sits in the tree
    const size_t identifier_bytes = sizeof(Disambiguator) + sizeof(TreeNode*) + 1;

    StructureStats stats;
    stats.set("logical_size", static_cast<double>(visible));
    stats.set("stored_elements", static_cast<double>(nodes));
    stats.set("tombstones", static_cast<double>(tombstones));
    stats.set("placeholders", static_cast<double>(placeholders));
    stats.set("pending_ops", 0);
    stats.set("avg_id_depth", visible ? static_cast<double>(depth_sum) / visible : 0.0);
    stats.set("max_id_depth", static_cast<double>(max_depth));
    stats.set("avg_id_bytes", static_cast<double>(identifier_bytes));
    stats.set("id_metadata_bytes", static_cast<double>(nodes * identifier_bytes));
    stats.set("node_bytes", static_cast<double>(nodes * bytes_per_node + slot_bytes));
    stats.set("index_bytes", 0);
    stats.set("tombstone_bytes", static_cast<double>((tombstones + placeholders) * bytes_per_node));
    stats.set("tree_depth", static_cast<double>(max_depth));

    return stats;
}

std::vector<std::string> Treedoc::check_invariants() const {
    std::vector<std::string> errors;
    size_t counted_nodes = 0;
    std::set<std::pair<uint32_t, uint32_t>> seen_disambiguators;

    for (size_t index = 0; index < root_.size(); ++index) {
        if (root_[index]->parent || root_[index]->side != kRootSide) 
            errors.push_back("root node has parent/side");

        if (index && !(root_[index - 1]->disambiguator < root_[index]->disambiguator)) {
            errors.push_back("root slot not strictly sorted");
        }
    }

    // walking twice on each node with first check checking if it
    // and queue its children, then (children_done) to check its visible counter
    std::vector<std::pair<const TreeNode*, bool>> pending;
    for (auto root = root_.rbegin(); root != root_.rend(); ++root) pending.push_back({*root, false});

    while (!pending.empty() && errors.size() < 8) {
        auto [node, children_done] = pending.back();
        pending.pop_back();

        if (children_done) {
            size_t expected = (node->state == Visible ? 1 : 0) + visible_in_slot(node->children[0]) +
                              visible_in_slot(node->children[1]);

            if (expected != node->visible_count) 
                errors.push_back("subtree visible counter mismatch");

            continue;
        }

        ++counted_nodes;

        if (!seen_disambiguators.insert({node->disambiguator.site, node->disambiguator.counter}).second) {
            errors.push_back("duplicate disambiguator");
        }

        if (node->state > Tombstone) 
            errors.push_back("invalid node state");

        pending.push_back({node, true});
        for (uint8_t side = 0; side < 2; ++side) {
            const Slot& slot = node->children[side];
            for (size_t index = 0; index < slot.size(); ++index) {
                if (slot[index]->parent != node || slot[index]->side != side) 
                    errors.push_back("parent/child link mismatch");

                if (index && !(slot[index - 1]->disambiguator < slot[index]->disambiguator)) {
                    errors.push_back("slot not strictly sorted");
                }

                pending.push_back({slot[index], false});
            }
        }

        if (collect_garbage_ && node->state != Visible && node->children[0].empty() && node->children[1].empty()) {
            errors.push_back("uncollected invisible leaf (tombstone GC)");
        }
    }

    if (counted_nodes != node_count_) 
        errors.push_back("node count mismatch");

    return errors;
}

}  // namespace crdt
