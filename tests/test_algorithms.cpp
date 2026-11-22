#include <set>

#include "crdt/algorithms/logoot.hpp"
#include "crdt/algorithms/lseq.hpp"
#include "crdt/algorithms/treedoc.hpp"
#include "helpers.hpp"

using namespace crdt;
using namespace test_helpers;

TEST(interface_empty_document) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);
        auto crdt = make_crdt(algorithm, 1);

        CHECK_EQ(crdt->size(), 0u);
        CHECK(!crdt->lookup(0).has_value());
        CHECK(crdt->traverse().empty());
        CHECK_THROWS(crdt->erase(0), std::out_of_range);
        CHECK_THROWS(crdt->insert(1, 'x'), std::out_of_range);
        CHECK_THROWS(crdt->id_at(0), std::out_of_range);

        crdt->insert(0, 'x');
        CHECK_EQ(crdt->size(), 1u);
        CHECK_EQ(crdt->lookup(0).value(), static_cast<Value>('x'));
        CHECK_EQ(crdt->text(), std::string("x"));
        CHECK_INVARIANTS(*crdt);
    }
}

TEST(differential_against_reference_model) {
    // editing a replica has to work like editing a std::vector
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 1; seed <= 15; ++seed) {
            test_framework::stats().property_cases++;
            test_framework::context() = algorithm_and_seed(algorithm, seed);

            RandomGenerator random(seed);
            auto initial = make_initial_document(random.below(200), seed);
            auto crdt = make_crdt(algorithm, 1);
            crdt->bulk_load(initial);
            std::vector<Value> reference = initial;

            for (int step = 0; step < 2000; ++step) {
                uint64_t mode = random.below(4);

                if (reference.empty() || random.chance(0.6)) {
                    size_t position = mode == 0 ? 0 : mode == 1 ? reference.size() : random.below(reference.size() + 1);
                    Value value = static_cast<Value>('A' + random.below(26));
                    crdt->insert(position, value);
                    reference.insert(reference.begin() + static_cast<long>(position), value);
                    CHECK_EQ(crdt->lookup(position).value(), value); // check if element shows up at requested index

                } else {
                    size_t position = random.below(reference.size());
                    crdt->erase(position);
                    reference.erase(reference.begin() + static_cast<long>(position));
                }

                if (step % 500 == 0) 
                    CHECK(crdt->traverse() == reference);
            }

            CHECK(crdt->traverse() == reference);
            CHECK_EQ(crdt->size(), reference.size());

            for (size_t position = 0; position < reference.size(); position += 37) {
                CHECK_EQ(crdt->lookup(position).value(), reference[position]);
            }

            CHECK_INVARIANTS(*crdt);
        }
    }
}

TEST(identifiers_unique_and_lookup_by_id) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);
        auto crdt = make_crdt(algorithm, 3);
        crdt->bulk_load(make_initial_document(300, 5));

        RandomGenerator random(9);
        for (int count = 0; count < 500; ++count) {
            crdt->insert(random.below(crdt->size() + 1), static_cast<Value>('a' + count % 26));
        }

        std::set<ElementId> ids;
        for (size_t position = 0; position < crdt->size(); ++position) {
            ElementId id = crdt->id_at(position);
            CHECK(ids.insert(id).second);
            CHECK_EQ(crdt->lookup_id(id).value(), crdt->lookup(position).value());
        }

        // deleted element can't be found by its id
        ElementId victim = crdt->id_at(10);
        Value next_value = crdt->lookup(11).value();
        crdt->erase(10);
        CHECK(!crdt->lookup_id(victim).has_value());
        CHECK_EQ(crdt->lookup(10).value(), next_value);
        CHECK(!crdt->lookup_id(ElementId("\xff\xff")).has_value()); // garbage id
        CHECK_INVARIANTS(*crdt);
    }
}

TEST(state_serialization_round_trip) {
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 1; seed <= 5; ++seed) {
            test_framework::context() = algorithm_and_seed(algorithm, seed);

            auto original = make_crdt(algorithm, 1);
            original->bulk_load(make_initial_document(100, seed));
            RandomGenerator random(seed);

            for (int step = 0; step < 800; ++step) {
                if (original->size() && random.chance(0.4)) 
                    original->erase(random.below(original->size()));
                else 
                    original->insert(random.below(original->size() + 1), static_cast<Value>('a' + random.below(26)));
            }

            Bytes snapshot = original->serialize();
            auto restored = make_crdt(algorithm, 1);
            restored->deserialize(snapshot);
            CHECK(restored->traverse() == original->traverse());
            CHECK(restored->serialize() == snapshot);
            CHECK_INVARIANTS(*restored);

            // restored replicas keep working and stay equal
            for (int step = 0; step < 200; ++step) {
                size_t position = random.below(original->size() + 1);
                original->insert(position, 'z');
                restored->insert(position, 'z');
            }

            CHECK(restored->traverse() == original->traverse());

            // different replicas can restore their state and receive operations
            auto other = make_crdt(algorithm, 2);
            other->deserialize(original->serialize());
            Operation operation = original->insert(0, 'q');
            CHECK_EQ(other->apply(round_trip(*original, operation)), ApplyStatus::Applied);
            CHECK(other->traverse() == original->traverse());
        }
    }
}

