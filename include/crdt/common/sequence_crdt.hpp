// common interface for the algorithms
#pragma once

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "crdt/common/operation.hpp"
#include "crdt/common/types.hpp"

namespace crdt {

class SequenceCRDT {
public:
    virtual ~SequenceCRDT() = default;

    virtual Algorithm algorithm() const = 0;
    std::string name() const { 
        return to_string(algorithm());
    }

    // LOCAL EDITS
    // change owned replica, return operation to send to other replicas

    // inserts `value` so that it ends up at visible index `position` (0 <= position <= size())
    virtual Operation insert(size_t position, Value value) = 0;
    // deletes the visible element at index `position` (0 <= position < size())
    virtual Operation erase(size_t position) = 0;

    // READING THE DOC

    virtual std::optional<Value> lookup(size_t position) const = 0;
    virtual std::optional<Value> lookup_id(const ElementId& id) const = 0;
    // identifier of the visible element at `position`
    virtual ElementId id_at(size_t position) const = 0;
    virtual std::vector<Value> traverse() const = 0;
    virtual size_t size() const = 0; // how many visible elements are there?

    // REMOTE EDITS (from other replicas)

    virtual ApplyStatus apply(const Operation& operation) = 0;
    virtual void encode_body(const Operation& operation, ByteWriter& writer) const = 0;
    virtual std::shared_ptr<const OperationBody> decode_body(OperationKind kind, ByteReader& reader) const = 0;
    // true if operations must arrive in cause before effect order (Logoot, LSEQ, Treedoc)
    // false for WOOT and RGA, because they keep tombstones and have a waiting list of their own
    virtual bool requires_causal_delivery() const = 0;

    // SAVING AND LOADING THE STATE
    
    virtual Bytes serialize() const = 0;
    virtual void deserialize(const Bytes& data) = 0;

    // builds starter document the same on each replica
    // identifiers belong to kInitialSite.
    // each algorithm uses its own layout.
    virtual void bulk_load(const std::vector<Value>& initial) = 0;

    
    // MEASURING

    // estimate of the bytes data structures hold (node sizes, identifiers, indexes),
    // without the memory allocator's own overhead
    virtual size_t get_memory_usage() const = 0;
    virtual StructureStats get_structure_statistics() const = 0;
    // holds all broken rules
    virtual std::vector<std::string> check_invariants() const = 0;

    std::string text() const { 
        return to_text(traverse());
    }
};

// settings for some algorithms
struct CRDTOptions {
    uint64_t seed = 2026; // seed for Logoot/LSEQ random digit choice
    uint64_t logoot_boundary = 1ull << 16; // how far from the left neighbour Logoot can place a digit
    unsigned lseq_base_bits = 4; // at each depth LSEQ has 2^(bits + depth) digits
    uint64_t lseq_boundary = 10; // how far from a neighbour LSEQ can place a digit
    bool treedoc_collect_garbage = true; // should deleted Treedoc leaves be thrown out?
};

std::unique_ptr<SequenceCRDT> make_crdt(Algorithm algorithm, ReplicaId site, const CRDTOptions& options = {});

}  // namespace crdt
