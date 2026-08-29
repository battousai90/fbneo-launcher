// src/SettingsPanel.cpp
#include "SettingsPanel.h"
#include "Countries.h"
#include <cctype>
#include "IconManager.h"
#include "AppContext.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "FbneoUpdateCheck.h"
#include "i18n.h"
#include <map>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <cstdlib>

SettingsPanel::~SettingsPanel() = default;

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 10) {
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_label_roms.set_text(_("ROMs Directories:"));
    m_label_dat.set_text(_("DAT Files Directory:"));
    m_label_previews.set_text(_("Previews:"));
    m_label_titles.set_text(_("Titles:"));
    m_label_fbneo.set_text(_("FBNeo Executable:"));
    m_button_add_roms.set_label(_("Add Directory"));
    m_button_remove_roms.set_label(_("Remove Selected"));
    m_button_browse_dat.set_label(_("Browse..."));
    m_button_browse_previews.set_label(_("Browse..."));
    m_button_download_previews.set_label(_("Download All Previews"));
    m_button_browse_titles.set_label(_("Browse..."));
    m_button_download_titles.set_label(_("Download All Titles"));
    m_button_browse_fbneo.set_label(_("Select"));
    m_button_download_fbneo.set_label(_("Download"));
    m_button_generate_dat.set_label(_("Generate DAT"));
    m_check_recursive.set_label(_("Scan directories recursively"));
    m_check_loose_files.set_label(_("Include loose ROM files (non-zip)"));
    m_label_theme.set_text(_("Theme:"));
    m_label_language.set_text(_("Language:"));

    set_margin_start(10);
    set_margin_end(10);
    set_margin_top(10);

    // Initialize TreeView columns
    m_columns_roms.add(m_col_path);
    m_model_roms = Gtk::ListStore::create(m_columns_roms);
    m_treeview_roms.set_model(m_model_roms);
    m_treeview_roms.append_column("ROM Directory Path", m_col_path);

    // === TOP PANE: ROM Directories (resizable via Paned handle) ===
    auto top_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
    top_box->set_margin_bottom(4);

    m_label_roms.set_halign(Gtk::ALIGN_START);
    top_box->pack_start(m_label_roms, Gtk::PACK_SHRINK);

    m_scrolled_roms.add(m_treeview_roms);
    m_scrolled_roms.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    m_scrolled_roms.set_size_request(-1, 80);  // minimum height, freely resizable upward
    top_box->pack_start(m_scrolled_roms, Gtk::PACK_EXPAND_WIDGET);

    auto roms_button_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    roms_button_box->set_halign(Gtk::ALIGN_START);

    auto pixbuf_add = IconManager::load("icons/folder-add.svg", 16, 16);
    auto image_add = Gtk::make_managed<Gtk::Image>(pixbuf_add);
    m_button_add_roms.set_image(*image_add);
    m_button_add_roms.set_always_show_image(true);
    m_button_add_roms.set_label(_("Add"));
    m_button_add_roms.set_size_request(80, 30);
    m_button_add_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_add_roms_path_clicked));

    auto pixbuf_remove = IconManager::load("icons/folder-remove.svg", 16, 16);
    auto image_remove = Gtk::make_managed<Gtk::Image>(pixbuf_remove);
    m_button_remove_roms.set_image(*image_remove);
    m_button_remove_roms.set_always_show_image(true);
    m_button_remove_roms.set_label(_("Remove"));
    m_button_remove_roms.set_size_request(90, 30);
    m_button_remove_roms.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_remove_roms_path_clicked));

    roms_button_box->pack_start(m_button_add_roms, Gtk::PACK_SHRINK);
    roms_button_box->pack_start(m_button_remove_roms, Gtk::PACK_SHRINK);
    top_box->pack_start(*roms_button_box, Gtk::PACK_SHRINK);

    // === BOTTOM PANE: rest of settings (fixed height) ===
    auto bottom_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 6);
    bottom_box->set_margin_top(4);

    auto grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(8);
    grid->set_row_spacing(6);
    grid->set_column_homogeneous(false);

    // --- DAT Files Directory ---
    grid->attach(m_label_dat, 0, 0, 1, 1);
    m_label_dat.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_dat, 1, 0, 1, 1);
    m_entry_dat.set_hexpand(true);
    
    // DAT Browse button in column 2
    auto pixbuf_browse_dat = IconManager::load("icons/folder-browse.svg", 16, 16);
    auto image_browse_dat = Gtk::make_managed<Gtk::Image>(pixbuf_browse_dat);
    m_button_browse_dat.set_image(*image_browse_dat);
    m_button_browse_dat.set_always_show_image(true);
    m_button_browse_dat.set_label(_("Browse"));
    m_button_browse_dat.set_size_request(90, 30);
    m_button_browse_dat.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_dat);
    });
    grid->attach(m_button_browse_dat, 2, 0, 1, 1);

    // Generate DAT button in column 3
    auto pixbuf_generate = IconManager::load("icons/generate-dat.svg", 16, 16);
    auto image_generate = Gtk::make_managed<Gtk::Image>(pixbuf_generate);
    m_button_generate_dat.set_image(*image_generate);
    m_button_generate_dat.set_always_show_image(true);
    m_button_generate_dat.set_label(_("Generate DAT"));
    m_button_generate_dat.set_size_request(120, 30);
    m_button_generate_dat.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_generate_dat_clicked));
    grid->attach(m_button_generate_dat, 3, 0, 1, 1);

    // --- Previews ---
    grid->attach(m_label_previews, 0, 1, 1, 1);
    m_label_previews.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_previews, 1, 1, 1, 1);
    m_entry_previews.set_hexpand(true);

    // Previews Select button in column 2
    auto pixbuf_browse_previews = IconManager::load("icons/folder-browse.svg", 16, 16);
    auto image_browse_previews = Gtk::make_managed<Gtk::Image>(pixbuf_browse_previews);
    m_button_browse_previews.set_image(*image_browse_previews);
    m_button_browse_previews.set_always_show_image(true);
    m_button_browse_previews.set_label(_("Select"));
    m_button_browse_previews.set_size_request(90, 30);
    m_button_browse_previews.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_previews);
    });
    grid->attach(m_button_browse_previews, 2, 1, 1, 1);

    // Download Previews button in column 3
    auto pixbuf_download_previews = IconManager::load("icons/download.svg", 16, 16);
    auto image_download_previews = Gtk::make_managed<Gtk::Image>(pixbuf_download_previews);
    m_button_download_previews.set_image(*image_download_previews);
    m_button_download_previews.set_always_show_image(true);
    m_button_download_previews.set_label(_("Download All Previews"));
    m_button_download_previews.set_size_request(150, 30);
    m_button_download_previews.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_download_previews_clicked));
    grid->attach(m_button_download_previews, 3, 1, 1, 1);

    // --- Titles ---
    grid->attach(m_label_titles, 0, 2, 1, 1);
    m_label_titles.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_titles, 1, 2, 1, 1);
    m_entry_titles.set_hexpand(true);

    // Titles Select button in column 2
    auto pixbuf_browse_titles = IconManager::load("icons/folder-browse.svg", 16, 16);
    auto image_browse_titles = Gtk::make_managed<Gtk::Image>(pixbuf_browse_titles);
    m_button_browse_titles.set_image(*image_browse_titles);
    m_button_browse_titles.set_always_show_image(true);
    m_button_browse_titles.set_label(_("Select"));
    m_button_browse_titles.set_size_request(90, 30);
    m_button_browse_titles.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_titles);
    });
    grid->attach(m_button_browse_titles, 2, 2, 1, 1);

    // Download Titles button in column 3
    auto pixbuf_download_titles = IconManager::load("icons/download.svg", 16, 16);
    auto image_download_titles = Gtk::make_managed<Gtk::Image>(pixbuf_download_titles);
    m_button_download_titles.set_image(*image_download_titles);
    m_button_download_titles.set_always_show_image(true);
    m_button_download_titles.set_label(_("Download All Titles"));
    m_button_download_titles.set_size_request(150, 30);
    m_button_download_titles.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_download_titles_clicked));
    grid->attach(m_button_download_titles, 3, 2, 1, 1);

    // --- FBNeo Executable ---
    grid->attach(m_label_fbneo, 0, 3, 1, 1);
    m_label_fbneo.set_halign(Gtk::ALIGN_START);
    grid->attach(m_entry_fbneo, 1, 3, 1, 1);
    m_entry_fbneo.set_hexpand(true);

    // FBNeo Select button in column 2
    auto pixbuf_select_exec = IconManager::load("icons/executable-select.svg", 16, 16);
    auto image_select_exec = Gtk::make_managed<Gtk::Image>(pixbuf_select_exec);
    m_button_browse_fbneo.set_image(*image_select_exec);
    m_button_browse_fbneo.set_always_show_image(true);
    m_button_browse_fbneo.set_label(_("Select"));
    m_button_browse_fbneo.set_size_request(90, 30);
    m_button_browse_fbneo.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog(_("Select FBNeo Executable"), Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

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
    grid->attach(m_button_browse_fbneo, 2, 3, 1, 1);

    // FBNeo Download button in column 3
    auto pixbuf_download = IconManager::load("icons/download.svg", 16, 16);
    auto image_download = Gtk::make_managed<Gtk::Image>(pixbuf_download);
    m_button_download_fbneo.set_image(*image_download);
    m_button_download_fbneo.set_always_show_image(true);
    m_button_download_fbneo.set_label(_("Download"));
    m_button_download_fbneo.set_size_request(100, 30);
    m_button_download_fbneo.signal_clicked().connect(sigc::mem_fun(*this, &SettingsPanel::on_download_fbneo_clicked));
    grid->attach(m_button_download_fbneo, 3, 3, 1, 1);

    // --- Appearance: Theme + Language ---
    m_label_theme.set_text(_("Theme:"));
    m_label_theme.set_halign(Gtk::ALIGN_START);
    grid->attach(m_label_theme, 0, 4, 1, 1);
    m_combo_theme.append("system", _("System"));
    m_combo_theme.append("dark",   _("Dark"));
    m_combo_theme.append("light",  _("Light"));
    m_combo_theme.set_active_id("dark");
    m_combo_theme.set_hexpand(true);
    grid->attach(m_combo_theme, 1, 4, 1, 1);

    m_label_language.set_text(_("Language:"));
    m_label_language.set_halign(Gtk::ALIGN_START);
    grid->attach(m_label_language, 0, 5, 1, 1);
    // Friendly names for known language codes; unknown codes show the raw code.
    // Each language is named in itself — someone looking for their own language
    // recognises "ไทย", not "th". Add an entry here whenever a locale/<code>.json
    // is added, otherwise the picker falls back to showing the bare code.
    static const std::map<std::string, std::string> lang_names = {
        {"en","English"}, {"fr","Français"}, {"es","Español"}, {"de","Deutsch"},
        {"pt","Português"}, {"zh","中文"}, {"ja","日本語"}, {"th","ไทย"}};
    m_combo_language.append("", _("System"));
    for (const auto& code : i18n::available_languages()) {
        auto it = lang_names.find(code);
        m_combo_language.append(code, it != lang_names.end() ? it->second : code);
    }
    m_combo_language.set_active_id("");
    m_combo_language.set_hexpand(true);
    grid->attach(m_combo_language, 1, 5, 1, 1);

    m_combo_theme.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_theme_changed.emit(m_combo_theme.get_active_id());
    });
    m_combo_language.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_language_changed.emit(m_combo_language.get_active_id());
    });

    // --- Online scores: who you are, and whether to send anything ---
    m_label_hiscore_player.set_text(_("Player name:"));
    m_label_hiscore_player.set_halign(Gtk::ALIGN_START);
    grid->attach(m_label_hiscore_player, 0, 6, 1, 1);
    m_entry_hiscore_player.set_hexpand(true);
    m_entry_hiscore_player.set_max_length(24);
    m_entry_hiscore_player.set_placeholder_text(_("shown on the leaderboard"));
    grid->attach(m_entry_hiscore_player, 1, 6, 1, 1);
    // Chosen, not deduced. Deriving it from the connection would give every
    // player on a home network no country at all — a private address has none
    // — and would get it wrong for anyone living away from their flag.
    m_label_hiscore_country.set_text(_("Country:"));
    m_label_hiscore_country.set_halign(Gtk::ALIGN_START);
    grid->attach(m_label_hiscore_country, 0, 7, 1, 1);
    m_entry_hiscore_country.set_hexpand(true);
    m_entry_hiscore_country.set_placeholder_text(_("start typing, e.g. France"));
    build_country_completion();
    grid->attach(m_entry_hiscore_country, 1, 7, 1, 1);

    m_check_hiscore_submit.set_label(_("Send my scores to the online leaderboard"));
    m_check_hiscore_submit.set_tooltip_text(
        _("Sends your score and how long you played after each session. "
          "Off by default; nothing leaves your machine until you turn it on."));

    // --- Scan options (recursive, include loose files) ---
    m_check_recursive.set_active(true);
    m_check_loose_files.set_active(true);

    bottom_box->pack_start(*grid, Gtk::PACK_SHRINK);
    bottom_box->pack_start(m_check_hiscore_submit, Gtk::PACK_SHRINK);
    bottom_box->pack_start(m_check_recursive, Gtk::PACK_SHRINK);
    bottom_box->pack_start(m_check_loose_files, Gtk::PACK_SHRINK);

    // === Assemble Paned: ROM section (resizable) on top, settings below ===
    m_paned_roms.pack1(*top_box, true, false);    // resizable, no shrink below minimum
    m_paned_roms.pack2(*bottom_box, false, false); // fixed height at bottom
    m_paned_roms.set_position(160);               // default height of ROM folder panel
    pack_start(m_paned_roms, Gtk::PACK_EXPAND_WIDGET);

    // Show all widgets
    show_all();  // Must be called at the end
}

