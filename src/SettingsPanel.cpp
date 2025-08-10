// src/SettingsPanel.cpp
#include "SettingsPanel.h"
#include <gtkmm/filechooserdialog.h>
#include <fstream>
#include <nlohmann/json.hpp>  // On va installer cette lib

SettingsPanel::~SettingsPanel() = default;

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 10) {
    m_box.set_margin_start(10);
    m_box.set_margin_end(10);
    m_box.set_margin_top(10);

    m_box.pack_start(m_label, Gtk::PACK_SHRINK);
    m_box.pack_start(m_entry_path);
    m_box.pack_start(m_button_browse, Gtk::PACK_SHRINK);

    m_button_browse.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_folder_clicked));

    add(m_box);
    show_all_children();
}

void SettingsPanel::on_folder_clicked() {
    auto dialog = Gtk::FileChooserDialog(
        "Select ROMs Folder",
        Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER
    );
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);

    auto current = m_entry_path.get_text();
    if (!current.empty()) {
        dialog.set_current_folder(current);
    }

    if (dialog.run() == Gtk::RESPONSE_OK) {
        m_entry_path.set_text(dialog.get_filename());
    }
}

std::string SettingsPanel::get_roms_path() const {
    return m_entry_path.get_text();
}

void SettingsPanel::set_roms_path(const std::string& path) {
    m_entry_path.set_text(path);
}

bool SettingsPanel::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;
        if (j.contains("roms_path")) {
            set_roms_path(j["roms_path"]);
        }
    } catch (...) {
        return false;
    }
    return true;
}

bool SettingsPanel::save_to_file(const std::string& filename) {
    nlohmann::json j;
    j["roms_path"] = get_roms_path();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4);
    return true;
}