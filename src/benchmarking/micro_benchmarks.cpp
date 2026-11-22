// how long operations take, depending on document size
//
// warmup at first (not measured). operations are timed in batches of `batch_ops` and every sample is the time
// per operation of one batch, because one operation is often shorter than the clock step (100 ns on Windows).
// every repetition starts again from a new copy of the doc
#include <algorithm>
#include <map>

#include "crdt/common/platform.hpp"
#include "crdt/common/random_generator.hpp"
#include "crdt/simulations/workload.hpp"
#include "experiments.hpp"
#include "heap_tracker.hpp"

namespace benchmarking {

using namespace crdt;

namespace {

double microseconds_since(int64_t start_ns) { return static_cast<double>(now_ns() - start_ns) / 1e3; }

double average(double total, size_t count) { return count ? total / static_cast<double>(count) : 0; }

// runs operations 0 .. count - 1, split into batches of about `batch` operations
template <class Run>
void time_operations(size_t count, size_t batch, std::vector<double>& batched, std::vector<double>* single,
                     Run run) {
    const size_t batches = std::max<size_t>(1, count / std::max<size_t>(1, batch));
    for (size_t number = 0; number < batches; ++number) {
        const size_t begin = number * count / batches;
        const size_t end = (number + 1) * count / batches;
        if (begin == end) {
            continue;
        }

        if (single && number % 2 == 1) {
            for (size_t index = begin; index < end; ++index) {
                int64_t start = now_ns();
                run(index);
                single->push_back(microseconds_since(start));
            }
        } else {
            int64_t start = now_ns();
            for (size_t index = begin; index < end; ++index) {
                run(index);
            }
            batched.push_back(microseconds_since(start) / static_cast<double>(end - begin));
        }
    }
}

}  // namespace

int run_micro(const ExperimentSetup& setup) {
    const Config& config = setup.config;
    const std::string section = "micro";

    auto selected_algorithms = algorithms(config, section);
    auto document_sizes = config.get_int_list(section, "doc_sizes", {1000, 10000, 100000});
    const size_t repetitions = static_cast<size_t>(config.get_int(section, "repetitions", 5));
    const size_t operations_per_repetition = static_cast<size_t>(config.get_int(section, "ops_per_repetition", 1000));
    const size_t warmup_operations = static_cast<size_t>(config.get_int(section, "warmup_ops", 200));
    const size_t batch = static_cast<size_t>(config.get_int(section, "batch_ops", 25));
    const size_t lookup_samples = static_cast<size_t>(config.get_int(section, "lookup_samples", 20000));
    const size_t traversal_repetitions = static_cast<size_t>(config.get_int(section, "traversal_repetitions", 10));
    const size_t serialization_repetitions =
        static_cast<size_t>(config.get_int(section, "serialization_repetitions", 5));
    const uint64_t seed = static_cast<uint64_t>(config.get_int(section, "seed", 1));
    CRDTOptions options = crdt_options(config);

    std::vector<std::string> header = {"algorithm", "doc_size", "metric", "unit"};
    for (auto& column : summary_columns()) header.push_back(column);
    CsvWriter latency_csv(join_path(setup.output_directory, setup.name + ".csv"), header);
    CsvWriter sizes_csv(join_path(setup.output_directory, setup.name + "_sizes.csv"),
                        {"algorithm", "doc_size", "state_bytes", "state_bytes_per_element",
                         "state_metadata_bytes_per_element", "insert_op_body_bytes", "delete_op_body_bytes",
                         "insert_op_wire_bytes", "id_bytes_avg", "heap_bytes", "heap_bytes_per_element", "est_bytes",
                         "insert_ops_per_sec", "delete_ops_per_sec", "lookup_ops_per_sec", "merge_ops_per_sec",
                         "avg_id_depth", "max_id_depth", "tree_depth"});

    for (Algorithm algorithm : selected_algorithms) {
        for (long long size_setting : document_sizes) {
            const size_t document_size = static_cast<size_t>(size_setting);
            print_progress("[micro] %s n=%zu\n", to_string(algorithm).c_str(), document_size);

            // starting doc, its heap usage, structure and snapshot
            std::vector<Value> initial = make_initial_document(document_size, seed);
            Bytes snapshot;
            size_t heap_bytes = 0;
            size_t estimated_bytes = 0;
            StructureStats structure;
            {
                heap_tracker::HeapScope heap_scope;
                auto base = make_crdt(algorithm, 1, options);
                base->bulk_load(initial);
                heap_bytes = heap_scope.delta();
                estimated_bytes = base->get_memory_usage();
                structure = base->get_structure_statistics();
                snapshot = base->serialize();
            }

            std::map<std::string, std::vector<double>> samples;

            // saving and loading the state
            for (size_t repetition = 0; repetition < serialization_repetitions; ++repetition) {
                auto restored = make_crdt(algorithm, 1, options);

                int64_t start = now_ns();
                restored->deserialize(snapshot);
                samples["state_deserialize_ms"].push_back(microseconds_since(start) / 1e3);

                start = now_ns();
                Bytes serialized = restored->serialize();
                samples["state_serialize_ms"].push_back(microseconds_since(start) / 1e3);
                do_not_optimize(serialized.size());
            }

            double insert_body_bytes = 0, delete_body_bytes = 0, insert_wire_bytes = 0, id_bytes = 0;
            size_t insert_count = 0, delete_count = 0, id_count = 0;

            for (size_t repetition = 0; repetition < repetitions; ++repetition) {
                RandomGenerator random(seed * 31 + repetition);
                auto local = make_crdt(algorithm, 1, options);
                local->deserialize(snapshot);
                auto remote = make_crdt(algorithm, 2, options);
                remote->deserialize(snapshot);

                // every operation made locally, later replayed on `remote`
                std::vector<Operation> log;
                log.reserve(warmup_operations * 2 + operations_per_repetition * 2);

                // warm up
                for (size_t count = 0; count < warmup_operations; ++count) {
                    log.push_back(local->insert(random.below(local->size() + 1), 'w'));
                    if (local->size() > 0) log.push_back(local->erase(random.below(local->size())));
                    do_not_optimize(local->lookup(random.below(std::max<size_t>(1, local->size()))));
                }
                const size_t warmup_log_size = log.size();

                // insertions, positions drawn before timing
                std::vector<size_t> positions;
                std::vector<Value> values;
                for (size_t count = 0; count < operations_per_repetition; ++count) {
                    positions.push_back(random.below(local->size() + count + 1));
                    values.push_back(static_cast<Value>('a' + random.below(26)));
                }
                time_operations(positions.size(), batch, samples["insert_latency_us"],
                                &samples["insert_single_us"], [&](size_t index) {
                                    log.push_back(local->insert(positions[index], values[index]));
                                });

                // lookups by position
                const size_t lookups_per_repetition = std::max<size_t>(1, lookup_samples / repetitions);
                positions.clear();
                for (size_t count = 0; count < lookups_per_repetition && local->size() > 0; ++count) {
                    positions.push_back(random.below(local->size()));
                }
                time_operations(positions.size(), batch, samples["lookup_pos_us"], nullptr, [&](size_t index) {
                    do_not_optimize(local->lookup(positions[index]));
                });

                // lookups by ID
                std::vector<ElementId> ids;
                for (size_t count = 0; count < std::min<size_t>(lookups_per_repetition, 2000) && local->size() > 0;
                     ++count) {
                    ids.push_back(local->id_at(random.below(local->size())));
                }
                for (const auto& id : ids) {
                    id_bytes += static_cast<double>(id.size());
                    ++id_count;
                }
                time_operations(ids.size(), batch, samples["lookup_id_us"], nullptr, [&](size_t index) {
                    do_not_optimize(local->lookup_id(ids[index]));
                });

                // reading the doc
                for (size_t count = 0; count < traversal_repetitions; ++count) {
                    int64_t start = now_ns();
                    auto document = local->traverse();
                    samples["traversal_ms"].push_back(microseconds_since(start) / 1e3);
                    do_not_optimize(document.size());
                }

                // deletes
                positions.clear();
                for (size_t count = 0; count < operations_per_repetition && count < local->size(); ++count) {
                    positions.push_back(random.below(local->size() - count));
                }
                time_operations(positions.size(), batch, samples["delete_latency_us"],
                                &samples["delete_single_us"], [&](size_t index) {
                                    log.push_back(local->erase(positions[index]));
                                });

                // encoding and decoding single op messages
                const size_t measured = log.size() - warmup_log_size;
                std::vector<Bytes> messages(log.size());
                std::vector<WireStats> wire_stats(log.size());
                std::vector<Operation> decoded(log.size());
                for (size_t index = 0; index < log.size(); ++index) {
                    log[index].sequence_number = index + 1;
                    log[index].origin = 1;
                }
                auto encode = [&](size_t index) {
                    ByteWriter writer(messages[index]);
                    encode_operation(*local, log[index], writer, &wire_stats[index]);
                };
                auto decode = [&](size_t index) {
                    ByteReader reader(messages[index]);
                    decoded[index] = decode_operation(*remote, reader);
                };
                for (size_t index = 0; index < warmup_log_size; ++index) {
                    encode(index);
                    decode(index);
                }
                time_operations(measured, batch, samples["op_encode_us"], nullptr, [&](size_t index) {
                    encode(warmup_log_size + index);
                });
                time_operations(measured, batch, samples["op_decode_us"], nullptr, [&](size_t index) {
                    decode(warmup_log_size + index);
                });

                for (size_t index = warmup_log_size; index < log.size(); ++index) {
                    if (log[index].kind == OperationKind::Insert) {
                        insert_body_bytes += static_cast<double>(wire_stats[index].body_bytes);
                        insert_wire_bytes += static_cast<double>(wire_stats[index].total());
                        ++insert_count;
                    } else {
                        delete_body_bytes += static_cast<double>(wire_stats[index].body_bytes);
                        ++delete_count;
                    }
                }

                // merges, where the remote replica applies every operation
                size_t not_applied = 0;
                auto apply = [&](size_t index) {
                    if (remote->apply(decoded[index]) != ApplyStatus::Applied) {
                        ++not_applied;
                    }
                };
                for (size_t index = 0; index < warmup_log_size; ++index) {
                    apply(index);
                }
                time_operations(measured, batch, samples["merge_remote_op_us"], &samples["merge_remote_single_us"],
                                [&](size_t index) {
                                    apply(warmup_log_size + index);
                                });
                if (not_applied > 0) {
                    print_progress("  warning: %zu remote operations were not applied\n", not_applied);
                }

                if (remote->traverse() != local->traverse())
                    print_progress("  ERROR: remote replica diverged!\n");

                // merging all measured operations into a fresh replica at once
                auto batch_replica = make_crdt(algorithm, 3, options);
                batch_replica->deserialize(snapshot);
                for (size_t index = 0; index < warmup_log_size; ++index) 
                    batch_replica->apply(decoded[index]);

                int64_t start = now_ns();
                for (size_t index = warmup_log_size; index < decoded.size(); ++index) 
                    batch_replica->apply(decoded[index]);

                samples["merge_batch_ms"].push_back(microseconds_since(start) / 1e3);
            }

            // write results
            auto unit_of = [](const std::string& metric) {
                return metric.size() > 3 && metric.compare(metric.size() - 3, 3, "_ms") == 0 ? "ms" : "us";
            };

            std::map<std::string, Summary> summaries;
            for (auto& [metric, values] : samples) {
                summaries[metric] = summarize(values);
                std::vector<std::string> row = {to_string(algorithm), std::to_string(document_size), metric,
                                                unit_of(metric)};

                for (auto& cell : summary_cells(summaries[metric])) 
                    row.push_back(cell);

                latency_csv.row(row);
            }

            // from the median batch, so one stalled batch does not change it
            auto operations_per_second = [&](const char* metric) {
                return summaries.count(metric) && summaries[metric].median > 0 ? 1e6 / summaries[metric].median
                                                                                : 0.0;
            };

            const double size = static_cast<double>(document_size);
            const double snapshot_bytes = static_cast<double>(snapshot.size());
            const double content_bytes = size; // one byte per ASCII letter

            sizes_csv.row({to_string(algorithm), std::to_string(document_size), format_number(snapshot_bytes),
                           format_number(document_size ? snapshot_bytes / size : 0),
                           format_number(document_size ? (snapshot_bytes - content_bytes) / size : 0),
                           format_number(average(insert_body_bytes, insert_count)),
                           format_number(average(delete_body_bytes, delete_count)),
                           format_number(average(insert_wire_bytes, insert_count)),
                           format_number(average(id_bytes, id_count)),
                           heap_tracker::enabled() ? format_number(static_cast<double>(heap_bytes)) : "",
                           heap_tracker::enabled() && document_size
                               ? format_number(static_cast<double>(heap_bytes) / size) : "",
                           format_number(static_cast<double>(estimated_bytes)),
                           format_number(operations_per_second("insert_latency_us")),
                           format_number(operations_per_second("delete_latency_us")),
                           format_number(operations_per_second("lookup_pos_us")),
                           format_number(operations_per_second("merge_remote_op_us")),
                           format_number(structure.get("avg_id_depth")), format_number(structure.get("max_id_depth")),
                           format_number(structure.get("tree_depth"))});
        }
    }

    write_metadata(join_path(setup.output_directory, setup.name + ".meta.json"), config, setup.name);
    return 0;
}

}  // namespace benchmarking