std::string SettingsPanel::get_hiscore_player() const {
    return std::string(m_entry_hiscore_player.get_text());
}

// -> the ISO code, or "" when the field is empty or holds something that is
// not a country. Resolved on read rather than pinned when a completion is
// accepted, so a name typed in full without touching the popup still counts.
std::string SettingsPanel::get_hiscore_country() const {
    std::string text = m_entry_hiscore_country.get_text();
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back()))  text.pop_back();
    if (text.empty()) return {};

    std::string upper = text;
    for (auto& c : upper) c = std::toupper((unsigned char)c);
    if (upper.size() == 2)
        for (const auto& c : kCountries)
            if (upper == c.code) return c.code;

    // Case-insensitive, and against BOTH the translated name and the English
    // one. The translation is what the field shows, but a config written under
    // another language — or a name typed in English — must still resolve.
    Glib::ustring folded = Glib::ustring(text).lowercase();
    for (const auto& c : kCountries)
        if (folded == Glib::ustring(_(c.name)).lowercase()
         || folded == Glib::ustring(c.name).lowercase())
            return c.code;

    // Typed but unrecognised: no country rather than a guess. A wrong flag on
    // a leaderboard is worse than none, and the field is optional anyway.
    return {};
}

void SettingsPanel::set_hiscore_country(const std::string& code) {
    for (const auto& c : kCountries)
        if (code == c.code) { m_entry_hiscore_country.set_text(_(c.name)); return; }
    m_entry_hiscore_country.set_text("");
}

