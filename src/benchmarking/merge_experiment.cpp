// merging and sync time vs num of replicas being edited at once
//
// group of replicas (N) start and make `ops_per_replica` num of edits without seeing others.
// what's measured:
// merge latency - fresh replica receives N * k ops by taking an operation from each replica, that time is measured
// sync time - replicas encode their operations once and other replicas have to decode and apply them
//  until they have the same doc.
// encoding, decoding and applying are timed (CPU time + network delays)
#include <algorithm>
#include <map>

#include "crdt/common/platform.hpp"
#include "crdt/simulations/replica.hpp"
#include "crdt/simulations/workload.hpp"
#include "experiments.hpp"

namespace benchmarking {

using namespace crdt;

namespace {

double elapsed_ns(int64_t start, int64_t end) { return static_cast<double>(end - start); }

}  // namespace

int run_merge(const ExperimentSetup& setup) {
    const Config& config = setup.config;
    const std::string section = "merge";

    auto selected_algorithms = algorithms(config, section);
    auto replica_counts = config.get_int_list(section, "replica_counts", {2, 5, 10, 20, 50});
    const size_t initial_size = static_cast<size_t>(config.get_int(section, "initial_doc_size", 10000));
    const size_t operations_per_replica = static_cast<size_t>(config.get_int(section, "ops_per_replica", 200));
    const size_t repetitions = static_cast<size_t>(config.get_int(section, "repetitions", 3));
    const uint64_t seed = static_cast<uint64_t>(config.get_int(section, "seed", 1));
    auto patterns = config.get_list(section, "patterns", {"hotspot"});
    CRDTOptions options = crdt_options(config);

    std::vector<std::string> header = {"algorithm", "pattern", "replicas", "metric", "unit"};
    for (auto& column : summary_columns()) header.push_back(column);
    CsvWriter merge_csv(join_path(setup.output_directory, setup.name + ".csv"), header);
    CsvWriter sync_csv(join_path(setup.output_directory, setup.name + "_sync.csv"),
                       {"algorithm", "pattern", "replicas", "repetition", "ops_total", "converged", "sync_total_ms",
                        "sync_encode_ms", "sync_decode_ms", "sync_apply_ms", "sync_per_replica_ms", "bytes_exchanged",
                        "messages", "bytes_per_op", "final_doc_size", "tombstones", "avg_id_depth"});

    for (const std::string& pattern : patterns) {
        for (Algorithm algorithm : selected_algorithms) {
            for (long long count_setting : replica_counts) {
                const size_t replica_count = static_cast<size_t>(count_setting);
                print_progress("[merge] %s %s R=%zu\n", to_string(algorithm).c_str(), pattern.c_str(), replica_count);
                std::map<std::string, std::vector<double>> samples;

                for (size_t repetition = 0; repetition < repetitions; ++repetition) {
                    // the same starting state on every replica
                    Bytes snapshot;
                    {
                        auto template_crdt = make_crdt(algorithm, kInitialSite, options);
                        template_crdt->bulk_load(make_initial_document(initial_size, seed + repetition));
                        snapshot = template_crdt->serialize();
                    }

                    std::vector<std::unique_ptr<Replica>> replicas;
                    for (size_t index = 0; index < replica_count; ++index) {
                        ReplicaId id = static_cast<ReplicaId>(index + 1);
                        auto crdt = make_crdt(algorithm, id, options);
                        crdt->deserialize(snapshot);
                        replicas.push_back(std::make_unique<Replica>(id, std::move(crdt)));
                    }

                    // concurrent local edits
                    std::vector<std::vector<Operation>> operations(replica_count);
                    for (size_t index = 0; index < replica_count; ++index) {
                        WorkloadConfig workload;
                        workload.pattern = pattern_from_string(pattern);
                        workload.same_position = initial_size / 2;
                        WorkloadGenerator generator(workload, seed * 7777 + repetition * 131 + index);

                        Replica& replica = *replicas[index];
                        for (size_t count = 0; count < operations_per_replica; ++count) {
                            Edit edit = generator.next(replica.crdt().size());
                            operations[index].push_back(edit.kind == OperationKind::Insert
                                                            ? replica.insert(edit.position, edit.value)
                                                            : replica.erase(edit.position));
                        }
                    }

                    // merge latency as a new observer, one operation from each replica in turn
                    {
                        ReplicaId observer_id = static_cast<ReplicaId>(replica_count + 1);
                        auto crdt = make_crdt(algorithm, observer_id, options);
                        crdt->deserialize(snapshot);
                        Replica observer(observer_id, std::move(crdt), DeliveryMode::Causal, false);

                        int64_t merge_start = now_ns();
                        for (size_t count = 0; count < operations_per_replica; ++count) {
                            for (size_t origin = 0; origin < replica_count; ++origin) {
                                int64_t start = now_ns();
                                observer.receive(operations[origin][count]);
                                samples["merge_latency_us"].push_back(elapsed_ns(start, now_ns()) / 1e3);
                            }
                        }

                        samples["merge_all_ms"].push_back(elapsed_ns(merge_start, now_ns()) / 1e6);
                    }

                    // full sync
                    double encode_ns = 0, decode_ns = 0, apply_ns = 0;
                    size_t bytes_exchanged = 0, messages = 0;

                    std::vector<Bytes> encoded(replica_count);
                    for (size_t origin = 0; origin < replica_count; ++origin) {
                        int64_t start = now_ns();
                        encoded[origin] = encode_batch(replicas[origin]->crdt(), operations[origin]);
                        encode_ns += elapsed_ns(start, now_ns());
                    }

                    for (size_t receiver = 0; receiver < replica_count; ++receiver) {
                        for (size_t origin = 0; origin < replica_count; ++origin) {
                            if (origin == receiver) 
                                continue;

                            bytes_exchanged += encoded[origin].size();
                            ++messages;

                            int64_t decode_start = now_ns();
                            auto incoming = decode_batch(replicas[receiver]->crdt(), encoded[origin]);
                            int64_t apply_start = now_ns();
                            for (const auto& operation : incoming) 
                                replicas[receiver]->receive(operation);

                            int64_t apply_end = now_ns();

                            decode_ns += elapsed_ns(decode_start, apply_start);
                            apply_ns += elapsed_ns(apply_start, apply_end);
                        }
                    }

                    auto reference = replicas[0]->crdt().traverse();
                    bool converged = true;
                    for (size_t index = 1; index < replica_count; ++index) {
                        converged = converged && replicas[index]->crdt().traverse() == reference;
                    }

                    if (!converged) 
                        print_progress("  ERROR: replicas did not converge\n");

                    const double total_ms = (encode_ns + decode_ns + apply_ns) / 1e6;
                    samples["sync_total_ms"].push_back(total_ms);
                    samples["sync_per_replica_ms"].push_back(total_ms / static_cast<double>(replica_count));

                    const size_t deliveries =
                        std::max<size_t>(1, replica_count * operations_per_replica * (replica_count - 1));

                    auto structure = replicas[0]->crdt().get_structure_statistics();
                    sync_csv.row({to_string(algorithm), pattern, std::to_string(replica_count),
                                  std::to_string(repetition), std::to_string(replica_count * operations_per_replica),
                                  converged ? "1" : "0", format_number(total_ms), format_number(encode_ns / 1e6),
                                  format_number(decode_ns / 1e6), format_number(apply_ns / 1e6),
                                  format_number(total_ms / static_cast<double>(replica_count)),
                                  format_number(static_cast<double>(bytes_exchanged)), std::to_string(messages),
                                  format_number(static_cast<double>(bytes_exchanged) / static_cast<double>(deliveries)),
                                  std::to_string(reference.size()), format_number(structure.get("tombstones")),
                                  format_number(structure.get("avg_id_depth"))});
                }

                for (auto& [metric, values] : samples) {
                    std::vector<std::string> row = {to_string(algorithm), pattern, std::to_string(replica_count), metric,
                                                    metric.find("_ms") != std::string::npos ? "ms" : "us"};
                                                    
                    for (auto& cell : summary_cells(summarize(values))) 
                        row.push_back(cell);
                    merge_csv.row(row);
                }
            }
        }
    }

    write_metadata(join_path(setup.output_directory, setup.name + ".meta.json"), config, setup.name);
    return 0;
}

}  // namespace benchmarking
