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
    std::string system;  // System type extracted from DAT header
    std::vector<Rom> roms;
    std::string status = "missing";  // "available", "missing", "incorrect", "incomplete"
    
    // Video information
    std::string video_type = "";        // "raster", "vector", etc.
    std::string orientation = "";       // "horizontal", "vertical"
    std::string width = "";
    std::string height = "";
    std::string aspect_x = "";
    std::string aspect_y = "";
    
    // Driver information
    std::string driver_status = "";     // "good", "preliminary", "nodump"
    
    // Additional information
    std::string comment = "";
    std::string cloneof = "";
    std::string romof = "";
    std::string sourcefile = "";

    bool is_available() const { return status == "available"; }
};