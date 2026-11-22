// tests of the network simulator, the workload generator, statistics and the config reader
#include <cmath>

#include "config.hpp"
#include "crdt/common/stats.hpp"
#include "crdt/simulations/network.hpp"
#include "helpers.hpp"

using namespace crdt;
using namespace test_helpers;

namespace {

SimConfig base_config(Algorithm algorithm) {
    SimConfig config;
    config.algorithm = algorithm;
    config.replicas = 4;
    config.operations_per_replica = 300;
    config.initial_document_size = 100;
    config.workload.pattern = Pattern::Mixed;
    config.seed = 5;
    return config;
}

void expect_converged(const SimResult& result, const SimConfig& config) {
    CHECK_MSG(result.converged, result.error);

    auto scalars = result.scalars;
    CHECK_EQ(scalars["invariant_violations"], 0.0);
    CHECK_EQ(scalars["remote_applications"],
             static_cast<double>(config.replicas * config.operations_per_replica * (config.replicas - 1)));
    test_framework::stats().convergence_checks++;
}

}  // namespace

// dependencies are sent as a delta (only entries that changed since the previous operation),
// and a receiver still holds an operation back until everything it causally depends on is applied
TEST(replica_dependency_delta_keeps_causal_order) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);
        auto make = [&](ReplicaId id) {
            return std::make_unique<Replica>(id, make_crdt(algorithm, id), DeliveryMode::Causal);
        };
        auto a = make(1), b = make(2), c = make(3);

        Operation b1 = b->insert(0, 'b');
        a->receive(b1);
        Operation a1 = a->insert(1, 'x'); // has seen b1 -> depends on (2, 1)
        Operation a2 = a->insert(2, 'y'); // nothing new since a1 -> no dependencies
        CHECK_EQ(a1.dependencies.size(), size_t{1});
        CHECK_EQ(a1.dependencies[0].replica, ReplicaId{2});
        CHECK_EQ(a1.dependencies[0].count, uint64_t{1});
        CHECK(a2.dependencies.empty());

        // c receives a2 and a1 before b1, a2 waits for a1 (sequence order), a1 waits for b1
        CHECK_EQ(c->receive(a2).buffered, size_t{1});
        CHECK_EQ(c->receive(a1).buffered, size_t{1});
        CHECK_EQ(c->receive(b1).applied.size(), size_t{3});
        CHECK(c->crdt().traverse() == a->crdt().traverse());
        CHECK(c->check_causal_invariants().empty());

        // b hears, its next operation lists just that one new entry
        b->receive(a1);
        b->receive(a2);
        Operation b2 = b->insert(0, 'z');
        CHECK_EQ(b2.dependencies.size(), size_t{1});
        CHECK_EQ(b2.dependencies[0].replica, ReplicaId{1});
        CHECK_EQ(b2.dependencies[0].count, uint64_t{2});
    }
}

TEST(network_default_scenario_converges) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        SimConfig config = base_config(algorithm);
        SimResult result = run_simulation(config);
        expect_converged(result, config);
        CHECK(result.distributions["propagation_ms"].mean >= config.network.latency_ms); // includes the network delay
        CHECK(result.scalars["messages"] > 0);
    }
}

TEST(network_scenarios_reorder_loss_batching_bursts) {
    for (Algorithm algorithm : all_algorithms()) {
        for (int scenario = 0; scenario < 5; ++scenario) {
            test_framework::context() = to_string(algorithm) + " scenario=" + std::to_string(scenario);
            SimConfig config = base_config(algorithm);

            if (scenario == 0) { // high jitter => heavy reordering
                config.network.latency_ms = 5;
                config.network.jitter_ms = 200;
            } else if (scenario == 1) { // 25% loss with retransmission
                config.network.loss_rate = 0.25;
            } else if (scenario == 2) { // batching
                config.network.sync_interval_ms = 500;
                config.network.max_batch_operations = 16;
            } else if (scenario == 3) { // FIFO links + editing bursts
                config.network.reorder = false;
                config.burst_size = 25;
                config.workload.pattern = Pattern::Hotspot;
            } else { // slow network + uniform jitter + same position
                config.network.latency_ms = 150;
                config.network.jitter = JitterDistribution::Uniform;
                config.network.jitter_ms = 100;
                config.workload.pattern = Pattern::SamePosition;
            }

            SimResult result = run_simulation(config);
            expect_converged(result, config);

            if (scenario == 0) CHECK(result.scalars["causally_buffered"] > 0);
            if (scenario == 1) {
                CHECK(result.scalars["retransmissions"] > 0);
                CHECK(result.scalars["lost_messages"] > 0);
            }
            if (scenario == 2) {
                const double unbatched = static_cast<double>(config.replicas * config.operations_per_replica * (config.replicas - 1));
                CHECK(result.scalars["messages"] < unbatched);
            }
        }
    }
}

TEST(network_loss_without_retransmission_breaks_eventual_delivery) {
    SimConfig config = base_config(Algorithm::RGA);
    config.network.loss_rate = 0.2;
    config.network.retransmit = false;

    SimResult result = run_simulation(config);
    CHECK(!result.converged); // documents why reliable delivery is required
    CHECK(result.scalars["lost_messages"] > 0);
}

