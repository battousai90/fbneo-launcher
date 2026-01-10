// src/FilterCache.cpp
#include "FilterCache.h"
#include "Game.h"
#include "AppContext.h"
#include <fstream>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <cctype>

void FilterCache::FilterData::to_json(nlohmann::json& j) const {
    j = nlohmann::json{
        {"systems", systems},
        {"manufacturers", manufacturers},
        {"years", years},
        {"sources", sources}
    };
}

void FilterCache::FilterData::from_json(const nlohmann::json& j) {
    if (j.contains("systems")) {
        systems = j["systems"].get<std::vector<std::string>>();
    }
    if (j.contains("manufacturers")) {
        manufacturers = j["manufacturers"].get<std::vector<std::string>>();
    }
    if (j.contains("years")) {
        years = j["years"].get<std::vector<std::string>>();
    }
    if (j.contains("sources")) {
        sources = j["sources"].get<std::vector<std::string>>();
    }
}

std::string FilterCache::get_cache_file_path() {
    return AppContext::get_user_config_dir() + "/filter_cache.json";
}

bool FilterCache::load_from_file(const std::string& cache_file, FilterData& data) {
    try {
        if (!std::filesystem::exists(cache_file)) {
            std::cout << "[INFO] Filter cache file not found: " << cache_file << std::endl;
            return false;
        }

        std::ifstream file(cache_file);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Cannot open filter cache file: " << cache_file << std::endl;
            return false;
        }

        nlohmann::json j;
        file >> j;
        data.from_json(j);
        
        std::cout << "[INFO] Filter cache loaded successfully with " 
                  << data.systems.size() << " systems, "
                  << data.manufacturers.size() << " manufacturers, "
                  << data.years.size() << " years, "
                  << data.sources.size() << " sources" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load filter cache: " << e.what() << std::endl;
        return false;
    }
}

bool FilterCache::save_to_file(const std::string& cache_file, const FilterData& data) {
    try {
        // Create directory if it doesn't exist
        std::filesystem::path cache_path(cache_file);
        std::filesystem::create_directories(cache_path.parent_path());

        nlohmann::json j;
        data.to_json(j);

        std::ofstream file(cache_file);
        if (!file.is_open()) {
            std::cerr << "[ERROR] Cannot create filter cache file: " << cache_file << std::endl;
            return false;
        }

        file << j.dump(2); // Pretty print with 2-space indentation
        
        std::cout << "[INFO] Filter cache saved successfully with " 
                  << data.systems.size() << " systems, "
                  << data.manufacturers.size() << " manufacturers, "
                  << data.years.size() << " years, "
                  << data.sources.size() << " sources" << std::endl;
        
        return true;
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to save filter cache: " << e.what() << std::endl;
        return false;
    }
}

