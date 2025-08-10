// src/RomScanner.cpp
#include "Game.h"
#include <filesystem>
#include <iomanip>

class RomScanner {
public:
    static void check_availability(Game& game, const std::string& roms_path) {
        bool all_present = true;
        bool all_correct = true;

        for (const auto& rom : game.roms) {
            std::string rom_path = roms_path + "/" + rom.name;
            if (!std::filesystem::exists(rom_path)) {
                all_present = false;
                break;
            }

            // Vérifie la taille
            size_t file_size = std::filesystem::file_size(rom_path);
            if (file_size != rom.size) {
                all_correct = false;
                continue;
            }

            // TODO: Vérifier le CRC (optionnel, plus lent)
            // Pour l'instant, on suppose que la taille = bon CRC
        }

        if (!all_present) {
            game.status = "missing";
        } else if (!all_correct) {
            game.status = "incorrect";
        } else {
            game.status = "available";
        }
    }
};