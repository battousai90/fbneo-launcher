// src/SplashScreen.h
#pragma once

#include <gtkmm.h>
#include <string>

class SplashScreen : public Gtk::Window {
public:
    SplashScreen();
    virtual ~SplashScreen();
    
    void set_progress(double progress, const std::string& message);
    void show_splash();
    void hide_splash();
    
private:
    Gtk::Box m_root{Gtk::ORIENTATION_VERTICAL, 0}; // painted panel behind everything
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL, 16};
    Gtk::Image m_logo;
    Gtk::Label m_title_label;
    Gtk::Label m_subtitle_label;
    Gtk::Label m_status_label;
    Gtk::ProgressBar m_progress_bar;
    Gtk::Box m_content_box{Gtk::ORIENTATION_VERTICAL, 8};
    
    void setup_ui();
};