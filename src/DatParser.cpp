// src/DatParser.cpp
#include "Game.h"
#include "DatParser.h"
#include "DatabaseManager.h"
#include <pugixml.hpp>
#include <iostream>
#include <filesystem>
#include <algorithm>
#include <chrono>

std::vector<Game> DatParser::parse(const std::string& filepath) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());

    if (!result) {
        std::cerr << "Erreur : impossible de charger " << filepath << " (" << result.description() << ")" << std::endl;
        return {};
    }

    std::vector<Game> games;
    auto games_node = doc.child("datafile");
    if (!games_node) {
        std::cerr << "Erreur : pas de <datafile> dans " << filepath << std::endl;
        return {};
    }

    for (auto game_node : games_node.children("game")) {
        Game game;
        game.name = game_node.attribute("name").value();
        game.description = game_node.child("description").text().get();
        game.year = game_node.child("year").text().get();
        game.manufacturer = game_node.child("manufacturer").text().get();
        
        // Additional game attributes
        game.cloneof = game_node.attribute("cloneof").value();
        game.romof = game_node.attribute("romof").value();
        game.sourcefile = game_node.attribute("sourcefile").value();
        game.comment = game_node.child("comment").text().get();

        // Video information
        auto video_node = game_node.child("video");
        if (video_node) {
            game.video_type = video_node.attribute("type").value();
            game.orientation = video_node.attribute("orientation").value();
            game.width = video_node.attribute("width").value();
            game.height = video_node.attribute("height").value();
            game.aspect_x = video_node.attribute("aspectx").value();
            game.aspect_y = video_node.attribute("aspecty").value();
        }
        
        // Driver information
        auto driver_node = game_node.child("driver");
        if (driver_node) {
            game.driver_status = driver_node.attribute("status").value();
        }

        for (auto rom_node : game_node.children("rom")) {
            Rom rom;
            rom.name = rom_node.attribute("name").value();
            rom.size = rom_node.attribute("size").as_ullong();
            rom.crc = rom_node.attribute("crc").value();
            game.roms.push_back(rom);
        }

        // Extract system name from header
        auto header = games_node.child("header");
        if (header) {
            std::string headerName = header.child("name").text().get();
            game.system = extractSystemFromHeader(headerName);
        } else {
            game.system = "Unknown";
        }

        game.status = "missing";  // tous les jeux commencent comme missing
        games.push_back(game);
    }

    return games;
}

std::vector<Game> DatParser::parseAllDats(const std::string& directory) {
    std::vector<Game> allGames;
    
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return allGames;
    }
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            std::cout << "Chargement du fichier DAT : " << entry.path().filename() << std::endl;
            auto games = parse(entry.path().string());
            allGames.insert(allGames.end(), games.begin(), games.end());
        }
    }
    
    std::cout << "Total de jeux chargés : " << allGames.size() << std::endl;
    return allGames;
}

bool DatParser::parseToDatabase(const std::string& filepath, std::shared_ptr<DatabaseManager> db) {
    pugi::xml_document doc;
    pugi::xml_parse_result result = doc.load_file(filepath.c_str());

    if (!result) {
        std::cerr << "Erreur : impossible de charger " << filepath << " (" << result.description() << ")" << std::endl;
        return false;
    }

    auto games_node = doc.child("datafile");
    if (!games_node) {
        std::cerr << "Erreur : pas de <datafile> dans " << filepath << std::endl;
        return false;
    }

    // Extract system name from header once
    std::string system = "Unknown";
    auto header = games_node.child("header");
    if (header) {
        std::string headerName = header.child("name").text().get();
        system = extractSystemFromHeader(headerName);
    }
    
    // Get filename for dat_source
    std::string filename = std::filesystem::path(filepath).filename().string();

    int games_count = 0;
    for (auto game_node : games_node.children("game")) {
        Game game;
        game.name = game_node.attribute("name").value();
        game.description = game_node.child("description").text().get();
        game.year = game_node.child("year").text().get();
        game.manufacturer = game_node.child("manufacturer").text().get();
        game.system = system;
        
        // Additional game attributes
        game.cloneof = game_node.attribute("cloneof").value();
        game.romof = game_node.attribute("romof").value();
        game.sourcefile = game_node.attribute("sourcefile").value();
        game.comment = game_node.child("comment").text().get();

        // Video information
        auto video_node = game_node.child("video");
        if (video_node) {
            game.video_type = video_node.attribute("type").value();
            game.orientation = video_node.attribute("orientation").value();
            game.width = video_node.attribute("width").value();
            game.height = video_node.attribute("height").value();
            game.aspect_x = video_node.attribute("aspectx").value();
            game.aspect_y = video_node.attribute("aspecty").value();
        }
        
        // Driver information
        auto driver_node = game_node.child("driver");
        if (driver_node) {
            game.driver_status = driver_node.attribute("status").value();
        }

        // Parse ROMs
        for (auto rom_node : game_node.children("rom")) {
            Rom rom;
            rom.name = rom_node.attribute("name").value();
            rom.size = rom_node.attribute("size").as_ullong();
            rom.crc = rom_node.attribute("crc").value();
            game.roms.push_back(rom);
        }

        game.status = "missing";  // Default status
        game.dat_source = filename;  // Set the source DAT file
        
        if (!db->insertGame(game)) {
            std::cerr << "Erreur insertion jeu: " << game.name << std::endl;
            return false;
        }
        
        games_count++;
    }
    
    // Register the DAT file in the database
    auto ftime = std::filesystem::last_write_time(std::filesystem::path(filepath));
    time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
    size_t file_size = std::filesystem::file_size(filepath);
    db->registerDatFile(filename, filepath, last_modified, file_size, games_count);
    
    std::cout << "Imported " << games_count << " games from " << filepath << std::endl;
    return true;
}