TEST(network_simulation_is_deterministic) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        SimConfig config = base_config(algorithm);
        config.network.loss_rate = 0.1;
        SimResult first = run_simulation(config);
        SimResult second = run_simulation(config);

        for (const char* key : {"messages", "bytes_total", "final_doc_size", "convergence_time_ms", "retransmissions",
                                "causally_buffered", "tombstones"}) {
            CHECK_EQ(first.scalars[key], second.scalars[key]);
        }
        CHECK_EQ(first.distributions["propagation_ms"].p99, second.distributions["propagation_ms"].p99);
    }
}

TEST(network_separates_sync_components) {
    SimConfig config = base_config(Algorithm::LSEQ);
    config.network.sync_interval_ms = 200;
    SimResult result = run_simulation(config);
    expect_converged(result, config);

    // propagation = batching wait + network delay + causal wait (means add up)
    const auto& dists = result.distributions;
    double sum = dists.at("batching_wait_ms").mean + dists.at("network_delay_ms").mean + dists.at("causal_wait_ms").mean;
    CHECK(std::fabs(sum - dists.at("propagation_ms").mean) < 1e-6 * std::max(1.0, sum));
    CHECK(dists.at("batching_wait_ms").mean > 0);
    CHECK(dists.at("network_delay_ms").min >= config.network.latency_ms - 1e-9);
    CHECK(result.scalars["cpu_apply_ms"] > 0);
}

TEST(network_many_replicas_stress) {
    for (Algorithm algorithm : all_algorithms()) {
        test_framework::context() = to_string(algorithm);

        SimConfig config = base_config(algorithm);
        config.replicas = 20;
        config.operations_per_replica = 150;
        config.network.jitter_ms = 60;
        config.workload.pattern = Pattern::Hotspot;
        SimResult result = run_simulation(config);
        expect_converged(result, config);
    }
}

TEST(workload_generator_positions_valid_and_deterministic) {
    for (Pattern pattern : all_patterns()) {
        test_framework::context() = to_string(pattern);

        WorkloadConfig workload;
        workload.pattern = pattern;
        workload.same_position = 7;
        WorkloadGenerator first(workload, 3);
        WorkloadGenerator second(workload, 3);

        size_t document_size = 50;
        size_t deletions = 0;
        for (int step = 0; step < 5000; ++step) {
            Edit edit = first.next(document_size);
            Edit twin = second.next(document_size);
            CHECK(edit.kind == twin.kind && edit.position == twin.position && edit.value == twin.value);

            if (edit.kind == OperationKind::Insert) {
                CHECK(edit.position <= document_size);
                ++document_size;
            } else {
                CHECK(edit.position < document_size);
                --document_size;
                ++deletions;
            }
        }

        bool insert_only = pattern == Pattern::FrontInsert || pattern == Pattern::EndInsert ||
                           pattern == Pattern::RandomInsert || pattern == Pattern::SamePosition;
        if (insert_only) CHECK_EQ(deletions, 0u);
        if (pattern == Pattern::DeletionHeavy || pattern == Pattern::UniformMixed) CHECK(deletions > 1000);
    }
}

TEST(statistics_summary_values) {
    std::vector<double> values;
    for (int value = 1; value <= 100; ++value) values.push_back(value);

    Summary summary = summarize(values);
    CHECK_EQ(summary.count, 100u);
    CHECK(std::fabs(summary.mean - 50.5) < 1e-9);
    CHECK(std::fabs(summary.median - 50.5) < 1e-9);
    CHECK(std::fabs(summary.p95 - 95.05) < 1e-9);
    CHECK(std::fabs(summary.p99 - 99.01) < 1e-9);
    CHECK_EQ(summary.min, 1.0);
    CHECK_EQ(summary.max, 100.0);
    CHECK(std::fabs(summary.stddev - 29.011491975882) < 1e-6);
    CHECK(summary.ci95 > 5.6 && summary.ci95 < 5.8);

    SampleSink sink(10, 1);
    for (int count = 0; count < 1000; ++count) sink.add(1.0);
    CHECK_EQ(sink.summary().count, 1000u);
    CHECK_EQ(sink.summary().mean, 1.0);
}

TEST(config_parser) {
    auto config = benchmarking::Config::parse(
        "# comment\r\n[meta]\r\ntype = sim ; trailing\r\n[experiment]\nalgorithms = rga, lseq\nreplicas=2,5\n"
        "flag = yes\nratio = 0.25\n");

    CHECK_EQ(config.get("meta", "type"), std::string("sim"));
    CHECK_EQ(config.get_list("experiment", "algorithms").size(), 2u);
    CHECK_EQ(config.get_int_list("experiment", "replicas")[1], 5LL);
    CHECK(config.get_bool("experiment", "flag", false));
    CHECK_EQ(config.get_double("experiment", "ratio", 0), 0.25);

    config.apply_override("experiment.replicas=7");
    CHECK_EQ(config.get_int("experiment", "replicas", 0), 7LL);
    CHECK_THROWS(benchmarking::Config::parse("[bad\n"), std::runtime_error);
}
