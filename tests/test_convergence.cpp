// tests that replicas end up with the same document (convergence), that the order of
// delivery does not matter, that concurrent edits work and that no edits are lost.
// most tests are just repeated with random seeds.
#include <map>
#include <set>

#include "helpers.hpp"

using namespace crdt;
using namespace test_helpers;

namespace {

// random concurrent edits with random (shuffled, sometimes duplicated) delivery.
// every inserted value is unique and remember which values were deleted, so at the
// end the document must hold exactly this: initial + inserted - deleted.
void random_session(Algorithm algorithm, uint64_t seed, size_t replica_count, size_t steps, DeliveryMode mode,
                    double duplicate_prob) {
    test_framework::stats().property_cases++;
    test_framework::context() = algorithm_and_seed(algorithm, seed) + " R=" + std::to_string(replica_count) +
                    (mode == DeliveryMode::Direct ? " direct" : "");

    RandomGenerator random(seed * 0x9E37 + replica_count);
    std::vector<Value> initial(random.below(30));
    for (size_t index = 0; index < initial.size(); ++index) initial[index] = static_cast<Value>(index + 1);  // unique

    auto replicas = make_replicas(algorithm, replica_count, initial, mode);
    std::vector<std::vector<Operation>> inbox(replica_count);
    std::set<Value> present(initial.begin(), initial.end());
    std::set<Value> deleted;
    Value next_value = 1000;

    for (size_t step = 0; step < steps; ++step) {
        size_t actor = random.below(replica_count);

        if (random.chance(0.55)) {
            // local edit, broadcast into every other inbox
            Replica& replica = *replicas[actor];
            size_t size = replica.crdt().size();
            Operation operation;
            if (size > 0 && random.chance(0.35)) {
                size_t position = random.below(size);
                deleted.insert(replica.crdt().lookup(position).value()); // concurrent deletes may hit the same value
                operation = replica.erase(position);
            } else {
                present.insert(next_value);
                operation = replica.insert(random.below(size + 1), next_value++);
            }

            for (size_t receiver = 0; receiver < replica_count; ++receiver) {
                if (receiver == actor) continue;
                inbox[receiver].push_back(round_trip(replica.crdt(), operation));
                if (random.chance(duplicate_prob)) inbox[receiver].push_back(operation);
            }
        } else {
            // deliver a few random pending messages
            auto& box = inbox[actor];
            size_t deliveries = std::min<size_t>(box.size(), 1 + random.below(4));
            for (size_t count = 0; count < deliveries; ++count) {
                size_t index = random.below(box.size());
                Operation operation = box[index];
                box.erase(box.begin() + static_cast<long>(index));
                replicas[actor]->receive(operation);
            }
        }
    }

    // deliver everything that is left, in random order
    for (size_t receiver = 0; receiver < replica_count; ++receiver) {
        shuffle(inbox[receiver], random);
        for (auto& operation : inbox[receiver]) 
            replicas[receiver]->receive(operation);
        inbox[receiver].clear();
    }

    CHECK_CONVERGED(replicas);
    for (auto& replica : replicas) {
        CHECK_INVARIANTS(*replica);
        CHECK_EQ(replica->causal_buffer_size(), 0u);
    }

    auto document = replicas[0]->crdt().traverse();
    std::multiset<Value> actual(document.begin(), document.end());
    std::multiset<Value> expected;
    for (Value value : present) {
        if (!deleted.count(value)) 
            expected.insert(value);
    }

    CHECK_MSG(actual == expected, "lost, duplicated or resurrected elements");
}

}  // namespace

TEST(convergence_random_concurrent_sessions) {
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 1; seed <= 40; ++seed) {
            random_session(algorithm, seed, 2 + seed % 5, 400, DeliveryMode::Causal, 0.1);
        }
    }
}

TEST(convergence_with_heavy_duplication) {
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 100; seed < 110; ++seed) random_session(algorithm, seed, 4, 400, DeliveryMode::Causal, 0.9);
    }
}

TEST(convergence_without_causal_layer_tombstone_algorithms) {
    // WOOT and RGA hold back early operations themselves
    for (Algorithm algorithm : {Algorithm::WOOT, Algorithm::RGA}) {
        for (uint64_t seed = 200; seed < 230; ++seed) {
            random_session(algorithm, seed, 3 + seed % 3, 400, DeliveryMode::Direct, 0.2);
        }
    }
}

TEST(order_independence_all_permutations_sampled) {
    // the same concurrent operations delivered in many different orders to
    // replicas must always give the same document.
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 1; seed <= 6; ++seed) {
            test_framework::context() = algorithm_and_seed(algorithm, seed);

            RandomGenerator random(seed);
            auto initial = make_initial_document(10, seed);
            auto replicas = make_replicas(algorithm, 3, initial);
            std::vector<Operation> all_operations;
            Value next_value = 500;
            for (auto& replica : replicas) {
                for (int count = 0; count < 4; ++count) 
                    all_operations.push_back(random_edit(*replica, random, next_value));
            }

            std::string reference;
            for (int permutation = 0; permutation < 40; ++permutation) {
                test_framework::stats().property_cases++;

                auto observers = make_replicas(algorithm, 1, initial);
                auto operations = all_operations;
                shuffle(operations, random);
                // the causal layer buffers operations whose predecessors are missing
                for (auto& operation : operations) observers[0]->receive(operation);

                std::string text = observers[0]->crdt().text();
                if (permutation == 0) 
                    reference = text;

                CHECK_EQ(text, reference);
                CHECK_INVARIANTS(*observers[0]);
            }
        }
    }
}

