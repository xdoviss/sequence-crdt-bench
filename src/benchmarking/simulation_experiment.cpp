// network simulation experimenting
#include <algorithm>

#include "crdt/simulations/network.hpp"
#include "experiments.hpp"

namespace benchmarking {

using namespace crdt;

namespace {

NetworkConfig scenario(const Config& config, const std::string& name) {
    const std::string section = "scenario." + name;
    if (!config.data().count(section)) throw std::runtime_error("missing section [" + section + "]");

    NetworkConfig network;
    network.latency_ms = config.get_double(section, "latency_ms", network.latency_ms);
    network.jitter_ms = config.get_double(section, "jitter_ms", network.jitter_ms);
    network.jitter = jitter_from_string(config.get(section, "jitter", "exponential"));
    network.reorder = config.get_bool(section, "reorder", network.reorder);
    network.loss_rate = config.get_double(section, "loss_rate", network.loss_rate);
    network.retransmit = config.get_bool(section, "retransmit", network.retransmit);
    network.retransmit_timeout_ms = config.get_double(section, "rto_ms", network.retransmit_timeout_ms);
    network.sync_interval_ms = config.get_double(section, "sync_interval_ms", network.sync_interval_ms);
    network.max_batch_operations =
        static_cast<size_t>(config.get_int(section, "max_batch_ops", static_cast<long long>(network.max_batch_operations)));

    return network;
}

const std::vector<std::string> kDistributions = {
    "propagation_ms",  "batching_wait_ms", "network_delay_ms", "causal_wait_ms", "local_op_us",
    "remote_apply_us", "encode_us_per_op", "decode_us_per_op", "message_bytes"};

const std::vector<std::string> kScalars = {
    "converged", "invariant_violations", "ops_total", "inserts", "deletes", "messages", "retransmissions",
    "lost_messages", "acks", "op_deliveries", "duplicate_deliveries", "causally_buffered", "rejected_ops",
    "remote_applications", "bytes_total", "bytes_payload", "bytes_envelope", "bytes_acks", "bytes_retransmitted",
    "avg_message_bytes", "bytes_per_op_delivery", "payload_bytes_per_op", "envelope_bytes_per_op",
    "metadata_overhead_ratio", "last_generation_ms", "convergence_time_ms", "settle_time_ms", "cpu_local_ms",
    "cpu_encode_ms", "cpu_decode_ms", "cpu_apply_ms", "cpu_processing_ms", "wall_ms", "process_cpu_ms",
    "cpu_utilization", "local_ops_per_sec", "remote_apply_ops_per_sec", "processing_ops_per_sec",
    "mem_per_replica_bytes", "mem_bytes_per_element", "final_doc_size", "stored_elements", "tombstones",
    "placeholders", "avg_id_depth", "max_id_depth", "tree_depth", "state_bytes"};

struct SweepPoint {
    std::string scenario;
    std::string pattern;
    long long replicas;
    long long operations_per_replica;
    long long initial_document_size;
    double edit_interval_ms;
    long long burst_size;
    size_t repetition;
};

}  // namespace

int run_simulations(const ExperimentSetup& setup) {
    const Config& config = setup.config;
    const std::string section = "experiment";

    auto selected_algorithms = algorithms(config, section);
    auto replica_counts = config.get_int_list(section, "replicas", {5});
    auto edit_counts = config.get_int_list(section, "ops_per_replica", {1000});
    auto document_sizes = config.get_int_list(section, "initial_doc_size", {1000});
    auto patterns = config.get_list(section, "patterns", {"mixed"});
    auto scenarios = config.get_list(section, "scenarios", {"default"});
    auto intervals = config.get_double_list(section, "op_interval_ms", {50});
    auto bursts = config.get_int_list(section, "burst_size", {1});
    const size_t repetitions = static_cast<size_t>(config.get_int(section, "repetitions", 1));
    const uint64_t seed = static_cast<uint64_t>(config.get_int(section, "seed", 1));

    const std::string delivery = config.get(section, "delivery", "paper");
    if (delivery != "causal" && delivery != "direct" && delivery != "paper") {
        throw std::runtime_error("experiment.delivery must be causal, direct or paper");
    }
    auto delivery_for = [&](Algorithm algorithm) {
        if (delivery == "direct" || (delivery == "paper" && algorithm == Algorithm::WOOT)) return DeliveryMode::Direct;
        return DeliveryMode::Causal;
    };
    const size_t hotspot_width = static_cast<size_t>(config.get_int(section, "hotspot_width", 20));
    CRDTOptions options = crdt_options(config);

    // every combination of all sweep dimensions
    std::vector<SweepPoint> points;
    for (const auto& scenario_name : scenarios)
        for (const auto& pattern : patterns)
            for (long long replicas : replica_counts)
                for (long long edits : edit_counts)
                    for (long long document_size : document_sizes)
                        for (double interval : intervals)
                            for (long long burst : bursts)
                                for (size_t repetition = 0; repetition < repetitions; ++repetition)
                                    points.push_back(
                                        {scenario_name, pattern, replicas, edits, document_size, interval, burst, repetition});

    std::vector<std::string> header = {"algorithm", "pattern", "scenario", "replicas", "ops_per_replica",
                                       "initial_doc_size", "op_interval_ms", "offered_ops_per_sec", "burst_size",
                                       "repetition", "seed", "latency_ms", "jitter_ms", "reorder", "loss_rate",
                                       "sync_interval_ms", "max_batch_ops", "delivery"};
    for (auto& key : kScalars)
        header.push_back(key);

    for (auto& distribution : kDistributions) {
        for (auto& statistic : {"mean", "median", "p95", "p99", "max", "ci95"}) header.push_back(distribution + "_" + statistic);
    }
    CsvWriter output(join_path(setup.output_directory, setup.name + ".csv"), header);

    size_t failures = 0;
    for (const SweepPoint& point : points) {
        NetworkConfig network = scenario(config, point.scenario);

        for (Algorithm algorithm : selected_algorithms) {
            SimConfig simulation;
            simulation.algorithm = algorithm;
            simulation.crdt = options;
            simulation.replicas = static_cast<size_t>(point.replicas);
            simulation.operations_per_replica = static_cast<size_t>(point.operations_per_replica);
            simulation.initial_document_size = static_cast<size_t>(point.initial_document_size);
            simulation.workload.pattern = pattern_from_string(point.pattern);
            simulation.workload.hotspot_width = hotspot_width;
            simulation.network = network;
            simulation.edit_interval_ms = point.edit_interval_ms;
            simulation.burst_size = static_cast<size_t>(std::max<long long>(1, point.burst_size));
            simulation.seed = seed + point.repetition;
            simulation.delivery = delivery_for(algorithm);

            print_progress("[sim:%s] %s %s %s R=%lld ops=%lld n0=%lld iv=%g burst=%lld rep=%zu ... ", setup.name.c_str(),
                           to_string(algorithm).c_str(), point.scenario.c_str(), point.pattern.c_str(), point.replicas,
                           point.operations_per_replica, point.initial_document_size, point.edit_interval_ms, point.burst_size,
                           point.repetition);

            SimResult result = run_simulation(simulation);
            print_progress("%s (%.0f ms)\n", result.converged ? "converged" : ("FAILED: " + result.error).c_str(),
                           result.scalars["wall_ms"]);
            if (!result.converged) ++failures;

            // how many edits per second all users together try to make
            const double offered_rate =
                point.edit_interval_ms > 0 ? static_cast<double>(point.replicas) * 1000.0 / point.edit_interval_ms : 0;

            std::vector<std::string> row = {
                to_string(algorithm), point.pattern, point.scenario, std::to_string(point.replicas),
                std::to_string(point.operations_per_replica), std::to_string(point.initial_document_size),
                format_number(point.edit_interval_ms), format_number(offered_rate), std::to_string(point.burst_size),
                std::to_string(point.repetition), std::to_string(simulation.seed), format_number(network.latency_ms),
                format_number(network.jitter_ms), network.reorder ? "1" : "0", format_number(network.loss_rate),
                format_number(network.sync_interval_ms), std::to_string(network.max_batch_operations),
                simulation.delivery == DeliveryMode::Causal ? "causal" : "direct"};

            for (auto& key : kScalars) 
                row.push_back(format_number(result.scalars[key]));

            for (auto& distribution : kDistributions) {
                const Summary& summary = result.distributions[distribution];
                for (double value : {summary.mean, summary.median, summary.p95, summary.p99, summary.max, summary.ci95}) {
                    row.push_back(format_number(value));
                }
            }

            output.row(row);
        }
    }

    write_metadata(join_path(setup.output_directory, setup.name + ".meta.json"), config, setup.name,
                   {{"runs", std::to_string(output.rows())}, {"failed_runs", std::to_string(failures)}});
                   
    return failures == 0 ? 0 : 2;
}

}  // namespace benchmarking
