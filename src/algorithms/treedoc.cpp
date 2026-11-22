#include "crdt/algorithms/treedoc.hpp"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace crdt {

namespace {

constexpr uint8_t kSnapshotTag = 'T';
constexpr Disambiguator kNone{0, 0}; // atoms of the exploded starting tree have no disambiguator

bool is_none(Disambiguator disambiguator) { return disambiguator.counter == 0 && disambiguator.site == 0; }

// packs single bits into bytes, lowest bit first
class BitWriter {
public:
    explicit BitWriter(ByteWriter& writer) : writer_(writer) {}

    void put(bool bit) {
        if (bit) current_ = static_cast<uint8_t>(current_ | (1u << used_));
        if (++used_ == 8) flush();
    }

    void flush() {
        if (used_ == 0) return;
        writer_.write_byte(current_);
        current_ = 0;
        used_ = 0;
    }

private:
    ByteWriter& writer_;
    uint8_t current_ = 0;
    unsigned used_ = 0;
};

class BitReader {
public:
    explicit BitReader(ByteReader& reader) : reader_(reader) {}

    bool get() {
        if (used_ == 8) {
            current_ = reader_.read_byte(); // throws at the end of the input
            used_ = 0;
        }
        return (current_ >> used_++) & 1u;
    }

private:
    ByteReader& reader_;
    uint8_t current_ = 0;
    unsigned used_ = 8;
};

}  // namespace

Treedoc::Treedoc(ReplicaId site, bool collect_garbage) : site_(site), collect_garbage_(collect_garbage) {}

Treedoc::~Treedoc() { clear(); }

// deletes all nodes
// no recursion, as tree can be many many many levels deep
void Treedoc::clear() {
    std::vector<Major*> pending;
    if (root_) 
        pending.push_back(root_);

    while (!pending.empty()) {
        Major* major = pending.back();
        pending.pop_back();

        for (Major* child : major->children) {
            if (child) pending.push_back(child);
        }
        if (major->extra) {
            for (const MiniChild& child : major->extra->mini_children) pending.push_back(child.major);
        }

        delete major;
    }

    root_ = nullptr;
    mini_total_ = 0;
    major_total_ = 0;
}

//mini-nodes of one major node (kept sorted by disambiguator)
bool Treedoc::find_mini(const Major* major, Disambiguator disambiguator, uint32_t* index) {
    const Mini* begin = minis(major);
    const Mini* end = begin + major->mini_count;
    const Mini* position = std::lower_bound(
        begin, end, disambiguator, [](const Mini& mini, Disambiguator key) { return mini.disambiguator < key; });

    *index = static_cast<uint32_t>(position - begin);
    return position != end && position->disambiguator == disambiguator;
}

uint32_t Treedoc::add_mini(Major* major, Disambiguator disambiguator) {
    uint32_t index;
    find_mini(major, disambiguator, &index);

    Mini mini;
    mini.disambiguator = disambiguator;
    if (major->mini_count == 0) {
        major->single = mini;
    } else {
        if (!major->extra) major->extra = std::make_unique<Extra>();
        std::vector<Mini>& list = major->extra->minis;
        if (major->mini_count == 1) {
            list.clear();
            list.push_back(major->single);
            major->single = Mini{};
        }
        list.insert(list.begin() + index, mini);
    }

    ++major->mini_count;
    ++mini_total_;
    return index;
}

void Treedoc::remove_mini(Major* major, uint32_t index) {
    if (major->mini_count == 1) {
        major->single = Mini{};
    } else {
        std::vector<Mini>& list = major->extra->minis;
        list.erase(list.begin() + index);
        if (list.size() == 1) {
            major->single = list.front();
            list.clear();
            list.shrink_to_fit();
        }
    }

    --major->mini_count;
    --mini_total_;
    if (major->extra && major->extra->minis.empty() && major->extra->mini_children.empty()) major->extra.reset();
}

Treedoc::Major* Treedoc::mini_child(const Major* major, Disambiguator owner, uint8_t side) {
    if (!major->extra) return nullptr;
    for (const MiniChild& child : major->extra->mini_children) {
        if (child.side == side && child.owner == owner) return child.major;
    }

    return nullptr;
}

bool Treedoc::has_mini_children(const Major* major, Disambiguator owner) {
    return mini_child(major, owner, 0) || mini_child(major, owner, 1);
}