void SettingsPanel::build_country_completion() {
    m_country_model = Gtk::ListStore::create(m_country_cols);
    // Sorted on the TRANSLATED name: kCountries is alphabetical in English, so
    // a French list would run Afghanistan, Albanie, Algérie… then jump about.
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(sizeof(kCountries) / sizeof(kCountries[0]));
    for (const auto& c : kCountries) sorted.emplace_back(_(c.name), c.code);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return g_utf8_collate(a.first.c_str(), b.first.c_str()) < 0;
              });
    for (const auto& [name, code] : sorted) {
        auto row = *m_country_model->append();
        row[m_country_cols.name] = name;
        row[m_country_cols.code] = code;
    }
    m_country_completion = Gtk::EntryCompletion::create();
    m_country_completion->set_model(m_country_model);
    m_country_completion->set_text_column(m_country_cols.name);
    m_country_completion->set_minimum_key_length(1);
    m_country_completion->set_popup_completion(true);
    // No inline completion: it would type ahead of the user inside the entry,
    // and "FR" would become "France" mid-keystroke while they meant "FRO".
    m_country_completion->set_inline_completion(false);

    // Matches a prefix of the name OR the two-letter code, so both "fra" and
    // "fr" find France. GTK hands `key` already folded to lower case.
    m_country_completion->set_match_func(
        [this](const Glib::ustring& key, const Gtk::TreeModel::const_iterator& iter) {
            if (!iter) return false;
            Glib::ustring name = (*iter)[m_country_cols.name];
            Glib::ustring code = (*iter)[m_country_cols.code];
            return name.lowercase().find(key) == 0 || code.lowercase().find(key) == 0;
        });
    m_entry_hiscore_country.set_completion(m_country_completion);

    // Unrecognised text is marked as you type. Without this the field fails
    // silently — a typo simply means no flag, discovered days later on the
    // leaderboard, with nothing on screen to explain it.
    m_entry_hiscore_country.signal_changed().connect([this] {
        bool typed = !m_entry_hiscore_country.get_text().empty();
        bool known = !get_hiscore_country().empty();
        auto ctx = m_entry_hiscore_country.get_style_context();
        if (typed && !known) ctx->add_class("error");
        else                 ctx->remove_class("error");
    });
}

