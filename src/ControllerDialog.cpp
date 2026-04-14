// src/ControllerDialog.cpp
#include "ControllerDialog.h"
#include <iostream>

ControllerDialog::ControllerDialog(Gtk::Window& parent,
                                   ControllerConfig& config,
                                   const std::string& config_path)
    : Gtk::Dialog("Controller Configuration", parent,
                  Gtk::DIALOG_DESTROY_WITH_PARENT)
    , m_config(config)
    , m_config_path(config_path)
{
    set_default_size(520, 540);
    set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    m_binding_labels.resize(2 * GAME_ACTION_COUNT, nullptr);
    m_bind_buttons.resize(2 * GAME_ACTION_COUNT, nullptr);

    m_devices = ControllerManager::list_devices();

    build_player_tab(0);
    build_player_tab(1);

    get_content_area()->pack_start(m_notebook, Gtk::PACK_EXPAND_WIDGET);
    get_content_area()->set_spacing(8);

    // Buttons
    add_button("Cancel", Gtk::RESPONSE_CANCEL);
    auto* save_btn = add_button("Save", Gtk::RESPONSE_OK);
    save_btn->signal_clicked().connect(
        sigc::mem_fun(*this, &ControllerDialog::on_save_clicked));

    set_default_response(Gtk::RESPONSE_OK);
    show_all_children();
}

ControllerDialog::~ControllerDialog() {
    stop_binding();
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
    if (!cur.empty())
        combo->set_active_id(cur);
    else
        combo->set_active(0);

    combo->set_hexpand(true);
    combo->signal_changed().connect(
        sigc::bind(sigc::mem_fun(*this, &ControllerDialog::on_device_changed), p));
    dev_row->pack_start(*combo, Gtk::PACK_EXPAND_WIDGET);
    m_device_combos[p] = combo;

    // Refresh button
    auto* refresh_btn = Gtk::make_managed<Gtk::Button>("↻");
    refresh_btn->set_tooltip_text("Refresh controller list");
    refresh_btn->signal_clicked().connect([this, p]() {
        m_devices = ControllerManager::list_devices();
        // Rebuild combo
        std::string cur_id = m_device_combos[p]->get_active_id();
        m_device_combos[p]->remove_all();
        m_device_combos[p]->append("", "— None —");
        for (const auto& d : m_devices)
            m_device_combos[p]->append(d.path, d.name + "  (" + d.path + ")");
        if (!cur_id.empty())
            m_device_combos[p]->set_active_id(cur_id);
        else
            m_device_combos[p]->set_active(0);
    });
    dev_row->pack_start(*refresh_btn, Gtk::PACK_SHRINK);
    tab_box->pack_start(*dev_row, Gtk::PACK_SHRINK);

    // ── Column headers ───────────────────────────────────────────────────
    auto* sep = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL);
    tab_box->pack_start(*sep, Gtk::PACK_SHRINK);

    auto* hdr = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 0);
    auto* h1  = Gtk::make_managed<Gtk::Label>();
    h1->set_markup("<b>Action</b>");
    h1->set_halign(Gtk::ALIGN_START);
    h1->set_size_request(140, -1);
    auto* h2  = Gtk::make_managed<Gtk::Label>();
    h2->set_markup("<b>Assigned</b>");
    h2->set_halign(Gtk::ALIGN_START);
    h2->set_size_request(120, -1);
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
            sigc::bind(sigc::bind(
                sigc::mem_fun(*this, &ControllerDialog::start_binding),
                action), p));
        m_bind_buttons[p * GAME_ACTION_COUNT + a] = btn;

        auto* clear_btn = Gtk::make_managed<Gtk::Button>("✕");
        clear_btn->set_tooltip_text("Clear this binding");
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
    auto* sep2 = Gtk::make_managed<Gtk::Separator>(Gtk::ORIENTATION_HORIZONTAL);
    tab_box->pack_start(*sep2, Gtk::PACK_SHRINK);

    auto* clear_all = Gtk::make_managed<Gtk::Button>("Clear All Bindings");
    clear_all->set_halign(Gtk::ALIGN_START);
    clear_all->signal_clicked().connect([this, p]() {
        m_config.players[p].bindings.clear();
        refresh_bindings(p);
    });
    tab_box->pack_start(*clear_all, Gtk::PACK_SHRINK);

    std::string tab_label = "Player " + std::to_string(p + 1);
    m_notebook.append_page(*tab_box, tab_label);

    // Initialize labels from current config
    refresh_bindings(p);
}

