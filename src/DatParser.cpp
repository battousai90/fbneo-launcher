// src/DatParser.cpp
#include "Game.h"
#include "DatParser.h"
#include <pugixml.hpp>
#include <iostream>
#include <filesystem>
#include <algorithm>

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