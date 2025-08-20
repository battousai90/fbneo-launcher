// src/DownloadDialog.h
#pragma once

#include <gtkmm.h>
#include <string>
#include <thread>
#include <atomic>
#include <functional>

class DownloadDialog : public Gtk::Dialog {
public:
    DownloadDialog(Gtk::Window& parent, const std::string& url, const std::string& destination);
    virtual ~DownloadDialog();

    // Set settings entry to update automatically on success
    void set_settings_entry(Gtk::Entry* entry);
    
    void start_download();
    void update_progress(double progress, const std::string& status);

private:
    void on_cancel_clicked();
    void download_complete(bool success, const std::string& message);
    void download_worker();
    bool extract_zip(const std::string& zip_path, const std::string& extract_path);
    
    // UI Elements
    Gtk::Box m_content_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label m_status_label{"Preparing download..."};
    Gtk::ProgressBar m_progress_bar;
    Gtk::Label m_progress_label{"0%"};
    Gtk::Button m_cancel_button{"Cancel"};
    
    // Download parameters
    std::string m_url;
    std::string m_destination;
    std::string m_temp_file;
    
    // Threading
    std::thread m_download_thread;
    std::atomic<bool> m_cancel_requested{false};
    
    // Settings entry to update
    Gtk::Entry* m_settings_entry;
    
    // Progress updates (thread-safe via dispatcher)
    Glib::Dispatcher m_progress_dispatcher;
    Glib::Dispatcher m_complete_dispatcher;
    
    // Shared data for thread communication
    struct {
        std::atomic<double> progress{0.0};
        std::atomic<bool> success{false};
        std::string status_text;
        std::string final_message;
        std::string extracted_path;
    } m_shared_data;
};