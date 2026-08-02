// src/main.cpp
#include "MainWindow.h"
#include "SplashScreen.h"
#include "DatabaseManager.h"
#include "AppContext.h"
#include "i18n.h"
#include <gtkmm.h>
#include <thread>
#include <chrono>
#include <iostream>
#include <fstream>
#include <memory>
#include <nlohmann/json.hpp>

int main(int argc, char *argv[]) {
    // Redirect stderr to a log file for debugging
    std::string log_path = AppContext::get_user_config_dir() + "/debug.log";
    static std::ofstream debug_log(log_path, std::ios::app);
    std::cerr.rdbuf(debug_log.rdbuf());
    std::cerr << "\n=== Application started at " << std::time(nullptr) << " ===" << std::endl;

    auto app = Gtk::Application::create(argc, argv, "org.gilbert.fbneo-launcher");

    // Initialize translations before any UI string is built. Use the language saved
    // in settings if any; otherwise auto-detect the system language (English fallback).
    std::string ui_lang;
    try {
        std::ifstream cfg(AppContext::get_config_path());
        if (cfg) {
            nlohmann::json j; cfg >> j;
            if (j.contains("language")) ui_lang = j["language"].get<std::string>();
        }
    } catch (...) {}
    i18n::init(AppContext::get_locale_dir(), ui_lang);

    // Créer et afficher le splash screen
    SplashScreen splash;
    splash.show_splash();
    
    // Initialisation
    splash.set_progress(0.1, "Initializing application...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    splash.set_progress(0.2, "Loading configuration...");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // === Initialize Database ===
    splash.set_progress(0.3, "Connecting to database...");
    std::string db_path = AppContext::get_user_config_dir() + "/games.db";
    
    std::cout << "[DEBUG] Using database: " << db_path << std::endl;
    auto database = std::make_shared<DatabaseManager>(db_path);
    if (!database->initialize()) {
        std::cerr << "[ERROR] Failed to initialize database" << std::endl;
        splash.set_progress(1.0, "Database error - continuing anyway...");
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }

    // === Load Games from Database ===
    splash.set_progress(0.4, "Loading game database...");
    std::vector<Game> preloaded_games;
    try {
        preloaded_games = database->getAllGames();
        
        if (preloaded_games.empty()) {
            std::cout << "[INFO] Database is empty - will show empty interface" << std::endl;
            splash.set_progress(0.7, "Database is empty...");
        } else {
            std::cout << "[INFO] Loaded " << preloaded_games.size() << " games from database" << std::endl;
            splash.set_progress(0.7, "Loaded " + std::to_string(preloaded_games.size()) + " games...");
        }
    } catch (const std::exception& e) {
        std::cerr << "[ERROR] Failed to load games: " << e.what() << std::endl;
        splash.set_progress(0.7, "Failed to load games - continuing...");
    }
    
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // === Create Main Window ===
    splash.set_progress(0.8, "Setting up interface...");
    
    // Créer la fenêtre principale avec callback de progression et jeux préchargés
    MainWindow window(database, [&splash](double progress, const std::string& message) {
        splash.set_progress(progress, message);
    }, preloaded_games);
    
    // Finalisation
    splash.set_progress(1.0, "Ready!");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Masquer le splash et afficher la fenêtre principale
    splash.hide_splash();
    
    return app->run(window);
}