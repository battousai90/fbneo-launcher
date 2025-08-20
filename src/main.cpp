// src/main.cpp
#include "MainWindow.h"
#include "SplashScreen.h"
#include <gtkmm.h>
#include <thread>
#include <chrono>

int main(int argc, char *argv[]) {
    auto app = Gtk::Application::create(argc, argv, "org.gilbert.fbneo-launcher");

    // Créer et afficher le splash screen
    SplashScreen splash;
    splash.show_splash();
    
    // Simuler les étapes de chargement avec progression
    splash.set_progress(0.1, "Initializing application...");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    splash.set_progress(0.3, "Loading configuration...");
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    splash.set_progress(0.5, "Setting up interface...");
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Créer la fenêtre principale avec callback de progression
    MainWindow window([&splash](double progress, const std::string& message) {
        splash.set_progress(progress, message);
    });
    
    // Masquer le splash et afficher la fenêtre principale
    splash.hide_splash();
    
    return app->run(window);
}