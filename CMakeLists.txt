cmake_minimum_required(VERSION 3.16)
project(crdt_eval VERSION 1.0.0 LANGUAGES CXX)

# ---------------------------------------------------------------------------
# build settings (the same for every algorithm, so the comparison is fair)
# ---------------------------------------------------------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_property(multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT multi_config AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CRDT_SANITIZERS "Build with AddressSanitizer + UBSan (GCC/Clang, debugging only)" OFF)
option(CRDT_NATIVE "Optimize for the build machine (-march=native); off by default for portability" OFF)
option(CRDT_WARNINGS_AS_ERRORS "Treat compiler warnings as errors" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc /bigobj)
  add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
  if(CRDT_WARNINGS_AS_ERRORS)
    add_compile_options(/WX)
  endif()
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
  if(CRDT_WARNINGS_AS_ERRORS)
    add_compile_options(-Werror)
  endif()
  if(CRDT_NATIVE)
    add_compile_options(-march=native)
  endif()
  if(CRDT_SANITIZERS)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
  endif()
endif()

# the build type and flags are stored inside the programs and written to every result file
if(multi_config)
  set(build_type_text "$<CONFIG>")
  set(flags_text "${CMAKE_CXX_FLAGS} (config-specific flags: see CMAKE_CXX_FLAGS_<CONFIG>)")
else()
  string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
  set(build_type_text "${CMAKE_BUILD_TYPE}")
  set(flags_text "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}}")
endif()
string(STRIP "${flags_text}" flags_text)

# ---------------------------------------------------------------------------
# core library: the CRDTs, shared code, replicas and the network simulation
# ---------------------------------------------------------------------------
add_library(crdt_core STATIC
  src/common/common.cpp
  src/common/stats.cpp
  src/common/platform.cpp
  src/algorithms/woot.cpp
  src/algorithms/rga.cpp
  src/algorithms/position_list.cpp
  src/algorithms/logoot.cpp
  src/algorithms/lseq.cpp
  src/algorithms/treedoc.cpp
  src/algorithms/factory.cpp
  src/simulations/replica.cpp
  src/simulations/workload.cpp
  src/simulations/network.cpp
)
target_include_directories(crdt_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_definitions(crdt_core PRIVATE
  CRDT_BUILD_TYPE="${build_type_text}"
  CRDT_CXX_FLAGS="${flags_text}")

# ---------------------------------------------------------------------------
# experiment runner
# ---------------------------------------------------------------------------
add_executable(crdt_bench
  src/benchmarking/main.cpp
  src/benchmarking/config.cpp
  src/benchmarking/results.cpp
  src/benchmarking/heap_tracker.cpp
  src/benchmarking/micro_benchmarks.cpp
  src/benchmarking/merge_experiment.cpp
  src/benchmarking/memory_experiment.cpp
  src/benchmarking/simulation_experiment.cpp
)
target_include_directories(crdt_bench PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmarking)
target_link_libraries(crdt_bench PRIVATE crdt_core)

# ---------------------------------------------------------------------------
# tests
# ---------------------------------------------------------------------------
enable_testing()
add_executable(crdt_tests
  tests/test_main.cpp
  tests/test_ordered_sequence.cpp
  tests/test_algorithms.cpp
  tests/test_convergence.cpp
  tests/test_replica_network.cpp
  src/benchmarking/config.cpp
)
target_include_directories(crdt_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/tests ${CMAKE_CURRENT_SOURCE_DIR}/src/benchmarking)
target_link_libraries(crdt_tests PRIVATE crdt_core)
add_test(NAME crdt_tests COMMAND crdt_tests --json ${CMAKE_BINARY_DIR}/test_summary.json)
