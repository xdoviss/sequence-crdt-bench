// helpers for timing and machine description
#pragma once

#include <chrono>
#include <cstdint>
#include <string>

namespace crdt {

// nanosecond clock, used to measure how long something takes
inline int64_t now_ns() {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

// how much CPU time (user + system) this process has used so far
double process_cpu_seconds();

// description of the machine and build (written next to every result file)
struct EnvironmentInfo {
    std::string compiler;
    std::string build_type;
    std::string cxx_flags;
    std::string os;
    std::string cpu_model;
    unsigned logical_cores = 0;
    uint64_t total_ram_bytes = 0;
    std::string timestamp_utc;
    double timer_resolution_ns = 0; // smallest step now_ns() can show
    double timer_overhead_ns = 0; // cost of one now_ns() call
};

EnvironmentInfo environment_info();

// stops the compiler from deleting a computation which result we never use,
// which would make the benchmark measure nothing
template <class ValueType>
inline void do_not_optimize(const ValueType& value) {
#if defined(_MSC_VER)
    static volatile const void* sink;
    sink = &value;
#else
    asm volatile("" : : "r,m"(value) : "memory");
#endif
}

}  // namespace crdt