TEST(snapshot_decoding_rejects_corrupt_input) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto crdt = make_crdt(algorithm, 1);
        crdt->bulk_load(make_initial_document(50, 1));
        crdt->insert(3, 'x');
        Bytes snapshot = crdt->serialize();

        // every truncation gets detected
        for (size_t cut = 0; cut < snapshot.size(); cut += std::max<size_t>(1, snapshot.size() / 25)) {
            Bytes truncated(snapshot.begin(), snapshot.begin() + static_cast<long>(cut));
            auto restored = make_crdt(algorithm, 1);
            CHECK_THROWS(restored->deserialize(truncated), DecodeError);
        }

        // random garbage must never crash
        RandomGenerator random(3);
        for (int attempt = 0; attempt < 200; ++attempt) {
            Bytes garbage(random.below(64));
            for (auto& byte : garbage) byte = static_cast<uint8_t>(random.below(256));

            auto restored = make_crdt(algorithm, 1);
            try {
                restored->deserialize(garbage); // accepted garbage must not crash later use
                auto sequence = restored->traverse();
                (void)restored->serialize();
                CHECK_EQ(sequence.size(), restored->size());
            } catch (const DecodeError&) {
            }
        }
    }
}

TEST(operation_wire_round_trip_and_invalid_ops) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto source = make_crdt(algorithm, 1);
        auto target = make_crdt(algorithm, 2);
        auto initial = make_initial_document(40, 2);
        source->bulk_load(initial);
        target->bulk_load(initial);

        RandomGenerator random(4);
        for (int step = 0; step < 300; ++step) {
            Operation operation = (source->size() && random.chance(0.3)) ? source->erase(random.below(source->size()))
                                                               : source->insert(random.below(source->size() + 1), 'k');
            operation.sequence_number = static_cast<uint64_t>(step + 1);
            CHECK_EQ(target->apply(round_trip(*source, operation)), ApplyStatus::Applied);
        }

        CHECK(target->traverse() == source->traverse());

        // garbage operation bytes must raise DecodeError, but never crash
        for (int attempt = 0; attempt < 300; ++attempt) {
            Bytes garbage(1 + random.below(40));
            for (auto& byte : garbage) byte = static_cast<uint8_t>(random.below(256));
            try {
                ByteReader reader(garbage);
                Operation operation = decode_operation(*target, reader);
                ApplyStatus status = target->apply(operation); // syntactically valid garbage - any status, no crash
                (void)status;
            } catch (const DecodeError&) {
            }
        }
        CHECK_INVARIANTS(*target);

        // an operation body of another algorithm is rejected
        Algorithm other = algorithm == Algorithm::RGA ? Algorithm::WOOT : Algorithm::RGA;
        auto foreign = make_crdt(other, 9);
        Operation foreign_operation = foreign->insert(0, 'f');
        CHECK_EQ(target->apply(foreign_operation), ApplyStatus::Rejected);

        Operation empty;
        CHECK_EQ(target->apply(empty), ApplyStatus::Rejected);
    }
}

TEST(bulk_load_is_deterministic_and_identical) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);
        for (size_t length : {0u, 1u, 2u, 17u, 1000u, 50000u}) {
            auto initial = make_initial_document(length, 7);
            auto first = make_crdt(algorithm, 1);
            auto second = make_crdt(algorithm, 2);
            first->bulk_load(initial);
            second->bulk_load(initial);

            CHECK(first->serialize() == second->serialize());
            CHECK(first->traverse() == initial);
            CHECK_INVARIANTS(*first);
        }
    }
}

