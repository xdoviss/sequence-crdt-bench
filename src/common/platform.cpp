#include "crdt/common/platform.hpp"

#include <cstring>
#include <ctime>
#include <fstream>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <intrin.h>
#elif defined(__APPLE__)
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/utsname.h>
#else
#include <sys/resource.h>
#include <sys/utsname.h>
#include <unistd.h>
#endif

// the build system passes these in; the fallbacks are only for unusual builds.
#ifndef CRDT_BUILD_TYPE
#define CRDT_BUILD_TYPE "unknown"
#endif
#ifndef CRDT_CXX_FLAGS
#define CRDT_CXX_FLAGS "unknown"
#endif

namespace crdt {

double process_cpu_seconds() {
#if defined(_WIN32)
    FILETIME creation_time, exit_time, kernel_time, user_time;
    if (!GetProcessTimes(GetCurrentProcess(), &creation_time, &exit_time, &kernel_time, &user_time)) return 0;

    auto to_seconds = [](FILETIME file_time) {
        ULARGE_INTEGER ticks;
        ticks.LowPart = file_time.dwLowDateTime;
        ticks.HighPart = file_time.dwHighDateTime;
        return static_cast<double>(ticks.QuadPart) * 1e-7;  // windows counts in 100 ns steps
    };
    return to_seconds(kernel_time) + to_seconds(user_time);
#else
    struct rusage usage {};
    getrusage(RUSAGE_SELF, &usage);
    return static_cast<double>(usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) +
           static_cast<double>(usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) * 1e-6;
#endif
}

namespace {

std::string compiler_name() {
    std::ostringstream out;
#if defined(__clang__)
    out << "Clang " << __clang_major__ << "." << __clang_minor__ << "." << __clang_patchlevel__;
#if defined(__apple_build_version__)
    out << " (Apple)";
#endif
#elif defined(__GNUC__)
    out << "GCC " << __GNUC__ << "." << __GNUC_MINOR__ << "." << __GNUC_PATCHLEVEL__;
#elif defined(_MSC_VER)
    out << "MSVC " << _MSC_FULL_VER;
#else
    out << "unknown compiler";
#endif
    out << ", C++ " << __cplusplus;
    return out.str();
}

std::string os_name() {
#if defined(_WIN32)
    // the RtlGetVersion function reports the real windows version (GetVersionEx can lie about it)
    std::ostringstream out;
    out << "Windows";
    using RtlGetVersionFunction = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        auto get_version = reinterpret_cast<RtlGetVersionFunction>(
            reinterpret_cast<void*>(GetProcAddress(ntdll, "RtlGetVersion")));
        RTL_OSVERSIONINFOW version_info{};
        version_info.dwOSVersionInfoSize = sizeof(version_info);
        if (get_version && get_version(&version_info) == 0) {
            out << " " << version_info.dwMajorVersion << "." << version_info.dwMinorVersion << " build "
                << version_info.dwBuildNumber;
        }
    }
    return out.str();
#else
    struct utsname system_name {};
    if (uname(&system_name) != 0) return "unknown";

    std::string description = std::string(system_name.sysname) + " " + system_name.release + " " + system_name.machine;
#if defined(__linux__)
    // prefer the distribution's friendly name, e.g. "Ubuntu 24.04 LTS"
    std::ifstream os_release("/etc/os-release");
    std::string line;
    while (std::getline(os_release, line)) {
        if (line.rfind("PRETTY_NAME=", 0) != 0) continue;

        std::string pretty = line.substr(12);
        if (pretty.size() >= 2 && pretty.front() == '"') pretty = pretty.substr(1, pretty.size() - 2);
        description = pretty + " (" + description + ")";
        break;
    }
#endif
    return description;
#endif
}

std::string cpu_name() {
#if defined(_WIN32)
    // the processor's brand text is returned by three cpuid calls, 16 bytes each
    int registers[4];
    char brand[49] = {0};
    __cpuid(registers, 0x80000000);
    if (static_cast<unsigned>(registers[0]) >= 0x80000004u) {
        for (int part = 0; part < 3; ++part) {
            __cpuid(registers, 0x80000002 + part);
            memcpy(brand + 16 * part, registers, 16);
        }
        return brand;
    }
    return "unknown";
#elif defined(__APPLE__)
    char buffer[256];
    size_t length = sizeof(buffer);
    if (sysctlbyname("machdep.cpu.brand_string", buffer, &length, nullptr, 0) == 0) return std::string(buffer);
    return "unknown";
#else
    std::ifstream cpuinfo("/proc/cpuinfo");
    std::string line;
    while (std::getline(cpuinfo, line)) {
        if (line.rfind("model name", 0) != 0 && line.rfind("Model", 0) != 0) continue;

        auto colon = line.find(':');
        if (colon != std::string::npos) return line.substr(colon + 2);
    }
    return "unknown";
#endif
}

uint64_t total_ram_bytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX memory_status{};
    memory_status.dwLength = sizeof(memory_status);
    if (GlobalMemoryStatusEx(&memory_status)) return memory_status.ullTotalPhys;
    return 0;
#elif defined(__APPLE__)
    uint64_t memory = 0;
    size_t length = sizeof(memory);
    if (sysctlbyname("hw.memsize", &memory, &length, nullptr, 0) == 0) return memory;
    return 0;
#else
    long pages = sysconf(_SC_PHYS_PAGES);
    long page_size = sysconf(_SC_PAGESIZE);
    return pages > 0 && page_size > 0 ? static_cast<uint64_t>(pages) * static_cast<uint64_t>(page_size) : 0;
#endif
}

std::string utc_now() {
    std::time_t now = std::time(nullptr);
    std::tm calendar{};
#if defined(_WIN32)
    gmtime_s(&calendar, &now);
#else
    gmtime_r(&now, &calendar);
#endif
    char buffer[32];
    std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &calendar);
    return buffer;
}

}  // namespace

EnvironmentInfo environment_info() {
    EnvironmentInfo info;
    info.compiler = compiler_name();
    info.build_type = CRDT_BUILD_TYPE;
    info.cxx_flags = CRDT_CXX_FLAGS;
    info.os = os_name();
    info.cpu_model = cpu_name();
    info.logical_cores = std::thread::hardware_concurrency();
    info.total_ram_bytes = total_ram_bytes();
    info.timestamp_utc = utc_now();
    return info;
}

}  // namespace crdt
