#include <algorithm>
#include <cctype>
#include <stdexcept>

#include "crdt/common/operation.hpp"
#include "crdt/common/sequence_crdt.hpp"
#include "crdt/common/types.hpp"

namespace crdt {

std::string to_string(Algorithm algorithm) {
    switch (algorithm) {
        case Algorithm::WOOT: return "WOOT";
        case Algorithm::Logoot: return "Logoot";
        case Algorithm::RGA: return "RGA";
        case Algorithm::LSEQ: return "LSEQ";
        case Algorithm::Treedoc: return "Treedoc";
    }
    return "?";
}

Algorithm algorithm_from_string(const std::string& name) {
    for (Algorithm algorithm : all_algorithms()) {
        std::string candidate = to_string(algorithm);
        bool same = candidate.size() == name.size() &&
                    std::equal(candidate.begin(), candidate.end(), name.begin(), [](char left, char right) {
                        return std::tolower(static_cast<unsigned char>(left)) ==
                               std::tolower(static_cast<unsigned char>(right));
                    });
                    
        if (same) 
            return algorithm;
    }
    throw std::invalid_argument("unknown algorithm: " + name);
}

std::vector<Algorithm> all_algorithms() {
    return {Algorithm::WOOT, Algorithm::Logoot, Algorithm::RGA, Algorithm::LSEQ, Algorithm::Treedoc};
}

std::string to_string(ApplyStatus status) {
    switch (status) {
        case ApplyStatus::Applied: return "Applied";
        case ApplyStatus::Duplicate: return "Duplicate";
        case ApplyStatus::Buffered: return "Buffered";
        case ApplyStatus::NoOp: return "NoOp";
        case ApplyStatus::Rejected: return "Rejected";
    }
    return "?";
}

std::string to_text(const std::vector<Value>& document) {
    std::string text;
    text.reserve(document.size());
    for (Value value : document) text.push_back(value < 128 ? static_cast<char>(value) : '?');
    return text;
}

std::vector<Value> from_text(const std::string& text) {
    std::vector<Value> document;
    for (unsigned char character : text) document.push_back(character);
    return document;
}

// envelope layout: kind (1 byte), origin, sequence number, number of dependencies,
// then (replica, count) for every dependency, all as varints. the body follows.
// origin + sequence number together identify an operation uniquely.
void encode_operation(const SequenceCRDT& crdt, const Operation& operation, ByteWriter& writer, WireStats* stats) {
    size_t start = writer.size();
    writer.write_byte(static_cast<uint8_t>(operation.kind));
    writer.write_varint(operation.origin);
    writer.write_varint(operation.sequence_number);
    writer.write_varint(operation.dependencies.size());
    for (const auto& dependency : operation.dependencies) {
        writer.write_varint(dependency.replica);
        writer.write_varint(dependency.count);
    }

    size_t body_start = writer.size();
    crdt.encode_body(operation, writer);

    if (stats) {
        stats->envelope_bytes += body_start - start;
        stats->body_bytes += writer.size() - body_start;
    }
}

Operation decode_operation(const SequenceCRDT& crdt, ByteReader& reader) {
    uint8_t kind = reader.read_byte();
    if (kind != static_cast<uint8_t>(OperationKind::Insert) && kind != static_cast<uint8_t>(OperationKind::Delete)) {
        throw DecodeError("bad operation kind");
    }

    Operation operation;
    operation.kind = static_cast<OperationKind>(kind);
    operation.origin = reader.read_u32();
    operation.sequence_number = reader.read_varint();

    operation.dependencies.resize(reader.read_count(2));
    for (auto& dependency : operation.dependencies) {
        dependency.replica = reader.read_u32();
        dependency.count = reader.read_varint();
    }

    operation.body = crdt.decode_body(operation.kind, reader);
    return operation;
}

Bytes encode_batch(const SequenceCRDT& crdt, const std::vector<Operation>& operations, WireStats* stats) {
    Bytes output;
    ByteWriter writer(output);
    writer.write_varint(operations.size());
    if (stats) stats->envelope_bytes += output.size();

    for (const auto& operation : operations) encode_operation(crdt, operation, writer, stats);
    return output;
}

std::vector<Operation> decode_batch(const SequenceCRDT& crdt, const Bytes& data) {
    ByteReader reader(data);
    size_t operation_count = reader.read_count(3);

    std::vector<Operation> operations;
    operations.reserve(operation_count);
    for (size_t index = 0; index < operation_count; ++index) operations.push_back(decode_operation(crdt, reader));

    if (!reader.done()) throw DecodeError("trailing bytes after batch");
    return operations;
}

}  // namespace crdt