FilterCache::FilterData FilterCache::generate_from_games(const std::vector<Game>& games) {
    std::set<std::string> systems_set, manufacturers_set, years_set, sources_set;
    
    std::cout << "[INFO] Generating filter cache from " << games.size() << " games..." << std::endl;
    
    // Whitelist of legitimate arcade/console manufacturers
    std::set<std::string> legitimate_manufacturers = {
        "Capcom", "Konami", "SNK", "Sega", "Taito", "Namco", "Nintendo", "Tecmo", "Data East", "Irem",
        "Midway", "Williams", "Atari", "Gottlieb", "Stern", "Bally", "Cinematronics", "Exidy",
        "Jaleco", "UPL", "Toaplan", "Cave", "Psikyo", "Kaneko", "Banpresto", "Hudson", "Acclaim",
        "American Sammy", "Face", "Nichibutsu", "Video System", "Mitchell", "Semicom", "Dooyong",
        "ADK", "Alpha Denshi", "Visco", "Noise Factory", "BrezzaSoft", "Evoga", "Playmore",
        "Raizing", "8ing", "Eighting", "IGS", "SemiCom", "Afega", "Comad", "ESD", "Limenko",
        "Metro", "SunA", "Yunsung", "NMK", "Allumer", "Crux", "Deco", "Electrocoin", "Enerdyne",
        "Examu", "Fields", "G-Mode", "Gaelco", "Hot-B", "Incredible Technologies", "Infogrames",
        "Inter", "Orca", "Romstar", "Sammy", "Seibu Kaihatsu", "Success", "Sun Electronics",
        "Team Play", "TecToy", "Tehkan", "Terminal", "Treco", "Unique", "Wadax", "Wood Place",
        "Compile", "Treasure", "AM7", "Aiky", "Aicom", "Arc", "Artdink", "Atlus", "Bits Laboratory"
    };
    
    for (const auto& game : games) {
        if (!game.system.empty()) {
            systems_set.insert(game.system);
        }
        
        if (!game.manufacturer.empty()) {
            std::string manufacturer = game.manufacturer;
            std::string normalized_manufacturer;
            
            // Normalize manufacturer names to match whitelist
            for (const auto& legit : legitimate_manufacturers) {
                if (manufacturer.find(legit) != std::string::npos) {
                    normalized_manufacturer = legit;
                    break;
                }
            }
            
            // Special case mappings for variations
            if (normalized_manufacturer.empty()) {
                if (manufacturer.find("IREM") != std::string::npos) {
                    normalized_manufacturer = "Irem";
                } else if (manufacturer.find("Data East") != std::string::npos || manufacturer.find("DECO") != std::string::npos) {
                    normalized_manufacturer = "Data East";
                } else if (manufacturer.find("Alpha") != std::string::npos && manufacturer.find("Denshi") != std::string::npos) {
                    normalized_manufacturer = "Alpha Denshi";
                } else if (manufacturer.find("Seibu") != std::string::npos) {
                    normalized_manufacturer = "Seibu Kaihatsu";
                } else if (manufacturer.find("Sun Elec") != std::string::npos) {
                    normalized_manufacturer = "Sun Electronics";
                } else if (manufacturer.find("Video Sys") != std::string::npos) {
                    normalized_manufacturer = "Video System";
                } else if (manufacturer.find("AM7") != std::string::npos || manufacturer.find("Sega AM7") != std::string::npos) {
                    normalized_manufacturer = "Sega";
                }
            }
            
            if (!normalized_manufacturer.empty()) {
                manufacturers_set.insert(normalized_manufacturer);
            }
        }
        
        if (!game.year.empty()) {
            std::string year = game.year;
            // Only accept 4-digit years between 1970-2030
            if (year.length() == 4 && std::all_of(year.begin(), year.end(), ::isdigit)) {
                int year_num = std::stoi(year);
                if (year_num >= 1970 && year_num <= 2030) {
                    years_set.insert(year);
                }
            }
        }
        
        if (!game.sourcefile.empty()) {
            // Extract part before slash for source filter
            std::string source = game.sourcefile;
            size_t slash_pos = source.find('/');
            if (slash_pos != std::string::npos) {
                source = source.substr(0, slash_pos);
            }
            // Only add if it's a reasonable source name (not too short/long)
            if (source.length() >= 2 && source.length() <= 20) {
                sources_set.insert(source);
            }
        }
    }
    
    FilterData data;
    data.systems.assign(systems_set.begin(), systems_set.end());
    data.manufacturers.assign(manufacturers_set.begin(), manufacturers_set.end());
    data.years.assign(years_set.begin(), years_set.end());
    data.sources.assign(sources_set.begin(), sources_set.end());
    
    // Sort years numerically instead of alphabetically
    std::sort(data.years.begin(), data.years.end(), [](const std::string& a, const std::string& b) {
        try {
            int year_a = std::stoi(a);
            int year_b = std::stoi(b);
            return year_a < year_b;
        } catch (...) {
            return a < b; // Fallback to string comparison for non-numeric years
        }
    });
    
    return data;
}