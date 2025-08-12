// src/DatParser.cpp
#include "Game.h"
#include "DatParser.h"
#include <pugixml.hpp>
#include <iostream>
#include <filesystem>

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

        for (auto rom_node : game_node.children("rom")) {
            Rom rom;
            rom.name = rom_node.attribute("name").value();
            rom.size = rom_node.attribute("size").as_ullong();
            rom.crc = rom_node.attribute("crc").value();
            game.roms.push_back(rom);
        }

        game.status = "unknown";  // sera mis à jour par RomScanner
        games.push_back(game);
    }

    return games;
};