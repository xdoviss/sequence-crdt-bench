// responsible for writing results into csv tables and a json file per experiment 
// for machine compiler, config info
#pragma once

#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "config.hpp"
#include "crdt/common/stats.hpp"

namespace benchmarking {

// formats a number for a CSV cell
std::string format_number(double value);

class CsvWriter {
public:
    CsvWriter(const std::string& path, std::vector<std::string> header);
    void row(const std::vector<std::string>& cells);
    size_t rows() const { return rows_; }

private:
    std::string path_;
    std::ofstream output_;
    size_t columns_;
    size_t rows_ = 0;
};

// column names and cells for a Summary, includes: n, mean, median, p95, p99, min, max, stddev, ci95.
std::vector<std::string> summary_columns();
std::vector<std::string> summary_cells(const crdt::Summary& summary);

// writes the JSON description of a run
void write_metadata(const std::string& path, const Config& config, const std::string& experiment,
                    const std::map<std::string, std::string>& extra = {});

std::string join_path(const std::string& directory, const std::string& file);

}  // namespace benchmarking
