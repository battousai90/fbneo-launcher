// src/ModelColumns.h
#pragma once
#include <gtkmm.h>

struct ModelColumns : public Gtk::TreeModel::ColumnRecord {
    Gtk::TreeModelColumn<Glib::RefPtr<Gdk::Pixbuf>> m_col_icon;
    Gtk::TreeModelColumn<Glib::ustring> m_col_name;
    Gtk::TreeModelColumn<Glib::ustring> m_col_title;
    Gtk::TreeModelColumn<Glib::ustring> m_col_year;
    Gtk::TreeModelColumn<Glib::ustring> m_col_manufacturer;
    Gtk::TreeModelColumn<Glib::ustring> m_col_system;
    Gtk::TreeModelColumn<Glib::ustring> m_col_status;
    
    // Video columns
    Gtk::TreeModelColumn<Glib::ustring> m_col_video_type;
    Gtk::TreeModelColumn<Glib::ustring> m_col_orientation;
    Gtk::TreeModelColumn<Glib::ustring> m_col_width;
    Gtk::TreeModelColumn<Glib::ustring> m_col_height;
    Gtk::TreeModelColumn<Glib::ustring> m_col_aspect;
    
    // Driver column
    Gtk::TreeModelColumn<Glib::ustring> m_col_driver_status;
    
    // Additional columns
    Gtk::TreeModelColumn<Glib::ustring> m_col_comment;
    Gtk::TreeModelColumn<Glib::ustring> m_col_cloneof;
    Gtk::TreeModelColumn<Glib::ustring> m_col_sourcefile;

    ModelColumns() {
        add(m_col_icon);
        add(m_col_name);
        add(m_col_title);
        add(m_col_year);
        add(m_col_manufacturer);
        add(m_col_system);
        add(m_col_status);
        add(m_col_video_type);
        add(m_col_orientation);
        add(m_col_width);
        add(m_col_height);
        add(m_col_aspect);
        add(m_col_driver_status);
        add(m_col_comment);
        add(m_col_cloneof);
        add(m_col_sourcefile);
    }
};