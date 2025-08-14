// src/ScanProgressDialog.h
#pragma once

#include <gtkmm.h>
#include <atomic>
#include "Game.h"

class ScanProgressDialog : public Gtk::Dialog {
public:
    ScanProgressDialog(Gtk::Window& parent);
    virtual ~ScanProgressDialog();

    void start_scan(const std::vector<Game>& games, const std::string& roms_path);
    bool is_cancelled() const { return m_cancel_requested; }
    const std::vector<Game>& get_scanned_games() const { return m_scanned_games; }

private:
    void on_cancel_clicked();
    void update_progress(int current, int total, const std::string& game_name);
    
    // UI components
    Gtk::Box m_content_box{Gtk::ORIENTATION_VERTICAL};
    Gtk::Label m_status_label;
    Gtk::ProgressBar m_progress_bar;
    Gtk::Label m_details_label;
    Gtk::Button m_cancel_button{"Cancel"};
    
    // Scan state
    std::atomic<bool> m_cancel_requested{false};
    std::vector<Game> m_scanned_games;
};