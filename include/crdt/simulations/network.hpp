// a simulation of N replicas editing one document over a simulated network,
// where every replica sends every operation to every other replica.
//
// network time is simulated ("virtual" milliseconds), while the CPU work (making an
// edit, encoding, decoding, applying) is measured with the real clock and reported
// separately. so the time an edit takes to reach others,
//   propagation = waiting for the next batch + network delay + waiting in the causal buffer,
// is never mixed up with local processing time.
//
// the path of one operation:
//   local edit -> outbox -> [batch / sync interval] -> encode -> network (delay, jitter,
//   maybe lost) -> decode -> causal buffer -> applied to the receiving replica's CRDT
#pragma once

#include <map>
#include <string>

#include "crdt/common/sequence_crdt.hpp"
#include "crdt/common/stats.hpp"
#include "crdt/simulations/replica.hpp"
#include "crdt/simulations/workload.hpp"

namespace crdt {

// how the random extra network delay ("jitter") is distributed.
enum class JitterDistribution { None, Uniform, Exponential };
std::string to_string(JitterDistribution distribution);
JitterDistribution jitter_from_string(const std::string& name);

struct NetworkConfig {
    double latency_ms = 20;  // fixed one-way delay
    double jitter_ms = 10;   // random extra delay (the mean for exponential, the maximum for uniform)
    JitterDistribution jitter = JitterDistribution::Exponential;
    bool reorder = true;           // false: messages between two replicas arrive in the order sent
    double loss_rate = 0;          // probability that a message (or an acknowledgement) is lost
    bool retransmit = true;        // resend unacknowledged messages (needed when loss_rate > 0)
    double retransmit_timeout_ms = 0;             // how long to wait before resending; 0 = choose automatically
    double sync_interval_ms = 0;   // 0: send every edit at once; > 0: send collected edits periodically
    size_t max_batch_operations = 64;     // at most this many operations per message
};

struct SimConfig {
    Algorithm algorithm = Algorithm::RGA;
    CRDTOptions crdt;
    size_t replicas = 3;
    size_t operations_per_replica = 1000;
    size_t initial_document_size = 1000;
    WorkloadConfig workload;
    NetworkConfig network;
    double edit_interval_ms = 50;  // average pause between a user's edits (exponentially distributed)
    size_t burst_size = 1;       // > 1: edits come in bursts of this many
    DeliveryMode delivery = DeliveryMode::Causal;
    uint64_t seed = 1;
};

struct SimResult {
    bool converged = false;
    std::string error;
    std::map<std::string, double> scalars;         // totals and averages
    std::map<std::string, Summary> distributions;  // latency distributions
};

SimResult run_simulation(const SimConfig& config);

}  // namespace crdt