bool SettingsPanel::is_hiscore_submit_enabled() const {
    return m_check_hiscore_submit.get_active();
}

std::string SettingsPanel::get_theme() const {
    auto id = m_combo_theme.get_active_id();
    return id.empty() ? "system" : std::string(id);
}
void SettingsPanel::set_theme(const std::string& mode) {
    m_suppress_appearance_signals = true;
    m_combo_theme.set_active_id(mode.empty() ? "system" : mode);
    m_suppress_appearance_signals = false;
}
std::string SettingsPanel::get_language() const {
    return std::string(m_combo_language.get_active_id());
}
void SettingsPanel::set_language(const std::string& code) {
    m_suppress_appearance_signals = true;
    m_combo_language.set_active_id(code);
    m_suppress_appearance_signals = false;
}

void SettingsPanel::on_folder_clicked(Gtk::Entry* entry) {
    auto dialog = Gtk::FileChooserDialog(_("Select Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

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
std::string SettingsPanel::get_previews_path() const { return m_entry_previews.get_text(); }
std::string SettingsPanel::get_titles_path() const { return m_entry_titles.get_text(); }
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
void SettingsPanel::set_previews_path(const std::string& path) { m_entry_previews.set_text(path); }
void SettingsPanel::set_titles_path(const std::string& path) { m_entry_titles.set_text(path); }
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
        if (j.contains("previews_path")) set_previews_path(j["previews_path"]);
        if (j.contains("titles_path")) set_titles_path(j["titles_path"]);
        if (j.contains("scan_recursive")) m_check_recursive.set_active(j["scan_recursive"].get<bool>());
        if (j.contains("scan_loose_files")) m_check_loose_files.set_active(j["scan_loose_files"].get<bool>());

        // Legacy compatibility for thumbnails_path
        if (j.contains("thumbnails_path") && !j.contains("previews_path")) {
            set_previews_path(j["thumbnails_path"]);
        }

        if (j.contains("fbneo_executable")) set_fbneo_executable(j["fbneo_executable"]);
        if (j.contains("theme")) set_theme(j["theme"].get<std::string>());
        if (j.contains("language")) set_language(j["language"].get<std::string>());
        if (j.contains("hiscore_player"))
            m_entry_hiscore_player.set_text(j["hiscore_player"].get<std::string>());
        // Absent means off. Opting in has to be a deliberate act, so a config
        // written before this option existed must not switch it on.
        if (j.contains("hiscore_country"))
            set_hiscore_country(j["hiscore_country"].get<std::string>());
        if (j.contains("hiscore_submit"))
            m_check_hiscore_submit.set_active(j["hiscore_submit"].get<bool>());
    } catch (...) {
        return false;
    }

    // A relative dat_path/fbneo_executable resolves against whatever directory
    // the process happened to be launched from, which silently differs between
    // a desktop launcher, a terminal, and a dev checkout — leading to reads and
    // writes quietly landing in two unrelated folders across runs. Pin any
    // legacy relative value to an absolute one and persist it immediately so
    // this migration only ever has to happen once.
    //
    // Anchor at $HOME, not the executable's own directory: the Flatpak build
    // runs from a read-only /app/bin, so get_executable_dir() would point
    // somewhere the app can never write to. $HOME is what a bare "./support/…"
    // has always actually resolved to for a normally-launched (desktop/Flatpak)
    // session in practice, since that is the working directory it starts in.
    bool migrated = false;
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : ".";
    auto pin_absolute = [&](std::string current, auto setter) {
        if (current.empty() || std::filesystem::path(current).is_absolute()) return;
        std::string absolute = (std::filesystem::path(home) / current).lexically_normal().string();
        (this->*setter)(absolute);
        migrated = true;
    };
    pin_absolute(get_dat_path(), &SettingsPanel::set_dat_path);
    pin_absolute(get_fbneo_executable(), &SettingsPanel::set_fbneo_executable);
    if (migrated) save_to_file(filename);

    return true;
}

bool SettingsPanel::save_to_file(const std::string& filename) {
    // Read existing file first to preserve keys written by other parts of the app
    // (e.g. launch_fullscreen, launch_integerscale, controllers)
    nlohmann::json j;
    {
        std::ifstream fi(filename);
        if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    }
    
    // Save multiple ROMs paths as array
    j["roms_paths"] = nlohmann::json::array();
    for (const auto& path : m_roms_paths) {
        j["roms_paths"].push_back(path);
    }
    
    // Keep legacy single path for compatibility (first path)
    j["roms_path"] = get_roms_path();
    
    j["dat_path"] = get_dat_path();
    j["previews_path"] = get_previews_path();
    j["titles_path"] = get_titles_path();
    j["fbneo_executable"] = get_fbneo_executable();
    j["scan_recursive"] = m_check_recursive.get_active();
    j["scan_loose_files"] = m_check_loose_files.get_active();
    j["theme"] = get_theme();
    j["language"] = get_language();
    j["hiscore_player"] = get_hiscore_player();
    j["hiscore_country"] = get_hiscore_country();
    j["hiscore_submit"] = m_check_hiscore_submit.get_active();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4); // Pretty print with 4 spaces
    return true;
}

