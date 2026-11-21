// tests for the treap used as the position index
#include <algorithm>

#include "crdt/common/ordered_sequence.hpp"
#include "crdt/common/random_generator.hpp"
#include "test_framework.hpp"

using namespace crdt;

TEST(ordered_sequence_matches_vector) {
    using Sequence = OrderedSequence<int>;

    for (uint64_t seed = 1; seed <= 20; ++seed) {
        test_framework::stats().property_cases++;
        test_framework::context() = "seed=" + std::to_string(seed);

        Sequence sequence(seed);
        std::vector<std::pair<int, bool>> reference; // (value, visible) in order
        std::vector<Sequence::Node*> nodes; // the node of each reference
        RandomGenerator random(seed);
        int next_value = 0;

        for (int step = 0; step < 3000; ++step) {
            uint64_t action = random.below(10);

            if (action < 4 || reference.empty()) {
                // insert after or before a random node
                size_t index = random.below(reference.size() + 1);
                bool visible = random.chance(0.8);
                Sequence::Node* node;
                if (random.chance(0.5)) {
                    node = sequence.insert_after(index == 0 ? nullptr : nodes[index - 1], next_value, visible);
                } else {
                    node = sequence.insert_before(index == reference.size() ? nullptr : nodes[index], next_value, visible);
                }
                reference.insert(reference.begin() + static_cast<long>(index), {next_value, visible});
                nodes.insert(nodes.begin() + static_cast<long>(index), node);
                ++next_value;
            } else if (action < 6) {
                size_t index = random.below(reference.size());
                sequence.erase(nodes[index]);
                reference.erase(reference.begin() + static_cast<long>(index));
                nodes.erase(nodes.begin() + static_cast<long>(index));
            } else if (action < 8) {
                size_t index = random.below(reference.size());
                bool visible = random.chance(0.5);
                sequence.set_visible(nodes[index], visible);
                reference[index].second = visible;
            } else {
                // compare positions and visible counts with the reference
                size_t index = random.below(reference.size());
                CHECK_EQ(sequence.index_of(nodes[index]), index);

                size_t visible_total = static_cast<size_t>(
                    std::count_if(reference.begin(), reference.end(), [](auto& entry) { return entry.second; }));
                CHECK_EQ(sequence.visible_size(), visible_total);

                if (visible_total) {
                    size_t rank = random.below(visible_total);
                    size_t seen = 0;
                    for (size_t position = 0; position < reference.size(); ++position) {
                        if (reference[position].second && seen++ == rank) {
                            CHECK_EQ(sequence.at_visible(rank)->value, reference[position].first);
                        }
                    }
                }
            }
        }

        std::string error;
        CHECK_MSG(sequence.check(&error), error);
        CHECK_EQ(sequence.size(), reference.size());

        size_t position = 0;
        for (auto* node = sequence.first(); node; node = Sequence::next(node), ++position) {
            CHECK_EQ(node->value, reference[position].first);
        }
        if (!reference.empty()) CHECK(sequence.last() == nodes.back());
    }
}

TEST(ordered_sequence_build_and_lower_bound) {
    OrderedSequence<int> sequence;
    std::vector<std::pair<int, bool>> items;
    for (int index = 0; index < 10000; ++index) items.push_back({index * 2, index % 3 != 0});
    sequence.build(std::move(items));

    std::string error;
    CHECK_MSG(sequence.check(&error), error);
    CHECK_EQ(sequence.size(), 10000u);

    for (int key : {-5, 0, 1, 2, 777, 19998, 19999}) {
        auto* node = sequence.lower_bound([&](int value) { return value < key; });
        int expected = key <= 0 ? 0 : ((key + 1) / 2) * 2;
        if (expected > 19998) CHECK(node == nullptr);
        else CHECK_EQ(node->value, expected);
    }

    CHECK(sequence.height() < 80);
}
