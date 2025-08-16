// src/SettingsPanel.cpp
#include "SettingsPanel.h"
#include <gtkmm/filechooserdialog.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>

SettingsPanel::~SettingsPanel() = default;

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 10) {
    set_margin_start(10);
    set_margin_end(10);
    set_margin_top(10);

    // Initialize TreeView columns
    m_columns_roms.add(m_col_path);
    m_model_roms = Gtk::ListStore::create(m_columns_roms);
    m_treeview_roms.set_model(m_model_roms);
    m_treeview_roms.append_column("ROM Directory Path", m_col_path);

    // Create a grid for aligned layout
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(8);   // Horizontal spacing between columns
    grid->set_row_spacing(6);      // Vertical spacing between rows
    grid->set_column_homogeneous(false);  // Allow different column widths

    // --- ROMs Directories ---
    grid->attach(m_label_roms, 0, 0, 3, 1);           // Label spans 3 columns
    m_label_roms.set_halign(Gtk::ALIGN_START);
    
    // TreeView for ROMs paths
    m_scrolled_roms.add(m_treeview_roms);
    m_scrolled_roms.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_roms.set_size_request(400, 100);
    grid->attach(m_scrolled_roms, 0, 1, 3, 1);
    
    // Buttons for managing ROMs paths
    auto roms_button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 5);
    m_button_add_roms.set_image_from_icon_name("list-add-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_add_roms.set_always_show_image(true);
    m_button_add_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_add_roms_path_clicked));
    
    m_button_remove_roms.set_image_from_icon_name("list-remove-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_remove_roms.set_always_show_image(true);
    m_button_remove_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_remove_roms_path_clicked));
    
    roms_button_box->pack_start(m_button_add_roms, Gtk::PACK_SHRINK);
    roms_button_box->pack_start(m_button_remove_roms, Gtk::PACK_SHRINK);
    grid->attach(*roms_button_box, 0, 2, 3, 1);

    // --- DAT Files Directory ---
    grid->attach(m_label_dat, 0, 3, 1, 1);
    m_label_dat.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_dat, 1, 3, 1, 1);
    m_entry_dat.set_hexpand(true);
    grid->attach(m_button_browse_dat, 2, 3, 1, 1);

    m_button_browse_dat.set_image_from_icon_name("folder-open-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_browse_dat.set_always_show_image(true);
    m_button_browse_dat.set_label("Browse");
    m_button_browse_dat.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_dat);
    });

    // --- Thumbnails ---
    grid->attach(m_label_thumbs, 0, 4, 1, 1);
    m_label_thumbs.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_thumbs, 1, 4, 1, 1);
    m_entry_thumbs.set_hexpand(true);
    grid->attach(m_button_browse_thumbs, 2, 4, 1, 1);

    m_button_browse_thumbs.set_image_from_icon_name("folder-open-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_browse_thumbs.set_always_show_image(true);
    m_button_browse_thumbs.set_label("Select");
    m_button_browse_thumbs.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_thumbs);
    });

    // --- FBNeo Executable ---
    grid->attach(m_label_fbneo, 0, 5, 1, 1);
    m_label_fbneo.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_fbneo, 1, 5, 1, 1);
    m_entry_fbneo.set_hexpand(true);
    grid->attach(m_button_browse_fbneo, 2, 5, 1, 1);

    m_button_browse_fbneo.set_image_from_icon_name("application-x-executable-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_browse_fbneo.set_always_show_image(true);
    m_button_browse_fbneo.set_label("Select");
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

    // Add the grid to the main container
    pack_start(*grid, Gtk::PACK_SHRINK);

    // --- Save & Cancel buttons ---
    Gtk::HBox button_box;
    button_box.pack_end(m_button_save, Gtk::PACK_SHRINK, 5);
    button_box.pack_end(m_button_cancel, Gtk::PACK_SHRINK, 5);

    m_button_save.set_image_from_icon_name("document-save-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_save.set_always_show_image(true);
    m_button_save.set_label("Save");

    m_button_cancel.set_image_from_icon_name("window-close-symbolic", Gtk::ICON_SIZE_BUTTON);
    m_button_cancel.set_always_show_image(true);
    m_button_cancel.set_label("Cancel");

    pack_start(button_box, Gtk::PACK_SHRINK);

    // Show all widgets
    show_all();  // Must be called at the end
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
std::string SettingsPanel::get_roms_path() const { 
    // Deprecated: return first path for compatibility
    return m_roms_paths.empty() ? "" : m_roms_paths[0]; 
}

std::vector<std::string> SettingsPanel::get_roms_paths() const { 
    return m_roms_paths; 
}

std::string SettingsPanel::get_dat_path() const { return m_entry_dat.get_text(); }
std::string SettingsPanel::get_thumbnails_path() const { return m_entry_thumbs.get_text(); }
std::string SettingsPanel::get_fbneo_executable() const { return m_entry_fbneo.get_text(); }

// --- Setters ---
void SettingsPanel::set_roms_path(const std::string& path) { 
    // Deprecated: set as first path for compatibility
    if (m_roms_paths.empty()) {
        m_roms_paths.push_back(path);
    } else {
        m_roms_paths[0] = path;
    }
    refresh_roms_list();
}

void SettingsPanel::set_roms_paths(const std::vector<std::string>& paths) {
    m_roms_paths = paths;
    refresh_roms_list();
}

void SettingsPanel::add_roms_path(const std::string& path) {
    m_roms_paths.push_back(path);
    refresh_roms_list();
}

void SettingsPanel::remove_roms_path(int index) {
    if (index >= 0 && index < static_cast<int>(m_roms_paths.size())) {
        m_roms_paths.erase(m_roms_paths.begin() + index);
        refresh_roms_list();
    }
}

void SettingsPanel::set_dat_path(const std::string& path) { m_entry_dat.set_text(path); }
void SettingsPanel::set_thumbnails_path(const std::string& path) { m_entry_thumbs.set_text(path); }
void SettingsPanel::set_fbneo_executable(const std::string& path) { m_entry_fbneo.set_text(path); }

// --- Load / Save ---
bool SettingsPanel::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;

        // Load multiple ROMs paths if available, fallback to single path for compatibility
        if (j.contains("roms_paths") && j["roms_paths"].is_array()) {
            std::vector<std::string> paths;
            for (const auto& path : j["roms_paths"]) {
                paths.push_back(path.get<std::string>());
            }
            set_roms_paths(paths);
        } else if (j.contains("roms_path")) {
            // Legacy single path support
            set_roms_path(j["roms_path"]);
        }
        
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
    
    // Save multiple ROMs paths as array
    j["roms_paths"] = nlohmann::json::array();
    for (const auto& path : m_roms_paths) {
        j["roms_paths"].push_back(path);
    }
    
    // Keep legacy single path for compatibility (first path)
    j["roms_path"] = get_roms_path();
    
    j["dat_path"] = get_dat_path();
    j["thumbnails_path"] = get_thumbnails_path();
    j["fbneo_executable"] = get_fbneo_executable();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4); // Pretty print with 4 spaces
    return true;
}

// --- New methods for ROMs path management ---
void SettingsPanel::on_add_roms_path_clicked() {
    auto dialog = Gtk::FileChooserDialog("Select ROMs Directory", Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dialog.add_button("Select", Gtk::RESPONSE_OK);

    if (dialog.run() == Gtk::RESPONSE_OK) {
        std::string path = dialog.get_filename();
        // Check if path already exists
        auto it = std::find(m_roms_paths.begin(), m_roms_paths.end(), path);
        if (it == m_roms_paths.end()) {
            add_roms_path(path);
        }
    }
}

void SettingsPanel::on_remove_roms_path_clicked() {
    auto selection = m_treeview_roms.get_selection();
    auto iter = selection->get_selected();
    if (iter) {
        // Find the index of the selected item
        auto path = m_model_roms->get_path(iter);
        int index = path[0];  // Get the first (and only) index
        remove_roms_path(index);
    }
}

void SettingsPanel::refresh_roms_list() {
    m_model_roms->clear();
    for (const auto& path : m_roms_paths) {
        auto row = *m_model_roms->append();
        row[m_col_path] = path;
    }
}