// creates an empty major node and links it as a child of `parent`
Treedoc::Major* Treedoc::new_major(Major* parent, uint8_t side, bool via_mini, Disambiguator via) {
    Major* major = new Major();
    major->parent = parent;
    major->side = parent ? side : kRootSide;
    major->via_mini = parent && via_mini;
    major->via = major->via_mini ? via : kNone;

    if (!parent) {
        root_ = major;
    } else if (via_mini) {
        if (!parent->extra) parent->extra = std::make_unique<Extra>();
        parent->extra->mini_children.push_back(MiniChild{via, side, major});
    } else {
        parent->children[side] = major;
    }

    ++major_total_;
    return major;
}

// unlinks an empty major node without children from its parent and deletes it
void Treedoc::detach(Major* major) {
    Major* parent = major->parent;
    if (!parent) {
        root_ = nullptr;
    } else if (major->via_mini) {
        std::vector<MiniChild>& list = parent->extra->mini_children;
        list.erase(std::find_if(list.begin(), list.end(), [&](const MiniChild& child) { return child.major == major; }));
        if (list.empty() && parent->extra->minis.empty()) parent->extra.reset();
    } else {
        parent->children[major->side] = nullptr;
    }

    delete major;
    --major_total_;
}

Treedoc::Spot Treedoc::leftmost(Major* major) {
    while (true) {
        if (major->children[0]) {
            major = major->children[0];
            continue;
        }

        if (major->mini_count > 0) {
            Major* left = mini_child(major, minis(major)[0].disambiguator, 0);
            if (!left) return Spot{major, 0};
            major = left;
            continue;
        }

        // empty placeholder major node, only its right child can hold anything
        if (!major->children[1]) return Spot{};
        major = major->children[1];
    }
}

Treedoc::Spot Treedoc::leftmost_from_mini(Major* major, uint32_t index) {
    Major* left = mini_child(major, minis(major)[index].disambiguator, 0);
    return left ? leftmost(left) : Spot{major, index};
}

Treedoc::Spot Treedoc::after_mini(Major* major, uint32_t index) {
    while (true) {
        if (index + 1 < major->mini_count) return leftmost_from_mini(major, index + 1);
        if (major->children[1]) return leftmost(major->children[1]);

        // the whole subtree of `major` is done, climb up
        while (true) {
            Major* parent = major->parent;
            if (!parent) return Spot{};

            if (major->via_mini) {
                uint32_t owner;
                find_mini(parent, major->via, &owner);
                if (major->side == 0) return Spot{parent, owner}; // left child of a mini-node, the mini-node is next
                major = parent;
                index = owner;
                break;
            }

            if (major->side == 0) {
                if (parent->mini_count > 0) return leftmost_from_mini(parent, 0);
                if (parent->children[1]) return leftmost(parent->children[1]);
            }
            major = parent;
        }
    }
}

Treedoc::Spot Treedoc::first() const { return root_ ? leftmost(root_) : Spot{}; }

// next mini-node, counting every mini-node (visible, tombstone, placeholder)
Treedoc::Spot Treedoc::successor(Spot spot) const {
    Major* right = mini_child(spot.major, mini_at(spot).disambiguator, 1);
    return right ? leftmost(right) : after_mini(spot.major, spot.index);
}

// visible mini-node with `rank` visible mini-nodes before it
Treedoc::Spot Treedoc::at_visible(size_t rank) const {
    Major* major = root_;
    if (!major || rank >= major->visible_count) return Spot{};

    while (true) {
        if (Major* left = major->children[0]) {
            if (rank < left->visible_count) {
                major = left;
                continue;
            }
            rank -= left->visible_count;
        }

        Major* next = nullptr;
        for (uint32_t index = 0; index < major->mini_count && !next; ++index) {
            const Mini& mini = minis(major)[index];
            Major* mini_left = major->extra ? mini_child(major, mini.disambiguator, 0) : nullptr;
            if (mini_left) {
                if (rank < mini_left->visible_count) {
                    next = mini_left;
                    break;
                }
                rank -= mini_left->visible_count;
            }

            if (mini.state == Visible) {
                if (rank == 0) return Spot{major, index};
                --rank;
            }

            Major* mini_right = major->extra ? mini_child(major, mini.disambiguator, 1) : nullptr;
            if (mini_right) {
                if (rank < mini_right->visible_count) {
                    next = mini_right;
                    break;
                }
                rank -= mini_right->visible_count;
            }
        }

        if (!next) next = major->children[1];
        if (!next) return Spot{};
        major = next;
    }
}

