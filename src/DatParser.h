// src/DatParser.h
#pragma once
#include <vector>
#include "Game.h"

class DatParser {
public:
    static std::vector<Game> parse(const std::string& filepath);
    static std::vector<Game> parseAllDats(const std::string& directory);
private:
    static std::string extractSystemFromHeader(const std::string& headerName);
};