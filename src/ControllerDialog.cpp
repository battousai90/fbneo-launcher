// src/ControllerDialog.cpp
#include "ControllerDialog.h"
#include "i18n.h"
#include <iostream>

namespace {
// Le nom que porte la touche sur le clavier du joueur. gdk_keyval_name rend
// un identifiant technique ("Left", "space", "a") : on le rend presentable
// sans le traduire, un nom de touche n'ayant de sens que tel qu'il est grave.
// Le nom que porte, sur le clavier branche, la touche designee par un code de
// l'emulateur. Sert aux prereglages : le code 0x1E est le A d'un QWERTY et le
// Q d'un AZERTY, et l'afficher au hasard tromperait la moitie des joueurs.
std::string key_name_for_code(int fbneo_key) {
    const unsigned keycode = ControllerManager::gtk_keycode_from_fbneo(fbneo_key);
    if (!keycode) return {};
    auto* keymap = gdk_keymap_get_for_display(gdk_display_get_default());
    if (!keymap) return {};
    guint* keyvals = nullptr;
    GdkKeymapKey* keys = nullptr;
    gint count = 0;
    if (!gdk_keymap_get_entries_for_keycode(keymap, keycode, &keys, &keyvals, &count)
        || count <= 0) {
        g_free(keys); g_free(keyvals);
        return {};
    }
    const char* raw = gdk_keyval_name(gdk_keyval_to_upper(keyvals[0]));
    std::string name = raw ? raw : "";
    g_free(keys); g_free(keyvals);
    if (name == "space") name = "Space";
    for (auto& c : name) if (c == '_') c = ' ';
    return name;
}

std::string key_name_from_event(const GdkEventKey* ev) {
    const char* raw = gdk_keyval_name(gdk_keyval_to_upper(ev->keyval));
    if (!raw) return {};
    std::string name = raw;
    if (name == "space") return "Space";
    for (auto& c : name) if (c == '_') c = ' ';
    return name;
}
}  // namespace

ControllerDialog::ControllerDialog(Gtk::Window& parent,
                                   const std::map<std::string, ControllerConfig>& profiles,
                                   const std::string& active_profile,
                                   const std::string& config_path)
    : Gtk::Dialog(_("Controller Configuration"), parent, Gtk::DIALOG_DESTROY_WITH_PARENT)
    , m_profiles(profiles)
    , m_active_profile_name(active_profile)
    , m_config_path(config_path)
{
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    m_profile_label.set_text(_("Profile:"));
    m_btn_new.set_label(_("New…"));
    m_btn_rename.set_label(_("Rename…"));
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
    analog_ui_from_config();   // widgets exist now; fill them from the profile
    vbox->pack_start(m_notebook, Gtk::PACK_EXPAND_WIDGET);

    add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    auto* save_btn = add_button(_("Save"), Gtk::RESPONSE_OK);
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
    analog_ui_from_config();
}

// ── Profile change (combo changed by user) ────────────────────────────────

void ControllerDialog::on_profile_changed() {
    if (m_profile_switching) return;
    save_active_to_profiles();
    load_profile(m_profile_combo.get_active_id());
}

// ── New profile ───────────────────────────────────────────────────────────

