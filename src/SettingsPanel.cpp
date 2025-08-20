// src/SettingsPanel.cpp
#include "SettingsPanel.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <fstream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>

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

    // Create a grid for aligned layout with 4 columns
    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(8);   // Horizontal spacing between columns
    grid->set_row_spacing(6);      // Vertical spacing between rows
    grid->set_column_homogeneous(false);  // Allow different column widths

    // --- ROMs Directories ---
    grid->attach(m_label_roms, 0, 0, 4, 1);           // Label spans 4 columns
    m_label_roms.set_halign(Gtk::ALIGN_START);
    
    // TreeView for ROMs paths
    m_scrolled_roms.add(m_treeview_roms);
    m_scrolled_roms.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_roms.set_size_request(400, 100);
    grid->attach(m_scrolled_roms, 0, 1, 4, 1);
    
    // Buttons for managing ROMs paths with better styling
    auto roms_button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    roms_button_box->set_halign(Gtk::ALIGN_START);
    
    auto pixbuf_add = Gdk::Pixbuf::create_from_file("assets/icons/folder-add.svg", 16, 16);
    auto image_add = Gtk::make_managed<Gtk::Image>(pixbuf_add);
    m_button_add_roms.set_image(*image_add);
    m_button_add_roms.set_always_show_image(true);
    m_button_add_roms.set_label("Add");
    m_button_add_roms.set_size_request(80, 30);
    m_button_add_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_add_roms_path_clicked));
    
    auto pixbuf_remove = Gdk::Pixbuf::create_from_file("assets/icons/folder-remove.svg", 16, 16);
    auto image_remove = Gtk::make_managed<Gtk::Image>(pixbuf_remove);
    m_button_remove_roms.set_image(*image_remove);
    m_button_remove_roms.set_always_show_image(true);
    m_button_remove_roms.set_label("Remove");
    m_button_remove_roms.set_size_request(90, 30);
    m_button_remove_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_remove_roms_path_clicked));
    
    roms_button_box->pack_start(m_button_add_roms, Gtk::PACK_SHRINK);
    roms_button_box->pack_start(m_button_remove_roms, Gtk::PACK_SHRINK);
    grid->attach(*roms_button_box, 0, 2, 4, 1);

    // --- DAT Files Directory ---
    grid->attach(m_label_dat, 0, 3, 1, 1);
    m_label_dat.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_dat, 1, 3, 1, 1);
    m_entry_dat.set_hexpand(true);
    
    // DAT Browse button in column 2
    auto pixbuf_browse_dat = Gdk::Pixbuf::create_from_file("assets/icons/folder-browse.svg", 16, 16);
    auto image_browse_dat = Gtk::make_managed<Gtk::Image>(pixbuf_browse_dat);
    m_button_browse_dat.set_image(*image_browse_dat);
    m_button_browse_dat.set_always_show_image(true);
    m_button_browse_dat.set_label("Browse");
    m_button_browse_dat.set_size_request(90, 30);
    m_button_browse_dat.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_dat);
    });
    grid->attach(m_button_browse_dat, 2, 3, 1, 1);
    
    // Generate DAT button in column 3
    auto pixbuf_generate = Gdk::Pixbuf::create_from_file("assets/icons/generate-dat.svg", 16, 16);
    auto image_generate = Gtk::make_managed<Gtk::Image>(pixbuf_generate);
    m_button_generate_dat.set_image(*image_generate);
    m_button_generate_dat.set_always_show_image(true);
    m_button_generate_dat.set_label("Generate DAT");
    m_button_generate_dat.set_size_request(120, 30);
    m_button_generate_dat.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_generate_dat_clicked));
    grid->attach(m_button_generate_dat, 3, 3, 1, 1);

    // --- Thumbnails ---
    grid->attach(m_label_thumbs, 0, 4, 1, 1);
    m_label_thumbs.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_thumbs, 1, 4, 1, 1);
    m_entry_thumbs.set_hexpand(true);
    
    // Thumbnails Select button in column 2
    auto pixbuf_browse_thumbs = Gdk::Pixbuf::create_from_file("assets/icons/folder-browse.svg", 16, 16);
    auto image_browse_thumbs = Gtk::make_managed<Gtk::Image>(pixbuf_browse_thumbs);
    m_button_browse_thumbs.set_image(*image_browse_thumbs);
    m_button_browse_thumbs.set_always_show_image(true);
    m_button_browse_thumbs.set_label("Select");
    m_button_browse_thumbs.set_size_request(90, 30);
    m_button_browse_thumbs.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_thumbs);
    });
    grid->attach(m_button_browse_thumbs, 2, 4, 1, 1);

    // --- FBNeo Executable ---
    grid->attach(m_label_fbneo, 0, 5, 1, 1);
    m_label_fbneo.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_fbneo, 1, 5, 1, 1);
    m_entry_fbneo.set_hexpand(true);
    
    // FBNeo Select button in column 2
    auto pixbuf_select_exec = Gdk::Pixbuf::create_from_file("assets/icons/executable-select.svg", 16, 16);
    auto image_select_exec = Gtk::make_managed<Gtk::Image>(pixbuf_select_exec);
    m_button_browse_fbneo.set_image(*image_select_exec);
    m_button_browse_fbneo.set_always_show_image(true);
    m_button_browse_fbneo.set_label("Select");
    m_button_browse_fbneo.set_size_request(90, 30);
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
    grid->attach(m_button_browse_fbneo, 2, 5, 1, 1);
    
    // FBNeo Download button in column 3
    auto pixbuf_download = Gdk::Pixbuf::create_from_file("assets/icons/download.svg", 16, 16);
    auto image_download = Gtk::make_managed<Gtk::Image>(pixbuf_download);
    m_button_download_fbneo.set_image(*image_download);
    m_button_download_fbneo.set_always_show_image(true);
    m_button_download_fbneo.set_label("Download");
    m_button_download_fbneo.set_size_request(100, 30);
    m_button_download_fbneo.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_download_fbneo_clicked));
    grid->attach(m_button_download_fbneo, 3, 5, 1, 1);

    // Add the grid to the main container
    pack_start(*grid, Gtk::PACK_SHRINK);

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

void SettingsPanel::on_download_fbneo_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;
    
    auto download_dialog = std::make_unique<DownloadDialog>(
        *parent_window,
        "https://github.com/finalburnneo/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        std::filesystem::current_path().string()
    );
    
    download_dialog->set_settings_entry(&m_entry_fbneo);
    download_dialog->start_download();
    download_dialog->run();
}

void SettingsPanel::on_generate_dat_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;
    
    GenerateDAT::execute(*parent_window, get_fbneo_executable(), &m_entry_dat);
}