// workload generation: decides what a simulated user types next
// (insert or delete, and at which visible position), given the current document size.
#pragma once

#include <string>
#include <vector>

#include "crdt/common/random_generator.hpp"
#include "crdt/common/types.hpp"

namespace crdt {

enum class Pattern {
    UniformMixed,      // 50% inserts / 50% deletes at random positions
    FrontInsert,       // always insert at the beginning
    EndInsert,         // always insert at the end (appending)
    RandomInsert,      // insert at random positions
    SamePosition,      // insert again and again at one fixed position
    SequentialTyping,  // typing at a cursor, with some backspaces and cursor jumps
    DeletionHeavy,     // 80% deletes / 20% inserts at random positions
    Mixed,             // a realistic mix of typing, backspacing and random edits
    Hotspot            // every replica edits the same small region in the middle
};

std::string to_string(Pattern pattern);
Pattern pattern_from_string(const std::string& name);
std::vector<Pattern> all_patterns();

struct WorkloadConfig {
    Pattern pattern = Pattern::UniformMixed;
    size_t same_position = 0;   // where SamePosition inserts (clamped to the document size)
    size_t hotspot_width = 20;  // how many visible elements wide the Hotspot region is
};

// one planned edit.
struct Edit {
    OperationKind kind = OperationKind::Insert;
    size_t position = 0;
    Value value = 'a';
};

class WorkloadGenerator {
public:
    WorkloadGenerator(const WorkloadConfig& config, uint64_t seed);
    // the next edit for a document that has `document_size` visible elements right now.
    Edit next(size_t document_size);

private:
    WorkloadConfig config_;
    RandomGenerator random_;
    size_t cursor_ = 0;
    bool cursor_placed_ = false;

    Value random_letter() { return static_cast<Value>('a' + random_.below(26)); }
    Edit random_edit(size_t document_size, double delete_probability);
    Edit typing_edit(size_t document_size, Value letter);
};

// a deterministic pseudo-random starting text of `length` letters.
std::vector<Value> make_initial_document(size_t length, uint64_t seed);

}  // namespace crdt