bool DatParser::parseAllDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db) {
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return false;
    }
    
    // Clear existing data
    if (!db->clearAllData()) {
        std::cerr << "Erreur vidage base de données" << std::endl;
        return false;
    }
    
    int total_games = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            std::cout << "Chargement du fichier DAT : " << entry.path().filename() << std::endl;
            if (parseToDatabase(entry.path().string(), db)) {
                // Count games in this DAT for progress
                total_games += 1; // We could count actual games if needed
            } else {
                std::cerr << "Erreur chargement " << entry.path().filename() << std::endl;
                return false;
            }
        }
    }
    
    std::cout << "Tous les DAT ont été chargés dans la base de données" << std::endl;
    return true;
}

bool DatParser::synchronizeDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db) {
    if (!std::filesystem::exists(directory) || !std::filesystem::is_directory(directory)) {
        std::cerr << "Erreur : répertoire DAT non trouvé : " << directory << std::endl;
        return false;
    }
    
    std::cout << "[SYNC] Starting DAT ↔ DB synchronization..." << std::endl;
    
    // 1. Collecter tous les fichiers DAT existants
    std::vector<std::string> existing_dat_files;
    std::vector<std::string> outdated_files;
    
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (entry.is_regular_file() && entry.path().extension() == ".dat") {
            std::string filename = entry.path().filename().string();
            existing_dat_files.push_back(filename);
            
            auto ftime = std::filesystem::last_write_time(entry);
            time_t last_modified = std::chrono::duration_cast<std::chrono::seconds>(ftime.time_since_epoch()).count();
            size_t file_size = std::filesystem::file_size(entry);
            
            if (!db->isDatFileUpToDate(filename, last_modified, file_size)) {
                outdated_files.push_back(entry.path().string());
                std::cout << "[SYNC] DAT modifié détecté: " << filename << std::endl;
            }
        }
    }
    
    // 2. Supprimer les DAT qui n'existent plus sur le disque
    if (!db->removeUnreferencedDatFiles(existing_dat_files)) {
        std::cerr << "[SYNC] Erreur nettoyage DAT obsolètes" << std::endl;
    }
    
    // 3. Supprimer les jeux des DAT modifiés/supprimés
    for (const auto& filepath : outdated_files) {
        std::string filename = std::filesystem::path(filepath).filename().string();
        std::cout << "[SYNC] Suppression des jeux du DAT: " << filename << std::endl;
        db->removeGamesFromDat(filename);
    }
    
    // 4. Recharger les DAT modifiés
    int updated_count = 0;
    for (const auto& filepath : outdated_files) {
        std::cout << "[SYNC] Rechargement: " << std::filesystem::path(filepath).filename() << std::endl;
        if (parseToDatabase(filepath, db)) {
            updated_count++;
        } else {
            std::cerr << "[SYNC] Erreur rechargement: " << filepath << std::endl;
        }
    }
    
    if (outdated_files.empty()) {
        std::cout << "[SYNC] ✅ No synchronization needed - DB up to date" << std::endl;
    } else {
        std::cout << "[SYNC] ✅ Synchronization completed: " << updated_count << "/" << outdated_files.size() << " DAT files reloaded" << std::endl;
    }
    
    return true;
}

std::string DatParser::extractSystemFromHeader(const std::string& headerName) {
    // Extract system name from header format: "FinalBurn Neo - System Games"
    size_t dashPos = headerName.find(" - ");
    if (dashPos != std::string::npos) {
        std::string systemPart = headerName.substr(dashPos + 3);
        // Remove " Games" suffix if present
        size_t gamesPos = systemPart.find(" Games");
        if (gamesPos != std::string::npos) {
            systemPart = systemPart.substr(0, gamesPos);
        }
        return systemPart;
    }
    return "Unknown";
};