// --- New methods for ROMs path management ---
void SettingsPanel::on_add_roms_path_clicked() {
    auto dialog = Gtk::FileChooserDialog(_("Select ROMs Directory"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

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
    
    // $HOME, not current_path(): the working directory a launch happens to
    // start in is not stable across a desktop icon vs. a terminal vs. a dev
    // checkout, so this could silently extract into a different folder each time.
    const char* home_env = std::getenv("HOME");
    auto download_dialog = std::make_unique<DownloadDialog>(
        *parent_window,
        // finalburnneo/FBNeo stopped maintaining the SDL/Linux build (missing DAT
        // export calls for several systems, upstream declined the fix) — this fork
        // tracks their master daily and carries just that fix. See website FAQ.
        "https://github.com/battousai90/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        home_env ? std::string(home_env) : std::filesystem::current_path().string()
    );
    
    download_dialog->set_settings_entry(&m_entry_fbneo);
    download_dialog->start_download();
    download_dialog->run();

    // Record what "latest" pointed at just now, so a future startup check has
    // a baseline to notice the next time our fork moves past this build.
    auto r = FbneoUpdateCheck::fetch_latest();
    if (r.ok) {
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
        j["fbneo_release_sha"] = r.sha;
        std::ofstream fo(path);
        if (fo) fo << j.dump(4);
    }
}

void SettingsPanel::on_generate_dat_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;
    
    GenerateDAT::execute(*parent_window, get_fbneo_executable(), get_dat_path(), &m_entry_dat);
}

void SettingsPanel::on_download_previews_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download previews button clicked" << std::endl;
}

void SettingsPanel::on_download_titles_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download titles button clicked" << std::endl;
}