size_t Treedoc::size() const { return root_ ? root_->visible_count : 0; }

void Treedoc::add_to_visible_counts(Major* major, int delta) {
    for (Major* ancestor = major; ancestor; ancestor = ancestor->parent) {
        ancestor->visible_count = static_cast<uint32_t>(static_cast<int64_t>(ancestor->visible_count) + delta);
    }
}

// a disambiguator is written only at the last element and where the path goes on under one
// specific mini-node
TreedocPath Treedoc::path_of(Spot spot) const {
    TreedocPath path;
    const Mini& target = mini_at(spot);
    path.push_back(PathStep{spot.major->side, !is_none(target.disambiguator), target.disambiguator});

    for (const Major* major = spot.major; major->parent; major = major->parent) {
        PathStep step;
        step.side = major->parent->side;
        if (major->via_mini) {
            step.has_disambiguator = true;
            step.disambiguator = major->via;
        }
        path.push_back(step);
    }

    std::reverse(path.begin(), path.end());
    return path;
}

Treedoc::Spot Treedoc::find(const TreedocPath& path) const {
    if (path.empty() || path[0].side != kRootSide || !root_) return Spot{};

    Major* major = root_;
    for (size_t step = 1; step < path.size(); ++step) {
        const uint8_t side = path[step].side;
        if (side > 1) return Spot{};

        const PathStep& previous = path[step - 1];
        major = previous.has_disambiguator ? mini_child(major, previous.disambiguator, side) : major->children[side];
        if (!major) return Spot{};
    }

    const PathStep& last = path.back();
    uint32_t index;
    if (!find_mini(major, last.has_disambiguator ? last.disambiguator : kNone, &index)) return Spot{};
    return Spot{major, index};
}

void Treedoc::write_path(ByteWriter& writer, const TreedocPath& path) const {
    writer.write_varint(path.size());

    BitWriter bits(writer);
    for (size_t step = 1; step < path.size(); ++step) bits.put(path[step].side == 1);
    for (const PathStep& step : path) bits.put(step.has_disambiguator);
    bits.flush();

    for (const PathStep& step : path) {
        if (!step.has_disambiguator) continue;
        writer.write_varint(step.disambiguator.counter);
        writer.write_varint(step.disambiguator.site);
    }
}

TreedocPath Treedoc::read_path(ByteReader& reader) const {
    const uint64_t length = reader.read_varint();
    if (length == 0) throw DecodeError("empty Treedoc path");

    // steps are added while their bits are read, so a bad length runs out of input first;
    // reserving is capped so that a bad length cannot allocate much either
    TreedocPath path;
    path.reserve(static_cast<size_t>(std::min<uint64_t>(length, 4096)));
    path.push_back(PathStep{kRootSide, false, kNone});
    BitReader bits(reader);
    for (uint64_t step = 1; step < length; ++step) path.push_back(PathStep{static_cast<uint8_t>(bits.get() ? 1 : 0), false, kNone});
    for (PathStep& step : path) step.has_disambiguator = bits.get();

    for (PathStep& step : path) {
        if (!step.has_disambiguator) continue;
        step.disambiguator.counter = reader.read_u32();
        step.disambiguator.site = reader.read_u32();
    }

    return path;
}

void Treedoc::make_tombstone(Spot spot) {
    mini_at(spot).state = Tombstone;
    add_to_visible_counts(spot.major, -1);
    if (collect_garbage_) collect(spot.major);
}

// garbage collection
void Treedoc::collect(Major* major) {
    while (major) {
        for (uint32_t index = major->mini_count; index-- > 0;) {
            const Mini& mini = minis(major)[index];
            if (mini.state != Visible && !has_mini_children(major, mini.disambiguator)) remove_mini(major, index);
        }

        if (major->mini_count > 0 || major->children[0] || major->children[1]) return;

        Major* parent = major->parent;
        detach(major);
        major = parent;
    }
}

