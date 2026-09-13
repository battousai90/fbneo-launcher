#pragma once
#include <gdkmm/pixbuf.h>
#include <gdkmm/rgba.h>
#include <string>

class IconManager {
public:
    static Glib::RefPtr<Gdk::Pixbuf> get_status_icon(const std::string& status);

    // Load an icon from the data directory, e.g. "icons/download.svg".
    //
    // Gdk::Pixbuf::create_from_file *throws* when the file is missing, and an
    // uncaught Glib::FileError in a widget constructor takes the whole
    // application down. A missing decorative icon should cost a button its
    // picture, nothing more : so this returns an empty RefPtr instead, which
    // Gtk::Image accepts happily.
    static Glib::RefPtr<Gdk::Pixbuf> load(const std::string& subpath, int w = 16, int h = 16);

    /* Un pictogramme monochrome, dans une couleur donnee.
     *
     * Les traces bc-*.svg sont dessines en blanc pur : c'est leur seule
     * couleur, et elle se substitue. On rend le meme fichier en encre
     * normale, attenuee ou accent selon le contexte, et le theme clair n'a
     * plus d'icones blanches invisibles. Un SVG qui porte d'autres couleurs
     * (manettes, pastilles d'etat) n'est pas monochrome : il est rendu tel
     * quel. Mis en cache par (fichier, taille, couleur). */
    static Glib::RefPtr<Gdk::Pixbuf> load_tinted(const std::string& subpath, int w, int h, const Gdk::RGBA& colour);
    // Vrai si le fichier ne contient que du blanc : il peut etre teinte.
    static bool is_monochrome(const std::string& subpath);
};