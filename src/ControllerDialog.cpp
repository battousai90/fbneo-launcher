// src/ControllerDialog.cpp
#include "ControllerDialog.h"
#include "i18n.h"
#include <iostream>

ControllerDialog::ControllerDialog(Gtk::Window& parent,
                                   const std::map<std::string, ControllerConfig>& profiles,
                                   const std::string& active_profile,
                                   const std::string& config_path)
    : Gtk::Dialog("Controller Configuration", parent, Gtk::DIALOG_DESTROY_WITH_PARENT)
    , m_profiles(profiles)
    , m_active_profile_name(active_profile)
    , m_config_path(config_path)
{
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_profile_label.set_text(_("Profile:"));
    m_btn_delete.set_label(_("Delete"));

    // Ensure active profile exists
    if (!m_profiles.count(m_active_profile_name)) {
        if (!m_profiles.empty()) m_active_profile_name = m_profiles.begin()->first;
        else { m_profiles["Default"] = ControllerConfig{}; m_active_profile_name = "Default"; }
    }
    m_config = m_profiles[m_active_profile_name];

    set_default_size(540, 600);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    m_binding_labels.resize(2 * GAME_ACTION_COUNT, nullptr);
    m_bind_buttons.resize(2 * GAME_ACTION_COUNT, nullptr);
    m_devices = ControllerManager::list_devices();

    // Layout: profile bar on top, notebook below
    auto* vbox = get_content_area();
    vbox->set_spacing(6);
    vbox->set_margin_start(10);
    vbox->set_margin_end(10);
    vbox->set_margin_top(8);
    vbox->set_margin_bottom(4);

    build_profile_bar();
    vbox->pack_start(m_profile_bar, Gtk::PACK_SHRINK);
    vbox->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL), Gtk::PACK_SHRINK);

    build_player_tab(0);
    build_player_tab(1);
    vbox->pack_start(m_notebook, Gtk::PACK_EXPAND_WIDGET);

    add_button("Cancel", Gtk::RESPONSE_CANCEL);
    auto* save_btn = add_button("Save", Gtk::RESPONSE_OK);
    save_btn->signal_clicked().connect(sigc::mem_fun(*this, &ControllerDialog::on_save_clicked));
    set_default_response(Gtk::RESPONSE_OK);
    show_all_children();
}

ControllerDialog::~ControllerDialog() { stop_binding(); }

// ── Profile bar ───────────────────────────────────────────────────────────

void ControllerDialog::build_profile_bar() {
    m_profile_label.set_halign(Gtk::ALIGN_CENTER);
    m_profile_bar.pack_start(m_profile_label, Gtk::PACK_SHRINK);

    populate_profile_combo();
    m_profile_combo.set_hexpand(true);
    m_profile_combo.signal_changed().connect(
        sigc::mem_fun(*this, &ControllerDialog::on_profile_changed));
    m_profile_bar.pack_start(m_profile_combo, Gtk::PACK_EXPAND_WIDGET);

    m_btn_new.signal_clicked().connect(sigc::mem_fun(*this, &ControllerDialog::on_new_profile_clicked));
    m_btn_rename.signal_clicked().connect(sigc::mem_fun(*this, &ControllerDialog::on_rename_profile_clicked));
    m_btn_delete.signal_clicked().connect(sigc::mem_fun(*this, &ControllerDialog::on_delete_profile_clicked));

    m_profile_bar.pack_start(m_btn_new,    Gtk::PACK_SHRINK);
    m_profile_bar.pack_start(m_btn_rename, Gtk::PACK_SHRINK);
    m_profile_bar.pack_start(m_btn_delete, Gtk::PACK_SHRINK);
}

void ControllerDialog::populate_profile_combo() {
    m_profile_switching = true;
    m_profile_combo.remove_all();
    for (const auto& [name, _] : m_profiles)
        m_profile_combo.append(name, name);
    m_profile_combo.set_active_id(m_active_profile_name);
    m_profile_switching = false;
}

// ── Save active m_config → m_profiles ────────────────────────────────────

void ControllerDialog::save_active_to_profiles() {
    m_profiles[m_active_profile_name] = m_config;
}

