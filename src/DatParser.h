// src/DatParser.h
#pragma once
#include <vector>
#include "Game.h"

class DatParser {
public:
    static std::vector<Game> parse(const std::string& filepath);
};