TEST(duplicate_operations_at_crdt_level) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto source = make_crdt(algorithm, 1);
        auto target = make_crdt(algorithm, 2);
        auto initial = make_initial_document(20, 1);
        source->bulk_load(initial);
        target->bulk_load(initial);

        Operation insert_operation = source->insert(5, 'd');
        CHECK_EQ(target->apply(insert_operation), ApplyStatus::Applied);
        CHECK_EQ(target->apply(insert_operation), ApplyStatus::Duplicate);

        Operation delete_operation = source->erase(5);
        CHECK_EQ(target->apply(delete_operation), ApplyStatus::Applied);
        ApplyStatus again = target->apply(delete_operation);
        CHECK(again == ApplyStatus::NoOp || again == ApplyStatus::Duplicate);
        CHECK(target->traverse() == source->traverse());

        // insert that comes after its delete
        // algos with tombstones should notice it, but
        // Logoot, LSEQ and Treedoc have to rely on Replica class dropping dupes.
        ApplyStatus late = target->apply(insert_operation);
        if (algorithm == Algorithm::WOOT || algorithm == Algorithm::RGA) {
            CHECK_EQ(late, ApplyStatus::Duplicate);
            CHECK(target->traverse() == source->traverse());
        }
        CHECK_INVARIANTS(*target);
    }
}

TEST(causally_premature_operations_inside_crdt) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto source = make_crdt(algorithm, 1);
        auto target = make_crdt(algorithm, 2);
        source->bulk_load(from_text("abc"));
        target->bulk_load(from_text("abc"));

        Operation insert_x = source->insert(1, 'X'); // aXbc
        Operation insert_y = source->insert(2, 'Y'); // aXYbc (depends on X)
        Operation delete_x = source->erase(1); // aYbc (deletes X)

        if (algorithm == Algorithm::WOOT || algorithm == Algorithm::RGA) {
            // algorithms with tombstones buffer operations, which have references missing
            CHECK_EQ(target->apply(delete_x), ApplyStatus::Buffered);
            CHECK_EQ(target->apply(insert_y), ApplyStatus::Buffered);
            CHECK_EQ(target->get_structure_statistics().get("pending_ops"), 2.0);
            CHECK_EQ(target->apply(insert_x), ApplyStatus::Applied);
            CHECK_EQ(target->get_structure_statistics().get("pending_ops"), 0.0);
        } else {
            // position IDs - check if insertions commute without causal order
            CHECK(target->requires_causal_delivery());
            CHECK_EQ(target->apply(insert_y), ApplyStatus::Applied);
            CHECK_EQ(target->apply(insert_x), ApplyStatus::Applied);
            CHECK_EQ(target->apply(delete_x), ApplyStatus::Applied);
        }

        CHECK_EQ(target->text(), std::string("aYbc"));
        CHECK(target->traverse() == source->traverse());
        CHECK_INVARIANTS(*target);
    }
}

TEST(ordering_rule_rga_newer_timestamp_first) {
    auto replicas = make_replicas(Algorithm::RGA, 2, from_text("ab"));
    Operation insert_x = replicas[0]->insert(1, 'X'); // (3,1) after 'a'
    Operation insert_y = replicas[1]->insert(1, 'Y'); // (3,2) after 'a'
    replicas[0]->receive(insert_y);
    replicas[1]->receive(insert_x);
    CHECK_CONVERGED(replicas);

    // Lamport counters are equal - higher site id should win and arrive first
    CHECK_EQ(replicas[0]->crdt().text(), std::string("aYXb"));

    Operation insert_z = replicas[0]->insert(1, 'Z'); // later timestamp (4,1) -> before older siblings
    replicas[1]->receive(insert_z);
    CHECK_EQ(replicas[1]->crdt().text(), std::string("aZYXb"));
}

TEST(ordering_rule_woot_and_treedoc_site_order) {
    for (Algorithm algorithm : {Algorithm::WOOT, Algorithm::Treedoc}) {
        test_framework::context() = to_string(algorithm);

        auto replicas = make_replicas(algorithm, 2, from_text("ab"));
        Operation insert_x = replicas[0]->insert(1, 'X');
        Operation insert_y = replicas[1]->insert(1, 'Y');
        replicas[0]->receive(insert_y);
        replicas[1]->receive(insert_x);
        CHECK_CONVERGED(replicas);

        CHECK_EQ(replicas[0]->crdt().text(), std::string("aXYb"));
    }
}