// ── Refresh binding labels from m_config ─────────────────────────────────

void ControllerDialog::refresh_bindings(int p) {
    for (int a = 0; a < GAME_ACTION_COUNT; ++a) {
        Gtk::Label* lbl = m_binding_labels[p * GAME_ACTION_COUNT + a];
        if (!lbl) continue;
        GameAction action = static_cast<GameAction>(a);
        auto it = m_config.players[p].bindings.find(action);
        lbl->set_text(it != m_config.players[p].bindings.end()
                      ? it->second.label()
                      : "—");
    }
}

// ── Device combo changed ──────────────────────────────────────────────────

void ControllerDialog::on_device_changed(int p) {
    std::string path = m_device_combos[p]->get_active_id();
    m_config.players[p].device_path = path;
    // Update device_name
    for (const auto& d : m_devices) {
        if (d.path == path) {
            m_config.players[p].device_name = d.name;
            break;
        }
    }
    if (path.empty()) m_config.players[p].device_name = "";
}

// ── Bind: start waiting for input ─────────────────────────────────────────

void ControllerDialog::start_binding(int p, GameAction action) {
    stop_binding(); // cancel any previous bind

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
        msg.set_secondary_text("Could not open " + device_path + ".\nCheck permissions (/dev/input group).");
        msg.run();
        return;
    }

    // Drain any pending events so we don't pick up stale state
    {
        InputBinding dummy;
        for (int i = 0; i < 50; ++i)
            if (!ControllerManager::poll_event(m_bind_fd, dummy)) break;
    }

    // Build a modal "press a button" dialog on the stack so response() works
    Gtk::Dialog wait_dlg(
        "Binding: " + std::string(game_action_name(action)),
        *this, Gtk::DIALOG_MODAL | Gtk::DIALOG_DESTROY_WITH_PARENT);
    wait_dlg.set_default_size(320, 140);
    wait_dlg.set_position(Gtk::WIN_POS_CENTER_ON_PARENT);

    std::string dev_name = m_config.players[p].device_name;
    if (dev_name.empty()) dev_name = device_path;

    auto* info = Gtk::make_managed<Gtk::Label>();
    info->set_markup(
        "<b>Press a button or move a stick\non <i>"
        + Glib::Markup::escape_text(dev_name) + "</i>…</b>");
    info->set_halign(Gtk::ALIGN_CENTER);
    info->set_margin_top(20);
    info->set_margin_bottom(10);
    wait_dlg.get_content_area()->add(*info);
    wait_dlg.add_button("Cancel", Gtk::RESPONSE_CANCEL);
    wait_dlg.show_all_children();

    Gtk::Label* bind_lbl = m_binding_labels[p * GAME_ACTION_COUNT
                                             + static_cast<int>(action)];

    // Poll every 50 ms; close the dialog as soon as input is detected
    m_poll_conn = Glib::signal_timeout().connect(
        [this, p, action, &wait_dlg, bind_lbl]() -> bool {
            InputBinding result;
            if (ControllerManager::poll_event(m_bind_fd, result)) {
                m_config.players[p].bindings[action] = result;
                if (bind_lbl) bind_lbl->set_text(result.label());
                wait_dlg.response(Gtk::RESPONSE_OK); // close dialog
                return false; // stop timeout
            }
            return true; // keep polling
        }, 50);

    wait_dlg.run(); // blocks until input detected or Cancel clicked
    stop_binding(); // disconnect timeout + close fd
}

void ControllerDialog::stop_binding() {
    if (m_poll_conn.connected()) m_poll_conn.disconnect();
    ControllerManager::close_device(m_bind_fd);
    m_bind_fd = -1;
}

// ── Save ─────────────────────────────────────────────────────────────────

void ControllerDialog::on_save_clicked() {
    ControllerManager::save_config(m_config, m_config_path);
    std::cout << "[ControllerDialog] Config saved to " << m_config_path << std::endl;

    // Push bindings to FBNeo's own config so they take effect on next launch
    std::string fbneo_dir = ControllerManager::get_fbneo_config_dir();
    ControllerManager::write_fbneo_config(m_config, fbneo_dir);
}
