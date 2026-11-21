#pragma once

#include <cstdint>
#include <memory>
#include <ostream>
#include <sstream>
#include <string>
#include <vector>

#include "crdt/simulations/replica.hpp"

namespace crdt {
// makes CHECK_EQ print these types when a check fails
inline std::ostream& operator<<(std::ostream& output, ApplyStatus status) { 
    return output << to_string(status); 
}

inline std::ostream& operator<<(std::ostream& output, OperationKind kind) {
    return output << (kind == OperationKind::Insert ? "Insert" : "Delete");
}
}  // namespace crdt

namespace test_framework {

struct Failure {
    std::string message;
};

// counters printed at the end and written to the JSON summary
struct Stats {
    uint64_t assertions = 0;
    uint64_t failed_assertions = 0;
    uint64_t convergence_checks = 0;
    uint64_t convergence_violations = 0;
    uint64_t invariant_checks = 0;
    uint64_t invariant_violations = 0;
    uint64_t property_cases = 0;
};
Stats& stats();

using TestFunction = void (*)();
struct TestCase {
    std::string name;
    TestFunction function;
};
std::vector<TestCase>& registry();

// creating one of these (done by the TEST macro) adds a test to the registry
struct Registrar {
    Registrar(const char* name, TestFunction function) { 
        registry().push_back({name, function}); 
    }
};

// extra text printed with a failure, e.g. the current algorithm and seed
std::string& context();

[[noreturn]] void fail(const char* file, int line, const std::string& what);

// checks that all replicas hold the same document
void check_converged(const std::vector<std::unique_ptr<crdt::Replica>>& replicas, const char* file, int line);
// checks the internal rules of a CRDT (for a replica of its causal bookkeeping too)
void check_invariants(const crdt::SequenceCRDT& crdt, const char* file, int line);
void check_invariants(const crdt::Replica& replica, const char* file, int line);

template <class Left, class Right>
std::string describe(const Left& left, const Right& right) {
    std::ostringstream output;
    output << "'" << left << "' vs '" << right << "'";
    return output.str();
}

} // namespace test_framework

#define TEST(name) \
    static void name(); \
    static ::test_framework::Registrar test_registrar_##name(#name, &name); \
    static void name()

#define CHECK(condition) \
    do { \
        ::test_framework::stats().assertions++; \
        if (!(condition)) ::test_framework::fail(__FILE__, __LINE__, "CHECK(" #condition ")"); \
    } while (0)

#define CHECK_MSG(condition, message) \
    do { \
        ::test_framework::stats().assertions++; \
        if (!(condition)) ::test_framework::fail(__FILE__, __LINE__, std::string("CHECK(" #condition ") ") + (message)); \
    } while (0)

#define CHECK_EQ(actual, expected) \
    do { \
        ::test_framework::stats().assertions++; \
        const auto test_actual = (actual); \
        const auto test_expected = (expected);  \
        if (!(test_actual == test_expected)) \
            ::test_framework::fail(__FILE__, __LINE__, \
                                   std::string("CHECK_EQ(" #actual ", " #expected ") ") + \
                                       ::test_framework::describe(test_actual, test_expected)); \
    } while (0)

#define CHECK_THROWS(expression, ExceptionType) \
    do { \
        ::test_framework::stats().assertions++; \
        bool test_thrown = false;  \
        try { \
            (void)(expression);  \
        } catch (const ExceptionType&) { \
            test_thrown = true; \
        }  \
        if (!test_thrown) \
            ::test_framework::fail(__FILE__, __LINE__, "expected " #ExceptionType " from " #expression); \
    } while (0)

#define CHECK_CONVERGED(replicas) ::test_framework::check_converged((replicas), __FILE__, __LINE__)
#define CHECK_INVARIANTS(subject) ::test_framework::check_invariants((subject), __FILE__, __LINE__)
