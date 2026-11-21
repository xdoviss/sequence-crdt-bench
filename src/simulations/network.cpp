#include "crdt/simulations/network.hpp"

#include <algorithm>
#include <cmath>
#include <memory>
#include <queue>
#include <stdexcept>
#include <tuple>

#include "crdt/common/platform.hpp"

namespace crdt {

std::string to_string(JitterDistribution distribution) {
    switch (distribution) {
        case JitterDistribution::None: return "none";
        case JitterDistribution::Uniform: return "uniform";
        case JitterDistribution::Exponential: return "exponential";
    }

    return "?";
}

JitterDistribution jitter_from_string(const std::string& name) {
    if (name == "none") return JitterDistribution::None;
    if (name == "uniform") return JitterDistribution::Uniform;
    if (name == "exponential") return JitterDistribution::Exponential;
    throw std::invalid_argument("unknown jitter distribution: " + name);
}

namespace {

constexpr size_t kAcknowledgementBytes = 8; // assumed size of one acknowledgement packet
constexpr size_t kMaxSamples = 1000000;  // per latency distribution (check SampleSink)

enum class EventType : uint8_t { Generate, Flush, Deliver, AcknowledgementArrives, Retransmit };

struct Event {
    double time;
    uint64_t order; // events at the same time run in the order they were scheduled
    EventType type;
    uint32_t replica;
    uint64_t message;
};

// makes std::priority_queue hand out the earliest event first
struct LaterEvent {
    bool operator()(const Event& left, const Event& right) const {
        return left.time != right.time ? left.time > right.time : left.order > right.order;
    }
};

struct Message {
    std::shared_ptr<const Bytes> data; // the same encoded batch is shared by all receivers
    uint32_t from = 0;
    uint32_t to = 0;
    uint32_t operation_count = 0;
    uint16_t attempts = 0;
    bool acknowledged = false;
};

// counters collected during run
struct Totals {
    double cpu_local_ns = 0, cpu_encode_ns = 0, cpu_decode_ns = 0, cpu_apply_ns = 0, setup_ms = 0;
    uint64_t messages_sent = 0, retransmissions = 0, lost = 0, acknowledgements = 0;
    uint64_t deliveries = 0, duplicates = 0, buffered = 0;
    uint64_t bytes_messages = 0, bytes_body = 0, bytes_envelope = 0, bytes_acknowledgements = 0, bytes_retransmitted = 0;
    uint64_t applied_remote = 0, rejected = 0, inserts = 0, deletes = 0;
    double last_apply_time = 0, last_generation_time = 0;
};

class Simulation {
public:
    explicit Simulation(const SimConfig& config)
        : config_(config),
          replica_count_(config.replicas),
          random_(config.seed ^ 0xA11CEull),
          propagation_(kMaxSamples, config.seed + 1),
          batching_wait_(kMaxSamples, config.seed + 2),
          network_delay_(kMaxSamples, config.seed + 3),
          causal_wait_(kMaxSamples, config.seed + 4),
          local_edit_us_(kMaxSamples, config.seed + 5),
          remote_apply_us_(kMaxSamples, config.seed + 6),
          encode_us_(kMaxSamples, config.seed + 7),
          decode_us_(kMaxSamples, config.seed + 8),
          message_bytes_(kMaxSamples, config.seed + 9) {
        if (replica_count_ < 1) throw std::invalid_argument("at least one replica required");

        const NetworkConfig& network = config.network;

        // wait longer for a slow message and its acknowledgement before resending
        retransmit_timeout_ =
            network.retransmit_timeout_ms > 0 ? network.retransmit_timeout_ms : 2 * (network.latency_ms + 3 * network.jitter_ms) + 10;
        acknowledgements_enabled_ = network.retransmit && network.loss_rate > 0;
        last_arrival_on_link_.assign(replica_count_ * replica_count_, 0.0);
    }