TEST(treedoc_paths_and_mini_node_children_like_paper) {
    auto replicas = make_replicas(Algorithm::Treedoc, 3, from_text("cd"));
    Operation insert_w = replicas[0]->insert(1, 'W');
    Operation insert_y = replicas[1]->insert(1, 'Y');
    for (auto& replica : replicas) {
        replica->receive(insert_w);
        replica->receive(insert_y);
    }
    CHECK_CONVERGED(replicas);
    CHECK_EQ(replicas[2]->crdt().text(), std::string("cWYd"));

    Operation insert_x = replicas[2]->insert(2, 'X'); // between the mini-siblings W and Y
    for (auto& replica : replicas) replica->receive(insert_x);
    CHECK_CONVERGED(replicas);
    CHECK_EQ(replicas[0]->crdt().text(), std::string("cWXYd"));

    const auto& path = static_cast<const TreedocInsert&>(*insert_x.body).path;
    CHECK(path.size() >= 2);
    CHECK(path.back().has_disambiguator);
    CHECK(path[path.size() - 2].has_disambiguator); // continues under mini-node W explicitly
    CHECK(path[path.size() - 2].disambiguator == static_cast<const TreedocInsert&>(*insert_w.body).path.back().disambiguator);
    for (size_t step = 0; step + 2 < path.size(); ++step) CHECK(!path[step].has_disambiguator); // exploded part, bits only

    // snapshot round trip keeps the mini-node child, and edits after it still converge
    auto restored = make_crdt(Algorithm::Treedoc, 9);
    restored->deserialize(replicas[0]->crdt().serialize());
    CHECK_EQ(restored->text(), std::string("cWXYd"));
    CHECK_INVARIANTS(*restored);

    // W has a child of its own (X), so deleting W keeps it as a tombstone, deleting X then
    // lets garbage collection remove both
    Operation erase_w = replicas[0]->erase(1);
    for (auto& replica : replicas) replica->receive(erase_w);
    CHECK_EQ(replicas[1]->crdt().get_structure_statistics().get("tombstones"), 1.0);
    Operation erase_x = replicas[1]->erase(1);
    for (auto& replica : replicas) replica->receive(erase_x);
    CHECK_CONVERGED(replicas);
    CHECK_EQ(replicas[2]->crdt().text(), std::string("cYd"));
    CHECK_EQ(replicas[2]->crdt().get_structure_statistics().get("tombstones"), 0.0);
    for (auto& replica : replicas) CHECK_INVARIANTS(replica->crdt());
}

// appends and prepends grow the tree by several levels and fill the empty positions, on every
// replica, concurrently, and garbage collection removes the empty nodes again
TEST(treedoc_tree_growing_heuristic) {
    auto replicas = make_replicas(Algorithm::Treedoc, 2, from_text("ab"));
    std::vector<Operation> operations;
    for (int count = 0; count < 300; ++count) {
        operations.push_back(replicas[0]->insert(replicas[0]->crdt().size(), static_cast<Value>('x')));
        operations.push_back(replicas[1]->insert(0, static_cast<Value>('y')));
    }
    for (auto& replica : replicas) {
        for (auto& operation : operations) {
            replica->receive(operation);
        }
    }
    CHECK_CONVERGED(replicas);
    CHECK_EQ(replicas[0]->crdt().size(), 602u);
    CHECK(replicas[0]->crdt().get_structure_statistics().get("max_id_depth") < 60);
    CHECK(replicas[0]->crdt().get_structure_statistics().get("placeholders") > 0);

    std::vector<Operation> deletions;
    while (replicas[1]->crdt().size() > 0) {
        deletions.push_back(replicas[1]->erase(0));
    }
    for (auto& operation : deletions) {
        replicas[0]->receive(operation);
    }
    CHECK_CONVERGED(replicas);
    CHECK_EQ(replicas[0]->crdt().get_structure_statistics().get("stored_elements"), 0.0);
    for (auto& replica : replicas) {
        CHECK_INVARIANTS(replica->crdt());
    }
}

TEST(ordering_rule_logoot_lseq_generate_between) {
    for (Algorithm algorithm : {Algorithm::Logoot, Algorithm::LSEQ}) {
        test_framework::context() = to_string(algorithm);

        auto crdt = make_crdt(algorithm, 4);
        RandomGenerator random(11);
        for (int count = 0; count < 3000; ++count) {
            size_t position = random.below(4) == 0 ? 0 : random.below(crdt->size() + 1);
            crdt->insert(position, 'i');
        }

        CHECK_INVARIANTS(*crdt); // strictly increasing identifiers, valid digits

        auto replicas = make_replicas(algorithm, 3, from_text("xy"));
        std::vector<Operation> operations;
        for (size_t writer = 0; writer < 3; ++writer) {
            for (int count = 0; count < 20; ++count) {
                operations.push_back(replicas[writer]->insert(1, static_cast<Value>('a' + writer)));
            }
        }

        for (auto& replica : replicas) {
            for (auto& operation : operations) 
                replica->receive(operation);
        }

        CHECK_CONVERGED(replicas);

        std::string text = replicas[0]->crdt().text();
        CHECK_EQ(text.front(), 'x');
        CHECK_EQ(text.back(), 'y');
        CHECK_EQ(text.size(), 62u);
    }
}

