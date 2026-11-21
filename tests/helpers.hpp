#pragma once

#include <algorithm>
#include <memory>
#include <vector>

#include "crdt/common/random_generator.hpp"
#include "crdt/simulations/replica.hpp"
#include "crdt/simulations/workload.hpp"
#include "test_framework.hpp"

namespace test_helpers {

using namespace crdt;

// `count` replicas that all start from the same snapshot of `initial`
inline std::vector<std::unique_ptr<Replica>> make_replicas(Algorithm algorithm, size_t count,
                                                           const std::vector<Value>& initial,
                                                           DeliveryMode mode = DeliveryMode::Causal,
                                                           const CRDTOptions& options = {}) {
    auto template_crdt = make_crdt(algorithm, kInitialSite, options);
    template_crdt->bulk_load(initial);
    Bytes snapshot = template_crdt->serialize();

    std::vector<std::unique_ptr<Replica>> replicas;

    for (size_t index = 0; index < count; ++index) {
        ReplicaId id = static_cast<ReplicaId>(index + 1);

        auto crdt = make_crdt(algorithm, id, options);

        crdt->deserialize(snapshot);
        replicas.push_back(std::make_unique<Replica>(id, std::move(crdt), mode));
    }

    return replicas;
}

// makes a random local edit. insertions use `next_value` so they can be increased and values can be made unique
inline Operation random_edit(Replica& replica, RandomGenerator& random, Value& next_value,
                             double delete_probability = 0.35) {
    size_t size = replica.crdt().size();
    if (size > 0 && random.chance(delete_probability))
        return replica.erase(random.below(size));

    return replica.insert(random.below(size + 1), next_value++);
}

// acts like a round trip from a network, send through wire format and back
inline Operation round_trip(const SequenceCRDT& codec, const Operation& operation) {
    Bytes bytes;
    ByteWriter writer(bytes);
    encode_operation(codec, operation, writer);

    ByteReader reader(bytes);
    
    return decode_operation(codec, reader);
}

inline void shuffle(std::vector<Operation>& operations, RandomGenerator& random) {
    for (size_t remaining = operations.size(); remaining > 1; --remaining) {
        std::swap(operations[remaining - 1], operations[random.below(remaining)]);
    }
}

inline std::string algorithm_and_seed(Algorithm algorithm, uint64_t seed) {
    return to_string(algorithm) + " seed=" + std::to_string(seed);
}

}  // namespace test_helpers