    SimResult run() {
        int64_t wall_start = now_ns();
        double cpu_start = process_cpu_seconds();

        setup();
        while (!events_.empty()) {
            Event event = events_.top();
            events_.pop();
            now_ = event.time;

            switch (event.type) {
                case EventType::Generate: on_generate(event.replica); break;
                case EventType::Flush: on_flush(event.replica, true); break;
                case EventType::Deliver: on_deliver(event.message); break;
                case EventType::AcknowledgementArrives:
                    messages_[event.message].acknowledged = true;
                    messages_[event.message].data.reset();
                    break;
                case EventType::Retransmit:
                    if (!messages_[event.message].acknowledged) transmit(event.message);
                    break;
            }
        }

        double wall_ms = static_cast<double>(now_ns() - wall_start) / 1e6;
        double cpu_ms = (process_cpu_seconds() - cpu_start) * 1e3;
        return finish(wall_ms, cpu_ms);
    }

private:
    const SimConfig& config_;
    size_t replica_count_;
    RandomGenerator random_;
    double now_ = 0;  // virtual time in milliseconds
    double retransmit_timeout_ = 0;
    bool acknowledgements_enabled_ = false;
    uint64_t events_scheduled_ = 0;
    std::priority_queue<Event, std::vector<Event>, LaterEvent> events_;

    std::vector<std::unique_ptr<Replica>> replicas_;
    std::vector<WorkloadGenerator> generators_;
    std::vector<size_t> edits_left_;
    std::vector<std::vector<Operation>> outbox_;
    // when each operation was made and sent per replica (indexed by sequence number - 1)
    std::vector<std::vector<double>> generated_at_, sent_at_;
    std::vector<Message> messages_;
    std::vector<double> last_arrival_on_link_;  // for FIFO links [from * replica_count + to]
    // when a buffered operation arrived - (receiver, origin index, sequence number)
    std::map<std::tuple<uint32_t, uint32_t, uint64_t>, double> arrival_of_buffered_;

    SampleSink propagation_, batching_wait_, network_delay_, causal_wait_;
    SampleSink local_edit_us_, remote_apply_us_, encode_us_, decode_us_, message_bytes_;
    Totals totals_;

    void schedule(double time, EventType type, uint32_t replica, uint64_t message = 0) {
        events_.push(Event{time, events_scheduled_++, type, replica, message});
    }

    // a random wait with the given average
    double exponential(double mean) { return mean <= 0 ? 0 : -mean * std::log(1.0 - random_.uniform()); }

    double sample_delay() {
        double jitter = 0;
        if (config_.network.jitter == JitterDistribution::Uniform) jitter = random_.uniform() * config_.network.jitter_ms;
        else if (config_.network.jitter == JitterDistribution::Exponential) jitter = exponential(config_.network.jitter_ms);
        return config_.network.latency_ms + jitter;
    }

    // every replica starts with the same snapshot, then the first edits and flushes are scheduled
    void setup() {
        int64_t start = now_ns();

        auto template_crdt = make_crdt(config_.algorithm, kInitialSite, config_.crdt);
        template_crdt->bulk_load(make_initial_document(config_.initial_document_size, config_.seed));
        Bytes snapshot = template_crdt->serialize();

        for (size_t index = 0; index < replica_count_; ++index) {
            ReplicaId id = static_cast<ReplicaId>(index + 1);
            auto crdt = make_crdt(config_.algorithm, id, config_.crdt);
            crdt->deserialize(snapshot);
            replicas_.push_back(std::make_unique<Replica>(id, std::move(crdt), config_.delivery, false));

            WorkloadConfig workload = config_.workload;
            if (workload.same_position == 0) workload.same_position = config_.initial_document_size / 2;
            generators_.emplace_back(workload, config_.seed * 1000003ull + id * 7919ull);

            edits_left_.push_back(config_.operations_per_replica);
            outbox_.emplace_back();
            generated_at_.emplace_back(config_.operations_per_replica, 0.0);
            sent_at_.emplace_back(config_.operations_per_replica, 0.0);
        }
        totals_.setup_ms = static_cast<double>(now_ns() - start) / 1e6;

        for (size_t index = 0; index < replica_count_; ++index) {
            const auto replica = static_cast<uint32_t>(index);
            if (config_.operations_per_replica > 0) {
                schedule(exponential(config_.edit_interval_ms * config_.burst_size), EventType::Generate, replica);
            }
            if (config_.network.sync_interval_ms > 0) {
                schedule(random_.uniform() * config_.network.sync_interval_ms, EventType::Flush, replica);
            }
        }
    }