void ControllerDialog::on_new_profile_clicked() {
    Gtk::Dialog dlg(_("New Profile"), *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_placeholder_text(_("Profile name…"));
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Create"),  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string name = entry->get_text();
    if (name.empty()) return;
    if (m_profiles.count(name)) {
        Gtk::MessageDialog warn(*this, Glib::ustring::compose(
            _("Profile \"%1\" already exists."), name),
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
    Gtk::Dialog dlg(_("Rename Profile"), *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(320, 100);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* entry = Gtk::make_managed<Gtk::Entry>();
    entry->set_text(m_active_profile_name);
    entry->set_activates_default(true);
    entry->set_margin_start(12); entry->set_margin_end(12);
    entry->set_margin_top(12);
    dlg.get_content_area()->pack_start(*entry, Gtk::PACK_SHRINK);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.add_button(_("Rename"),  Gtk::RESPONSE_OK);
    dlg.set_default_response(Gtk::RESPONSE_OK);
    dlg.show_all_children();

    if (dlg.run() != Gtk::RESPONSE_OK) return;
    std::string new_name = entry->get_text();
    if (new_name.empty() || new_name == m_active_profile_name) return;
    if (m_profiles.count(new_name)) {
        Gtk::MessageDialog warn(*this, Glib::ustring::compose(
            _("Profile \"%1\" already exists."), new_name),
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
        Gtk::MessageDialog warn(*this, _("Cannot delete the last profile."),
                                false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        warn.run();
        return;
    }

    Gtk::MessageDialog confirm(*this,
        Glib::ustring::compose(_("Delete profile \"%1\"?"), m_active_profile_name),
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
    auto* dev_lbl = Gtk::make_managed<Gtk::Label>(_("Controller:"));
    dev_lbl->set_halign(Gtk::ALIGN_START);
    dev_row->pack_start(*dev_lbl, Gtk::PACK_SHRINK);

    auto* combo = Gtk::make_managed<Gtk::ComboBoxText>();
    combo->append("", _("None "));
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
        m_device_combos[p]->append("", _("None "));
        for (const auto& d : m_devices)
            m_device_combos[p]->append(d.path, d.name + "  (" + d.path + ")");
        if (!cur_id.empty()) m_device_combos[p]->set_active_id(cur_id);
        else                 m_device_combos[p]->set_active(0);
    });
    dev_row->pack_start(*refresh_btn, Gtk::PACK_SHRINK);

    auto* identify_btn = Gtk::make_managed<Gtk::Button>(_("Identify"));
    identify_btn->set_tooltip_text(
        _("Press a button on a controller to assign it to this player."));
    identify_btn->signal_clicked().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::identify_device), p));
    dev_row->pack_start(*identify_btn, Gtk::PACK_SHRINK);

    tab_box->pack_start(*dev_row, Gtk::PACK_SHRINK);

    // ── Column headers ───────────────────────────────────────────────────
    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL), Gtk::PACK_SHRINK);

    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    auto* h1  = Gtk::make_managed<Gtk::Label>(); h1->set_markup("<b>" + Glib::Markup::escape_text(_("Action")) + "</b>");
    h1->set_halign(Gtk::ALIGN_START); h1->set_size_request(140, -1);
    auto* h2  = Gtk::make_managed<Gtk::Label>(); h2->set_markup("<b>" + Glib::Markup::escape_text(_("Assigned")) + "</b>");
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

        auto* name_lbl = Gtk::make_managed<Gtk::Label>(_(game_action_name(action)));
        name_lbl->set_halign(Gtk::ALIGN_START);
        name_lbl->set_size_request(140, -1);

        auto* bind_lbl = Gtk::make_managed<Gtk::Label>("");
        bind_lbl->set_halign(Gtk::ALIGN_START);
        bind_lbl->set_size_request(120, -1);
        m_binding_labels[p * GAME_ACTION_COUNT + a] = bind_lbl;

        auto* btn = Gtk::make_managed<Gtk::Button>(_("Bind"));
        btn->set_size_request(60, -1);
        btn->signal_clicked().connect(
            sigc::bind(sigc::bind(sigc::mem_fun(*this, &ControllerDialog::start_binding), action), p));
        m_bind_buttons[p * GAME_ACTION_COUNT + a] = btn;

        auto* clear_btn = Gtk::make_managed<Gtk::Button>("✕");
        clear_btn->set_tooltip_text(_("Clear this binding"));
        clear_btn->set_size_request(30, -1);
        clear_btn->signal_clicked().connect([this, p, action, bind_lbl]() {
            m_config.players[p].bindings.erase(action);
            bind_lbl->set_text("");
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

    // Prereglages : lier douze commandes une par une pour une manette
    // standard est un travail que le lanceur peut faire a la place du joueur.
    auto* preset_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    preset_row->pack_start(*Gtk::make_managed<Gtk::Label>(_("Preset:")), Gtk::PACK_SHRINK);

    auto* preset_combo = Gtk::make_managed<Gtk::ComboBoxText>();
    for (const auto& preset : controller_presets())
        preset_combo->append(preset.name, preset.name);
    preset_combo->set_active(0);
    preset_row->pack_start(*preset_combo, Gtk::PACK_SHRINK);

    auto* apply = Gtk::make_managed<Gtk::Button>(_("Apply"));
    apply->set_tooltip_text(
        _("A starting point, not a guarantee: Linux numbers the buttons in the "
          "order its driver declares them, and that order differs between "
          "controller families. Rebind any button that comes out wrong."));
    apply->signal_clicked().connect([this, p, preset_combo]() {
        const std::string chosen = preset_combo->get_active_id();
        for (const auto& preset : controller_presets()) {
            if (chosen != preset.name) continue;
            apply_preset(m_config.players[p], preset);
            // Les noms se relevent sur le clavier reellement branche : le
            // prereglage ne connait que des positions.
            for (auto& [action, binding] : m_config.players[p].bindings)
                if (binding.source == InputSource::KEY)
                    binding.key_name = key_name_for_code(binding.key);
            refresh_bindings(p);
            break;
        }
    });
    preset_row->pack_start(*apply, Gtk::PACK_SHRINK);
    tab_box->pack_start(*preset_row, Gtk::PACK_SHRINK);

    auto* clear_all = Gtk::make_managed<Gtk::Button>(_("Clear All Bindings"));
    clear_all->set_halign(Gtk::ALIGN_START);
    clear_all->signal_clicked().connect([this, p]() {
        m_config.players[p].bindings.clear();
        refresh_bindings(p);
    });
    tab_box->pack_start(*clear_all, Gtk::PACK_SHRINK);

    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL),
                        Gtk::PACK_SHRINK);
    tab_box->pack_start(*build_analog_section(p), Gtk::PACK_SHRINK);

    // Scrolled: the button grid plus the analog block is taller than the
    // dialog on a small screen, and a clipped Save button is unusable.
    auto* scroll = Gtk::make_managed<Gtk::ScrolledWindow>();
    scroll->set_policy(Gtk::POLICY_NEVER, Gtk::POLICY_AUTOMATIC);
    scroll->add(*tab_box);
    m_notebook.append_page(*scroll,
        Glib::ustring::compose(_("Player %1"), p + 1));
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
                      ? it->second.label() : "");
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

    // Le clavier suffit : exiger une manette ici empechait de configurer quoi
    // que ce soit sans en avoir une, alors que l'emulateur accepte les deux.
    // Une manette absente ou illisible n'est donc plus une erreur, seulement
    // une source de moins pendant l'attente.
    std::string device_path = m_config.players[p].device_path;
    m_bind_fd = device_path.empty() ? -1 : ControllerManager::open_device(device_path);

    // Drain stale events
    if (m_bind_fd >= 0) {
        InputBinding dummy;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(m_bind_fd, dummy)) break;
    }

    Gtk::Dialog wait_dlg(
        Glib::ustring::compose(_("Binding: %1"), _(game_action_name(action))),
        *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(320, 140);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    std::string dev_name = m_config.players[p].device_name;
    if (dev_name.empty()) dev_name = device_path;

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(
        m_bind_fd >= 0
            ? Glib::ustring::compose(_("Press a key, or a button on %1…"), dev_name)
            : Glib::ustring(_("Press a key…"))) + "</b>");
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20); info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    Gtk::Label* bind_lbl = m_binding_labels[p * GAME_ACTION_COUNT + static_cast<int>(action)];

    // Une touche pressee dans la fenetre d'attente vaut liaison, au meme titre
    // qu'un bouton de manette. Echap reste la sortie, sans quoi on ne pourrait
    // plus annuler une fois la capture lancee.
    wait_dlg.signal_key_press_event().connect(
        [this, p, action, &wait_dlg, bind_lbl](GdkEventKey* ev) -> bool {
            if (ev->keyval == GDK_KEY_Escape) return false;
            const int code = ControllerManager::fbneo_key_from_gtk(ev->hardware_keycode);
            if (code < 0) return true;                  // touche sans equivalent
            InputBinding result;
            result.valid  = true;
            result.source   = InputSource::KEY;
            result.key      = code;
            result.key_name = key_name_from_event(ev);
            m_config.players[p].bindings[action] = result;
            if (bind_lbl) bind_lbl->set_text(result.label());
            wait_dlg.response(Gtk::RESPONSE_OK);
            return true;
        }, false);

    m_poll_conn = Glib::signal_timeout().connect(
        [this, p, action, &wait_dlg, bind_lbl]() -> bool {
            if (m_bind_fd < 0) return true;             // clavier seul
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


// ── Analog tab ────────────────────────────────────────────────────────────

Gtk::Widget* ControllerDialog::build_analog_section(int p) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 6);

    auto* title = Gtk::make_managed<Gtk::Label>();
    title->set_markup("<b>" + Glib::Markup::escape_text(_("Analog controls")) + "</b>");
    title->set_xalign(0.0f);
    box->pack_start(*title, Gtk::PACK_SHRINK);

    auto* intro = Gtk::make_managed<Gtk::Label>();
    intro->set_markup("<i>" + Glib::Markup::escape_text(
        _("Some games have no D-pad: Out Run steers, Arkanoid uses a paddle, "
          "light-gun games aim. FBNeo leaves those on the keyboard arrows : "
          "these settings put them on your controller instead.")) + "</i>");
    intro->set_line_wrap(true);
    intro->set_xalign(0.0f);
    box->pack_start(*intro, Gtk::PACK_SHRINK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(10);
    grid->set_margin_top(4);

    int col = 0;
    for (const char* h : {N_("Control"), N_("Source"), N_("Axis"),
                          N_("Invert"), N_("Mode")}) {
        auto* lbl = Gtk::make_managed<Gtk::Label>();
        lbl->set_markup("<b>" + Glib::Markup::escape_text(_(h)) + "</b>");
        lbl->set_xalign(0.0f);
        grid->attach(*lbl, col++, 0, 1, 1);
    }

    for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
        auto role = static_cast<AnalogRole>(r);
        auto& w = m_analog_widgets[p][r];

        auto* name = Gtk::make_managed<Gtk::Label>(_(analog_role_name(role)));
        name->set_xalign(0.0f);
        grid->attach(*name, 0, r + 1, 1, 1);

        w.source = Gtk::make_managed<Gtk::ComboBoxText>();
        w.source->append("none",  _("Not used"));
        w.source->append("joy",   _("Joystick axis"));
        // Le clavier plutot que la souris : l'emulateur fait tourner un axe
        // avec deux touches, alors que son pilote SDL ne lit pas la souris du
        // tout. Proposer la souris revenait a offrir un reglage sans effet.
        w.source->append("keys",  _("Keyboard"));
        w.source->set_active_id("none");
        grid->attach(*w.source, 1, r + 1, 1, 1);

        w.axis = Gtk::make_managed<Gtk::SpinButton>();
        // Sans cela show_all_children() de la fenetre rendrait visibles les
        // deux champs, celui de la manette et celui du clavier, en annulant le
        // choix fait juste au-dessus.
        w.axis->set_no_show_all(true);
        w.axis->set_range(0, 15);
        w.axis->set_increments(1, 1);
        w.axis->set_width_chars(3);
        grid->attach(*w.axis, 2, r + 1, 1, 1);

        w.keys = Gtk::make_managed<Gtk::Button>(_("Set keys…"));
        w.keys->set_no_show_all(true);
        w.keys->set_tooltip_text(_("Two keys, one per direction."));
        grid->attach(*w.keys, 2, r + 1, 1, 1);

        w.invert = Gtk::make_managed<Gtk::CheckButton>();
        grid->attach(*w.invert, 3, r + 1, 1, 1);

        w.mode = Gtk::make_managed<Gtk::ComboBoxText>();
        w.mode->append("absolute", _("Absolute"));
        w.mode->append("relative", _("Relative"));
        w.mode->set_active_id("absolute");
        w.mode->set_tooltip_text(
            _("Absolute: the stick's position is the wheel's position : "
              "what a real wheel does. Relative: the stick turns a wheel "
              "that drifts back to centre."));
        grid->attach(*w.mode, 4, r + 1, 1, 1);

        auto sync = [this] { if (!m_analog_loading) analog_config_from_ui(); };
        // L'axe ne sert qu'a une manette, les touches qu'au clavier : montrer
        // les deux en permanence laissait croire qu'il fallait remplir tout.
        auto show_right_fields = [&w]() {
            const bool keyboard = w.source->get_active_id() == "keys";
            w.axis->set_visible(!keyboard);
            w.keys->set_visible(keyboard);
        };
        w.source->signal_changed().connect(show_right_fields);
        w.keys->signal_clicked().connect(
            [this, p, role]() { capture_analog_keys(p, role); });
        w.source->signal_changed().connect(sync);
        w.axis->signal_value_changed().connect(sync);
        w.invert->signal_toggled().connect(sync);
        w.mode->signal_changed().connect(sync);
    }
    box->pack_start(*grid, Gtk::PACK_SHRINK);
    return box;
}

