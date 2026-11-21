// parts of Logoot and LSEQ that are identical (see position_list.hpp)
#include "crdt/algorithms/position_list.hpp"

#include <algorithm>
#include <limits>
#include <stdexcept>

#include "crdt/algorithms/logoot.hpp"
#include "crdt/algorithms/lseq.hpp"

namespace crdt {

namespace {
constexpr size_t kMaxDepth = 1u << 16;  // safety stop in case identifiers ever grew without limit
}

template <class Rules>
PositionList<Rules>::PositionList(ReplicaId site, Rules rules, uint64_t seed)
    : site_(site), rules_(rules), random_(seed ^ (0xD1B54A32D192ED03ull * (site + 1))), sequence_(0xFEEDull ^ site) {}

// creates new IDs that sort between `left` and `right (nullptr can occur if start or end of doc)
//
// every ID ends with a digit where val min = 1 to gurantee room below.
// level by level walking down.
//
// bounds point to whether the ID built so far still equals to start of `left` or `right
// aka if the neighbour is limiting the next level.
//
// each level free digits are between `low` and `high`
// if there's a free digit, picks one within `boundary` of neighbour based on rules and stops
// else copy level (from left, from right or (0, site, clock) level) and continues deeper
template <class Rules>
typename PositionList<Rules>::Position PositionList<Rules>::generate(const Position* left, const Position* right) {
    ++clock_;
    Position result;
    bool left_bounds = left != nullptr;
    bool right_bounds = right != nullptr;

    for (size_t depth = 0;; ++depth) {
        if (depth >= kMaxDepth) 
            throw std::runtime_error("identifier depth limit exceeded");

        if (right_bounds && depth >= right->size()) 
            throw std::logic_error("generate: right is not greater than left");

        const bool left_has_level = left_bounds && depth < left->size();
        const uint64_t low = left_has_level ? static_cast<uint64_t>((*left)[depth].digit) : 0;
        const uint64_t high = right_bounds ? static_cast<uint64_t>((*right)[depth].digit) : rules_.base(depth);

        if (high > low + 1) {
            const uint64_t free_digits = high - low - 1;
            const uint64_t offset = 1 + random_.below(std::min(free_digits, rules_.boundary));
            const uint64_t digit = rules_.near_left(depth) ? low + offset : high - offset;
            result.push_back(Level{static_cast<typename Rules::Digit>(digit), site_, clock_});
            result.shrink_to_fit();
            return result;
        }

        if (left_has_level) {
            const Level& level = (*left)[depth];
            result.push_back(level);
            if (right_bounds && level < (*right)[depth]) 
                right_bounds = false;

            continue;
        }

        // low is 0 and high is max 1, so the right neighbour limits this level
        if (high == 0) {
            result.push_back((*right)[depth]); // right goes deeper (its last digit is at least 1)
        } else {
            result.push_back(Level{0, site_, clock_});
            right_bounds = false;
        }
        left_bounds = false;
    }
}

template <class Rules>
typename PositionList<Rules>::Node* PositionList<Rules>::first_not_before(const Position& position) const {
    return sequence_.lower_bound([&](const Entry& entry) { return compare_positions(entry.position, position) < 0; });
}

template <class Rules>
typename PositionList<Rules>::Node* PositionList<Rules>::find(const Position& position) const {
    Node* node = first_not_before(position);
    return (node && compare_positions(node->value.position, position) == 0) ? node : nullptr;
}

// non-empty, last digit at least 1 and every digit inside its level's base
template <class Rules>
bool PositionList<Rules>::is_valid(const Position& position) const {
    if (position.empty() || position.back().digit < 1) return false;

    for (size_t depth = 0; depth < position.size(); ++depth) {
        if (static_cast<uint64_t>(position[depth].digit) >= rules_.base(depth))
            return false;
    }

    return true;
}

template <class Rules>
Operation PositionList<Rules>::insert(size_t position, Value value) {
    if (position > size()) 
        throw std::out_of_range("insert position out of range");

    Node* left = position == 0 ? nullptr : sequence_.at_visible(position - 1);
    Node* right = position == size() ? nullptr : sequence_.at_visible(position);

    auto body = std::make_shared<PositionInsert<Rules>>();
    body->position = generate(left ? &left->value.position : nullptr, right ? &right->value.position : nullptr);
    body->value = value;

    // `right` is the first element after the new ID, so the new element goes before it
    sequence_.insert_before(right, Entry{body->position, value}, true);
    return make_operation(OperationKind::Insert, site_, std::move(body));
}

template <class Rules>
Operation PositionList<Rules>::erase(size_t position) {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        throw std::out_of_range("erase position out of range");

    auto body = std::make_shared<PositionDelete<Rules>>();
    body->position = node->value.position;
    sequence_.erase(node);
    return make_operation(OperationKind::Delete, site_, std::move(body));
}

template <class Rules>
std::optional<Value> PositionList<Rules>::lookup(size_t position) const {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        return std::nullopt;

    return node->value.value;
}

template <class Rules>
std::optional<Value> PositionList<Rules>::lookup_id(const ElementId& id) const {
    try {
        ByteReader reader(id);
        Node* node = find(read_position(reader));

        if (!node) 
            return std::nullopt;

        return node->value.value;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

template <class Rules>
ElementId PositionList<Rules>::id_at(size_t position) const {
    Node* node = sequence_.at_visible(position);
    
    if (!node) 
        throw std::out_of_range("id_at out of range");

    Bytes bytes;
    ByteWriter writer(bytes);
    write_position(writer, node->value.position);
    
    return bytes_to_id(bytes);
}

template <class Rules>
std::vector<Value> PositionList<Rules>::traverse() const {
    std::vector<Value> document;
    document.reserve(size());
    sequence_.for_each([&](const Node& node) { document.push_back(node.value.value); });
    return document;
}

template <class Rules>
ApplyStatus PositionList<Rules>::apply(const Operation& operation) {
    if (!operation.body)
         return ApplyStatus::Rejected;

    if (operation.kind == OperationKind::Insert) {
        auto* body = dynamic_cast<const PositionInsert<Rules>*>(operation.body.get());
        if (!body || !is_valid(body->position)) 
            return ApplyStatus::Rejected;

        Node* following = first_not_before(body->position);
        if (following && compare_positions(following->value.position, body->position) == 0) return ApplyStatus::Duplicate;

        sequence_.insert_before(following, Entry{body->position, body->value}, true);
        // keep clock ahead of IDs created before (for example restored from a snapshot)
        const Level& last = body->position.back();
        if (last.site == site_) clock_ = std::max(clock_, last.clock);
        return ApplyStatus::Applied;
    }

    auto* body = dynamic_cast<const PositionDelete<Rules>*>(operation.body.get());
    if (!body) return ApplyStatus::Rejected;

    // missing element means someone else already deleted it
    Node* node = find(body->position);
    if (!node) 
        return ApplyStatus::NoOp;

    sequence_.erase(node);
    return ApplyStatus::Applied;
}

// position is made of number of levels and THEN (digit, site, clock) per level
template <class Rules>
void PositionList<Rules>::write_position(ByteWriter& writer, const Position& position) const {
    writer.write_varint(position.size());
    for (const Level& level : position) {
        writer.write_varint(static_cast<uint64_t>(level.digit));
        writer.write_varint(level.site);
        writer.write_varint(level.clock);
    }
}

template <class Rules>
typename PositionList<Rules>::Position PositionList<Rules>::read_position(ByteReader& reader) const {
    Position position(reader.read_count(3));
    for (Level& level : position) {
        uint64_t digit = reader.read_varint();
        if (digit > static_cast<uint64_t>(std::numeric_limits<typename Rules::Digit>::max())) {
            throw DecodeError("digit out of range");
        }

        level.digit = static_cast<typename Rules::Digit>(digit);
        level.site = reader.read_u32();
        level.clock = reader.read_u32();
    }

    return position;
}

template <class Rules>
void PositionList<Rules>::encode_body(const Operation& operation, ByteWriter& writer) const {
    if (operation.kind == OperationKind::Insert) {
        const auto& body = static_cast<const PositionInsert<Rules>&>(*operation.body);
        write_position(writer, body.position);
        writer.write_varint(body.value);
    } else {
        write_position(writer, static_cast<const PositionDelete<Rules>&>(*operation.body).position);
    }
}

template <class Rules>
std::shared_ptr<const OperationBody> PositionList<Rules>::decode_body(OperationKind kind, ByteReader& reader) const {
    if (kind == OperationKind::Insert) {
        auto body = std::make_shared<PositionInsert<Rules>>();
        body->position = read_position(reader);
        body->value = reader.read_u32();

        return body;
    }

    auto body = std::make_shared<PositionDelete<Rules>>();
    body->position = read_position(reader);

    return body;
}

// snapshot - tag, element count, then (position, value) per element in document order
template <class Rules>
Bytes PositionList<Rules>::serialize() const {
    Bytes output;
    ByteWriter writer(output);
    writer.write_byte(Rules::kSnapshotTag);

    writer.write_varint(size());
    sequence_.for_each([&](const Node& node) {
        write_position(writer, node.value.position);
        writer.write_varint(node.value.value);
    });
    return output;
}

template <class Rules>
void PositionList<Rules>::deserialize(const Bytes& data) {
    ByteReader reader(data);
    if (reader.read_byte() != Rules::kSnapshotTag) throw DecodeError("snapshot tag mismatch");

    size_t entry_count = reader.read_count(5);
    std::vector<std::pair<Entry, bool>> items;
    items.reserve(entry_count);

    uint32_t clock = 0;
    for (size_t index = 0; index < entry_count; ++index) {
        Entry entry;
        entry.position = read_position(reader);
        entry.value = reader.read_u32();

        if (!is_valid(entry.position)) 
            throw DecodeError("invalid position in snapshot");

        if (!items.empty() && compare_positions(items.back().first.position, entry.position) >= 0) {
            throw DecodeError("snapshot positions not strictly increasing");
        }

        for (const Level& level : entry.position) {
            if (level.site == site_) 
                clock = std::max(clock, level.clock);
        }

        items.push_back({std::move(entry), true});
    }

    sequence_.build(std::move(items));
    clock_ = std::max(clock_, clock);
}

template <class Rules>
size_t PositionList<Rules>::get_memory_usage() const {
    size_t identifier_bytes = 0;
    sequence_.for_each([&](const Node& node) { identifier_bytes += node.value.position.capacity() * sizeof(Level); });
    return sizeof(*this) + sequence_.size() * Sequence::node_size() + identifier_bytes;
}

template <class Rules>
StructureStats PositionList<Rules>::get_structure_statistics() const {
    size_t depth_sum = 0;
    size_t max_depth = 0;
    size_t identifier_heap = 0;
    sequence_.for_each([&](const Node& node) {
        depth_sum += node.value.position.size();
        max_depth = std::max(max_depth, node.value.position.size());
        identifier_heap += node.value.position.capacity() * sizeof(Level);
    });

    size_t element_count = size();
    double identifier_bytes = static_cast<double>(identifier_heap + element_count * sizeof(Position));

    StructureStats stats;
    stats.set("logical_size", static_cast<double>(element_count));
    stats.set("stored_elements", static_cast<double>(element_count));
    stats.set("tombstones", 0);
    stats.set("placeholders", 0);
    stats.set("pending_ops", 0);
    stats.set("avg_id_depth", element_count ? static_cast<double>(depth_sum) / element_count : 0.0);
    stats.set("max_id_depth", static_cast<double>(max_depth));
    stats.set("avg_id_bytes", element_count ? identifier_bytes / element_count : 0.0);
    stats.set("id_metadata_bytes", identifier_bytes);
    stats.set("node_bytes", static_cast<double>(sequence_.size() * Sequence::node_size()));
    stats.set("index_bytes", 0);
    stats.set("tombstone_bytes", 0);
    stats.set("tree_depth", static_cast<double>(sequence_.height()));
    return stats;
}

template <class Rules>
std::vector<std::string> PositionList<Rules>::check_invariants() const {
    std::vector<std::string> errors;

    std::string index_error;
    if (!sequence_.check(&index_error)) 
        errors.push_back("index: " + index_error);

    if (sequence_.visible_size() != sequence_.size()) 
        errors.push_back(name() + " must not hold invisible entries");

    const Position* previous = nullptr;
    for (Node* node = sequence_.first(); node; node = Sequence::next(node)) {
        if (!is_valid(node->value.position)) {
            errors.push_back("invalid position identifier (digit range or last digit 0)");
            break;
        }

        if (previous && compare_positions(*previous, node->value.position) >= 0) {
            errors.push_back("positions not strictly increasing / duplicate identifier");
            break;
        }

        previous = &node->value.position;
    }
    
    return errors;
}

// the two algorithms
template class PositionList<LogootRules>;
template class PositionList<LseqRules>;

}  // namespace crdt
