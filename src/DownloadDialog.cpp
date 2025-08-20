// src/DownloadDialog.cpp
#include "DownloadDialog.h"
#include <curl/curl.h>
#include <zip.h>
#include <fstream>
#include <filesystem>
#include <iostream>
#include <nlohmann/json.hpp>

namespace fs = std::filesystem;

// Callback function for curl to write data
size_t write_data(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    return fwrite(ptr, size, nmemb, stream);
}

// Callback function for curl progress
int progress_callback(void *clientp, curl_off_t dltotal, curl_off_t dlnow, curl_off_t ultotal, curl_off_t ulnow) {
    DownloadDialog* dialog = static_cast<DownloadDialog*>(clientp);
    if (dltotal > 0) {
        double progress = static_cast<double>(dlnow) / static_cast<double>(dltotal);
        dialog->update_progress(progress, "Downloading FBNeo...");
    }
    return 0; // Continue download
}

DownloadDialog::DownloadDialog(Gtk::Window& parent, const std::string& url, const std::string& destination)
    : Gtk::Dialog("📥 Download FBNeo", parent, true)
    , m_url(url)
    , m_destination(destination)
    , m_settings_entry(nullptr) {
    
    set_size_request(500, 180);
    set_resizable(false);
    set_modal(true);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    
    // Setup content area with better styling
    get_content_area()->pack_start(m_content_box, Gtk::PACK_EXPAND_WIDGET);
    m_content_box.set_spacing(15);
    m_content_box.set_margin_start(25);
    m_content_box.set_margin_end(25);
    m_content_box.set_margin_top(20);
    m_content_box.set_margin_bottom(20);
    
    // Status label with better styling
    m_status_label.set_halign(Gtk::ALIGN_START);
    m_status_label.set_markup("<span size='large' weight='bold'>🚀 Preparing download...</span>");
    m_content_box.pack_start(m_status_label, Gtk::PACK_SHRINK);
    
    // Progress bar with better appearance
    m_progress_bar.set_show_text(false);
    m_progress_bar.set_size_request(-1, 25);
    m_content_box.pack_start(m_progress_bar, Gtk::PACK_SHRINK);
    
    // Progress percentage label with styling
    m_progress_label.set_halign(Gtk::ALIGN_CENTER);
    m_progress_label.set_markup("<span size='large' weight='bold'>0%</span>");
    m_content_box.pack_start(m_progress_label, Gtk::PACK_SHRINK);
    
    // Cancel button with icon
    auto cancel_icon = Gdk::Pixbuf::create_from_file("assets/icons/cancel.svg", 16, 16);
    auto cancel_image = Gtk::make_managed<Gtk::Image>(cancel_icon);
    m_cancel_button.set_image(*cancel_image);
    m_cancel_button.set_label("Cancel");
    m_cancel_button.set_always_show_image(true);
    add_button("Cancel", Gtk::RESPONSE_CANCEL);
    m_cancel_button.signal_clicked().connect(sigc::mem_fun(*this, &DownloadDialog::on_cancel_clicked));
    
    // Setup dispatchers for thread communication
    m_progress_dispatcher.connect([this]() {
        m_progress_bar.set_fraction(m_shared_data.progress.load());
        int percentage = static_cast<int>(m_shared_data.progress.load() * 100);
        m_progress_label.set_markup("<span size='large' weight='bold'>" + std::to_string(percentage) + "%</span>");
        m_status_label.set_markup("<span size='large' weight='bold'>📥 " + m_shared_data.status_text + "</span>");
    });
    
    m_complete_dispatcher.connect([this]() {
        if (m_shared_data.success.load()) {
            std::string fbneo_path = "./fbneo";
            
            // Create custom success dialog
            auto success_dialog = Gtk::Dialog("✅ Download Complete", *this, true);
            success_dialog.set_size_request(500, 220);
            success_dialog.set_resizable(false);
            success_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
            
            auto content_area = success_dialog.get_content_area();
            auto main_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 20);
            main_box->set_margin_start(25);
            main_box->set_margin_end(25);
            main_box->set_margin_top(25);
            main_box->set_margin_bottom(25);
            
            // Success message with better styling
            auto message_label = Gtk::make_managed<Gtk::Label>();
            message_label->set_markup("<span size='large' weight='bold' color='#51cf66'>✅  FBNeo has been downloaded and extracted successfully!</span>");
            message_label->set_line_wrap(true);
            message_label->set_halign(Gtk::ALIGN_CENTER);
            main_box->pack_start(*message_label, Gtk::PACK_SHRINK);
            
            // Button area with better styling
            auto button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
            button_box->set_halign(Gtk::ALIGN_CENTER);
            button_box->set_margin_top(10);
            
            // Set Path button with icon and better styling
            auto set_button = Gtk::make_managed<Gtk::Button>();
            auto set_icon = Gdk::Pixbuf::create_from_file("assets/icons/executable-select.svg", 18, 18);
            auto set_image = Gtk::make_managed<Gtk::Image>(set_icon);
            set_button->set_image(*set_image);
            set_button->set_label("Set as FBNeo Path");
            set_button->set_always_show_image(true);
            set_button->set_size_request(160, 35);
            
            // OK button with better styling
            auto ok_button = Gtk::make_managed<Gtk::Button>("OK");
            ok_button->set_size_request(80, 35);
            
            button_box->pack_start(*set_button, Gtk::PACK_SHRINK);
            button_box->pack_start(*ok_button, Gtk::PACK_SHRINK);
            main_box->pack_end(*button_box, Gtk::PACK_SHRINK);
            
            content_area->pack_start(*main_box, Gtk::PACK_EXPAND_WIDGET);
            
            bool set_path = false;
            set_button->signal_clicked().connect([&success_dialog, &set_path]() {
                set_path = true;
                success_dialog.response(Gtk::RESPONSE_OK);
            });
            
            ok_button->signal_clicked().connect([&success_dialog]() {
                success_dialog.response(Gtk::RESPONSE_CANCEL);
            });
            
            success_dialog.show_all();
            int result = success_dialog.run();
            bool should_set = set_path;
            
            if (should_set && m_settings_entry) {
                // Update settings entry
                m_settings_entry->set_text(fbneo_path);
                
                // Save settings
                std::ofstream config("config.json");
                if (config.is_open()) {
                    nlohmann::json j;
                    j["fbneo_executable"] = fbneo_path;
                    config << j.dump(4);
                    config.close();
                }
                
                // Show custom confirmation dialog
                auto confirm = Gtk::Dialog("✅ Path Updated", *this, true);
                confirm.set_size_request(450, 180);
                confirm.set_resizable(false);
                confirm.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
                
                auto confirm_content = confirm.get_content_area();
                auto confirm_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 20);
                confirm_box->set_margin_start(25);
                confirm_box->set_margin_end(25);
                confirm_box->set_margin_top(25);
                confirm_box->set_margin_bottom(25);
                
                auto confirm_label = Gtk::make_managed<Gtk::Label>();
                confirm_label->set_markup("<span size='large' weight='bold' color='#51cf66'>✅ FBNeo executable path has been set to:</span>\n\n<span style='italic'>" + fbneo_path + "</span>\n\n<span weight='bold'>Settings saved successfully!</span>");
                confirm_label->set_line_wrap(true);
                confirm_label->set_halign(Gtk::ALIGN_CENTER);
                confirm_box->pack_start(*confirm_label, Gtk::PACK_EXPAND_WIDGET);
                
                auto confirm_ok = Gtk::make_managed<Gtk::Button>("OK");
                confirm_ok->set_size_request(80, 35);
                confirm_ok->set_halign(Gtk::ALIGN_CENTER);
                confirm_ok->signal_clicked().connect([&confirm]() {
                    confirm.response(Gtk::RESPONSE_OK);
                });
                confirm_box->pack_end(*confirm_ok, Gtk::PACK_SHRINK);
                
                confirm_content->pack_start(*confirm_box, Gtk::PACK_EXPAND_WIDGET);
                confirm.show_all();
                confirm.run();
            }
            
            response(Gtk::RESPONSE_OK);
        } else {
            auto error_dialog = Gtk::Dialog("❌ Download Failed", *this, true);
            error_dialog.set_size_request(450, 180);
            error_dialog.set_resizable(false);
            error_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
            
            auto error_content = error_dialog.get_content_area();
            auto error_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 20);
            error_box->set_margin_start(25);
            error_box->set_margin_end(25);
            error_box->set_margin_top(25);
            error_box->set_margin_bottom(25);
            
            auto error_label = Gtk::make_managed<Gtk::Label>();
            error_label->set_markup("<span size='large' weight='bold' color='#ff6b6b'>❌ Download Failed</span>\n\n<span>" + m_shared_data.final_message + "</span>");
            error_label->set_line_wrap(true);
            error_label->set_halign(Gtk::ALIGN_CENTER);
            error_box->pack_start(*error_label, Gtk::PACK_EXPAND_WIDGET);
            
            auto error_ok = Gtk::make_managed<Gtk::Button>("OK");
            error_ok->set_size_request(80, 35);
            error_ok->set_halign(Gtk::ALIGN_CENTER);
            error_ok->signal_clicked().connect([&error_dialog]() {
                error_dialog.response(Gtk::RESPONSE_OK);
            });
            error_box->pack_end(*error_ok, Gtk::PACK_SHRINK);
            
            error_content->pack_start(*error_box, Gtk::PACK_EXPAND_WIDGET);
            error_dialog.show_all();
            error_dialog.run();
            response(Gtk::RESPONSE_CANCEL);
        }
    });
    
    show_all();
}