    // a user makes one burst of edits - they go to the outbox and out at once without batching
    void on_generate(uint32_t sender) {
        Replica& replica = *replicas_[sender];
        size_t burst = std::min(config_.burst_size, edits_left_[sender]);

        for (size_t count = 0; count < burst; ++count) {
            Edit edit = generators_[sender].next(replica.crdt().size());

            int64_t start = now_ns();
            Operation operation =
                edit.kind == OperationKind::Insert ? replica.insert(edit.position, edit.value) : replica.erase(edit.position);
            double elapsed_ns = static_cast<double>(now_ns() - start);

            totals_.cpu_local_ns += elapsed_ns;
            local_edit_us_.add(elapsed_ns / 1e3);
            (edit.kind == OperationKind::Insert ? totals_.inserts : totals_.deletes)++;
            generated_at_[sender][operation.sequence_number - 1] = now_;
            outbox_[sender].push_back(std::move(operation));
        }

        edits_left_[sender] -= burst;
        totals_.last_generation_time = std::max(totals_.last_generation_time, now_);

        if (config_.network.sync_interval_ms <= 0) on_flush(sender, false);
        if (edits_left_[sender] > 0) {
            schedule(now_ + exponential(config_.edit_interval_ms * config_.burst_size), EventType::Generate, sender);
        }
    }

    // sends everything in the outbox to every other replica, split in batches
    void on_flush(uint32_t sender, bool periodic) {
        auto& outbox = outbox_[sender];
        const SequenceCRDT& codec = replicas_[sender]->crdt();

        for (size_t start = 0; start < outbox.size(); start += config_.network.max_batch_operations) {
            size_t end = std::min(outbox.size(), start + config_.network.max_batch_operations);
            std::vector<Operation> batch(outbox.begin() + static_cast<std::ptrdiff_t>(start),
                                         outbox.begin() + static_cast<std::ptrdiff_t>(end));

            WireStats wire_stats;
            int64_t encode_start = now_ns();
            auto data = std::make_shared<const Bytes>(encode_batch(codec, batch, &wire_stats));
            double encode_ns = static_cast<double>(now_ns() - encode_start);

            totals_.cpu_encode_ns += encode_ns;
            for (size_t count = 0; count < batch.size(); ++count) {
                encode_us_.add(encode_ns / 1e3 / static_cast<double>(batch.size()));
            }

            for (const auto& operation : batch) sent_at_[sender][operation.sequence_number - 1] = now_;

            for (uint32_t receiver = 0; receiver < replica_count_; ++receiver) {
                if (receiver == sender) continue;

                Message message;
                message.data = data;
                message.from = sender;
                message.to = receiver;
                message.operation_count = static_cast<uint32_t>(batch.size());
                messages_.push_back(std::move(message));

                totals_.bytes_body += wire_stats.body_bytes;
                totals_.bytes_envelope += wire_stats.envelope_bytes;
                transmit(messages_.size() - 1);
            }
        }

        outbox.clear();
        if (periodic && edits_left_[sender] > 0) schedule(now_ + config_.network.sync_interval_ms, EventType::Flush, sender);
    }

