// src/GenerateDAT.cpp
#include "GenerateDAT.h"
#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

void GenerateDAT::execute(Gtk::Window& parent, const std::string& fbneo_executable, Gtk::Entry* dat_entry) {
    if (fbneo_executable.empty()) {
        Gtk::MessageDialog dialog(parent, "FBNeo Executable Missing", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
        dialog.set_secondary_text("Please configure the FBNeo executable path first.");
        dialog.run();
        return;
    }
    
    // Create support/lists/dat directory
    std::string dat_output_dir = "./support/lists/dat";
    try {
        std::filesystem::create_directories(dat_output_dir);
    } catch (const std::exception& e) {
        Gtk::MessageDialog dialog(parent, "Directory Creation Failed", false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK);
        dialog.set_secondary_text("Failed to create directory: " + dat_output_dir + "\n\nError: " + std::string(e.what()));
        dialog.run();
        return;
    }
    
    // Show progress dialog with better styling
    auto progress_dialog = Gtk::Dialog("⚙️ Generating DAT Files", parent, true);
    progress_dialog.set_size_request(500, 180);
    progress_dialog.set_resizable(false);
    progress_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    
    auto content_area = progress_dialog.get_content_area();
    auto main_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 20);
    main_box->set_margin_start(25);
    main_box->set_margin_end(25);
    main_box->set_margin_top(25);
    main_box->set_margin_bottom(25);
    
    auto message_label = Gtk::make_managed<Gtk::Label>();
    message_label->set_markup("<span size='large' weight='bold'>⚙️  Generating DAT files from FBNeo...</span>");
    message_label->set_halign(Gtk::ALIGN_CENTER);
    main_box->pack_start(*message_label, Gtk::PACK_SHRINK);
    
    auto progress_bar = Gtk::make_managed<Gtk::ProgressBar>();
    progress_bar->set_size_request(-1, 25);
    progress_bar->pulse();
    main_box->pack_start(*progress_bar, Gtk::PACK_SHRINK);
    
    content_area->pack_start(*main_box, Gtk::PACK_EXPAND_WIDGET);
    progress_dialog.show_all();
    
    // Execute fbneo -dat command
    std::string command = "\"" + fbneo_executable + "\" -dat";
    int result = std::system(command.c_str());
    
    progress_dialog.hide();
    
    if (result == 0) {
        show_success_dialog(parent, dat_output_dir, dat_entry);
    } else {
        auto error_dialog = Gtk::Dialog("❌ DAT Generation Failed", parent, true);
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
        error_label->set_markup("<span size='large' weight='bold' color='#ff6b6b'>❌ DAT Generation Failed</span>\n\n<span>Failed to generate DAT files.\n\nMake sure the FBNeo executable is valid and accessible.</span>");
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
    }
}

void GenerateDAT::show_success_dialog(Gtk::Window& parent, const std::string& dat_path, Gtk::Entry* dat_entry) {
    // Show custom success dialog with better styling
    auto success_dialog = Gtk::Dialog("✅ DAT Generation Complete", parent, true);
    success_dialog.set_size_request(500, 220);
    success_dialog.set_resizable(false);
    success_dialog.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);
    
    auto success_content = success_dialog.get_content_area();
    auto success_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 20);
    success_box->set_margin_start(25);
    success_box->set_margin_end(25);
    success_box->set_margin_top(25);
    success_box->set_margin_bottom(25);
    
    auto success_label = Gtk::make_managed<Gtk::Label>();
    success_label->set_markup("<span size='large' weight='bold' color='#51cf66'>✅  DAT files have been generated successfully!</span>");
    success_label->set_line_wrap(true);
    success_label->set_halign(Gtk::ALIGN_CENTER);
    success_box->pack_start(*success_label, Gtk::PACK_SHRINK);
    
    // Button area with better styling
    auto button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 15);
    button_box->set_halign(Gtk::ALIGN_CENTER);
    button_box->set_margin_top(10);
    
    // Set DAT Path button with better styling
    auto set_button = Gtk::make_managed<Gtk::Button>();
    auto set_icon = Gdk::Pixbuf::create_from_file("assets/icons/folder-browse.svg", 18, 18);
    auto set_image = Gtk::make_managed<Gtk::Image>(set_icon);
    set_button->set_image(*set_image);
    set_button->set_label("Set as DAT Path");
    set_button->set_always_show_image(true);
    set_button->set_size_request(150, 35);
    
    auto ok_button = Gtk::make_managed<Gtk::Button>("OK");
    ok_button->set_size_request(80, 35);
    
    button_box->pack_start(*set_button, Gtk::PACK_SHRINK);
    button_box->pack_start(*ok_button, Gtk::PACK_SHRINK);
    success_box->pack_end(*button_box, Gtk::PACK_SHRINK);
    
    success_content->pack_start(*success_box, Gtk::PACK_EXPAND_WIDGET);
    
    bool set_path = false;
    set_button->signal_clicked().connect([&success_dialog, &set_path]() {
        set_path = true;
        success_dialog.response(Gtk::RESPONSE_OK);
    });
    
    ok_button->signal_clicked().connect([&success_dialog]() {
        success_dialog.response(Gtk::RESPONSE_CANCEL);
    });
    
    success_dialog.show_all();
    success_dialog.run();
    
    if (set_path && dat_entry) {
        // Update DAT entry
        dat_entry->set_text(dat_path);
        
        // Save to config
        std::ofstream config("config.json");
        if (config.is_open()) {
            nlohmann::json j;
            j["dat_path"] = dat_path;
            config << j.dump(4);
            config.close();
        }
        
        // Custom confirmation dialog with better styling
        auto confirm = Gtk::Dialog("✅ Path Updated", parent, true);
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
        confirm_label->set_markup("<span size='large' weight='bold' color='#51cf66'>✅ DAT path has been set to:</span>\n\n<span style='italic'>" + dat_path + "</span>\n\n<span weight='bold'>Settings saved successfully!</span>");
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
}