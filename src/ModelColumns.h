// src/ModelColumns.h
#pragma once
#include <gtkmm.h>

struct ModelColumns : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> m_col_icon;
    Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    Gtk::TreeModelColumn<Glib::ustring> m_col_title;
    Gtk::TreeModelColumn<Glib::ustring> m_col_year;
    Gtk::TreeModelColumn<Glib::ustring> m_col_manufacturer;
    Gtk::TreeModelColumn<Glib::ustring> m_col_status;

    ModelColumns() {
        add(m_col_icon);
        add(m_col_name);
        add(m_col_title);
        add(m_col_year);
        add(m_col_manufacturer);
        add(m_col_status);
    }
};