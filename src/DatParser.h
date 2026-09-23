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
    static int parseToDatabase(const std::string& filepath, std::shared_ptr<DatabaseManager> db);
    static bool parseAllDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
    static bool synchronizeDatsToDatabase(const std::string& directory, std::shared_ptr<DatabaseManager> db);
    // L'emulateur dont vient un DAT, deduit de son en-tete.
    //
    // Un fichier DAT ne porte aucune autre marque d'origine : ni le nom du
    // fichier (l'utilisateur le renomme), ni le dossier (les deux emulateurs
    // peuvent partager le meme) ne sont fiables. L'en-tete <header><name>,
    // lui, est ecrit par le producteur du DAT et voyage avec le fichier.
    // Rend "mame" ou "fbneo" ; un en-tete inconnu vaut "fbneo", le seul
    // catalogue qui existait avant.
    static std::string emulatorFromHeader(const std::string& headerName);

private:
    static std::string extractSystemFromHeader(const std::string& headerName);
};