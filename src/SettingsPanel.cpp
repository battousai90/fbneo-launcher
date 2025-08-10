// src/SettingsPanel.cpp
#include "SettingsPanel.h"
#include <gtkmm/filechooserdialog.h>
#include <fstream>
#include <nlohmann/json.hpp>

SettingsPanel::~SettingsPanel() = default;

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 10) {
    m_box.set_margin_start(10);
    m_box.set_margin_end(10);
    m_box.set_margin_top(10);

    // --- ROMs ---
    m_box.pack_start(m_label_roms, Gtk::PACK_SHRINK);
    m_box.pack_start(m_entry_roms);
    m_box.pack_start(m_button_browse_roms, Gtk::PACK_SHRINK);
    m_button_browse_roms.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_roms);
    });

    // --- DAT ---
    m_box.pack_start(m_label_dat, Gtk::PACK_SHRINK);
    m_box.pack_start(m_entry_dat);
    m_box.pack_start(m_button_browse_dat, Gtk::PACK_SHRINK);
    m_button_browse_dat.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog("Select DAT File", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Open", Gtk::RESPONSE_OK);
        dialog.set_current_folder(Glib::path_get_dirname(m_entry_dat.get_text()));

        auto filter = Gtk::FileFilter::create();
        filter->set_name("DAT files");
        filter->add_pattern("*.dat");
        dialog.add_filter(filter);

        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_entry_dat.set_text(dialog.get_filename());
        }
    });

    // --- Thumbnails ---
    m_box.pack_start(m_label_thumbs, Gtk::PACK_SHRINK);
    m_box.pack_start(m_entry_thumbs);
    m_box.pack_start(m_button_browse_thumbs, Gtk::PACK_SHRINK);
    m_button_browse_thumbs.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_thumbs);
    });

        // --- FBNeo Executable ---
    m_box.pack_start(m_label_fbneo, Gtk::PACK_SHRINK);
    m_box.pack_start(m_entry_fbneo);
    m_box.pack_start(m_button_browse_fbneo, Gtk::PACK_SHRINK);
    m_button_browse_fbneo.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog("Select FBNeo Executable", Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
        dialog.add_button("Select", Gtk::RESPONSE_OK);

        auto filter = Gtk::FileFilter::create();
        filter->set_name("Executable");
        filter->add_pattern("*fbneo*");
        dialog.add_filter(filter);

        if (!m_entry_fbneo.get_text().empty()) {
            dialog.set_filename(m_entry_fbneo.get_text());
        }

        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_entry_fbneo.set_text(dialog.get_filename());
        }
    });

    add(m_box);
    show_all_children();
}

void SettingsPanel::on_folder_clicked(Gtk::Entry* entry) {
    auto dialog = Gtk::FileChooserDialog("Select Folder", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);

    auto current = entry->get_text();
    if (!current.empty()) {
        dialog.set_current_folder(Glib::path_get_dirname(current));
    }

    if (dialog.run() == Gtk::RESPONSE_OK) {
        entry->set_text(dialog.get_filename());
    }
}

// --- Getters ---
std::string SettingsPanel::get_roms_path() const { return m_entry_roms.get_text(); }
std::string SettingsPanel::get_dat_path() const { return m_entry_dat.get_text(); }
std::string SettingsPanel::get_thumbnails_path() const { return m_entry_thumbs.get_text(); }
std::string SettingsPanel::get_fbneo_executable() const { return m_entry_fbneo.get_text();}

// --- Setters ---
void SettingsPanel::set_roms_path(const std::string& path) { m_entry_roms.set_text(path); }
void SettingsPanel::set_dat_path(const std::string& path) { m_entry_dat.set_text(path); }
void SettingsPanel::set_thumbnails_path(const std::string& path) { m_entry_thumbs.set_text(path); }
void SettingsPanel::set_fbneo_executable(const std::string& path) { m_entry_fbneo.set_text(path);}

// --- Load / Save ---
bool SettingsPanel::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;

        if (j.contains("roms_path")) set_roms_path(j["roms_path"]);
        if (j.contains("dat_path")) set_dat_path(j["dat_path"]);
        if (j.contains("thumbnails_path")) set_thumbnails_path(j["thumbnails_path"]);
        if (j.contains("fbneo_executable")) set_fbneo_executable(j["fbneo_executable"]);
    } catch (...) {
        return false;
    }
    return true;
}

bool SettingsPanel::save_to_file(const std::string& filename) {
    nlohmann::json j;
    j["roms_path"] = get_roms_path();
    j["dat_path"] = get_dat_path();
    j["thumbnails_path"] = get_thumbnails_path();
    j["fbneo_executable"] = get_fbneo_executable();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4); // Formaté avec 4 espaces
    return true;
}