    // puts a message on the wire
    // message is either lost or arrives after a random delay with acknowledgements on
    // a resend is scheduled in case no acknowledgement comes back.
    void transmit(uint64_t message_id) {
        Message& message = messages_[message_id];
        message.attempts++;
        ++totals_.messages_sent;

        size_t size = message.data->size();
        totals_.bytes_messages += size;
        message_bytes_.add(static_cast<double>(size));
        if (message.attempts > 1) {
            ++totals_.retransmissions;
            totals_.bytes_retransmitted += size;
        }

        bool lost = config_.network.loss_rate > 0 && random_.chance(config_.network.loss_rate);
        if (lost) {
            ++totals_.lost;
        } else {
            double arrival = now_ + sample_delay();
            double& last_arrival = last_arrival_on_link_[static_cast<size_t>(message.from) * replica_count_ + message.to];
            // with FIFO links to not overtake an earlier message on the same link
            if (!config_.network.reorder) arrival = std::max(arrival, last_arrival + 1e-6);
            last_arrival = std::max(last_arrival, arrival);
            schedule(arrival, EventType::Deliver, message.to, message_id);
        }

        if (acknowledgements_enabled_) schedule(now_ + retransmit_timeout_, EventType::Retransmit, message.from, message_id);
    }

    // an operation was applied at `receiver` - record how long each stage of the road(?) took
    void record_applied(uint32_t receiver, ReplicaId origin, uint64_t sequence_number, bool arrived_just_now) {
        uint32_t origin_index = origin - 1;
        double generated = generated_at_[origin_index][sequence_number - 1];
        double sent = sent_at_[origin_index][sequence_number - 1];

        // an operation released from the causal buffer arrived earlier than now
        double arrival = now_;
        if (!arrived_just_now) {
            auto found = arrival_of_buffered_.find({receiver, origin_index, sequence_number});
            if (found != arrival_of_buffered_.end()) {
                arrival = found->second;
                arrival_of_buffered_.erase(found);
            }
        }

        propagation_.add(now_ - generated);
        batching_wait_.add(sent - generated);
        network_delay_.add(arrival - sent);
        causal_wait_.add(now_ - arrival);
        totals_.last_apply_time = std::max(totals_.last_apply_time, now_);
        ++totals_.applied_remote;
    }

    // message arrived - decode and hand every operation to the receiving replica and acknowledge
    void on_deliver(uint64_t message_id) {
        Message& message = messages_[message_id];
        if (!message.data) return;  // late copy of a message that was already acknowledged

        uint32_t receiver = message.to;
        Replica& replica = *replicas_[receiver];

        int64_t decode_start = now_ns();
        std::vector<Operation> operations = decode_batch(replica.crdt(), *message.data);
        double decode_ns = static_cast<double>(now_ns() - decode_start);

        totals_.cpu_decode_ns += decode_ns;
        for (size_t count = 0; count < operations.size(); ++count) {
            decode_us_.add(decode_ns / 1e3 / static_cast<double>(operations.size()));
        }

        for (const auto& operation : operations) {
            ++totals_.deliveries;
            int64_t apply_start = now_ns();
            ReceiveOutcome outcome = replica.receive(operation);
            double apply_ns = static_cast<double>(now_ns() - apply_start);

            totals_.cpu_apply_ns += apply_ns;
            totals_.duplicates += outcome.duplicates;
            totals_.rejected += outcome.rejected;
            if (outcome.buffered) {
                ++totals_.buffered;
                arrival_of_buffered_[{receiver, operation.origin - 1, operation.sequence_number}] = now_;
            }

            if (outcome.applied.empty()) continue;

            // one receive can release several buffered operations, therefore share time between them
            double per_operation_us = apply_ns / 1e3 / static_cast<double>(outcome.applied.size());
            for (const auto& [origin, sequence_number] : outcome.applied) {
                remote_apply_us_.add(per_operation_us);
                bool arrived_just_now = origin == operation.origin && sequence_number == operation.sequence_number;
                record_applied(receiver, origin, sequence_number, arrived_just_now);
            }
        }

        if (!acknowledgements_enabled_) {
            message.data.reset();
            return;
        }

        ++totals_.acknowledgements;
        totals_.bytes_acknowledgements += kAcknowledgementBytes;
        if (!random_.chance(config_.network.loss_rate)) {
            schedule(now_ + sample_delay(), EventType::AcknowledgementArrives, message.from, message_id);
        }
    }

