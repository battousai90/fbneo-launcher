// src/Game.h
#pragma once
#include <string>
#include <vector>

struct Rom {
    std::string name;
    size_t size;
    std::string crc;  // CRC32 en hexadécimal
};

struct Game {
    std::string name;
    std::string description;
    std::string year;
    std::string manufacturer;
    std::vector<Rom> roms;
    std::string status = "unknown";  // "available", "missing", "incorrect", "incomplete"

    bool is_available() const { return status == "available"; }
};