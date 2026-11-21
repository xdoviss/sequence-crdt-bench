// a small reader for the INI experiment files:
//
//   # comment
//   [section]
//   key = value
//   list = a, b, c
#pragma once

#include <map>
#include <string>
#include <vector>

namespace benchmarking {

class Config {
public:
    static Config load(const std::string& path);
    static Config parse(const std::string& text, const std::string& origin = "<string>");

    bool has(const std::string& section, const std::string& key) const;
    std::string get(const std::string& section, const std::string& key, const std::string& fallback = "") const;
    std::string require(const std::string& section, const std::string& key) const;
    long long get_int(const std::string& section, const std::string& key, long long fallback) const;
    double get_double(const std::string& section, const std::string& key, double fallback) const;
    bool get_bool(const std::string& section, const std::string& key, bool fallback) const;
    std::vector<std::string> get_list(const std::string& section, const std::string& key,
                                      const std::vector<std::string>& fallback = {}) const;
    std::vector<long long> get_int_list(const std::string& section, const std::string& key,
                                        const std::vector<long long>& fallback = {}) const;
    std::vector<double> get_double_list(const std::string& section, const std::string& key,
                                        const std::vector<double>& fallback = {}) const;

    // applies "section.key=value"
    void apply_override(const std::string& assignment);

    const std::map<std::string, std::map<std::string, std::string>>& data() const { return data_; }
    const std::string& origin() const { return origin_; }

private:
    std::map<std::string, std::map<std::string, std::string>> data_; // section -> key -> value
    std::string origin_; // file name, for error messages
};

}  // namespace benchmarking
