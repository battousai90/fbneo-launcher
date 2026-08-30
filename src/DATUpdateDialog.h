// src/DATUpdateDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include <memory>
#include <thread>
#include <mutex>
#include <atomic>
#include "DatabaseManager.h"

class DATUpdateDialog : public Gtk::Dialog {
public:
    DATUpdateDialog(Gtk::Window& parent, std::shared_ptr<DatabaseManager> db, const std::string& dat_path);
    virtual ~DATUpdateDialog();

    void start_update();
    bool was_cancelled() const { return m_cancelled.load(); }

private:
    void on_cancel_clicked();
    void update_progress(double percentage, const std::string& current_file, const std::string& message);
    void add_log_message(const std::string& message);
    void on_update_complete();

    // Threading
    void worker_thread();
    void on_progress_update();
    void on_update_finished();

    std::shared_ptr<DatabaseManager> m_db;
    std::string m_dat_path;
    std::atomic<bool> m_cancelled{false};
    
    // UI Components
    Gtk::Box m_main_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label m_title_label;
    
    // Progress section
    Gtk::Box m_progress_box{Gtk::ORIENTATION_VERTICAL, 5};
    Gtk::Label m_current_file_label;
    Gtk::ProgressBar m_progress_bar;
    Gtk::Label m_percentage_label;
    
    // Log section
    Gtk::Label m_log_title{"Details:"};
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
    
    // Shared data : writes from worker thread, reads from main thread.
    // m_shared_mutex protects m_current_file, m_current_message and m_log_messages.
    // m_current_progress and m_update_finished are atomics for lock-free access.
    mutable std::mutex m_shared_mutex;
    std::atomic<double> m_current_progress{0.0};
    std::string m_current_file;
    std::string m_current_message;
    std::vector<std::string> m_log_messages;
    std::atomic<bool> m_update_finished{false};
};