TEST(tombstone_handling_per_algorithm) {
    for (bool gc : {true, false}) {
        for (Algorithm algorithm : all_algorithms()) {
            test_framework::context() = to_string(algorithm) + (gc ? " gc" : " nogc");

            CRDTOptions options;
            options.treedoc_collect_garbage = gc;
            auto crdt = make_crdt(algorithm, 1, options);
            crdt->bulk_load(make_initial_document(1000, 3));

            RandomGenerator random(5);
            size_t deletions = 0;
            for (int count = 0; count < 600; ++count) {
                crdt->erase(random.below(crdt->size()));
                ++deletions;
            }

            auto stats = crdt->get_structure_statistics();
            double tombstones = stats.get("tombstones");
            CHECK_EQ(static_cast<size_t>(stats.get("logical_size")), 400u);

            if (algorithm == Algorithm::WOOT || algorithm == Algorithm::RGA) {
                CHECK_EQ(static_cast<size_t>(tombstones), deletions);
            } else if (algorithm == Algorithm::Treedoc && !gc) {
                CHECK_EQ(static_cast<size_t>(tombstones), deletions);
            } else if (algorithm == Algorithm::Treedoc) {
                CHECK(tombstones < static_cast<double>(deletions));  // leaves collected
            } else {
                CHECK_EQ(tombstones, 0.0);
            }
            CHECK_INVARIANTS(*crdt);

            // deletes everything
            while (crdt->size()) crdt->erase(0);
            CHECK(crdt->traverse().empty());
            if (algorithm == Algorithm::Treedoc && gc) {
                CHECK_EQ(crdt->get_structure_statistics().get("stored_elements"), 0.0);
            }

            CHECK_INVARIANTS(*crdt);
        }
    }
}

TEST(identifier_growth_characterization) {
    // when typing at the start Logoot IDs grow deep, BUT the per-depth 
    // strategy of LSEQ keeps them short
    // Treedoc grows the tree by several levels at once when appending or prepending, so its
    // paths stay far shorter than the number of appended atoms
    auto depth_after = [](Algorithm algorithm, Pattern pattern, size_t operation_count) {
        auto crdt = make_crdt(algorithm, 1);
        WorkloadConfig workload;
        workload.pattern = pattern;
        WorkloadGenerator generator(workload, 1);
        for (size_t count = 0; count < operation_count; ++count) {
            Edit edit = generator.next(crdt->size());
            crdt->insert(edit.position, edit.value);
        }

        return crdt->get_structure_statistics().get("max_id_depth");
    };

    double logoot_front = depth_after(Algorithm::Logoot, Pattern::FrontInsert, 2000);
    double lseq_front = depth_after(Algorithm::LSEQ, Pattern::FrontInsert, 2000);
    double treedoc_end = depth_after(Algorithm::Treedoc, Pattern::EndInsert, 2000);
    double treedoc_front = depth_after(Algorithm::Treedoc, Pattern::FrontInsert, 2000);
    double treedoc_random = depth_after(Algorithm::Treedoc, Pattern::RandomInsert, 2000);

    test_framework::context() = "logoot_front=" + std::to_string(logoot_front) + " lseq_front=" + std::to_string(lseq_front) +
                    " treedoc_end=" + std::to_string(treedoc_end) + " treedoc_front=" + std::to_string(treedoc_front) +
                    " treedoc_random=" + std::to_string(treedoc_random);

    CHECK(logoot_front > 20);
    CHECK(lseq_front < logoot_front);
    CHECK(treedoc_end < 200);
    CHECK(treedoc_front < 200);
    CHECK(treedoc_random < 100);
}

TEST(memory_and_statistics_reported) {
    const char* keys[] = {"logical_size", "stored_elements", "tombstones", "placeholders", "pending_ops",
                          "avg_id_depth", "max_id_depth", "avg_id_bytes", "node_bytes", "id_metadata_bytes",
                          "tombstone_bytes", "index_bytes", "tree_depth"};

    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto small = make_crdt(algorithm, 1);
        auto big = make_crdt(algorithm, 1);
        small->bulk_load(make_initial_document(100, 1));
        big->bulk_load(make_initial_document(10000, 1));
        CHECK(big->get_memory_usage() > small->get_memory_usage());

        auto stats = big->get_structure_statistics();
        for (const char* key : keys) 
            CHECK_MSG(stats.values.count(key) == 1, key);
            
        CHECK_EQ(stats.get("logical_size"), 10000.0);
    }
}