// ── Load a profile into m_config and refresh UI ───────────────────────────

void ControllerDialog::load_profile(const std::string& name) {
    m_active_profile_name = name;
    m_config = m_profiles.count(name) ? m_profiles[name] : ControllerConfig{};
    refresh_from_config();
}

// ── Profile change (combo changed by user) ────────────────────────────────

void ControllerDialog::on_profile_changed() {
    if (m_profile_switching) return;
    save_active_to_profiles();
    load_profile(m_profile_combo.get_active_id());
}

// ── New profile ───────────────────────────────────────────────────────────

void ControllerDialog::on_new_profile_clicked() {
    Gtk::Dialog dlg("New Profile", *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_placeholder_text("Profile name…");
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("Create",  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string name = entry->get_text();
    if (name.empty()) return;
    if (m_profiles.count(name)) {
        Gtk::MessageDialog warn(*this, "Profile \"" + name + "\" already exists.",
                                false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        warn.run();
        return;
    }

    save_active_to_profiles();
    // New profile starts as empty (no bindings, no device)
    m_profiles[name] = ControllerConfig{};

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(name);
    m_profile_switching = false;
    load_profile(name);
}

// ── Rename profile ────────────────────────────────────────────────────────

void ControllerDialog::on_rename_profile_clicked() {
    Gtk::Dialog dlg("Rename Profile", *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(m_active_profile_name);
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    dlg.add_button("Rename",  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string new_name = entry->get_text();
    if (new_name.empty() || new_name == m_active_profile_name) return;
    if (m_profiles.count(new_name)) {
        Gtk::MessageDialog warn(*this, "Profile \"" + new_name + "\" already exists.",
                                false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        warn.run();
        return;
    }

    save_active_to_profiles();
    m_profiles[new_name] = m_profiles[m_active_profile_name];
    m_profiles.erase(m_active_profile_name);
    m_active_profile_name = new_name;

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(new_name);
    m_profile_switching = false;
}

// ── Delete profile ────────────────────────────────────────────────────────

void ControllerDialog::on_delete_profile_clicked() {
    if (m_profiles.size() <= 1) {
        Gtk::MessageDialog warn(*this, "Cannot delete the last profile.",
                                false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        warn.run();
        return;
    }

    Gtk::MessageDialog confirm(*this,
        "Delete profile \"" + m_active_profile_name + "\"?",
        false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_OK_CANCEL, true);
    if (confirm.run() != Gtk::RESPONSE_OK) return;

    m_profiles.erase(m_active_profile_name);
    // Switch to first available profile
    std::string next = m_profiles.begin()->first;
    m_active_profile_name = next;
    m_config = m_profiles[next];

    populate_profile_combo();
    m_profile_switching = true;
    m_profile_combo.set_active_id(next);
    m_profile_switching = false;
    refresh_from_config();
}

// ── Build one player tab ──────────────────────────────────────────────────

void ControllerDialog::build_player_tab(int p) {
    auto* tab_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 8);
    tab_box->set_margin_start(12);
    tab_box->set_margin_end(12);
    tab_box->set_margin_top(10);
    tab_box->set_margin_bottom(10);

    // ── Device selector row ──────────────────────────────────────────────
    auto* dev_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    auto* dev_lbl = Gtk::make_managed<Gtk::Label>("Controller:");
    dev_lbl->set_halign(Gtk::ALIGN_START);
    dev_row->pack_start(*dev_lbl, Gtk::PACK_SHRINK);

    auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->append("", "— None —");
    for (const auto& d : m_devices)
        combo->append(d.path, d.name + "  (" + d.path + ")");

    const std::string& cur = m_config.players[p].device_path;
    if (!cur.empty()) combo->set_active_id(cur);
    else              combo->set_active(0);

    combo->set_hexpand(true);
    combo->signal_changed().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::on_device_changed), p));
    dev_row->pack_start(*combo, Gtk::PACK_EXPAND_WIDGET);
    m_device_combos[p] = combo;

    // Refresh button
    auto* refresh_btn = Gtk::make_managed<Gtk::Button>("↻");
    refresh_btn->set_tooltip_text(_("Refresh controller list"));
    refresh_btn->signal_clicked().connect([this, p]() {
        m_devices = ControllerManager::list_devices();
        std::string cur_id = m_device_combos[p]->get_active_id();
        m_device_combos[p]->remove_all();
        m_device_combos[p]->append("", "— None —");
        for (const auto& d : m_devices)
            m_device_combos[p]->append(d.path, d.name + "  (" + d.path + ")");
        if (!cur_id.empty()) m_device_combos[p]->set_active_id(cur_id);
        else                 m_device_combos[p]->set_active(0);
    });
    dev_row->pack_start(*refresh_btn, Gtk::PACK_SHRINK);
    tab_box->pack_start(*dev_row, Gtk::PACK_SHRINK);

    // ── Column headers ───────────────────────────────────────────────────
    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL), Gtk::PACK_SHRINK);

    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    auto* h1  = Gtk::make_managed<Gtk::Label>(); h1->set_markup("<b>Action</b>");
    h1->set_halign(Gtk::ALIGN_START); h1->set_size_request(140, -1);
    auto* h2  = Gtk::make_managed<Gtk::Label>(); h2->set_markup("<b>Assigned</b>");
    h2->set_halign(Gtk::ALIGN_START); h2->set_size_request(120, -1);
    hdr->pack_start(*h1, Gtk::PACK_SHRINK);
    hdr->pack_start(*h2, Gtk::PACK_SHRINK);
    tab_box->pack_start(*hdr, Gtk::PACK_SHRINK);

    // ── Action rows in a scrolled window ────────────────────────────────
    auto* scrolled = Gtk::make_managed<Gtk::ScrolledWindow>();
    scrolled->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scrolled->set_vexpand(true);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_column_spacing(12);
    grid->set_row_spacing(4);
    grid->set_margin_top(4);

    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        GameAction action = static_cast<GameAction>(a);

        auto* name_lbl = Gtk::make_managed<Gtk::Label>(game_action_name(action));
        name_lbl->set_halign(Gtk::ALIGN_START);
        name_lbl->set_size_request(140, -1);

        auto* bind_lbl = Gtk::make_managed<Gtk::Label>("—");
        bind_lbl->set_halign(Gtk::ALIGN_START);
        bind_lbl->set_size_request(120, -1);
        m_binding_labels[p * GAME_ACTION_COUNT + a] = bind_lbl;

        auto* btn = Gtk::make_managed<Gtk::Button>("Bind");
        btn->set_size_request(60, -1);
        btn->signal_clicked().connect(
            sigc::bind(sigc::bind(sigc::mem_fun(*this, &ControllerDialog::start_binding), action), p));
        m_bind_buttons[p * GAME_ACTION_COUNT + a] = btn;

        auto* clear_btn = Gtk::make_managed<Gtk::Button>("✕");
        clear_btn->set_tooltip_text(_("Clear this binding"));
        clear_btn->set_size_request(30, -1);
        clear_btn->signal_clicked().connect([this, p, action, bind_lbl]() {
            m_config.players[p].bindings.erase(action);
            bind_lbl->set_text("—");
        });

        grid->attach(*name_lbl,  0, a, 1, 1);
        grid->attach(*bind_lbl,  1, a, 1, 1);
        grid->attach(*btn,       2, a, 1, 1);
        grid->attach(*clear_btn, 3, a, 1, 1);
    }

    scrolled->add(*grid);
    tab_box->pack_start(*scrolled, Gtk::PACK_EXPAND_WIDGET);

    // ── Clear all button ─────────────────────────────────────────────────
    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL), Gtk::PACK_SHRINK);
    auto* clear_all = Gtk::make_managed<Gtk::Button>("Clear All Bindings");
    clear_all->set_halign(Gtk::ALIGN_START);
    clear_all->signal_clicked().connect([this, p]() {
        m_config.players[p].bindings.clear();
        refresh_bindings(p);
    });
    tab_box->pack_start(*clear_all, Gtk::PACK_SHRINK);

    m_notebook.append_page(*tab_box, "Player " + std::to_string(p + 1));
    refresh_bindings(p);
}