DownloadDialog::~DownloadDialog() {
    m_cancel_requested = true;
    if (m_download_thread.joinable()) {
        m_download_thread.join();
    }
}

void DownloadDialog::set_settings_entry(Gtk::Entry* entry) {
    m_settings_entry = entry;
}

void DownloadDialog::start_download() {
    m_download_thread = std::thread(&DownloadDialog::download_worker, this);
}

void DownloadDialog::on_cancel_clicked() {
    m_cancel_requested = true;
    response(Gtk::RESPONSE_CANCEL);
}

void DownloadDialog::update_progress(double progress, const std::string& status) {
    m_shared_data.progress.store(progress);
    m_shared_data.status_text = status;
    m_progress_dispatcher.emit();
}

void DownloadDialog::download_worker() {
    CURL *curl;
    FILE *fp;
    CURLcode res;
    
    // Create temp file path
    m_temp_file = m_destination + "/fbneo_download.zip";
    
    // Ensure destination directory exists
    try {
        fs::create_directories(m_destination);
    } catch (const std::exception& e) {
        m_shared_data.success.store(false);
        m_shared_data.final_message = "Failed to create destination directory: " + std::string(e.what());
        m_complete_dispatcher.emit();
        return;
    }
    
    curl = curl_easy_init();
    if (!curl) {
        m_shared_data.success.store(false);
        m_shared_data.final_message = "Failed to initialize curl";
        m_complete_dispatcher.emit();
        return;
    }
    
    fp = fopen(m_temp_file.c_str(), "wb");
    if (!fp) {
        curl_easy_cleanup(curl);
        m_shared_data.success.store(false);
        m_shared_data.final_message = "Failed to create temporary file";
        m_complete_dispatcher.emit();
        return;
    }
    
    // Configure curl
    curl_easy_setopt(curl, CURLOPT_URL, m_url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_data);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "fbneo-launcher/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, progress_callback);
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, this);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 300L); // 5 minute timeout
    
    // Perform download
    res = curl_easy_perform(curl);
    fclose(fp);
    
    if (m_cancel_requested) {
        curl_easy_cleanup(curl);
        fs::remove(m_temp_file);
        return;
    }
    
    if (res != CURLE_OK) {
        curl_easy_cleanup(curl);
        fs::remove(m_temp_file);
        m_shared_data.success.store(false);
        m_shared_data.final_message = "Download failed: " + std::string(curl_easy_strerror(res));
        m_complete_dispatcher.emit();
        return;
    }
    
    curl_easy_cleanup(curl);
    
    // Update status for extraction
    update_progress(1.0, "Extracting archive...");
    
    // Extract ZIP file
    if (!extract_zip(m_temp_file, m_destination)) {
        fs::remove(m_temp_file);
        m_shared_data.success.store(false);
        m_shared_data.final_message = "Failed to extract ZIP archive";
        m_complete_dispatcher.emit();
        return;
    }
    
    // Clean up temp file
    fs::remove(m_temp_file);
    
    // Success
    m_shared_data.success.store(true);
    m_shared_data.extracted_path = m_destination;
    m_shared_data.final_message = "FBNeo downloaded and extracted successfully to: " + m_shared_data.extracted_path;
    m_complete_dispatcher.emit();
}