    SimResult finish(double wall_ms, double cpu_ms) {
        SimResult result;
        auto& scalars = result.scalars;
        const Totals& totals = totals_;
        const uint64_t operations_total = static_cast<uint64_t>(replica_count_) * config_.operations_per_replica;

        // did everyone end up with the same document and received every operation?
        std::vector<Value> reference = replicas_[0]->crdt().traverse();
        bool identical = true;
        for (size_t index = 1; index < replica_count_ && identical; ++index) {
            identical = replicas_[index]->crdt().traverse() == reference;
        }

        bool complete = true;
        for (size_t receiver = 0; receiver < replica_count_; ++receiver) {
            for (size_t origin = 0; origin < replica_count_; ++origin) {
                if (replicas_[receiver]->delivered_from(static_cast<ReplicaId>(origin + 1)) != config_.operations_per_replica) {
                    complete = false;
                }
            }
        }

        result.converged = identical && complete;
        if (!identical) result.error = "replicas diverged";
        else if (!complete) result.error = "not all operations were delivered";

        size_t violations = 0;
        for (auto& replica : replicas_) {
            auto crdt_errors = replica->crdt().check_invariants();
            auto causal_errors = replica->check_causal_invariants();
            violations += crdt_errors.size() + causal_errors.size();
            if (!crdt_errors.empty() && result.error.empty()) result.error = crdt_errors.front();
        }

        // counters and bytes
        const double bytes_total = static_cast<double>(totals.bytes_messages + totals.bytes_acknowledgements);
        scalars["replicas"] = static_cast<double>(replica_count_);
        scalars["ops_total"] = static_cast<double>(operations_total);
        scalars["inserts"] = static_cast<double>(totals.inserts);
        scalars["deletes"] = static_cast<double>(totals.deletes);
        scalars["converged"] = result.converged ? 1 : 0;
        scalars["invariant_violations"] = static_cast<double>(violations);
        scalars["messages"] = static_cast<double>(totals.messages_sent);
        scalars["retransmissions"] = static_cast<double>(totals.retransmissions);
        scalars["lost_messages"] = static_cast<double>(totals.lost);
        scalars["acks"] = static_cast<double>(totals.acknowledgements);
        scalars["op_deliveries"] = static_cast<double>(totals.deliveries);
        scalars["duplicate_deliveries"] = static_cast<double>(totals.duplicates);
        scalars["causally_buffered"] = static_cast<double>(totals.buffered);
        scalars["rejected_ops"] = static_cast<double>(totals.rejected);
        scalars["remote_applications"] = static_cast<double>(totals.applied_remote);
        scalars["bytes_total"] = bytes_total;
        scalars["bytes_messages"] = static_cast<double>(totals.bytes_messages);
        scalars["bytes_payload"] = static_cast<double>(totals.bytes_body);
        scalars["bytes_envelope"] = static_cast<double>(totals.bytes_envelope);
        scalars["bytes_acks"] = static_cast<double>(totals.bytes_acknowledgements);
        scalars["bytes_retransmitted"] = static_cast<double>(totals.bytes_retransmitted);
        scalars["avg_message_bytes"] = totals.messages_sent ? static_cast<double>(totals.bytes_messages) /
                                                                  static_cast<double>(totals.messages_sent)
                                                            : 0;

        // per operation and receiver (fanout is every operation goes to replicas - 1 others)
        const double receivers = static_cast<double>(replica_count_ > 1 ? replica_count_ - 1 : 1);
        const double fanout = static_cast<double>(operations_total) * receivers;
        scalars["bytes_per_op_delivery"] = bytes_total / fanout;
        scalars["payload_bytes_per_op"] = operations_total ? static_cast<double>(totals.bytes_body) / fanout : 0;
        scalars["envelope_bytes_per_op"] = operations_total ? static_cast<double>(totals.bytes_envelope) / fanout : 0;

        // share of sent bytes that is not document content (single byte per inserted value)
        const double content_bytes = static_cast<double>(totals.inserts) * receivers;
        scalars["metadata_overhead_ratio"] = bytes_total > 0 ? (bytes_total - content_bytes) / bytes_total : 0;

        // time
        scalars["last_generation_ms"] = totals.last_generation_time;
        scalars["convergence_time_ms"] = std::max(totals.last_apply_time, totals.last_generation_time);
        scalars["settle_time_ms"] = std::max(0.0, totals.last_apply_time - totals.last_generation_time);
        scalars["setup_ms"] = totals.setup_ms;
        scalars["cpu_local_ms"] = totals.cpu_local_ns / 1e6;
        scalars["cpu_encode_ms"] = totals.cpu_encode_ns / 1e6;
        scalars["cpu_decode_ms"] = totals.cpu_decode_ns / 1e6;
        scalars["cpu_apply_ms"] = totals.cpu_apply_ns / 1e6;

        const double processing_ms =
            (totals.cpu_local_ns + totals.cpu_encode_ns + totals.cpu_decode_ns + totals.cpu_apply_ns) / 1e6;
        scalars["cpu_processing_ms"] = processing_ms;
        scalars["wall_ms"] = wall_ms;
        scalars["process_cpu_ms"] = cpu_ms;
        scalars["cpu_utilization"] = wall_ms > 0 ? cpu_ms / wall_ms : 0;
        scalars["local_ops_per_sec"] =
            totals.cpu_local_ns > 0 ? static_cast<double>(operations_total) / (totals.cpu_local_ns / 1e9) : 0;
        scalars["remote_apply_ops_per_sec"] =
            totals.cpu_apply_ns > 0 ? static_cast<double>(totals.applied_remote) / (totals.cpu_apply_ns / 1e9) : 0;
        scalars["processing_ops_per_sec"] =
            processing_ms > 0 ? static_cast<double>(operations_total + totals.applied_remote) / (processing_ms / 1e3) : 0;

        // memory and structure (memory is averaged over replicas, structure taken from the first)
        double memory = 0;
        for (auto& replica : replicas_) memory += static_cast<double>(replica->crdt().get_memory_usage());
        scalars["mem_per_replica_bytes"] = memory / static_cast<double>(replica_count_);

        StructureStats structure = replicas_[0]->crdt().get_structure_statistics();
        for (const char* key : {"logical_size", "stored_elements", "tombstones", "placeholders", "avg_id_depth",
                                "max_id_depth", "avg_id_bytes", "tree_depth", "pending_ops"}) {
            scalars[key] = structure.get(key);
        }
        scalars["final_doc_size"] = static_cast<double>(reference.size());
        scalars["state_bytes"] = static_cast<double>(replicas_[0]->crdt().serialize().size());
        scalars["mem_bytes_per_element"] =
            reference.empty() ? 0 : scalars["mem_per_replica_bytes"] / static_cast<double>(reference.size());

        // distributions
        result.distributions["propagation_ms"] = propagation_.summary();
        result.distributions["batching_wait_ms"] = batching_wait_.summary();
        result.distributions["network_delay_ms"] = network_delay_.summary();
        result.distributions["causal_wait_ms"] = causal_wait_.summary();
        result.distributions["local_op_us"] = local_edit_us_.summary();
        result.distributions["remote_apply_us"] = remote_apply_us_.summary();
        result.distributions["encode_us_per_op"] = encode_us_.summary();
        result.distributions["decode_us_per_op"] = decode_us_.summary();
        result.distributions["message_bytes"] = message_bytes_.summary();
        return result;
    }
};

}  // namespace

SimResult run_simulation(const SimConfig& config) { return Simulation(config).run(); }

}  // namespace crdt
