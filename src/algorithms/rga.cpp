#include "crdt/algorithms/rga.hpp"

#include <algorithm>
#include <stdexcept>

#include "crdt/algorithms/waiting_operations.hpp"

namespace crdt {

namespace {

constexpr uint8_t kSnapshotTag = 'R';
constexpr RgaId kHeadId{0, 0};

// hash map key packs (counter, site) into 64 bits
// 20 bits for the site (~million replicas) and 44 bits for the counter
constexpr uint64_t kSiteBits = 20;

uint64_t index_key(RgaId id) { return (id.counter << kSiteBits) | id.site; }

bool fits_in_key(RgaId id) { return id.site < (1u << kSiteBits) && id.counter < (1ull << (64 - kSiteBits)); }

void write_id(ByteWriter& writer, RgaId id) {
    writer.write_varint(id.counter);
    writer.write_varint(id.site);
}

RgaId read_id(ByteReader& reader) {
    RgaId id;
    id.counter = reader.read_varint();
    id.site = reader.read_u32();
    return id;
}

}  // namespace

Rga::Rga(ReplicaId site) : site_(site), sequence_(0xBADC0DEull ^ site) {
    if (site >= (1u << kSiteBits)) throw std::invalid_argument("RGA site id too large");

    head_ = sequence_.insert_after(nullptr, RgaElement{kHeadId, 0}, false);
    id_index_[index_key(kHeadId)] = head_;
}

Rga::Node* Rga::find(RgaId id) const {
    if (!fits_in_key(id))
        return nullptr;

    auto found = id_index_.find(index_key(id));
    return found == id_index_.end() ? nullptr : found->second;
}

ApplyStatus Rga::try_apply(const Operation& operation) {
    if (operation.kind == OperationKind::Insert) {
        auto* body = dynamic_cast<const RgaInsert*>(operation.body.get());

        // element is always newer than the one it was inserted after
        if (!body || body->id.counter == 0 || !fits_in_key(body->id) || !(body->reference < body->id)) {
            return ApplyStatus::Rejected;
        }

        if (find(body->id)) 
            return ApplyStatus::Duplicate;

        Node* anchor = find(body->reference);
        if (!anchor) 
            return ApplyStatus::Buffered;

        // skip elements that are newer, as they were inserted after the same reference and should come first
        for (Node* following = Sequence::next(anchor); following && body->id < following->value.id;
             following = Sequence::next(following)) {
            anchor = following;
        }

        Node* inserted = sequence_.insert_after(anchor, RgaElement{body->id, body->value}, true);
        id_index_[index_key(body->id)] = inserted;
        clock_ = std::max(clock_, body->id.counter);

        return ApplyStatus::Applied;
    }

    auto* body = dynamic_cast<const RgaDelete*>(operation.body.get());
    if (!body || body->id.counter == 0) return ApplyStatus::Rejected;

    Node* node = find(body->id);

    if (!node) 
        return ApplyStatus::Buffered;

    if (!node->visible) 
        return ApplyStatus::NoOp;

    sequence_.set_visible(node, false);

    return ApplyStatus::Applied;
}

ApplyStatus Rga::apply(const Operation& operation) {
    return apply_or_wait(waiting_, operation, [this](const Operation& next) { return try_apply(next); });
}

Operation Rga::insert(size_t position, Value value) {
    if (position > size()) throw std::out_of_range("RGA insert position out of range");

    Node* reference = position == 0 ? head_ : sequence_.at_visible(position - 1);
    auto body = std::make_shared<RgaInsert>();
    body->id = RgaId{++clock_, site_};
    body->reference = reference->value.id;
    body->value = value;

    // new id is newer than anything we have seen, so nothing needs to be skipped
    Node* inserted = sequence_.insert_after(reference, RgaElement{body->id, value}, true);
    id_index_[index_key(body->id)] = inserted;

    return make_operation(OperationKind::Insert, site_, std::move(body));
}

// delete locally, element becomes a tombstone
Operation Rga::hide(Node* node) {
    sequence_.set_visible(node, false);

    auto body = std::make_shared<RgaDelete>();
    body->id = node->value.id;
    return make_operation(OperationKind::Delete, site_, std::move(body));
}

Operation Rga::erase(size_t position) {
    Node* node = sequence_.at_visible(position);
    if (!node)
        throw std::out_of_range("RGA erase position out of range");

    return hide(node);
}

std::optional<Value> Rga::lookup(size_t position) const {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        return std::nullopt;

    return node->value.value;
}

std::optional<Value> Rga::lookup_id(const ElementId& id) const {
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

ElementId Rga::id_at(size_t position) const {
    Node* node = sequence_.at_visible(position);
    if (!node) 
        throw std::out_of_range("RGA id_at out of range");

    Bytes bytes;
    ByteWriter writer(bytes);
    write_id(writer, node->value.id);
    return bytes_to_id(bytes);
}

std::vector<Value> Rga::traverse() const {
    std::vector<Value> document;
    document.reserve(size());
    sequence_.for_each([&](const Node& node) {
        if (node.visible) 
            document.push_back(node.value.value);
    });

    return document;
}

void Rga::encode_body(const Operation& operation, ByteWriter& writer) const {
    if (operation.kind == OperationKind::Insert) {
        const auto& body = static_cast<const RgaInsert&>(*operation.body);
        write_id(writer, body.id);
        write_id(writer, body.reference);
        writer.write_varint(body.value);
    } else {
        write_id(writer, static_cast<const RgaDelete&>(*operation.body).id);
    }
}

std::shared_ptr<const OperationBody> Rga::decode_body(OperationKind kind, ByteReader& reader) const {
    if (kind == OperationKind::Insert) {
        auto body = std::make_shared<RgaInsert>();
        body->id = read_id(reader);
        body->reference = read_id(reader);
        body->value = reader.read_u32();

        return body;
    }

    auto body = std::make_shared<RgaDelete>();
    body->id = read_id(reader);

    return body;
}

// list order already contains each decision from integration, therefore
// snapshots only need (id, value, visible) per element in order
Bytes Rga::serialize() const {
    Bytes output;
    ByteWriter writer(output);
    writer.write_byte(kSnapshotTag);
    writer.write_varint(clock_);

    writer.write_varint(sequence_.size() - 1);
    for (Node* node = Sequence::next(head_); node; node = Sequence::next(node)) {
        write_id(writer, node->value.id);
        writer.write_varint(node->value.value);
        writer.write_byte(node->visible ? 1 : 0);
    }

    write_waiting(*this, waiting_, writer);
    return output;
}

void Rga::deserialize(const Bytes& data) {
    ByteReader reader(data);
    if (reader.read_byte() != kSnapshotTag)
        throw DecodeError("not an RGA snapshot");

    uint64_t clock = reader.read_varint();

    size_t element_count = reader.read_count(4);
    std::vector<std::pair<RgaElement, bool>> items;
    items.reserve(element_count + 1);
    items.push_back({RgaElement{kHeadId, 0}, false});
    for (size_t index = 0; index < element_count; ++index) {
        RgaElement element;
        element.id = read_id(reader);
        element.value = reader.read_u32();
        bool visible = reader.read_byte() != 0;
        if (!fits_in_key(element.id) || element.id.counter == 0) 
            throw DecodeError("bad RGA id");

        clock = std::max(clock, element.id.counter);
        items.push_back({element, visible});
    }

    std::vector<Operation> waiting = read_waiting(*this, reader);

    load_elements(std::move(items));
    if (id_index_.size() != element_count + 1) 
        throw DecodeError("duplicate ids in RGA snapshot");

    waiting_ = std::move(waiting);
    clock_ = clock;
}

// the starting document looks like site 0 had typed it from left to right
void Rga::bulk_load(const std::vector<Value>& initial) {
    std::vector<std::pair<RgaElement, bool>> items;
    items.reserve(initial.size() + 1);
    items.push_back({RgaElement{kHeadId, 0}, false});
    for (size_t index = 0; index < initial.size(); ++index) {
        items.push_back({RgaElement{RgaId{index + 1, kInitialSite}, initial[index]}, true});
    }

    load_elements(std::move(items));
    waiting_.clear();
    clock_ = initial.size();
}

// replaces the whole list and rebuilds the id lookup
void Rga::load_elements(std::vector<std::pair<RgaElement, bool>>&& items) {
    size_t count = items.size();
    sequence_.build(std::move(items));

    id_index_.clear();
    id_index_.reserve(count);
    for (Node* node = sequence_.first(); node; node = Sequence::next(node)) id_index_[index_key(node->value.id)] = node;

    head_ = sequence_.first();
}

size_t Rga::get_memory_usage() const {
    return sizeof(*this) + sequence_.size() * Sequence::node_size() + unordered_map_bytes(id_index_) +
           waiting_.size() * (sizeof(Operation) + sizeof(RgaInsert));
}

StructureStats Rga::get_structure_statistics() const {
    size_t stored = sequence_.size() - 1; // without the head
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
    stats.set("avg_id_bytes", static_cast<double>(sizeof(RgaId)));
    stats.set("id_metadata_bytes", static_cast<double>(stored * sizeof(RgaId)));
    stats.set("node_bytes", static_cast<double>(sequence_.size() * Sequence::node_size()));
    stats.set("index_bytes", static_cast<double>(unordered_map_bytes(id_index_)));
    stats.set("tombstone_bytes", static_cast<double>(tombstones * bytes_per_node));
    stats.set("tree_depth", static_cast<double>(sequence_.height()));
    return stats;
}

std::vector<std::string> Rga::check_invariants() const {
    std::vector<std::string> errors;

    std::string index_error;
    if (!sequence_.check(&index_error)) 
        errors.push_back("index: " + index_error);

    if (sequence_.first() != head_) 
        errors.push_back("head is not the first element");

    if (id_index_.size() != sequence_.size()) 
        errors.push_back("id index size mismatch (duplicate ids?)");

    for (Node* node = Sequence::next(head_); node; node = Sequence::next(node)) {
        if (find(node->value.id) != node) {
            errors.push_back("element missing from id index");
            break;
        }

        if (node->value.id.counter == 0 || node->value.id.counter > clock_) {
            errors.push_back("timestamp outside Lamport clock range");
            break;
        }
    }
    
    return errors;
}

}  // namespace crdt