// ── Refresh all UI from m_config ──────────────────────────────────────────

void ControllerDialog::refresh_from_config() {
    for (int p = 0; p < 2; ++p) {
        if (!m_device_combos[p]) continue;
        const std::string& cur = m_config.players[p].device_path;
        if (!cur.empty()) m_device_combos[p]->set_active_id(cur);
        else              m_device_combos[p]->set_active(0);
        refresh_bindings(p);
    }
}

// ── Refresh binding labels from m_config ─────────────────────────────────

void ControllerDialog::refresh_bindings(int p) {
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        Gtk::Label* lbl = m_binding_labels[p * GAME_ACTION_COUNT + a];
        if (!lbl) continue;
        GameAction action = static_cast<GameAction>(a);
        auto it = m_config.players[p].bindings.find(action);
        lbl->set_text(it != m_config.players[p].bindings.end()
                      ? it->second.label() : "—");
    }
}

// ── Device combo changed ──────────────────────────────────────────────────

void ControllerDialog::on_device_changed(int p) {
    std::string path = m_device_combos[p]->get_active_id();
    m_config.players[p].device_path = path;
    for (const auto& d : m_devices)
        if (d.path == path) { m_config.players[p].device_name = d.name; break; }
    if (path.empty()) m_config.players[p].device_name = "";
}

