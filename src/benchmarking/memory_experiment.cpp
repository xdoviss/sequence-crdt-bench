// benchmarks memory use and growth
//
// heap bytes are measured by counting every allocation and reported
// next to the CRDTs estimate (get_memory_usage) and its statistics
// (stored elements, tombstones, indentifier depth, and so on and so forth)
//
// bulk - memory vs doc size
// growth - memory vs num of edits
// cycles - many rounds of inserts then the same amount of deletes
// deletion - memory left after deleting a big part of the doc
#include <algorithm>

#include "crdt/common/platform.hpp"
#include "crdt/common/random_generator.hpp"
#include "crdt/simulations/workload.hpp"
#include "experiments.hpp"
#include "heap_tracker.hpp"

namespace benchmarking {

using namespace crdt;

namespace {

struct MemoryRow {
    std::string algorithm;
    std::string experiment;
    std::string pattern;
    double x_value = 0; // document size, edit count, cycle number or deleted fraction
    double operations = 0;
    size_t heap = 0;
    size_t peak = 0;
    bool truncated = false;
};

std::vector<std::string> header() {
    return {"algorithm",         "experiment",      "pattern",      "x",            "ops_applied",
            "logical_size",      "stored_elements", "tombstones",   "placeholders", "heap_bytes",
            "peak_heap_bytes",   "est_bytes",       "heap_bytes_per_logical",       "heap_bytes_per_stored",
            "heap_bytes_per_op", "avg_id_depth",    "max_id_depth", "avg_id_bytes", "tree_depth",
            "serialized_bytes",  "truncated"};
}

// adds the CRDT's statistics to the measured row and writes it
void write_row(CsvWriter& writer, const MemoryRow& row, const SequenceCRDT& crdt) {
    StructureStats stats = crdt.get_structure_statistics();
    double logical = stats.get("logical_size");
    double stored = stats.get("stored_elements");
    size_t serialized = crdt.serialize().size();
    double heap = static_cast<double>(row.heap);

    writer.row({row.algorithm, row.experiment, row.pattern, format_number(row.x_value), format_number(row.operations),
                format_number(logical), format_number(stored), format_number(stats.get("tombstones")),
                format_number(stats.get("placeholders")), format_number(heap),
                format_number(static_cast<double>(row.peak)), format_number(static_cast<double>(crdt.get_memory_usage())),
                format_number(logical > 0 ? heap / logical : 0), format_number(stored > 0 ? heap / stored : 0),
                format_number(row.operations > 0 ? heap / row.operations : 0), format_number(stats.get("avg_id_depth")),
                format_number(stats.get("max_id_depth")), format_number(stats.get("avg_id_bytes")),
                format_number(stats.get("tree_depth")), format_number(static_cast<double>(serialized)),
                row.truncated ? "1" : "0"});
}

}  // namespace

int run_memory(const ExperimentSetup& setup) {
    const Config& config = setup.config;
    const std::string section = "memory";
    if (!heap_tracker::enabled()) print_progress("[memory] warning: heap tracking unavailable on this platform\n");

    auto selected_algorithms = algorithms(config, section);
    auto document_sizes = config.get_int_list(section, "doc_sizes", {1000, 10000, 100000});
    auto patterns = config.get_list(section, "growth_patterns", {"random_insert", "end_insert", "uniform_mixed"});
    const size_t growth_edits = static_cast<size_t>(config.get_int(section, "growth_ops", 20000));
    const size_t checkpoints = static_cast<size_t>(config.get_int(section, "growth_checkpoints", 10));
    const double time_budget_s = config.get_double(section, "time_budget_s", 120);
    const double memory_budget_mb = config.get_double(section, "memory_budget_mb", 2048);
    const size_t cycle_document_size = static_cast<size_t>(config.get_int(section, "cycle_doc_size", 10000));
    const size_t cycles = static_cast<size_t>(config.get_int(section, "cycles", 10));
    const size_t edits_per_cycle = static_cast<size_t>(config.get_int(section, "cycle_ops", 1000));
    const size_t deletion_document_size = static_cast<size_t>(config.get_int(section, "deletion_doc_size", 10000));
    auto fractions = config.get_double_list(section, "delete_fractions", {0.25, 0.5, 0.75, 0.9, 1.0});
    const uint64_t seed = static_cast<uint64_t>(config.get_int(section, "seed", 1));
    CRDTOptions options = crdt_options(config);

    CsvWriter writer(join_path(setup.output_directory, setup.name + ".csv"), header());

    for (Algorithm algorithm : selected_algorithms) {
        const std::string name = to_string(algorithm);

        // bulk
        for (long long size_setting : document_sizes) {
            size_t document_size = static_cast<size_t>(size_setting);
            print_progress("[memory] %s bulk n=%zu\n", name.c_str(), document_size);

            std::vector<Value> initial = make_initial_document(document_size, seed);
            heap_tracker::HeapScope heap_scope;
            auto crdt = make_crdt(algorithm, 1, options);
            crdt->bulk_load(initial);

            MemoryRow row{name, "bulk", "bulk_load", static_cast<double>(document_size), 0,
                          heap_scope.delta(), heap_scope.peak_delta(), false};
            write_row(writer, row, *crdt);
        }

        // growth
        for (const auto& pattern : patterns) {
            print_progress("[memory] %s growth %s\n", name.c_str(), pattern.c_str());

            heap_tracker::HeapScope heap_scope;
            auto crdt = make_crdt(algorithm, 1, options);
            WorkloadConfig workload;
            workload.pattern = pattern_from_string(pattern);
            WorkloadGenerator generator(workload, seed + 99);

            const size_t checkpoint_every = std::max<size_t>(1, growth_edits / checkpoints);
            const int64_t start = now_ns();
            bool truncated = false;

            for (size_t applied = 1; applied <= growth_edits; ++applied) {
                // the returned operation is dropped, need to measure the CRDT, not kept messages
                Edit edit = generator.next(crdt->size());
                if (edit.kind == OperationKind::Insert) 
                    crdt->insert(edit.position, edit.value);
                else
                    crdt->erase(edit.position);

                // stops early if run times out or uses too much memory
                const double elapsed_s = static_cast<double>(now_ns() - start) / 1e9;
                const bool over_budget = elapsed_s > time_budget_s ||
                                         static_cast<double>(heap_scope.delta()) > memory_budget_mb * 1048576.0;
                if (applied < growth_edits && over_budget) 
                    truncated = true;

                if (applied % checkpoint_every == 0 || applied == growth_edits || truncated) {
                    MemoryRow row{name, "growth", pattern, static_cast<double>(applied), static_cast<double>(applied),
                                  heap_scope.delta(), heap_scope.peak_delta(), truncated};
                    write_row(writer, row, *crdt);
                    if (truncated) {
                        print_progress("  time/memory budget exhausted after %zu ops (truncated)\n", applied);
                        break;
                    }
                }
            }
        }

        // cycles
        {
            print_progress("[memory] %s cycles\n", name.c_str());
            std::vector<Value> initial = make_initial_document(cycle_document_size, seed);

            heap_tracker::HeapScope heap_scope;
            auto crdt = make_crdt(algorithm, 1, options);
            crdt->bulk_load(initial);
            RandomGenerator random(seed + 5);

            MemoryRow first_row{name, "cycles", "insert_delete", 0, 0, heap_scope.delta(), heap_scope.peak_delta(), false};
            write_row(writer, first_row, *crdt);

            for (size_t cycle = 1; cycle <= cycles; ++cycle) {
                for (size_t count = 0; count < edits_per_cycle; ++count) crdt->insert(random.below(crdt->size() + 1), 'c');
                for (size_t count = 0; count < edits_per_cycle && crdt->size() > 0; ++count) {
                    crdt->erase(random.below(crdt->size()));
                }

                MemoryRow row{name, "cycles", "insert_delete", static_cast<double>(cycle),
                              static_cast<double>(2 * cycle * edits_per_cycle), heap_scope.delta(),
                              heap_scope.peak_delta(), false};
                write_row(writer, row, *crdt);
            }
        }

        // deletion
        for (double fraction : fractions) {
            print_progress("[memory] %s deletion %.2f\n", name.c_str(), fraction);
            std::vector<Value> initial = make_initial_document(deletion_document_size, seed);

            heap_tracker::HeapScope heap_scope;
            auto crdt = make_crdt(algorithm, 1, options);
            crdt->bulk_load(initial);
            RandomGenerator random(seed + 11);

            size_t deletions = static_cast<size_t>(fraction * static_cast<double>(deletion_document_size) + 0.5);
            for (size_t count = 0; count < deletions && crdt->size() > 0; ++count) crdt->erase(random.below(crdt->size()));

            MemoryRow row{name, "deletion", "delete_fraction", fraction, static_cast<double>(deletions),
                          heap_scope.delta(), heap_scope.peak_delta(), false};
            write_row(writer, row, *crdt);
        }
    }

    write_metadata(join_path(setup.output_directory, setup.name + ".meta.json"), config, setup.name,
                   {{"heap_tracking", heap_tracker::enabled() ? "operator new/delete hook (usable size)" : "unavailable"}});
                   
    return 0;
}

}  // namespace benchmarking
