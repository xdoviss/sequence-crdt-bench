// experiments crdt_bench can run.
// read INI files for configs and writes CSVs and meta.json to report results
#pragma once

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

#include "config.hpp"
#include "crdt/common/sequence_crdt.hpp"
#include "results.hpp"

namespace benchmarking {

struct ExperimentSetup {
    Config config;
    std::string output_directory;
    std::string name;
};

int run_micro(const ExperimentSetup& setup); // speed of single operations vs document size
int run_merge(const ExperimentSetup& setup); // merging and synchronising vs number of replicas
int run_memory(const ExperimentSetup& setup); // memory use and growth of the data structures
int run_simulations(const ExperimentSetup& setup); // network simulations over many settings

// algorithm settings shared by all experiments
crdt::CRDTOptions crdt_options(const Config& config);

// the `algorithms` list of a section (all five by default)
std::vector<crdt::Algorithm> algorithms(const Config& config, const std::string& section);

// progress messages go to stderr, so they never mix with results.
inline void print_progress(const char* format, ...) {
    va_list arguments;
    va_start(arguments, format);
    std::vfprintf(stderr, format, arguments);
    va_end(arguments);
    std::fflush(stderr);
}

}  // namespace benchmarking
