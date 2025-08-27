// src/ROMScanDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include "DatabaseManager.h"

class ROMScanDialog : public Gtk::Dialog {
public:
    ROMScanDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::vector<std::string>& roms_paths);
    virtual ~ROMScanDialog();
    
    void start_scan();
    bool was_cancelled() const { return m_cancelled; }
    int get_found_count() const { return m_found_count; }

private:
    void on_cancel_clicked();
    void update_progress(double percentage, const std::string& current_file, const std::string& message);
    void add_log_message(const std::string& message);
    void on_scan_complete();
    
    // Threading
    void worker_thread();
    void on_progress_update();
    void on_scan_finished();
    
    std::shared_ptr<DatabaseManager> m_db;
    std::vector<std::string> m_roms_paths;
    bool m_cancelled;
    int m_found_count;
    
    // UI Components
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label m_title_label;
    
    // Progress section
    Gtk::Box m_progress_box{Gtk::ORIENTATION_VERTICAL, 5};
    Gtk::Label m_current_file_label;
    Gtk::ProgressBar m_progress_bar;
    Gtk::Label m_percentage_label;
    
    // Log section
    Gtk::Label m_log_title{"Logs:"};
    Gtk::ScrolledWindow m_log_scrolled;
    Gtk::TextView m_log_view;
    Glib::RefPtr<Gtk::TextBuffer> m_log_buffer;
    
    // Buttons
    Gtk::ButtonBox m_button_box{Gtk::ORIENTATION_HORIZONTAL};
    Gtk::Button m_cancel_button{"Cancel"};
    Gtk::Button m_close_button{"Close"};
    
    // Threading
    std::thread m_worker_thread;
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_finished_dispatcher;
    
    // Shared data (protected by dispatcher)
    double m_current_progress;
    std::string m_current_file;
    std::string m_current_message;
    std::vector<std::string> m_log_messages;
    bool m_scan_finished;
};