// src/DatParser.h
#pragma once
#include <vector>
#include <memory>
#include "Game.h"
#include "DatabaseManager.h"

class DatParser {
public:
    static std::vector<Game> parse(const std::string& filepath);
    static std::vector<Game> parseAllDats(const std::string& directory);
    
    // New database-based methods
    static bool parseToDatabase(const std::string& filepath, std::shared_ptr<DatabaseManager> db);
    static bool parseAllDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
    static bool synchronizeDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
private:
    static std::string extractSystemFromHeader(const std::string& headerName);
};