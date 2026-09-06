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
// Etiquette d'une liaison analogique, telle que le joueur la lit.
static std::string analog_label(const AnalogBinding& b) {
    if (!b.is_set()) return _("Not used");
    if (b.source == AnalogSource::KEY_PAIR) {
        auto shown = [](int code, const std::string& name) {
            return name.empty() ? key_label(code) : name;
        };
        return shown(b.key_neg, b.key_neg_name) + " / " + shown(b.key_pos, b.key_pos_name);
    }
    return Glib::ustring::compose(_("Axis %1"), b.index).raw();
}

}  // namespace

ControllerDialog::ControllerDialog(const std::map<std::string, ControllerConfig>& profiles,
                                   const std::string& active_profile,
                                   const std::string& config_path)
    : Gtk::Dialog()
    , m_profiles(profiles)
    , m_active_profile_name(active_profile)
    , m_config_path(config_path)
{
    // Widgets carry English literals in the header as a fallback; the
    // translated text can only be applied once the catalogue is loaded.
    /* Assez grande d'emblee.
     *
     * La fenetre s'ouvrait a sa taille naturelle, c'est-a-dire au plus
     * serre : douze lignes de liaisons, deux onglets et une section
     * analogique tenaient dans une boite ou tout se touchait. */
    set_title(_("Controller Configuration"));
    set_default_size(940, 760);

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

    /* Trois boutons deviennent trois entrees d'un menu.
     *
     * Creer, renommer et supprimer un profil sont des gestes RARES : les
     * garder en permanence a cote du selecteur donnait autant de poids a
     * « supprimer » qu'a « choisir », alors que l'un se fait tous les jours
     * et l'autre presque jamais. Les gestionnaires ne changent pas.
     */
    m_mi_new.set_label(_("New profile..."));
    m_mi_rename.set_label(_("Rename..."));
    m_mi_delete.set_label(_("Delete"));
    m_mi_new.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_new_profile_clicked));
    m_mi_rename.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_rename_profile_clicked));
    m_mi_delete.signal_activate().connect(sigc::mem_fun(*this, &ControllerDialog::on_delete_profile_clicked));
    m_profile_menu.append(m_mi_new);
    m_profile_menu.append(m_mi_rename);
    m_profile_menu.append(*Gtk::make_managed<Gtk::SeparatorMenuItem>());
    m_profile_menu.append(m_mi_delete);
    m_profile_menu.show_all();
    m_btn_profile_more.set_label("\u22ef");
    m_btn_profile_more.set_tooltip_text(_("Profile actions"));
    m_btn_profile_more.set_popup(m_profile_menu);
    m_profile_bar.pack_start(m_btn_profile_more, Gtk::PACK_SHRINK);
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

    // Juste apres le choix de la manette : brancher, designer, puis laisser
    // le lanceur poser les douze liaisons est une seule et meme demarche.
    auto* auto_btn = Gtk::make_managed<Gtk::Button>(_("Configure automatically"));
    auto_btn->get_style_context()->add_class("suggested-action");
    auto_btn->set_tooltip_text(
        _("Press each control in turn. Close a prompt to skip that control."));
    auto_btn->signal_clicked().connect([this, p]() { run_auto_configure(p); });
    dev_row->pack_start(*auto_btn, Gtk::PACK_SHRINK);

    tab_box->pack_start(*dev_row, Gtk::PACK_SHRINK);

    // Juste sous le choix de la manette, parce que c'est la suite immediate
    // du meme geste : on branche une manette, on dit laquelle, et le profil
    // se remplit. Lier douze commandes une par une pour un modele courant est
    // un travail que le lanceur peut faire a la place du joueur.
    auto* preset_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 8);
    preset_row->pack_start(*Gtk::make_managed<Gtk::Label>(_("Preset")), Gtk::PACK_SHRINK);

    auto* preset_combo = Gtk::make_managed<Gtk::ComboBoxText>();
    for (const auto& preset : controller_presets())
        preset_combo->append(preset.name, preset.name);
    preset_combo->set_active(0);
    preset_row->pack_start(*preset_combo, Gtk::PACK_SHRINK);

    auto* apply = Gtk::make_managed<Gtk::Button>(_("Apply"));
    apply->set_tooltip_text(
        _("Fills this profile's buttons and axes. A starting point, not a "
          "guarantee: Linux numbers the buttons in the order its driver "
          "declares them, and that order differs between controller families. "
          "Rebind anything that comes out wrong."));
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

    /* Trois groupes, comme la maquette : Directions, Boutons, Systeme.
     *
     * Douze lignes d'affilee se lisaient comme une liste sans structure,
     * alors qu'elles se rangent naturellement en trois familles qu'un joueur
     * distingue d'instinct sur sa manette. Le regroupement est PUREMENT
     * visuel : l'ordre de l'enumeration ne change pas, et l'assistant le
     * parcourt toujours de la meme facon.
     */
    struct Group { const char* title; int first, last; };
    const Group groups[] = {
        { N_("Directions"), (int)GameAction::UP,      (int)GameAction::RIGHT   },
        { N_("Buttons"),    (int)GameAction::BUTTON1, (int)GameAction::BUTTON6 },
        { N_("System"),     (int)GameAction::START,   (int)GameAction::COIN    },
    };

    int row = 0;
    for (const auto& g : groups) {
        auto* head = Gtk::make_managed<Gtk::Label>();
        head->set_markup("<b>" + Glib::Markup::escape_text(_(g.title)) + "</b>");
        head->set_halign(Gtk::ALIGN_START);
        head->set_margin_top(row == 0 ? 0 : 14);
        head->set_margin_bottom(4);
        grid->attach(*head, 0, row++, 2, 1);

        for (int a = g.first; a <= g.last; ++a) {
            GameAction action = static_cast<GameAction>(a);

            auto* name_lbl = Gtk::make_managed<Gtk::Label>(_(game_action_name(action)));
            name_lbl->set_halign(Gtk::ALIGN_START);
            name_lbl->set_size_request(160, -1);

            /* La valeur EST le bouton : un clic pour lier, Suppr pour
             * effacer, comme dans n'importe quelle table de raccourcis. */
            auto* bind_lbl = Gtk::make_managed<Gtk::Label>("");
            bind_lbl->set_halign(Gtk::ALIGN_START);
            bind_lbl->set_ellipsize(Pango::ELLIPSIZE_END);
            m_binding_labels[p * GAME_ACTION_COUNT + a] = bind_lbl;

            auto* btn = Gtk::make_managed<Gtk::Button>();
            btn->add(*bind_lbl);
            btn->set_size_request(220, -1);
            btn->set_tooltip_text(_("Click to bind, Delete to clear"));
            btn->get_style_context()->add_class("bind-cell");
            btn->signal_clicked().connect(
                sigc::bind(sigc::bind(sigc::mem_fun(*this, &ControllerDialog::start_binding), action), p));
            btn->add_events(Gdk::KEY_PRESS_MASK);
            btn->signal_key_press_event().connect([this, p, action, bind_lbl](GdkEventKey* ev) {
                if (ev->keyval != GDK_KEY_Delete && ev->keyval != GDK_KEY_BackSpace)
                    return false;
                m_config.players[p].bindings.erase(action);
                bind_lbl->set_text(_("Not set"));
                return true;
            }, false);
            m_bind_buttons[p * GAME_ACTION_COUNT + a] = btn;

            grid->attach(*name_lbl, 0, row, 1, 1);
            grid->attach(*btn,      1, row, 1, 1);
            ++row;
        }
    }

    scrolled->add(*grid);
    tab_box->pack_start(*scrolled, Gtk::PACK_EXPAND_WIDGET);

    // ── Clear all button ─────────────────────────────────────────────────
    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL), Gtk::PACK_SHRINK);


    auto* clear_all = Gtk::make_managed<Gtk::Button>(_("Clear All Bindings"));
    clear_all->set_halign(Gtk::ALIGN_START);
    clear_all->signal_clicked().connect([this, p]() {
        m_config.players[p].bindings.clear();
        refresh_bindings(p);
    });
    tab_box->pack_start(*clear_all, Gtk::PACK_SHRINK);

    tab_box->pack_start(*Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL),
                        Gtk::PACK_SHRINK);
    /* Repliee par defaut.
     *
     * Volant, pedales et visee ne concernent qu'une poignee de jeux, mais
     * la section occupait le bas de chaque onglet pour tout le monde. Elle
     * reste a un clic pour qui en a besoin, et disparait pour les autres.
     */
    auto* analog_exp = Gtk::make_managed<Gtk::Expander>(
        std::string("<b>") + _("Advanced / Analog controls") + "</b>");
    analog_exp->set_use_markup(true);
    analog_exp->set_expanded(false);
    analog_exp->set_tooltip_text(
        _("Steering wheels, light guns, paddles. Not needed for most games."));
    analog_exp->add(*build_analog_section(p));
    tab_box->pack_start(*analog_exp, Gtk::PACK_SHRINK);

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
        // « Not set » plutot qu'une case vide : maintenant que la valeur est
        // le bouton, une case vide ressemble a un bouton sans libelle et on
        // ne devine pas qu'elle se clique.
        lbl->set_text(it != m_config.players[p].bindings.end()
                      ? it->second.label() : std::string(_("Not set")));
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
        _("Some games have no D-pad: Out Run steers, After Burner has a stick, "
          "Arkanoid uses a paddle. Bind them the same way as a button.")) + "</i>");
    intro->set_line_wrap(true);
    intro->set_xalign(0.0f);
    box->pack_start(*intro, Gtk::PACK_SHRINK);

    auto* grid = Gtk::make_managed<Gtk::Grid>();
    grid->set_row_spacing(6);
    grid->set_column_spacing(10);
    grid->set_margin_top(4);

    // Une seule colonne a remplir. L'ancienne demandait une source, un numero
    // d'axe, une inversion et un mode : quatre reglages a comprendre pour
    // designer un stick, alors qu'il suffit de le bouger. Le numero d'axe et
    // le mode se deduisent de ce qu'on capture, il ne reste que l'inversion,
    // qui elle se voit tout de suite quand on joue.
    int col = 0;
    for (const char* h : {N_("Control"), N_("Bound to"), N_("Invert")}) {
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

        w.bind = Gtk::make_managed<Gtk::Button>(_("Not used"));
        w.bind->set_hexpand(true);
        w.bind->signal_clicked().connect(
            [this, p, role]() { capture_analog(p, role); });
        grid->attach(*w.bind, 1, r + 1, 1, 1);

        w.invert = Gtk::make_managed<Gtk::CheckButton>();
        w.invert->set_tooltip_text(_("Tick this if the control goes the wrong way."));
        w.invert->signal_toggled().connect(
            [this] { if (!m_analog_loading) analog_config_from_ui(); });
        grid->attach(*w.invert, 2, r + 1, 1, 1);
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
            if (!w.bind) continue;
            auto it = m_config.players[p].analog.find(role);
            // Le joueur 1 part des valeurs d'origine, le joueur 2 de rien :
            // une deuxieme manette est rarement branchee, et proposer des
            // liaisons pour un appareil absent trompe sur ce qui est actif.
            AnalogBinding b = it != m_config.players[p].analog.end() ? it->second
                            : (p == 0 ? default_analog_binding(role) : AnalogBinding{});
            w.value = b;
            w.bind->set_label(analog_label(b));
            w.invert->set_active(b.invert);
        }
    }
    m_analog_loading = false;
}