void ControllerDialog::analog_ui_from_config() {
    m_analog_loading = true;
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p) {
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto role = static_cast<AnalogRole>(r);
            auto& w = m_analog_widgets[p][r];
            if (!w.source) continue;
            auto it = m_config.players[p].analog.find(role);
            // Player 1 falls back to the pad defaults so the page shows what
            // will actually happen, not an empty form that implies nothing is
            // bound. Player 2 starts unset: these games are single-control.
            AnalogBinding b = it != m_config.players[p].analog.end() ? it->second
                            : (p == 0 ? default_analog_binding(role) : AnalogBinding{});
            w.key_neg = b.key_neg;
            w.key_pos = b.key_pos;
            w.key_neg_name = b.key_neg_name;
            w.key_pos_name = b.key_pos_name;
            if (w.keys) {
                auto shown = [](int code, const std::string& name) {
                    return name.empty() ? key_label(code) : name;
                };
                w.keys->set_label(b.key_neg >= 0 && b.key_pos >= 0
                                  ? shown(b.key_neg, b.key_neg_name) + " / "
                                        + shown(b.key_pos, b.key_pos_name)
                                  : std::string(_("Set keys…")));
            }
            // Une configuration enregistree sur la souris retombe sur "aucun" :
            // l'entree n'existe plus dans la liste, et laisser le reglage
            // afficher un choix absent aurait montre une ligne vide.
            w.source->set_active_id(b.source == AnalogSource::KEY_PAIR ? "keys"
                                  : b.source == AnalogSource::JOY_AXIS ? "joy" : "none");
            w.axis->set_value(b.index < 0 ? 0 : b.index);
            w.invert->set_active(b.invert);
            w.mode->set_active_id(b.relative ? "relative" : "absolute");
        }
    }
    // Applique la visibilite apres remplissage : un axe pour la manette, deux
    // touches pour le clavier, jamais les deux a la fois.
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p)
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto& w = m_analog_widgets[p][r];
            if (!w.source || !w.axis || !w.keys) continue;
            const bool keyboard = w.source->get_active_id() == "keys";
            w.axis->set_visible(!keyboard);
            w.keys->set_visible(keyboard);
        }
    m_analog_loading = false;
}

