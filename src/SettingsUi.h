// src/SettingsUi.h
//
// Les briques communes de l'ecran des reglages.
//
// Les quatre pages montrent les memes objets : des cartes titrees, des lignes
// « pictogramme / intitule / explication / controle », des tuiles portant une
// icone, des boutons a pictogramme et des pastilles d'etat. Les fabriquer ici
// une fois est la seule facon d'obtenir quatre pages qui appartiennent au meme
// systeme : une hauteur de ligne corrigee a un endroit se corrige partout, et
// une page ajoutee plus tard nait deja conforme.
//
// La matiere vient du CSS (.set-* dans style-common.css et style-dark.css) ;
// ce fichier ne pose que la geometrie et l'assemblage. Aucune couleur n'est
// ecrite en dur ici.
#pragma once

#include <gtkmm.h>
#include <string>

namespace SettingsUi {

// Les mesures partagees. Un chiffre recopie a la main dans une page est un
// alignement qui finira par diverger des trois autres.
constexpr int kIconSection = 18;  // pictogramme d'un titre de carte
constexpr int kIconRow     = 17;  // pictogramme d'une ligne de reglage
constexpr int kIconButton  = 17;  // pictogramme dans un bouton
constexpr int kTileSection = 32;  // tuile carree portant le pictogramme de section
constexpr int kTileRow     = 28;
constexpr int kFieldWidth  = 300; // largeur d'une liste deroulante / d'un champ
constexpr int kCardSpacing = 11;  // entre deux cartes d'une page

// Une image chargee depuis assets/icons, deja centree.
Gtk::Image* image(const std::string& icon_file, int size);

// Le pictogramme dans son carre arrondi. `accent` peint la tuile en violet :
// reserve a l'en-tete de la fenetre, pour ne pas banaliser l'accent.
Gtk::Widget* tile(const std::string& icon_file, int icon_size, int box_size,
                  bool accent = false);

// Une carte : cadre arrondi, en-tete (tuile, titre, sous-titre, et de la place
// a droite pour des actions), puis un corps a remplir.
struct Card {
    Gtk::Box* frame = nullptr;  // ce qu'on empaquete dans la page
    Gtk::Box* body  = nullptr;  // ou poser le contenu
    Gtk::Box* head  = nullptr;  // pack_end pour une action alignee sur le titre
};
Card card(const std::string& icon_file, const std::string& title,
          const std::string& subtitle);

// Le cadre interieur qui regroupe des lignes, avec un filet entre chacune.
Gtk::Box* rows();
void      add_row(Gtk::Box* container, Gtk::Widget& row);

// Une ligne de reglage. `trailing` est le controle pose a droite ; il peut
// etre nul quand la ligne ne fait qu'informer.
Gtk::Widget* row(const std::string& icon_file, const std::string& title,
                 const std::string& subtitle, Gtk::Widget* trailing);

// Un bouton a pictogramme. `accent` donne le traitement du bouton principal
// (celui de Play et de Save), `danger` celui d'une action destructrice.
enum class Tone { Normal, Accent, Danger };
Gtk::Button* button(const std::string& label, const std::string& icon_file = "",
                    Tone tone = Tone::Normal);

// Un texte d'etat, precede ou non d'une pastille ronde.
enum class State { Ok, Warn, Error, Muted };
Gtk::Label*  status_label(const std::string& text, State state);
Gtk::Widget* status_dot(const std::string& text, State state);

// Les etiquettes courantes, pour ne pas repeter xalign + classe partout.
Gtk::Label* title_label(const std::string& text);
Gtk::Label* sub_label(const std::string& text);
Gtk::Label* card_title_label(const std::string& text);

// Un filet horizontal de 1 px, a la couleur des separateurs de l'ecran.
Gtk::Widget* hairline();

}  // namespace SettingsUi
