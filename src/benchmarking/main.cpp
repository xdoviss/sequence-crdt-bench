#include <exception>
#include <filesystem>
#include <iostream>

#include "crdt/common/platform.hpp"
#include "crdt/simulations/workload.hpp"
#include "experiments.hpp"
#include "heap_tracker.hpp"

namespace benchmarking {

crdt::CRDTOptions crdt_options(const Config& config) {
    crdt::CRDTOptions options;
    options.seed = static_cast<uint64_t>(config.get_int("crdt", "seed", static_cast<long long>(options.seed)));
    options.logoot_boundary = static_cast<uint64_t>(
        config.get_int("crdt", "logoot_boundary", static_cast<long long>(options.logoot_boundary)));
    options.lseq_base_bits = static_cast<unsigned>(config.get_int("crdt", "lseq_base_bits", options.lseq_base_bits));
    options.lseq_boundary =
        static_cast<uint64_t>(config.get_int("crdt", "lseq_boundary", static_cast<long long>(options.lseq_boundary)));
    options.treedoc_collect_garbage = config.get_bool("crdt", "treedoc_gc", options.treedoc_collect_garbage);

    return options;
}

std::vector<crdt::Algorithm> algorithms(const Config& config, const std::string& section) {
    std::vector<crdt::Algorithm> selected;
    for (const auto& name : config.get_list(section, "algorithms", {"woot", "logoot", "rga", "lseq", "treedoc"})) {
        selected.push_back(crdt::algorithm_from_string(name));
    }

    return selected;
}

}  // namespace benchmarking

namespace {

void print_usage() {
    std::cerr << "usage:\n"
                 "  crdt_bench run <config.ini> [--out DIR] [--name NAME] [--set section.key=value ...]\n"
                 "  (memory experiments: use crdt_bench_heap, the build with the heap-counting hook)\n"
                 "  crdt_bench env\n"
                 "  crdt_bench list\n";
}

int print_environment() {
    auto environment = crdt::environment_info();
    std::cout << "compiler:      " << environment.compiler << "\n"
              << "build type:    " << environment.build_type << "\n"
              << "cxx flags:     " << environment.cxx_flags << "\n"
              << "os:            " << environment.os << "\n"
              << "cpu:           " << environment.cpu_model << "\n"
              << "logical cores: " << environment.logical_cores << "\n"
              << "ram:           " << (environment.total_ram_bytes >> 20) << " MiB\n"
              << "time (UTC):    " << environment.timestamp_utc << "\n"
              << "heap hook:     " << heap_tracker::hook_description() << "\n";

    return 0;
}

int print_lists() {
    std::cout << "algorithms:";
    for (auto algorithm : crdt::all_algorithms()) 
        std::cout << " " << crdt::to_string(algorithm);

    std::cout << "\npatterns:  ";
    for (auto pattern : crdt::all_patterns()) 
        std::cout << " " << crdt::to_string(pattern);

    std::cout << "\nexperiment types: micro merge memory sim\n";

    return 0;
}

int run_experiment(int argument_count, char** arguments) {
    if (argument_count < 3) {
        print_usage();
        return 1;
    }

    benchmarking::ExperimentSetup setup;
    std::string path = arguments[2];
    setup.config = benchmarking::Config::load(path);
    setup.output_directory = "results";
    setup.name = std::filesystem::path(path).stem().string();

    for (int index = 3; index < argument_count; ++index) {
        std::string argument = arguments[index];
        bool has_value = index + 1 < argument_count;

        if (argument == "--out" && has_value) 
            setup.output_directory = arguments[++index];
        else if (argument == "--name" && has_value)
            setup.name = arguments[++index];
        else if (argument == "--set" && has_value)
            setup.config.apply_override(arguments[++index]);
        else {
            std::cerr << "unknown argument: " << argument << "\n";
            return 1;
        }
    }

    setup.name = setup.config.get("meta", "name", setup.name);
    std::filesystem::create_directories(setup.output_directory);
    std::string type = setup.config.require("meta", "type");
    benchmarking::print_progress("== %s (%s) -> %s\n", setup.name.c_str(), type.c_str(), setup.output_directory.c_str());

    int64_t start = crdt::now_ns();
    int exit_code;
    if (type == "micro") 
        exit_code = benchmarking::run_micro(setup);
    else if (type == "merge")
        exit_code = benchmarking::run_merge(setup);
    else if (type == "memory")
        exit_code = benchmarking::run_memory(setup);
    else if (type == "sim")
        exit_code = benchmarking::run_simulations(setup);
    else 
        throw std::runtime_error("unknown experiment type: " + type);

    benchmarking::print_progress("== %s finished in %.1f s (rc=%d)\n", setup.name.c_str(),
                                 static_cast<double>(crdt::now_ns() - start) / 1e9, exit_code);

    return exit_code;
}

}  // namespace

int main(int argument_count, char** arguments) {
    try {
        std::string command = argument_count >= 2 ? arguments[1] : "";
        if (command == "run") 
            return run_experiment(argument_count, arguments);
        if (command == "env")
            return print_environment();
        if (command == "list")
            return print_lists();

        print_usage();
        return 1;
    } catch (const std::exception& error) {
        std::cerr << "error: " << error.what() << "\n";
        return 1;
    }
}
