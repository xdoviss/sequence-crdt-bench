// types for all the algorithms
#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace crdt {

// "site" in the CRDT papers
// site 0 is reserved for the initial document content that all replicas start with
using ReplicaId = uint32_t;
constexpr ReplicaId kInitialSite = 0;

// a single character in the doc
using Value = uint32_t;

using Bytes = std::vector<uint8_t>;

// unique id of the element squished into a string of bytes
// so each algorithm can use their own ID types
using ElementId = std::string;

enum class Algorithm { 
    WOOT, 
    Logoot, 
    RGA, 
    LSEQ, 
    Treedoc
};

std::string to_string(Algorithm algorithm);
Algorithm algorithm_from_string(const std::string& name);
std::vector<Algorithm> all_algorithms();

enum class OperationKind : uint8_t { 
    Insert = 1, 
    Delete = 2 
};

// state after the CRDT received an op from other replica
enum class ApplyStatus {
    Applied, // applied
    Duplicate, // did nothing
    Buffered, // in the waiting list
    NoOp, // similar to duplicate, but valid (element that already got deleted was deleted)
    Rejected // malformed op
};

std::string to_string(ApplyStatus status);


// what CRDTs keep in memory
struct StructureStats {
    std::map<std::string, double> values;

    void set(const std::string& key, double value) { 
        values[key] = value;
    }

    double get(const std::string& key) const {
        auto found = values.find(key);
        return found == values.end() ? 0.0 : found->second;
    }
};

// converts between a document and plain text (for tests)
std::string to_text(const std::vector<Value>& document);
std::vector<Value> from_text(const std::string& text);

}  // namespace crdt
