#include "config.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace benchmarking {

namespace {

std::string trim(const std::string& text) {
    size_t begin = 0;
    size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return text.substr(begin, end - begin);
}

// splits "a, b, c" into {"a", "b", "c"}, drops empty parts
std::vector<std::string> split(const std::string& text, char separator) {
    std::vector<std::string> parts;
    std::string part;
    std::istringstream input(text);
    while (std::getline(input, part, separator)) {
        std::string trimmed = trim(part);
        if (!trimmed.empty()) 
            parts.push_back(trimmed);
    }

    return parts;
}

// removes comment - "#" or ";" at the start of the line or after a space
std::string strip_comment(const std::string& line) {
    for (size_t index = 0; index < line.size(); ++index) {
        bool comment_char = line[index] == '#' || line[index] == ';';
        bool starts_word = index == 0 || std::isspace(static_cast<unsigned char>(line[index - 1]));
        if (comment_char && starts_word)
            return line.substr(0, index);
    }
    return line;
}

}  // namespace

Config Config::load(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) 
        throw std::runtime_error("cannot open config file: " + path);

    std::stringstream contents;
    contents << file.rdbuf();

    return parse(contents.str(), path);
}

Config Config::parse(const std::string& text, const std::string& origin) {
    Config config;
    config.origin_ = origin;

    std::istringstream input(text);
    std::string line;
    std::string section = "global";
    int line_number = 0;

    while (std::getline(input, line)) {
        ++line_number;
        if (!line.empty() && line.back() == '\r') line.pop_back(); // windows line endings

        std::string trimmed = trim(strip_comment(line));
        if (trimmed.empty()) continue;

        const std::string location = origin + ":" + std::to_string(line_number);
        if (trimmed.front() == '[') {
            if (trimmed.back() != ']') 
                throw std::runtime_error(location + ": bad section header");

            section = trim(trimmed.substr(1, trimmed.size() - 2));
            config.data_[section];
            continue;
        }

        auto equals = trimmed.find('=');
        if (equals == std::string::npos) 
            throw std::runtime_error(location + ": expected key = value");
        config.data_[section][trim(trimmed.substr(0, equals))] = trim(trimmed.substr(equals + 1));
    }
    return config;
}

bool Config::has(const std::string& section, const std::string& key) const {
    auto found = data_.find(section);
    return found != data_.end() && found->second.count(key) > 0;
}

std::string Config::get(const std::string& section, const std::string& key, const std::string& fallback) const {
    return has(section, key) ? data_.at(section).at(key) : fallback;
}

std::string Config::require(const std::string& section, const std::string& key) const {
    if (!has(section, key)) throw std::runtime_error("missing config key [" + section + "] " + key + " in " + origin_);
    return get(section, key);
}

long long Config::get_int(const std::string& section, const std::string& key, long long fallback) const {
    return has(section, key) ? std::stoll(get(section, key)) : fallback;
}

double Config::get_double(const std::string& section, const std::string& key, double fallback) const {
    return has(section, key) ? std::stod(get(section, key)) : fallback;
}

bool Config::get_bool(const std::string& section, const std::string& key, bool fallback) const {
    if (!has(section, key)) return fallback;

    std::string value = get(section, key);
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char character) { return static_cast<char>(std::tolower(character)); });

    if (value == "1" || value == "true" || value == "yes" || value == "on") return true;
    if (value == "0" || value == "false" || value == "no" || value == "off") return false;
    throw std::runtime_error("bad boolean for " + section + "." + key + ": " + value);
}

std::vector<std::string> Config::get_list(const std::string& section, const std::string& key,
                                          const std::vector<std::string>& fallback) const {
    return has(section, key) ? split(get(section, key), ',') : fallback;
}

std::vector<long long> Config::get_int_list(const std::string& section, const std::string& key,
                                            const std::vector<long long>& fallback) const {
    if (!has(section, key)) return fallback;

    std::vector<long long> values;
    for (const auto& item : get_list(section, key)) 
        values.push_back(std::stoll(item));

    return values;
}

std::vector<double> Config::get_double_list(const std::string& section, const std::string& key,
                                            const std::vector<double>& fallback) const {
    if (!has(section, key)) return fallback;

    std::vector<double> values;
    for (const auto& item : get_list(section, key))
        values.push_back(std::stod(item));
        
    return values;
}

void Config::apply_override(const std::string& assignment) {
    auto equals = assignment.find('=');
    auto dot = assignment.find('.');
    if (equals == std::string::npos || dot == std::string::npos || dot > equals) {
        throw std::runtime_error("override must look like section.key=value: " + assignment);
    }

    std::string section = trim(assignment.substr(0, dot));
    std::string key = trim(assignment.substr(dot + 1, equals - dot - 1));
    data_[section][key] = trim(assignment.substr(equals + 1));
}

}  // namespace benchmarking
