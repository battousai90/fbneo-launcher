// src/FilterCache.h
#pragma once

#include <string>
#include <vector>
#include <set>
#include <nlohmann/json.hpp>

struct Game; // Forward declaration

class FilterCache {
public:
    struct FilterData {
        std::vector<std::string> systems;
        std::vector<std::string> manufacturers;
        std::vector<std::string> years;
        std::vector<std::string> sources;
        
        // Convert to/from JSON
        void to_json(nlohmann::json& j) const;
        void from_json(const nlohmann::json& j);
    };

    static bool load_from_file(const std::string& cache_file, FilterData& data);
    static bool save_to_file(const std::string& cache_file, const FilterData& data);
    static FilterData generate_from_games(const std::vector<Game>& games);

private:
    static std::string get_cache_file_path();
};