Operation Treedoc::insert(size_t position, Value value) {
    if (position > size()) throw std::out_of_range("Treedoc insert position out of range");

    Spot previous = position ? at_visible(position - 1) : Spot{};
    Major* target = nullptr;

    if (previous) {
        Major* major = previous.major;
        const Disambiguator owner = mini_at(previous).disambiguator;
        const bool right_taken = mini_child(major, owner, 1) != nullptr;

        if (!right_taken && previous.index + 1 < major->mini_count) {
            target = new_major(major, 1, true, owner);
        } else if (!right_taken && !major->children[1]) {
            target = new_major(major, 1, false, kNone);
        } else {
            Spot following = successor(previous);
            target = new_major(following.major, 0, false, kNone);
        }
    } else if (root_) {
        target = new_major(first().major, 0, false, kNone);
    } else {
        target = new_major(nullptr, kRootSide, false, kNone);
    }

    const uint32_t index = add_mini(target, Disambiguator{++counter_, site_});
    Mini& mini = minis(target)[index];
    mini.state = Visible;
    mini.value = value;
    add_to_visible_counts(target, +1);

    auto body = std::make_shared<TreedocInsert>();
    body->path = path_of(Spot{target, index});
    body->value = value;

    return make_operation(OperationKind::Insert, site_, std::move(body));
}

Operation Treedoc::erase(size_t position) {
    Spot spot = at_visible(position);
    if (!spot) throw std::out_of_range("Treedoc erase position out of range");

    auto body = std::make_shared<TreedocDelete>();
    body->path = path_of(spot);
    make_tombstone(spot);

    return make_operation(OperationKind::Delete, site_, std::move(body));
}

std::optional<Value> Treedoc::lookup(size_t position) const {
    Spot spot = at_visible(position);
    if (!spot) return std::nullopt;

    return mini_at(spot).value;
}

