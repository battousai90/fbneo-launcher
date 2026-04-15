// src/ControllerDialog.h
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>
#include <map>
#include "ControllerConfig.h"
#include "ControllerManager.h"

class ControllerDialog : public Gtk::Dialog {
public:
    // Takes a copy of all profiles + the active profile name.
    // On Save, writes everything to config_path.
    ControllerDialog(Gtk::Window& parent,
                     const std::map<std::string, ControllerConfig>& profiles,
                     const std::string& active_profile,
                     const std::string& config_path);
    ~ControllerDialog();

private:
    // ── Profile state ─────────────────────────────────────────────────────
    std::map<std::string, ControllerConfig> m_profiles;
    std::string                             m_active_profile_name;
    std::string                             m_config_path;
    bool                                    m_profile_switching{false};

    // Working copy of the active profile
    ControllerConfig m_config;

    // ── Profile bar widgets ───────────────────────────────────────────────
    Gtk::Box          m_profile_bar{Gtk::ORIENTATION_HORIZONTAL, 6};
    Gtk::Label        m_profile_label{"Profile:"};
    Gtk::ComboBoxText m_profile_combo;
    Gtk::Button       m_btn_new{"New…"};
    Gtk::Button       m_btn_rename{"Rename…"};
    Gtk::Button       m_btn_delete{"Delete"};

    // ── Player tabs ───────────────────────────────────────────────────────
    Gtk::Notebook              m_notebook;
    std::vector<JoystickInfo>  m_devices;
    Gtk::ComboBoxText*         m_device_combos[2]{};

    // Binding labels / buttons: indexed [player * GAME_ACTION_COUNT + action]
    std::vector<Gtk::Label*>   m_binding_labels;
    std::vector<Gtk::Button*>  m_bind_buttons;

    // Active "press a button" state
    int              m_bind_fd  = -1;
    sigc::connection m_poll_conn;

    // ── UI builders ───────────────────────────────────────────────────────
    void build_profile_bar();
    void build_player_tab(int p);

    // ── Profile management ────────────────────────────────────────────────
    void populate_profile_combo();
    void save_active_to_profiles();   // sync m_config → m_profiles[m_active]
    void load_profile(const std::string& name); // sync m_profiles[name] → m_config + refresh UI

    void on_profile_changed();
    void on_new_profile_clicked();
    void on_rename_profile_clicked();
    void on_delete_profile_clicked();

    // ── Binding helpers ───────────────────────────────────────────────────
    void refresh_bindings(int p);
    void refresh_from_config();       // update device combos + all labels from m_config
    void on_device_changed(int p);
    void start_binding(int p, GameAction action);
    void stop_binding();
    void on_save_clicked();
};