// ── Bind: start waiting for input ─────────────────────────────────────────

void ControllerDialog::start_binding(int p, GameAction action) {
    stop_binding();

    std::string device_path = m_config.players[p].device_path;
    if (device_path.empty()) {
        Gtk::MessageDialog msg(*this, "No controller selected",
                               false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        msg.set_secondary_text("Please select a controller device first.");
        msg.run();
        return;
    }

    m_bind_fd = ControllerManager::open_device(device_path);
    if (m_bind_fd < 0) {
        Gtk::MessageDialog msg(*this, "Cannot open device",
                               false, Gtk::MESSAGE_ERROR, Gtk::BUTTONS_OK, true);
        msg.set_secondary_text("Could not open " + device_path
                               + ".\nCheck permissions (/dev/input group).");
        msg.run();
        return;
    }

    // Drain stale events
    { InputBinding dummy; for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(m_bind_fd, dummy)) break; }

    Gtk::Dialog wait_dlg(
        "Binding: " + std::string(game_action_name(action)),
        *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(320, 140);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    std::string dev_name = m_config.players[p].device_name;
    if (dev_name.empty()) dev_name = device_path;

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>Press a button or move a stick\non <i>"
                     + Glib::Markup::escape_text(dev_name) + "</i>…</b>");
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20); info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    Gtk::Label* bind_lbl = m_binding_labels[p * GAME_ACTION_COUNT + static_cast<int>(action)];

    m_poll_conn = Glib::signal_timeout().connect(
        [this, p, action, &wait_dlg, bind_lbl]() -> bool {
            InputBinding result;
            if (ControllerManager::poll_event(m_bind_fd, result)) {
                m_config.players[p].bindings[action] = result;
                if (bind_lbl) bind_lbl->set_text(result.label());
                wait_dlg.response(Gtk::RESPONSE_OK);
                return false;
            }
            return true;
        }, 50);

    wait_dlg.run();
    stop_binding();
}

void ControllerDialog::stop_binding() {
    if (m_poll_conn.connected()) m_poll_conn.disconnect();
    ControllerManager::close_device(m_bind_fd);
    m_bind_fd = -1;
}

// ── Save ─────────────────────────────────────────────────────────────────

void ControllerDialog::on_save_clicked() {
    // Sync current working config into profile map
    save_active_to_profiles();

    // Persist all profiles to config.json
    ControllerManager::save_profiles(m_profiles, m_active_profile_name, m_config_path);
    std::cout << "[ControllerDialog] Profiles saved (active: " << m_active_profile_name << ")\n";

    // Write FBNeo input config for the active profile
    std::string fbneo_dir = ControllerManager::get_fbneo_config_dir();
    ControllerManager::write_fbneo_config(m_config, fbneo_dir);
}