// Demande les deux touches d'un axe, l'une apres l'autre. Deux captures
// separees plutot qu'une saisie libre : le joueur appuie sur la touche qu'il
// veut vraiment, et on enregistre le code que l'emulateur attend sans lui
// demander de le connaitre.
void ControllerDialog::capture_analog_keys(int player, AnalogRole role) {
    auto& w = m_analog_widgets[player][(int)role];

    int captured[2] = {-1, -1};
    std::string names[2];
    const char* prompts[2] = {
        N_("Press the key for one direction…"),
        N_("Now the key for the other direction…"),
    };

    for (int step = 0; step < 2; ++step) {
        Gtk::Dialog dlg(_(analog_role_name(role)), *this,
                        Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
        dlg.set_default_size(340, 130);
        dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

        auto* info = Gtk::make_managed<Gtk::Label>();
        info->set_markup("<b>" + Glib::Markup::escape_text(_(prompts[step])) + "</b>");
        info->set_margin_top(20);
        info->set_margin_bottom(10);
        dlg.get_content_area()->add(*info);
        dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dlg.show_all_children();

        dlg.signal_key_press_event().connect(
            [&dlg, &captured, &names, step](GdkEventKey* ev) -> bool {
                if (ev->keyval == GDK_KEY_Escape) return false;
                const int code = ControllerManager::fbneo_key_from_gtk(ev->hardware_keycode);
                if (code < 0) return true;
                captured[step] = code;
                names[step]    = key_name_from_event(ev);
                dlg.response(Gtk::RESPONSE_OK);
                return true;
            }, false);

        if (dlg.run() != Gtk::RESPONSE_OK || captured[step] < 0) return;
    }

    w.key_neg = captured[0];
    w.key_pos = captured[1];
    w.key_neg_name = names[0];
    w.key_pos_name = names[1];
    w.keys->set_label(names[0] + " / " + names[1]);
    analog_config_from_ui();
}

void ControllerDialog::analog_config_from_ui() {
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p) {
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto role = static_cast<AnalogRole>(r);
            auto& w = m_analog_widgets[p][r];
            if (!w.source) continue;
            std::string src = w.source->get_active_id();
            AnalogBinding b;
            b.source = src == "joy"   ? AnalogSource::JOY_AXIS
                     : src == "mouse" ? AnalogSource::MOUSE_AXIS
                                      : AnalogSource::NONE;
            b.index    = (int)w.axis->get_value();
            b.invert   = w.invert->get_active();
            b.relative = w.mode->get_active_id() == "relative";
            m_config.players[p].analog[role] = b;
        }
    }
}


