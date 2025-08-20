// src/SplashScreen.cpp
#include "SplashScreen.h"
#include <iostream>

SplashScreen::SplashScreen() {
    setup_ui();
}

SplashScreen::~SplashScreen() = default;

void SplashScreen::setup_ui() {
    // Configuration de la fenêtre
    set_title("FBNeo Launcher - Loading...");
    set_default_size(400, 300);
    set_position(Gtk::WIN_POS_CENTER);
    set_resizable(false);
    set_decorated(false);  // Sans bordures pour un look splash screen
    set_modal(true);
    set_type_hint(Gdk::WINDOW_TYPE_HINT_SPLASHSCREEN);
    
    // Style de la fenêtre
    set_border_width(20);
    get_style_context()->add_class("splash-screen");
    
    // Configuration du contenu principal
    m_main_box.set_halign(Gtk::ALIGN_CENTER);
    m_main_box.set_valign(Gtk::ALIGN_CENTER);
    m_main_box.set_spacing(20);
    
    // Logo/Icône (utilise une icône par défaut si pas d'image disponible)
    try {
        auto pixbuf = Gdk::Pixbuf::create_from_file("assets/icons/fbneo-logo.png", 64, 64);
        m_logo.set(pixbuf);
    } catch (...) {
        // Fallback: utilise une icône système
        m_logo.set_from_icon_name("applications-games", Gtk::ICON_SIZE_DIALOG);
    }
    m_logo.set_halign(Gtk::ALIGN_CENTER);
    
    // Titre
    m_title_label.set_markup("<span size='xx-large' weight='bold'>FBNeo Launcher</span>");
    m_title_label.set_halign(Gtk::ALIGN_CENTER);
    m_title_label.get_style_context()->add_class("title");
    
    // Configuration de la box de contenu
    m_content_box.set_halign(Gtk::ALIGN_CENTER);
    m_content_box.set_spacing(10);
    
    // Label de status
    m_status_label.set_text("Initializing...");
    m_status_label.set_halign(Gtk::ALIGN_CENTER);
    m_status_label.get_style_context()->add_class("status");
    
    // Barre de progression
    m_progress_bar.set_size_request(300, 20);
    m_progress_bar.set_halign(Gtk::ALIGN_CENTER);
    m_progress_bar.set_fraction(0.0);
    m_progress_bar.set_show_text(true);
    m_progress_bar.get_style_context()->add_class("progress");
    
    // Assemblage des widgets
    m_content_box.pack_start(m_status_label, Gtk::PACK_SHRINK);
    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    
    m_main_box.pack_start(m_logo, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_title_label, Gtk::PACK_SHRINK);
    m_main_box.pack_start(m_content_box, Gtk::PACK_EXPAND_WIDGET);
    
    add(m_main_box);
    
    // Style CSS personnalisé
    auto css = Gtk::CssProvider::create();
    css->load_from_data(R"(
        .splash-screen {
            background: linear-gradient(135deg, #667eea 0%, #764ba2 100%);
            border-radius: 10px;
            border: 2px solid #4a5568;
        }
        
        .title {
            color: white;
            text-shadow: 2px 2px 4px rgba(0,0,0,0.5);
        }
        
        .status {
            color: #e2e8f0;
            font-size: 14px;
        }
        
        .progress {
            border-radius: 10px;
        }
    )");
    
    auto screen = Gdk::Screen::get_default();
    Gtk::StyleContext::add_provider_for_screen(screen, css, GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
    
    show_all();
}

void SplashScreen::set_progress(double progress, const std::string& message) {
    // Assure-toi que progress est entre 0.0 et 1.0
    progress = std::max(0.0, std::min(1.0, progress));
    
    m_progress_bar.set_fraction(progress);
    m_progress_bar.set_text(std::to_string(static_cast<int>(progress * 100)) + "%");
    m_status_label.set_text(message);
    
    // Force la mise à jour de l'affichage
    while (Gtk::Main::events_pending()) {
        Gtk::Main::iteration();
    }
    
    std::cout << "[SPLASH] Progress: " << static_cast<int>(progress * 100) << "% - " << message << std::endl;
}

void SplashScreen::show_splash() {
    show();
    present();
    
    // Force l'affichage immédiat
    while (Gtk::Main::events_pending()) {
        Gtk::Main::iteration();
    }
    
    std::cout << "[SPLASH] Splash screen shown" << std::endl;
}

void SplashScreen::hide_splash() {
    hide();
    std::cout << "[SPLASH] Splash screen hidden" << std::endl;
}