std::optional<Value> Treedoc::lookup_id(const ElementId& id) const {
    try {
        ByteReader reader(id);
        Spot spot = find(read_path(reader));
        if (!spot || mini_at(spot).state != Visible) return std::nullopt;

        return mini_at(spot).value;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

ElementId Treedoc::id_at(size_t position) const {
    Spot spot = at_visible(position);
    if (!spot) throw std::out_of_range("Treedoc id_at out of range");

    Bytes bytes;
    ByteWriter writer(bytes);
    write_path(writer, path_of(spot));
    return bytes_to_id(bytes);
}

std::vector<Value> Treedoc::traverse() const {
    std::vector<Value> document;
    document.reserve(size());
    for (Spot spot = first(); spot; spot = successor(spot)) {
        if (mini_at(spot).state == Visible) document.push_back(mini_at(spot).value);
    }

    return document;
}

ApplyStatus Treedoc::apply(const Operation& operation) {
    if (!operation.body) return ApplyStatus::Rejected;

    if (operation.kind == OperationKind::Insert) {
        auto* body = dynamic_cast<const TreedocInsert*>(operation.body.get());
        if (!body || body->path.empty() || body->path[0].side != kRootSide) return ApplyStatus::Rejected;
        for (size_t step = 1; step < body->path.size(); ++step) {
            if (body->path[step].side > 1) return ApplyStatus::Rejected;
        }

        // a new atom always has its own disambiguator
        const PathStep& last = body->path.back();
        if (!last.has_disambiguator || is_none(last.disambiguator)) return ApplyStatus::Rejected;

        // follow the path, recreate missing ancestors as placeholders
        Major* major = root_ ? root_ : new_major(nullptr, kRootSide, false, kNone);
        for (size_t step = 1; step < body->path.size(); ++step) {
            const PathStep& previous = body->path[step - 1];
            const uint8_t side = body->path[step].side;

            if (previous.has_disambiguator) {
                uint32_t owner;
                if (!find_mini(major, previous.disambiguator, &owner)) add_mini(major, previous.disambiguator);
                Major* child = mini_child(major, previous.disambiguator, side);
                major = child ? child : new_major(major, side, true, previous.disambiguator);
            } else {
                Major* child = major->children[side];
                major = child ? child : new_major(major, side, false, kNone);
            }
        }

        uint32_t index;
        if (find_mini(major, last.disambiguator, &index)) {
            if (minis(major)[index].state != Placeholder) return ApplyStatus::Duplicate;
        } else {
            index = add_mini(major, last.disambiguator);
        }

        Mini& mini = minis(major)[index];
        mini.state = Visible;
        mini.value = body->value;
        add_to_visible_counts(major, +1);

        if (last.disambiguator.site == site_) counter_ = std::max(counter_, last.disambiguator.counter);

        return ApplyStatus::Applied;
    }

    auto* body = dynamic_cast<const TreedocDelete*>(operation.body.get());
    if (!body) return ApplyStatus::Rejected;

    Spot spot = find(body->path);
    if (!spot || mini_at(spot).state != Visible) return ApplyStatus::NoOp;

    make_tombstone(spot);
    return ApplyStatus::Applied;
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

template <class Visitor>
void Treedoc::for_each_major(Visitor visit) const {
    std::vector<const Major*> pending;
    if (root_) pending.push_back(root_);

    while (!pending.empty()) {
        const Major* major = pending.back();
        pending.pop_back();
        visit(major);

        for (const Major* child : major->children) {
            if (child) pending.push_back(child);
        }
        if (major->extra) {
            for (const MiniChild& child : major->extra->mini_children) pending.push_back(child.major);
        }
    }
}

Bytes Treedoc::serialize() const {
    Bytes output;
    ByteWriter writer(output);
    writer.write_byte(kSnapshotTag);
    writer.write_byte(root_ ? 1 : 0);

    std::vector<const Major*> pending;
    if (root_) pending.push_back(root_);
    std::vector<const Major*> children;

    while (!pending.empty()) {
        const Major* major = pending.back();
        pending.pop_back();

        writer.write_byte(static_cast<uint8_t>((major->children[0] ? 1 : 0) | (major->children[1] ? 2 : 0)));
        writer.write_varint(major->mini_count);

        children.clear();
        if (major->children[0]) children.push_back(major->children[0]);
        for (uint32_t index = 0; index < major->mini_count; ++index) {
            const Mini& mini = minis(major)[index];
            const Major* left = mini_child(major, mini.disambiguator, 0);
            const Major* right = mini_child(major, mini.disambiguator, 1);

            writer.write_varint(mini.disambiguator.counter);
            writer.write_varint(mini.disambiguator.site);
            writer.write_byte(static_cast<uint8_t>(mini.state | (left ? 4 : 0) | (right ? 8 : 0)));
            if (mini.state != Placeholder) writer.write_varint(mini.value);

            if (left) children.push_back(left);
            if (right) children.push_back(right);
        }
        if (major->children[1]) children.push_back(major->children[1]);

        // the waiting list is a stack, so children go in reversed to come out in order
        for (auto child = children.rbegin(); child != children.rend(); ++child) pending.push_back(*child);
    }

    return output;
}

void Treedoc::deserialize(const Bytes& data) {
    clear();
    counter_ = 0;
    try {
        ByteReader reader(data);
        if (reader.read_byte() != kSnapshotTag) throw DecodeError("not a Treedoc snapshot");

        // where the next major node in the stream is attached
        struct Attach {
            Major* parent;
            uint8_t side;
            bool via_mini;
            Disambiguator via;
        };

        std::vector<Attach> pending;
        if (reader.read_byte() != 0) pending.push_back(Attach{nullptr, kRootSide, false, kNone});
        std::vector<Major*> top_down;
        std::vector<Attach> children;

        while (!pending.empty()) {
            Attach attach = pending.back();
            pending.pop_back();

            Major* major = new_major(attach.parent, attach.side, attach.via_mini, attach.via);
            top_down.push_back(major);

            const uint8_t flags = reader.read_byte();
            if (flags > 3) throw DecodeError("bad Treedoc node flags");
            const size_t mini_count = reader.read_count(3);

            children.clear();
            if (flags & 1) children.push_back(Attach{major, 0, false, kNone});

            for (size_t count = 0; count < mini_count; ++count) {
                Disambiguator disambiguator;
                disambiguator.counter = reader.read_u32();
                disambiguator.site = reader.read_u32();
                if (major->mini_count > 0 && !(minis(major)[major->mini_count - 1].disambiguator < disambiguator)) {
                    throw DecodeError("mini-nodes not sorted");
                }

                const uint8_t state_and_flags = reader.read_byte();
                const uint8_t state = static_cast<uint8_t>(state_and_flags & 3u);
                if (state > Tombstone || (state_and_flags & ~15u)) throw DecodeError("bad mini-node state");

                const uint32_t index = add_mini(major, disambiguator);
                Mini& mini = minis(major)[index];
                mini.state = state;
                if (state != Placeholder) mini.value = reader.read_u32();
                if (disambiguator.site == site_) counter_ = std::max(counter_, disambiguator.counter);

                if (state_and_flags & 4) children.push_back(Attach{major, 0, true, disambiguator});
                if (state_and_flags & 8) children.push_back(Attach{major, 1, true, disambiguator});
            }
            if (flags & 2) children.push_back(Attach{major, 1, false, kNone});

            for (auto child = children.rbegin(); child != children.rend(); ++child) pending.push_back(*child);
        }

        // children come after their parents in `top_down`, so walking it backwards
        // lets every major node add its finished count to its parent
        for (auto iterator = top_down.rbegin(); iterator != top_down.rend(); ++iterator) {
            Major* major = *iterator;
            for (uint32_t index = 0; index < major->mini_count; ++index) {
                if (minis(major)[index].state == Visible) ++major->visible_count;
            }
            if (major->parent) major->parent->visible_count += major->visible_count;
        }
    } catch (...) {
        clear();
        throw;
    }
}

// "explode" - the middle atom at the root, the halves below it.
// exploded atoms have no disambiguator, so their paths are plain bit strings
Treedoc::Major* Treedoc::build_balanced(const std::vector<Value>& values, size_t low, size_t high, Major* parent,
                                        uint8_t side) {
    if (low >= high) return nullptr;

    const size_t middle = low + (high - low) / 2;
    Major* major = new_major(parent, side, false, kNone);
    const uint32_t index = add_mini(major, kNone);
    minis(major)[index].state = Visible;
    minis(major)[index].value = values[middle];

    Major* left = build_balanced(values, low, middle, major, 0);
    Major* right = build_balanced(values, middle + 1, high, major, 1);
    major->visible_count = 1 + (left ? left->visible_count : 0) + (right ? right->visible_count : 0);

    return major;
}

void Treedoc::bulk_load(const std::vector<Value>& initial) {
    clear();
    counter_ = 0;
    build_balanced(initial, 0, initial.size(), nullptr, kRootSide);
}

size_t Treedoc::get_memory_usage() const {
    size_t bytes = sizeof(*this);
    for_each_major([&](const Major* major) {
        bytes += sizeof(Major);
        if (major->extra) {
            bytes += sizeof(Extra) + major->extra->minis.capacity() * sizeof(Mini) +
                     major->extra->mini_children.capacity() * sizeof(MiniChild);
        }
    });

    return bytes;
}

StructureStats Treedoc::get_structure_statistics() const {
    size_t visible = 0, tombstones = 0, placeholder_minis = 0, empty_majors = 0;
    size_t depth_sum = 0, max_depth = 0, node_bytes = 0;

    // depth = number of major nodes on the path from the root
    std::vector<std::pair<const Major*, size_t>> pending;
    if (root_) pending.push_back({root_, 1});
    while (!pending.empty()) {
        auto [major, depth] = pending.back();
        pending.pop_back();
        max_depth = std::max(max_depth, depth);

        node_bytes += sizeof(Major);
        if (major->extra) {
            node_bytes += sizeof(Extra) + major->extra->minis.capacity() * sizeof(Mini) +
                          major->extra->mini_children.capacity() * sizeof(MiniChild);
        }
        if (major->mini_count == 0) ++empty_majors;

        for (uint32_t index = 0; index < major->mini_count; ++index) {
            const Mini& mini = minis(major)[index];
            if (mini.state == Visible) {
                ++visible;
                depth_sum += depth;
            } else if (mini.state == Tombstone) {
                ++tombstones;
            } else {
                ++placeholder_minis;
            }
        }

        for (const Major* child : major->children) {
            if (child) pending.push_back({child, depth + 1});
        }
        if (major->extra) {
            for (const MiniChild& child : major->extra->mini_children) pending.push_back({child.major, depth + 1});
        }
    }

    // in memory an atom stores only its disambiguator and its place in the tree
    const size_t identifier_bytes = sizeof(Disambiguator) + sizeof(Major*) + 1;
    const size_t stored = mini_total_ + empty_majors;

    StructureStats stats;
    stats.set("logical_size", static_cast<double>(visible));
    stats.set("stored_elements", static_cast<double>(stored));
    stats.set("tombstones", static_cast<double>(tombstones));
    stats.set("placeholders", static_cast<double>(placeholder_minis + empty_majors));
    stats.set("pending_ops", 0);
    stats.set("avg_id_depth", visible ? static_cast<double>(depth_sum) / static_cast<double>(visible) : 0.0);
    stats.set("max_id_depth", static_cast<double>(max_depth));
    stats.set("avg_id_bytes", static_cast<double>(identifier_bytes));
    stats.set("id_metadata_bytes", static_cast<double>(stored * identifier_bytes));
    stats.set("node_bytes", static_cast<double>(node_bytes));
    stats.set("index_bytes", 0);
    stats.set("tombstone_bytes",
              static_cast<double>((tombstones + placeholder_minis) * sizeof(Mini) + empty_majors * sizeof(Major)));
    stats.set("tree_depth", static_cast<double>(max_depth));

    return stats;
}

std::vector<std::string> Treedoc::check_invariants() const {
    std::vector<std::string> errors;
    size_t counted_minis = 0, counted_majors = 0;
    std::set<std::pair<uint32_t, uint32_t>> seen_disambiguators;

    if (root_ && (root_->parent || root_->side != kRootSide || root_->via_mini)) errors.push_back("root has parent/side");

    for_each_major([&](const Major* major) {
        if (errors.size() >= 8) return;
        ++counted_majors;
        counted_minis += major->mini_count;

        if (major->mini_count >= 2 && (!major->extra || major->extra->minis.size() != major->mini_count)) {
            errors.push_back("mini-node storage mismatch");
            return;
        }
        if (major->mini_count < 2 && major->extra && !major->extra->minis.empty()) {
            errors.push_back("unused mini-node list");
        }

        size_t expected_visible = 0;
        for (uint32_t index = 0; index < major->mini_count; ++index) {
            const Mini& mini = minis(major)[index];
            if (index && !(minis(major)[index - 1].disambiguator < mini.disambiguator)) {
                errors.push_back("mini-nodes not strictly sorted");
            }
            if (mini.state > Tombstone) errors.push_back("invalid mini-node state");
            if (mini.state == Visible) ++expected_visible;
            if (!is_none(mini.disambiguator) &&
                !seen_disambiguators.insert({mini.disambiguator.counter, mini.disambiguator.site}).second) {
                errors.push_back("duplicate disambiguator");
            }
            if (collect_garbage_ && mini.state != Visible && !has_mini_children(major, mini.disambiguator)) {
                errors.push_back("uncollected invisible mini-node (tombstone GC)");
            }
        }

        for (uint8_t side = 0; side < 2; ++side) {
            const Major* child = major->children[side];
            if (!child) continue;
            if (child->parent != major || child->side != side || child->via_mini) {
                errors.push_back("parent/child link mismatch");
            }
            expected_visible += child->visible_count;
        }

        if (major->extra) {
            std::set<std::pair<std::pair<uint32_t, uint32_t>, uint8_t>> seen_children;
            for (const MiniChild& child : major->extra->mini_children) {
                uint32_t owner;
                if (!find_mini(major, child.owner, &owner)) errors.push_back("mini-node child without its mini-node");
                if (child.major->parent != major || !child.major->via_mini || !(child.major->via == child.owner) ||
                    child.major->side != child.side || child.side > 1) {
                    errors.push_back("mini-node child link mismatch");
                }
                if (!seen_children.insert({{child.owner.counter, child.owner.site}, child.side}).second) {
                    errors.push_back("duplicate mini-node child");
                }
                expected_visible += child.major->visible_count;
            }
        }

        if (expected_visible != major->visible_count) errors.push_back("subtree visible counter mismatch");

        const bool has_children = major->children[0] || major->children[1] ||
                                  (major->extra && !major->extra->mini_children.empty());
        if (major->mini_count == 0 && !has_children) errors.push_back("empty leaf major node");
    });

    if (counted_minis != mini_total_ || counted_majors != major_total_) errors.push_back("node count mismatch");

    return errors;
}

}  // namespace crdt
