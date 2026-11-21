#include "crdt/algorithms/woot.hpp"

#include <algorithm>
#include <stdexcept>

#include "crdt/algorithms/waiting_operations.hpp"

namespace crdt {

namespace {

// the begin and end markers use a site number owned by no replicas
constexpr uint32_t kMarkerSite = 0xFFFFFFFFu;
constexpr WootId kBeginId{kMarkerSite, 0};
constexpr WootId kEndId{kMarkerSite, 1};
constexpr uint8_t kSnapshotTag = 'W';

const WootChar kBeginMarker{kBeginId, kBeginId, kEndId, 0};
const WootChar kEndMarker{kEndId, kBeginId, kEndId, 0};

// packs (site, clock) into one number for the hash map
uint64_t index_key(WootId id) { 
    return (static_cast<uint64_t>(id.site) << 32) | id.clock;
}

// the site is written as site + 1, marker site fits into one byte
void write_id(ByteWriter& writer, WootId id) {
    writer.write_varint(static_cast<uint32_t>(id.site + 1u));
    writer.write_varint(id.clock);
}

WootId read_id(ByteReader& reader) {
    WootId id;
    id.site = static_cast<uint32_t>(reader.read_u32() - 1u);
    id.clock = reader.read_u32();
    return id;
}

void write_character(ByteWriter& writer, const WootChar& character) {
    write_id(writer, character.id);
    write_id(writer, character.previous_id);
    write_id(writer, character.next_id);
    writer.write_varint(character.value);
}

WootChar read_character(ByteReader& reader) {
    WootChar character;
    character.id = read_id(reader);
    character.previous_id = read_id(reader);
    character.next_id = read_id(reader);
    character.value = reader.read_u32();
    return character;
}

}  // namespace

Woot::Woot(ReplicaId site) : site_(site), sequence_(0xC0FFEEull ^ site) {
    begin_ = sequence_.insert_after(nullptr, kBeginMarker, false);
    end_ = sequence_.insert_before(nullptr, kEndMarker, false);
    id_index_[index_key(kBeginId)] = begin_;
    id_index_[index_key(kEndId)] = end_;
}

Woot::Node* Woot::find(WootId id) const {
    auto found = id_index_.find(index_key(id));
    return found == id_index_.end() ? nullptr : found->second;
}

// differs from paper, uses a loop instead of recursion
//
// if nothing is between `previous` & `next` - the character will go there
// else chars inserted by other replicas go in this gap concurrently.
// only looking at chars in the gap whose neighbours are outside the gap (typed directly)
// find where new char fits between them by ID order and then repeat with the smaller gap
void Woot::integrate(const WootChar& character, Node* previous, Node* next) {
    std::vector<Node*> candidates;
    while (true) {
        size_t previous_index = sequence_.index_of(previous);
        size_t next_index = sequence_.index_of(next);
        if (next_index - previous_index == 1) {
            Node* node = sequence_.insert_before(next, character, true);
            id_index_[index_key(character.id)] = node;
            return;
        }

        candidates.clear();
        candidates.push_back(previous);
        for (Node* between = Sequence::next(previous); between != next; between = Sequence::next(between)) {
            Node* its_previous = find(between->value.previous_id);
            Node* its_next = find(between->value.next_id);

            if (sequence_.index_of(its_previous) <= previous_index && sequence_.index_of(its_next) >= next_index) {
                candidates.push_back(between);
            }
        }

        candidates.push_back(next);

        size_t slot = 1;
        while (slot < candidates.size() - 1 && candidates[slot]->value.id < character.id) 
            ++slot;

        previous = candidates[slot - 1];
        next = candidates[slot];
    }
}

Operation Woot::insert(size_t position, Value value) {
    if (position > size()) 
        throw std::out_of_range("WOOT insert position out of range");

    Node* previous = position == 0 ? begin_ : sequence_.at_visible(position - 1);
    Node* next = position == size() ? end_ : sequence_.at_visible(position);
    WootChar character{WootId{site_, ++clock_}, previous->value.id, next->value.id, value};
    integrate(character, previous, next);

    auto body = std::make_shared<WootInsert>();
    body->character = character;

    return make_operation(OperationKind::Insert, site_, std::move(body));
}

// delete -> creates tombstone
Operation Woot::hide(Node* node) {
    sequence_.set_visible(node, false);

    auto body = std::make_shared<WootDelete>();
    body->id = node->value.id;

    return make_operation(OperationKind::Delete, site_, std::move(body));
}

Operation Woot::erase(size_t position) {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        throw std::out_of_range("WOOT erase position out of range");

    return hide(node);
}

std::optional<Value> Woot::lookup(size_t position) const {
    Node* node = sequence_.at_visible(position);
    if (!node)
        return std::nullopt;

    return node->value.value;
}

std::optional<Value> Woot::lookup_id(const ElementId& id) const {
    try {
        ByteReader reader(id);
        Node* node = find(read_id(reader));
        if (!node || !node->visible)
            return std::nullopt;

        return node->value.value;
    } catch (const DecodeError&) {
        return std::nullopt;
    }
}

ElementId Woot::id_at(size_t position) const {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        throw std::out_of_range("WOOT id_at out of range");

    Bytes bytes;
    ByteWriter writer(bytes);
    write_id(writer, node->value.id);
    return bytes_to_id(bytes);
}

std::vector<Value> Woot::traverse() const {
    std::vector<Value> document;
    document.reserve(size());
    sequence_.for_each([&](const Node& node) {
        if (node.visible) 
            document.push_back(node.value.value);
    });
    return document;
}

ApplyStatus Woot::try_apply(const Operation& operation) {
    if (operation.kind == OperationKind::Insert) {
        auto* body = dynamic_cast<const WootInsert*>(operation.body.get());
        if (!body || body->character.id.site == kMarkerSite)
            return ApplyStatus::Rejected;

        if (find(body->character.id))
            return ApplyStatus::Duplicate;

        Node* previous = find(body->character.previous_id);
        Node* next = find(body->character.next_id);
        if (!previous || !next) 
            return ApplyStatus::Buffered;
        if (sequence_.index_of(previous) >= sequence_.index_of(next))
            return ApplyStatus::Rejected;

        integrate(body->character, previous, next);
        return ApplyStatus::Applied;
    }

    auto* body = dynamic_cast<const WootDelete*>(operation.body.get());
    if (!body || body->id.site == kMarkerSite)
        return ApplyStatus::Rejected;

    Node* node = find(body->id);
    if (!node)
        return ApplyStatus::Buffered;
    if (!node->visible)
        return ApplyStatus::NoOp;

    sequence_.set_visible(node, false);
    return ApplyStatus::Applied;
}

ApplyStatus Woot::apply(const Operation& operation) {
    return apply_or_wait(waiting_, operation, [this](const Operation& next) { return try_apply(next); });
}

void Woot::encode_body(const Operation& operation, ByteWriter& writer) const {
    if (operation.kind == OperationKind::Insert)
        write_character(writer, static_cast<const WootInsert&>(*operation.body).character);
    else
        write_id(writer, static_cast<const WootDelete&>(*operation.body).id);
}

std::shared_ptr<const OperationBody> Woot::decode_body(OperationKind kind, ByteReader& reader) const {
    if (kind == OperationKind::Insert) {
        auto body = std::make_shared<WootInsert>();
        body->character = read_character(reader);

        return body;
    }

    auto body = std::make_shared<WootDelete>();
    body->id = read_id(reader);

    return body;
}

// snapshot = tag, character count, (character, visible) for each and then the waiting info
Bytes Woot::serialize() const {
    Bytes output;
    ByteWriter writer(output);
    writer.write_byte(kSnapshotTag);

    writer.write_varint(sequence_.size() - 2);
    for (Node* node = Sequence::next(begin_); node != end_; node = Sequence::next(node)) {
        write_character(writer, node->value);
        writer.write_byte(node->visible ? 1 : 0);
    }

    write_waiting(*this, waiting_, writer);

    return output;
}

void Woot::deserialize(const Bytes& data) {
    ByteReader reader(data);
    if (reader.read_byte() != kSnapshotTag) \
        throw DecodeError("not a WOOT snapshot");

    size_t character_count = reader.read_count(7);
    std::vector<std::pair<WootChar, bool>> items;
    items.reserve(character_count + 2);
    items.push_back({kBeginMarker, false});

    uint32_t clock = 0;
    for (size_t index = 0; index < character_count; ++index) {
        WootChar character = read_character(reader);
        bool visible = reader.read_byte() != 0;
        if (character.id.site == site_) 
            clock = std::max(clock, character.id.clock);

        items.push_back({character, visible});
    }

    items.push_back({kEndMarker, false});

    std::vector<Operation> waiting = read_waiting(*this, reader);

    load_characters(std::move(items));
    if (id_index_.size() != character_count + 2) 
        throw DecodeError("duplicate ids in WOOT snapshot");

    waiting_ = std::move(waiting);
    clock_ = clock;
}

// starting document looks like site 0 had typed it from left to right
void Woot::bulk_load(const std::vector<Value>& initial) {
    std::vector<std::pair<WootChar, bool>> items;
    items.reserve(initial.size() + 2);
    items.push_back({kBeginMarker, false});

    WootId previous = kBeginId;
    for (size_t index = 0; index < initial.size(); ++index) {
        WootId id{kInitialSite, static_cast<uint32_t>(index + 1)};
        items.push_back({WootChar{id, previous, kEndId, initial[index]}, true});
        previous = id;
    }
    items.push_back({kEndMarker, false});

    load_characters(std::move(items));
    waiting_.clear();
    clock_ = 0;
}

// replaces the whole document and rebuilds the id lookup
void Woot::load_characters(std::vector<std::pair<WootChar, bool>>&& items) {
    size_t count = items.size();
    sequence_.build(std::move(items));

    id_index_.clear();
    id_index_.reserve(count);
    for (Node* node = sequence_.first(); node; node = Sequence::next(node)) id_index_[index_key(node->value.id)] = node;

    begin_ = sequence_.first();
    end_ = sequence_.last();
}

size_t Woot::get_memory_usage() const {
    return sizeof(*this) + sequence_.size() * Sequence::node_size() + unordered_map_bytes(id_index_) +
           waiting_.size() * (sizeof(Operation) + sizeof(WootInsert));
}

StructureStats Woot::get_structure_statistics() const {
    size_t stored = sequence_.size() - 2; // without the two markers
    size_t visible = size();
    size_t tombstones = stored - visible;
    size_t bytes_per_node = Sequence::node_size() + sizeof(std::pair<const uint64_t, Node*>) + sizeof(void*);

    StructureStats stats;
    stats.set("logical_size", static_cast<double>(visible));
    stats.set("stored_elements", static_cast<double>(stored));
    stats.set("tombstones", static_cast<double>(tombstones));
    stats.set("placeholders", 0);
    stats.set("pending_ops", static_cast<double>(waiting_.size()));
    stats.set("avg_id_depth", 1);
    stats.set("max_id_depth", 1);
    stats.set("avg_id_bytes", static_cast<double>(sizeof(WootId)));
    stats.set("id_metadata_bytes", static_cast<double>(stored * (3 * sizeof(WootId))));
    stats.set("node_bytes", static_cast<double>(sequence_.size() * Sequence::node_size()));
    stats.set("index_bytes", static_cast<double>(unordered_map_bytes(id_index_)));
    stats.set("tombstone_bytes", static_cast<double>(tombstones * bytes_per_node));
    stats.set("tree_depth", static_cast<double>(sequence_.height()))
    ;
    return stats;
}

std::vector<std::string> Woot::check_invariants() const {
    std::vector<std::string> errors;

    std::string index_error;
    if (!sequence_.check(&index_error)) errors.push_back("index: " + index_error);
    if (sequence_.first() != begin_ || sequence_.last() != end_) errors.push_back("begin/end markers misplaced");
    if (id_index_.size() != sequence_.size()) errors.push_back("id index size mismatch (duplicate ids?)");

    // every character must sit strictly between the two neighbours it was typed between
    for (Node* node = Sequence::next(begin_); node && node != end_; node = Sequence::next(node)) {
        if (find(node->value.id) != node) {
            errors.push_back("element missing from id index");
            break;
        }

        Node* previous = find(node->value.previous_id);
        Node* next = find(node->value.next_id);

        if (!previous || !next) {
            errors.push_back("dangling previous/next reference");
            break;
        }

        size_t index = sequence_.index_of(node);

        if (!(sequence_.index_of(previous) < index && index < sequence_.index_of(next))) {
            errors.push_back("WOOT order invariant previous < c < next violated");
            break;
        }
    }
    
    return errors;
}

}  // namespace crdt