// Demande les deux touches d'un axe, l'une apres l'autre. Deux captures
// separees plutot qu'une saisie libre : le joueur appuie sur la touche qu'il
// veut vraiment, et on enregistre le code que l'emulateur attend sans lui
// demander de le connaitre.

// Lie une commande analogique en la faisant bouger, comme on lie un bouton en
// appuyant dessus. Un axe de manette suffit ; au clavier il en faut deux, une
// touche par sens, et l'axe doit revenir au centre quand on relache, ce que le
// code choisit tout seul plutot que de le demander.
void ControllerDialog::capture_analog(int player, AnalogRole role) {
    stop_binding();
    auto& w = m_analog_widgets[player][(int)role];

    const std::string device_path = m_config.players[player].device_path;
    m_bind_fd = device_path.empty() ? -1 : ControllerManager::open_device(device_path);
    if (m_bind_fd >= 0) {
        InputBinding drain;
        for (int i = 0; i < 50; ++i) if (!ControllerManager::poll_event(m_bind_fd, drain)) break;
    }

    Gtk::Dialog dlg(_(analog_role_name(role)), *this,
                    Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    dlg.set_default_size(380, 150);
    dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup("<b>" + Glib::Markup::escape_text(
        _("Move a stick or a trigger, or press a key…")) + "</b>");
    info->set_line_wrap(true);
    info->set_margin_top(20);
    info->set_margin_bottom(10);
    dlg.get_content_area()->add(*info);
    dlg.add_button(_("Not used"), Gtk::RESPONSE_REJECT);
    dlg.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dlg.show_all_children();

    AnalogBinding captured;
    int first_key = -1;
    std::string first_name;

    dlg.signal_key_press_event().connect(
        [&](GdkEventKey* ev) -> bool {
            if (ev->keyval == GDK_KEY_Escape) return false;
            const int code = ControllerManager::fbneo_key_from_gtk(ev->hardware_keycode);
            if (code < 0 || code == first_key) return true;
            if (first_key < 0) {
                first_key  = code;
                first_name = key_name_from_event(ev);
                info->set_markup("<b>" + Glib::Markup::escape_text(
                    _("Now the key for the other direction…")) + "</b>");
                return true;
            }
            captured.source   = AnalogSource::KEY_PAIR;
            captured.relative = true;          // le clavier ne tient pas une position
            captured.key_neg  = first_key;
            captured.key_neg_name = first_name;
            captured.key_pos  = code;
            captured.key_pos_name = key_name_from_event(ev);
            dlg.response(Gtk::RESPONSE_OK);
            return true;
        }, false);

    m_poll_conn = Glib::signal_timeout().connect([&]() -> bool {
        if (m_bind_fd < 0) return true;
        InputBinding event;
        if (ControllerManager::poll_event(m_bind_fd, event) && event.is_axis) {
            captured.source   = AnalogSource::JOY_AXIS;
            captured.index    = event.axis;
            captured.relative = false;         // la position du stick EST celle du volant
            dlg.response(Gtk::RESPONSE_OK);
            return false;
        }
        return true;
    }, 50);

    const int answer = dlg.run();
    stop_binding();

    if (answer == Gtk::RESPONSE_REJECT) captured = AnalogBinding{};
    else if (answer != Gtk::RESPONSE_OK || !captured.is_set()) return;

    captured.invert = w.invert->get_active();
    w.value = captured;
    w.bind->set_label(analog_label(captured));
    analog_config_from_ui();
}

void ControllerDialog::analog_config_from_ui() {
    for (int p = 0; p < 2 && p < (int)m_config.players.size(); ++p) {
        for (int r = 0; r < ANALOG_ROLE_COUNT; ++r) {
            auto& w = m_analog_widgets[p][r];
            if (!w.bind) continue;
            AnalogBinding b = w.value;
            b.invert = w.invert->get_active();
            m_config.players[p].analog[static_cast<AnalogRole>(r)] = b;
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


/* Assistant de configuration.
 *
 * Enchaine les douze commandes dans l'ordre de l'enumeration, qui est aussi
 * l'ordre naturel d'une manette : directions, boutons, Start, Coin. La
 * capture existante se ferme d'elle-meme des qu'une entree est detectee,
 * l'enchainement est donc automatique sans code supplementaire.
 *
 * Trois garanties, apprises en ecrivant le plan :
 *  - on PEUT passer une commande. Tous les pads n'ont pas six boutons, et
 *    bloquer sur « Button 6 » condamnerait l'assistant a moitie parcours ;
 *  - rien n'est ecrit dans le profil avant la fin. On part d'une copie et on
 *    restitue l'ancienne configuration si le joueur renonce ;
 *  - le resume final dit ce qui a ete lie, parce qu'apres douze pressions on
 *    ne se souvient plus de ce qu'on a saute.
 */
void ControllerDialog::run_auto_configure(int p) {
    const PlayerConfig backup = m_config.players[p];

    m_config.players[p].bindings.clear();
    refresh_bindings(p);

    int bound = 0, skipped = 0;
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        const GameAction action = static_cast<GameAction>(a);
        start_binding(p, action);
        // La capture ne rend rien : on regarde si elle a effectivement pose
        // une liaison. Absente, c'est que le joueur a ferme la fenetre, ce
        // qu'on interprete comme « cette commande, je la saute ».
        if (m_config.players[p].bindings.count(action)) ++bound;
        else                                            ++skipped;
    }

    Gtk::MessageDialog done(
        *this,
        Glib::ustring::compose(_("%1 controls bound, %2 skipped."),
                               bound, skipped),
        false, Gtk::MESSAGE_QUESTION, Gtk::BUTTONS_NONE, true);
    done.set_secondary_text(_("Keep this configuration?"));
    done.add_button(_("Discard"), Gtk::RESPONSE_CANCEL);
    done.add_button(_("Keep"),    Gtk::RESPONSE_OK);
    if (done.run() != Gtk::RESPONSE_OK) {
        m_config.players[p] = backup;   // l'ancienne configuration revient intacte
    }
    refresh_bindings(p);
}
