// src/ScreenshotAssignDialog.h
//
// Shown after a play session if FBNeo's built-in F6 screenshot hotkey was
// used during it : lets the user pick which capture(s) become this game's
// Title/Preview artwork. Purely launcher-side: FBNeo itself is untouched,
// F6 already writes plain timestamped PNGs on its own.
#pragma once
#include <gtkmm.h>
#include <string>
#include <vector>

class ScreenshotAssignDialog : public Gtk::Dialog {
public:
    ScreenshotAssignDialog(Gtk::Window& parent, const std::vector<std::string>& screenshot_paths);
    virtual ~ScreenshotAssignDialog() = default;

    // Valid after run() returns Gtk::RESPONSE_OK : empty string means "none chosen".
    std::string get_title_choice() const { return m_title_choice; }
    std::string get_preview_choice() const { return m_preview_choice; }

private:
    struct Columns : public Gtk::TreeModel::ColumnRecord {
        Columns() { add(thumb); add(filename); add(full_path); add(is_title); add(is_preview); }
        Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> thumb;
        Gtk::TreeModelColumn<Glib::ustring> filename;
        Gtk::TreeModelColumn<std::string> full_path;
        Gtk::TreeModelColumn<bool> is_title;
        Gtk::TreeModelColumn<bool> is_preview;
    };

    void on_title_toggled(const Glib::ustring& path);
    void on_preview_toggled(const Glib::ustring& path);
    void on_save_clicked();

    Columns m_columns;
    Glib::RefPtr<Gtk::ListStore> m_model;
    Gtk::TreeView m_view;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box m_box{Gtk::ORIENTATION_VERTICAL, 10};
    Gtk::Label m_hint;

    std::string m_title_choice;
    std::string m_preview_choice;
};
