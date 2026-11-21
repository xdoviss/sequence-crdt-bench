#include "results.hpp"

#include <cmath>
#include <cstdio>
#include <filesystem>
#include <stdexcept>

#include "crdt/common/platform.hpp"

namespace benchmarking {

namespace {

std::string json_escape(const std::string& text) {
    std::string escaped;
    for (char character : text) {
        switch (character) {
            case '"': escaped += "\\\""; break;
            case '\\': escaped += "\\\\"; break;
            case '\n': escaped += "\\n"; break;
            case '\r': escaped += "\\r"; break;
            case '\t': escaped += "\\t"; break;
            default:
                if (static_cast<unsigned char>(character) < 0x20) {
                    char code[8];
                    std::snprintf(code, sizeof(code), "\\u%04x", character);
                    escaped += code;
                } else {
                    escaped += character;
                }
        }
    }
    return escaped;
}

std::string quoted(const std::string& text) { return "\"" + json_escape(text) + "\""; }

}  // namespace

std::string format_number(double value) {
    if (std::isnan(value) || std::isinf(value)) return "";

    char buffer[64];
    if (value == std::floor(value) && std::fabs(value) < 1e15) std::snprintf(buffer, sizeof(buffer), "%.0f", value);
    else std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    return buffer;
}

std::string join_path(const std::string& directory, const std::string& file) {
    return (std::filesystem::path(directory) / file).string();
}

CsvWriter::CsvWriter(const std::string& path, std::vector<std::string> header)
    : path_(path), output_(path, std::ios::binary), columns_(header.size()) {
    if (!output_) 
        throw std::runtime_error("cannot write " + path);
    row(header);
    rows_ = 0; // header
}

void CsvWriter::row(const std::vector<std::string>& cells) {
    if (cells.size() != columns_) throw std::logic_error("CSV row has wrong number of cells in " + path_);

    for (size_t index = 0; index < cells.size(); ++index) {
        if (index) output_ << ',';

        // patching for , " and ' chars
        const std::string& cell = cells[index];
        if (cell.find_first_of(",\"\n") == std::string::npos) {
            output_ << cell;
            continue;
        }
        output_ << '"';
        for (char character : cell) output_ << (character == '"' ? std::string("\"\"") : std::string(1, character));
        output_ << '"';
    }

    output_ << '\n';
    output_.flush(); // if a long run fails - keep partial results at least..
    ++rows_;
}

std::vector<std::string> summary_columns() {
    return {"n", "mean", "median", "p95", "p99", "min", "max", "stddev", "ci95"};
}

std::vector<std::string> summary_cells(const crdt::Summary& summary) {
    return {format_number(static_cast<double>(summary.count)), format_number(summary.mean),
            format_number(summary.median),                     format_number(summary.p95),
            format_number(summary.p99),                        format_number(summary.min),
            format_number(summary.max),                        format_number(summary.stddev),
            format_number(summary.ci95)};
}

void write_metadata(const std::string& path, const Config& config, const std::string& experiment,
                    const std::map<std::string, std::string>& extra) {
    crdt::EnvironmentInfo environment = crdt::environment_info();
    std::ofstream output(path, std::ios::binary);
    if (!output) 
        throw std::runtime_error("cannot write " + path);

    output << "{\n";
    output << "  \"experiment\": " << quoted(experiment) << ",\n";
    output << "  \"config_file\": " << quoted(config.origin()) << ",\n";

    output << "  \"environment\": {\n";
    output << "    \"compiler\": " << quoted(environment.compiler) << ",\n";
    output << "    \"build_type\": " << quoted(environment.build_type) << ",\n";
    output << "    \"cxx_flags\": " << quoted(environment.cxx_flags) << ",\n";
    output << "    \"os\": " << quoted(environment.os) << ",\n";
    output << "    \"cpu\": " << quoted(environment.cpu_model) << ",\n";
    output << "    \"logical_cores\": " << environment.logical_cores << ",\n";
    output << "    \"total_ram_bytes\": " << environment.total_ram_bytes << ",\n";
    output << "    \"timestamp_utc\": " << quoted(environment.timestamp_utc) << "\n";
    output << "  },\n";

    output << "  \"config\": {";
    bool first_section = true;
    for (const auto& [section, entries] : config.data()) {
        output << (first_section ? "\n" : ",\n") << "    " << quoted(section) << ": {";
        first_section = false;

        bool first_entry = true;
        for (const auto& [key, value] : entries) {
            output << (first_entry ? "" : ", ") << quoted(key) << ": " << quoted(value);
            first_entry = false;
        }

        output << "}";
    }

    output << "\n  }";

    for (const auto& [key, value] : extra)
        output << ",\n  " << quoted(key) << ": " << quoted(value);
        
    output << "\n}\n";
}

}  // namespace benchmarking
