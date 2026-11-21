// test runner
// crdt_tests [name filter] [--json summary.json]
#include <chrono>
#include <cstring>
#include <fstream>
#include <iostream>

#include "crdt/common/platform.hpp"
#include "test_framework.hpp"

namespace test_framework {

Stats& stats() {
    static Stats instance;
    return instance;
}

std::vector<TestCase>& registry() {
    static std::vector<TestCase> tests;
    return tests;
}

std::string& context() {
    static std::string current;
    return current;
}

void fail(const char* file, int line, const std::string& what) {
    stats().failed_assertions++;

    std::ostringstream message;
    message << file << ":" << line << ": " << what;
    if (!context().empty()) message << "  [" << context() << "]";
    throw Failure{message.str()};
}

void check_converged(const std::vector<std::unique_ptr<crdt::Replica>>& replicas, const char* file, int line) {
    stats().convergence_checks++;
    stats().assertions++;
    if (replicas.empty()) return;

    auto reference = replicas[0]->crdt().traverse();
    for (size_t index = 1; index < replicas.size(); ++index) {
        if (replicas[index]->crdt().traverse() == reference) continue;

        stats().convergence_violations++;
        fail(file, line,
             "convergence violation: replica " + std::to_string(replicas[index]->id()) + " differs from replica " +
                 std::to_string(replicas[0]->id()) + " ('" + replicas[index]->crdt().text().substr(0, 60) +
                 "' vs '" + replicas[0]->crdt().text().substr(0, 60) + "')");
    }
}

void check_invariants(const crdt::SequenceCRDT& crdt, const char* file, int line) {
    stats().invariant_checks++;
    stats().assertions++;

    auto violations = crdt.check_invariants();
    if (!violations.empty()) {
        stats().invariant_violations++;
        fail(file, line, crdt.name() + " invariant violated: " + violations.front());
    }
}

void check_invariants(const crdt::Replica& replica, const char* file, int line) {
    check_invariants(replica.crdt(), file, line);

    stats().invariant_checks++;
    stats().assertions++;
    auto violations = replica.check_causal_invariants();
    if (!violations.empty()) {
        stats().invariant_violations++;
        fail(file, line, "causal metadata invariant violated: " + violations.front());
    }
}

}  // namespace test_framework

namespace {

std::string json_escape(const std::string& text) {
    std::string escaped;
    for (char character : text) {
        if (character == '\n') {
            escaped += "\\n";
            continue;
        }
        if (character == '"' || character == '\\') escaped += '\\';
        escaped += character;
    }
    return escaped;
}

void write_json_summary(const std::string& path, size_t passed, size_t failed, double seconds,
                        const std::vector<std::pair<std::string, std::string>>& failures) {
    const auto& stats = test_framework::stats();
    auto environment = crdt::environment_info();

    std::ofstream output(path, std::ios::binary);
    output << "{\n  \"tests_total\": " << passed + failed << ",\n  \"tests_passed\": " << passed
           << ",\n  \"tests_failed\": " << failed << ",\n  \"assertions\": " << stats.assertions
           << ",\n  \"failed_assertions\": " << stats.failed_assertions
           << ",\n  \"property_cases\": " << stats.property_cases
           << ",\n  \"convergence_checks\": " << stats.convergence_checks
           << ",\n  \"convergence_violations\": " << stats.convergence_violations
           << ",\n  \"invariant_checks\": " << stats.invariant_checks
           << ",\n  \"invariant_violations\": " << stats.invariant_violations << ",\n  \"duration_s\": " << seconds
           << ",\n  \"compiler\": \"" << json_escape(environment.compiler) << "\",\n  \"os\": \""
           << json_escape(environment.os) << "\",\n  \"failures\": [";

    for (size_t index = 0; index < failures.size(); ++index) {
        output << (index ? ",\n    " : "\n    ") << "{\"test\": \"" << json_escape(failures[index].first)
               << "\", \"message\": \"" << json_escape(failures[index].second) << "\"}";
    }
    output << (failures.empty() ? "]\n}\n" : "\n  ]\n}\n");
}

double milliseconds_since(std::chrono::steady_clock::time_point start) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

}  // namespace

int main(int argument_count, char** arguments) {
    std::string filter;
    std::string json_path;
    for (int index = 1; index < argument_count; ++index) {
        if (std::strcmp(arguments[index], "--json") == 0 && index + 1 < argument_count) json_path = arguments[++index];
        else filter = arguments[index];
    }

    size_t passed = 0;
    size_t failed = 0;
    std::vector<std::pair<std::string, std::string>> failures;
    auto suite_start = std::chrono::steady_clock::now();

    for (const auto& test : test_framework::registry()) {
        if (!filter.empty() && test.name.find(filter) == std::string::npos) continue;

        test_framework::context().clear();
        auto test_start = std::chrono::steady_clock::now();
        try {
            test.function();
            ++passed;
            std::cout << "[ PASS ] " << test.name << " (" << static_cast<long>(milliseconds_since(test_start))
                      << " ms)\n";
        } catch (const test_framework::Failure& failure) {
            ++failed;
            failures.push_back({test.name, failure.message});
            std::cout << "[ FAIL ] " << test.name << "\n         " << failure.message << "\n";
        } catch (const std::exception& error) {
            ++failed;
            failures.push_back({test.name, std::string("unexpected exception: ") + error.what()});
            std::cout << "[ FAIL ] " << test.name << "\n         unexpected exception: " << error.what() << "  ["
                      << test_framework::context() << "]\n";
        }
        std::cout.flush();
    }

    double seconds = milliseconds_since(suite_start) / 1e3;
    const auto& stats = test_framework::stats();
    std::cout << "\n"
              << passed << " passed, " << failed << " failed; " << stats.assertions << " assertions ("
              << stats.failed_assertions << " failed), " << stats.property_cases << " property cases, "
              << stats.convergence_checks << " convergence checks (" << stats.convergence_violations
              << " violations), " << stats.invariant_checks << " invariant checks (" << stats.invariant_violations
              << " violations) in " << seconds << " s\n";

    if (!json_path.empty()) write_json_summary(json_path, passed, failed, seconds, failures);
    return failed == 0 ? 0 : 1;
}