// ── Identify a controller by pressing a button on it ──────────────────────

void ControllerDialog::identify_device(int p) {
    stop_binding();

    m_devices = ControllerManager::list_devices();
    if (m_devices.empty()) {
        Gtk::MessageDialog msg(*this, _("No controller found"),
                               false, Gtk::MESSAGE_WARNING, Gtk::BUTTONS_OK, true);
        msg.set_secondary_text(_("Plug a controller in, then press Identify again."));
        msg.run();
        return;
    }

    // Every device is opened at once and watched together: the whole point is
    // that we do not know which one the player is holding, so we cannot ask
    // them to pick it first.
    std::vector<int> fds;
    fds.reserve(m_devices.size());
    for (const auto& d : m_devices) fds.push_back(ControllerManager::open_device(d.path));

    // Drain events queued before the dialog opened, or a stick resting off
    // centre would answer instantly and pick the wrong pad.
    for (int fd : fds) {
        if (fd < 0) continue;
        InputBinding dummy;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(fd, dummy)) break;
    }

    Gtk::Dialog wait_dlg(_("Identify controller"), *this,
                         Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(360, 150);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(Glib::ustring::compose(
        _("Press a button on the controller you want for player %1…"), p + 1)) + "</b>");
    info->set_line_wrap(true);
    info->set_justify(Gtk::JUSTIFY_CENTER);
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20); info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    int found = -1;
    auto conn = Glib::signal_timeout().connect(
        [this, &fds, &found, &wait_dlg]() -> bool {
            for (size_t i = 0; i < fds.size(); ++i) {
                if (fds[i] < 0) continue;
                InputBinding result;
                if (ControllerManager::poll_event(fds[i], result)) {
                    found = (int)i;
                    wait_dlg.response(Gtk::RESPONSE_OK);
                    return false;
                }
            }
            return true;
        }, 50);

    wait_dlg.run();
    if (conn.connected()) conn.disconnect();
    for (int fd : fds) if (fd >= 0) ControllerManager::close_device(fd);

    if (found < 0) return;                 // cancelled, or nothing pressed
    // Selecting in the combo fires on_device_changed, which is what actually
    // records the choice : no need to touch m_config here.
    if (m_device_combos[p]) m_device_combos[p]->set_active_id(m_devices[found].path);
}