bool DownloadDialog::extract_zip(const std::string& zip_path, const std::string& extract_path) {
    int err = 0;
    zip_t *z = zip_open(zip_path.c_str(), 0, &err);
    
    if (z == nullptr) {
        return false;
    }
    
    zip_int64_t num_entries = zip_get_num_entries(z, 0);
    if (num_entries < 0) {
        zip_close(z);
        return false;
    }
    
    for (zip_int64_t i = 0; i < num_entries; i++) {
        if (m_cancel_requested) {
            zip_close(z);
            return false;
        }
        
        const char* name = zip_get_name(z, i, 0);
        if (name == nullptr) {
            continue;
        }
        
        // Create full path
        std::string full_path = extract_path + "/" + name;
        
        // Create directories if needed
        if (name[strlen(name) - 1] == '/') {
            try {
                fs::create_directories(full_path);
            } catch (...) {
                // Ignore directory creation errors
            }
            continue;
        }
        
        // Create parent directories
        try {
            fs::create_directories(fs::path(full_path).parent_path());
        } catch (...) {
            // Ignore directory creation errors
        }
        
        // Extract file
        zip_file_t *f = zip_fopen_index(z, i, 0);
        if (f == nullptr) {
            continue;
        }
        
        std::ofstream outfile(full_path, std::ios::binary);
        if (!outfile) {
            zip_fclose(f);
            continue;
        }
        
        char buffer[8192];
        zip_int64_t bytes_read;
        while ((bytes_read = zip_fread(f, buffer, sizeof(buffer))) > 0) {
            outfile.write(buffer, bytes_read);
        }
        
        outfile.close();
        zip_fclose(f);
        
        // Make executable if it's the fbneo binary
        if (name == std::string("fbneo") || 
            (full_path.find("fbneo") != std::string::npos && 
             full_path.find(".exe") == std::string::npos)) {
            try {
                fs::permissions(full_path, fs::perms::owner_all | fs::perms::group_read | fs::perms::others_read);
            } catch (...) {
                // Ignore permission errors
            }
        }
    }
    
    zip_close(z);
    return true;
}