TEST(adversarial_all_replicas_insert_at_same_position) {
    for (Algorithm algorithm : all_algorithms()) {
        for (size_t replica_count : {2u, 5u, 10u}) {
            test_framework::context() = to_string(algorithm) + " R=" + std::to_string(replica_count);

            auto replicas = make_replicas(algorithm, replica_count, from_text("[]"));
            std::vector<Operation> operations;
            Value value = 100;
            for (int round = 0; round < 20; ++round) {
                for (auto& replica : replicas) 
                    operations.push_back(replica->insert(1, value++)); // everybody types between [ and ]
            }

            RandomGenerator random(replica_count);
            for (auto& replica : replicas) {
                auto shuffled = operations;
                shuffle(shuffled, random);
                
                for (auto& operation : shuffled) 
                    replica->receive(operation);
            }
            CHECK_CONVERGED(replicas);

            // nothing overwritten or lost and every value that's present exactly once between the brackets
            std::string text = replicas[0]->crdt().text();
            CHECK_EQ(replicas[0]->crdt().size(), 2 + 20 * replica_count);
            CHECK_EQ(text.front(), '[');
            CHECK_EQ(text.back(), ']');

            auto document = replicas[0]->crdt().traverse();
            std::set<Value> unique_values(document.begin() + 1, document.end() - 1);
            CHECK_EQ(unique_values.size(), 20 * replica_count);
            for (auto& replica : replicas) 
                CHECK_INVARIANTS(*replica);
        }
    }
}

TEST(adversarial_concurrent_delete_of_same_element) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto replicas = make_replicas(algorithm, 4, from_text("abcdef"));
        std::vector<Operation> operations;
        for (auto& replica : replicas) 
            operations.push_back(replica->erase(2)); // everyone deletes "c"

        for (auto& replica : replicas) {
            for (auto& operation : operations)
                replica->receive(operation);
        }

        CHECK_CONVERGED(replicas);
        CHECK_EQ(replicas[0]->crdt().text(), std::string("abdef"));
        for (auto& replica : replicas)
            CHECK_INVARIANTS(*replica);
    }
}

TEST(adversarial_insert_next_to_concurrently_deleted_element) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto replicas = make_replicas(algorithm, 3, from_text("abc"));
        Operation delete_b = replicas[0]->erase(1); // delete "b"
        Operation insert_x = replicas[1]->insert(2, 'X'); // after "b"
        Operation insert_y = replicas[2]->insert(1, 'Y'); // before "b"

        std::vector<Operation> operations = {delete_b, insert_x, insert_y};
        RandomGenerator random(1);
        for (auto& replica : replicas) {
            auto shuffled = operations;
            shuffle(shuffled, random);
            
            for (auto& operation : shuffled) 
                replica->receive(operation);
        }
        CHECK_CONVERGED(replicas);
        CHECK_EQ(replicas[0]->crdt().text(), std::string("aYXc"));

        // delete both ends concurrently and insert in the middle
        std::vector<Operation> more;
        more.push_back(replicas[0]->erase(0));
        more.push_back(replicas[1]->erase(3));
        more.push_back(replicas[2]->insert(2, 'Z'));
        for (auto& replica : replicas) {
            for (auto& operation : more) 
                replica->receive(operation);
        }

        CHECK_CONVERGED(replicas);
        CHECK_EQ(replicas[0]->crdt().text(), std::string("YZX"));
        for (auto& replica : replicas) 
            CHECK_INVARIANTS(*replica);
    }
}

TEST(concurrent_insert_and_delete_is_deterministic) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        std::string first_result;
        for (int run = 0; run < 3; ++run) {
            RandomGenerator random(77);
            auto replicas = make_replicas(algorithm, 3, make_initial_document(30, 3));
            std::vector<Operation> operations;
            Value next_value = 1;
            for (int count = 0; count < 60; ++count) {
                operations.push_back(random_edit(*replicas[random.below(3)], random, next_value, 0.5));
            }

            for (auto& replica : replicas) {
                for (auto& operation : operations) replica->receive(operation);
            }

            CHECK_CONVERGED(replicas);
            if (run == 0) first_result = replicas[0]->crdt().text();
            CHECK_EQ(replicas[0]->crdt().text(), first_result);
        }
    }
}

TEST(causal_buffer_holds_premature_operations) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        auto replicas = make_replicas(algorithm, 3, from_text("abc"));
        Operation insert_x = replicas[0]->insert(1, 'X');
        replicas[1]->receive(insert_x);
        Operation delete_x = replicas[1]->erase(1); // deleting X causally depends on insert_x

        // replica 3 receives the delete before the insert
        ReceiveOutcome early = replicas[2]->receive(delete_x);
        CHECK_EQ(early.buffered, 1u);
        CHECK(early.applied.empty());
        CHECK_EQ(replicas[2]->causal_buffer_size(), 1u);
        CHECK_INVARIANTS(*replicas[2]);

        ReceiveOutcome late = replicas[2]->receive(insert_x);
        CHECK_EQ(late.applied.size(), 2u); // insert_x, then the buffered delete_x
        CHECK_EQ(replicas[2]->causal_buffer_size(), 0u);
        CHECK_EQ(replicas[2]->crdt().text(), std::string("abc"));

        // duplicates of delivered and of buffered operations are dropped
        CHECK_EQ(replicas[2]->receive(insert_x).duplicates, 1u);
        CHECK_EQ(replicas[2]->receive(delete_x).duplicates, 1u);
        CHECK_EQ(replicas[2]->delivered_from(1), 1u);
        CHECK_EQ(replicas[2]->delivered_from(2), 1u);
        CHECK_INVARIANTS(*replicas[2]);
    }
}

TEST(stress_many_replicas_long_sessions) {
    for (Algorithm algorithm : all_algorithms()) {
        for (uint64_t seed = 900; seed < 903; ++seed) 
            random_session(algorithm, seed, 8, 6000, DeliveryMode::Causal, 0.05);
    }
}
