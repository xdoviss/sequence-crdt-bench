#include "crdt/simulations/workload.hpp"

#include <algorithm>
#include <stdexcept>

namespace crdt {

namespace {
// note: the insert-only patterns below still toss a "delete?" coin with probability 0.
// it never comes up, but it uses up one random number, which keeps the random sequence
// (and therefore every result) identical to the reported experiments.
constexpr double kNeverDelete = 0.0;

constexpr double kCursorJumpProbability = 0.02;
constexpr double kTypingBackspaceProbability = 0.10;
constexpr double kMixedBackspaceProbability = 0.20;
constexpr double kMixedRandomEditProbability = 0.25;
}  // namespace

std::string to_string(Pattern pattern) {
    switch (pattern) {
        case Pattern::UniformMixed: return "uniform_mixed";
        case Pattern::FrontInsert: return "front_insert";
        case Pattern::EndInsert: return "end_insert";
        case Pattern::RandomInsert: return "random_insert";
        case Pattern::SamePosition: return "same_position";
        case Pattern::SequentialTyping: return "sequential_typing";
        case Pattern::DeletionHeavy: return "deletion_heavy";
        case Pattern::Mixed: return "mixed";
        case Pattern::Hotspot: return "hotspot";
    }
    return "?";
}

Pattern pattern_from_string(const std::string& name) {
    for (Pattern pattern : all_patterns()) {
        if (to_string(pattern) == name) return pattern;
    }
    throw std::invalid_argument("unknown workload pattern: " + name);
}

std::vector<Pattern> all_patterns() {
    return {Pattern::UniformMixed,  Pattern::FrontInsert, Pattern::EndInsert,
            Pattern::RandomInsert,  Pattern::SamePosition, Pattern::SequentialTyping,
            Pattern::DeletionHeavy, Pattern::Mixed,       Pattern::Hotspot};
}

WorkloadGenerator::WorkloadGenerator(const WorkloadConfig& config, uint64_t seed) : config_(config), random_(seed) {}

// delete a random element with the given probability, otherwise insert at a random place.
Edit WorkloadGenerator::random_edit(size_t document_size, double delete_probability) {
    if (document_size > 0 && random_.chance(delete_probability)) {
        return Edit{OperationKind::Delete, random_.below(document_size), 0};
    }

    size_t position = random_.below(document_size + 1);
    return Edit{OperationKind::Insert, position, random_letter()};
}

// a person typing at a cursor: mostly letters, sometimes a backspace, rarely a jump
// elsewhere. the Mixed pattern also makes a random edit somewhere else now and then.
Edit WorkloadGenerator::typing_edit(size_t document_size, Value letter) {
    const bool mixed = config_.pattern == Pattern::Mixed;
    if (!cursor_placed_) {
        cursor_ = random_.below(document_size + 1);
        cursor_placed_ = true;
    }
    cursor_ = std::min(cursor_, document_size);  // other replicas may have shortened the document

    if (mixed && random_.chance(kMixedRandomEditProbability)) {
        // an edit before the cursor shifts the cursor along with the text
        Edit edit = random_edit(document_size, 0.4);
        if (edit.kind == OperationKind::Insert && edit.position <= cursor_) ++cursor_;
        if (edit.kind == OperationKind::Delete && edit.position < cursor_) --cursor_;
        return edit;
    }

    if (random_.chance(kCursorJumpProbability)) cursor_ = random_.below(document_size + 1);

    double backspace_probability = mixed ? kMixedBackspaceProbability : kTypingBackspaceProbability;
    if (cursor_ > 0 && random_.chance(backspace_probability)) {
        --cursor_;
        return Edit{OperationKind::Delete, cursor_, 0};
    }
    return Edit{OperationKind::Insert, cursor_++, letter};
}

Edit WorkloadGenerator::next(size_t document_size) {
    // the letter for an insertion is always drawn first, even if this turns into a delete
    // or a random edit draws its own letter
    const Value letter = random_letter();

    switch (config_.pattern) {
        case Pattern::UniformMixed: return random_edit(document_size, 0.5);
        case Pattern::DeletionHeavy: return random_edit(document_size, 0.8);
        case Pattern::RandomInsert: return random_edit(document_size, kNeverDelete);

        case Pattern::FrontInsert:
            if (document_size > 0 && random_.chance(kNeverDelete)) return Edit{OperationKind::Delete, 0, 0};
            return Edit{OperationKind::Insert, 0, letter};

        case Pattern::EndInsert:
            if (document_size > 0 && random_.chance(kNeverDelete)) return Edit{OperationKind::Delete, document_size - 1, 0};
            return Edit{OperationKind::Insert, document_size, letter};

        case Pattern::SamePosition: {
            size_t target = std::min(config_.same_position, document_size);
            if (document_size > target && random_.chance(kNeverDelete)) return Edit{OperationKind::Delete, target, 0};
            return Edit{OperationKind::Insert, target, letter};
        }

        case Pattern::SequentialTyping:
        case Pattern::Mixed: return typing_edit(document_size, letter);

        case Pattern::Hotspot: {
            // a window of `width` elements around the middle of the document
            size_t width = std::min(config_.hotspot_width, document_size);
            size_t start = document_size / 2 >= width / 2 ? document_size / 2 - width / 2 : 0;
            if (width > 0 && random_.chance(0.3)) return Edit{OperationKind::Delete, start + random_.below(width), 0};
            return Edit{OperationKind::Insert, start + random_.below(width + 1), letter};
        }
    }
    return Edit{OperationKind::Insert, 0, letter};
}

std::vector<Value> make_initial_document(size_t length, uint64_t seed) {
    RandomGenerator random(seed ^ 0x1D0Cull);
    std::vector<Value> document(length);
    for (auto& value : document) value = static_cast<Value>('a' + random.below(26));
    return document;
}

}  // namespace crdt
