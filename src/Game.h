// src/Game.h
#pragma once
#include <string>
#include <vector>

struct Rom {
    std::string name;
    size_t size;
    std::string crc;
};

struct Game {
    std::string name;           // nom interne (ex: sf2)
    std::string description;    // titre affiché
    std::string year;
    std::string manufacturer;
    std::vector<Rom> roms;
    std::string status = "unknown";  // "available", "incorrect", "missing"

    bool is_available() const { return status == "available"; }
};