// src/SettingsUi.h
//
// Les briques communes des ecrans de Bootcade : reglages, manette, gestion
// des ROMs.
//
// Ces ecrans montrent les memes objets : des cartes titrees, des lignes
// « pictogramme / intitule / explication / controle », des tuiles portant une
// icone, des boutons a pictogramme et des pastilles d'etat ; les ecrans de
// donnees y ajoutent des pastilles-compteurs, une barre de filtres, une table,
// un journal et un panneau de detail. Les fabriquer ici une fois est la seule
// facon d'obtenir des ecrans qui appartiennent au meme systeme : une hauteur
// de ligne corrigee a un endroit se corrige partout, et un ecran ajoute plus
// tard nait deja conforme.
//
// La matiere vient du CSS (.set-* dans style-common.css et style-dark.css) ;
// ce fichier ne pose que la geometrie et l'assemblage. Aucune couleur n'est
// ecrite en dur ici.
#pragma once

#include <gtkmm.h>
#include <string>
#include <vector>

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
    Gtk::Box*   frame    = nullptr;  // ce qu'on empaquete dans la page
    Gtk::Box*   body     = nullptr;  // ou poser le contenu
    Gtk::Box*   head     = nullptr;  // pack_end pour une action alignee sur le titre
    Gtk::Label* title    = nullptr;  // pour retitrer la carte apres coup
    Gtk::Label* subtitle = nullptr;  // nul quand la carte n'en a pas
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

/* Une notification, dans le langage visuel de Bootcade.
 *
 * Gtk::MessageDialog donne la boite du bureau : fond gris, gros pictogramme
 * generique, bouton plat. Posee a cote d'un ecran de reglages peint en bleu
 * nuit, elle a l'air de venir d'une autre application. Celle-ci reprend la
 * tuile a pictogramme, les titres et le bouton principal des cartes.
 *
 * Bloquante, comme le MessageDialog qu'elle remplace : elle rend la main
 * quand le joueur a lu.
 */
void notice(Gtk::Window& parent, const std::string& title,
            const std::string& message,
            const std::string& icon_file = "bc-info.svg");

// ═══ Les briques des ecrans de donnees ═════════════════════════════════════
//
// Elles ne portent que les cinq teintes que l'application connait deja : les
// trois couleurs d'etat des pastilles, l'accent, et le neutre translucide.
// Un ecran qui voudrait une sixieme couleur n'a pas un besoin de couleur, il
// a un besoin de classement.

enum class PillTone { Neutral, Accent, Ok, Warn, Error };

/* Une pastille « intitule + compteur ».
 *
 * Deux usages, un seul dessin : informative (« Total 29 519 »), ou filtre :
 * cliquable, avec un etat actif / inactif, pour que le resume d'un ecran
 * soit aussi son filtre. Fond a peine teinte, bordure et texte a la couleur
 * du ton : jamais d'aplat.
 */
class Pill : public Gtk::Box {
public:
    Pill(const std::string& label, PillTone tone, bool filter = false);

    void set_label(const std::string& text);
    // Sans compteur affiche tant que set_count n'a pas ete appele.
    void set_count(long count);
    void clear_count();
    void set_tone(PillTone tone);

    // Filtre seulement : sans effet sur une pastille informative.
    bool active() const;
    void set_active(bool on);
    sigc::signal<void, bool>& signal_toggled() { return m_toggled; }

private:
    void apply_tone();
    PillTone           m_tone;
    bool               m_filter;
    Gtk::ToggleButton* m_button = nullptr;   // filtre
    Gtk::Widget*       m_face   = nullptr;   // ce qui porte les classes
    Gtk::Label         m_label;
    Gtk::Label         m_count;
    sigc::signal<void, bool> m_toggled;
};

/* La barre au-dessus d'une table : une recherche locale, des listes
 * deroulantes que l'ecran ajoute a sa guise, et a droite le decompte de ce
 * que la table montre. Un seul signal quand quelque chose change ; la
 * saisie est temporisee, pour ne pas refiltrer 29 000 lignes a chaque
 * touche.
 */
class FilterBar : public Gtk::Box {
public:
    explicit FilterBar(const std::string& search_placeholder);

    Gtk::ComboBoxText* add_combo(const std::string& label);
    std::string search_text() const;
    void        set_summary(const std::string& text);
    Gtk::Entry& entry() { return m_entry; }

    sigc::signal<void>& signal_changed() { return m_changed; }

private:
    void schedule();
    Gtk::Entry        m_entry;
    Gtk::Label        m_summary;
    sigc::connection  m_pending;
    sigc::signal<void> m_changed;
};

/* Une table de donnees : la TreeView de GTK, dans le cadre des cartes, avec
 * ce qu'un ecran de bureau attend d'elle : selection simple ou multiple,
 * defilement, tri par colonne, menu contextuel. Les colonnes et le modele
 * restent a l'appelant : la table ne sait rien de ce qu'elle montre.
 */
struct ColumnOptions {
    bool expand    = false;
    bool mono      = false;   // CRC, noms de fichiers
    int  min_width = -1;
    bool sortable  = true;
    float xalign   = 0.0f;
};

class Table : public Gtk::Box {
public:
    explicit Table(Gtk::SelectionMode mode = Gtk::SELECTION_SINGLE);

    Gtk::TreeView&       view()     { return m_view; }
    Gtk::ScrolledWindow& scrolled() { return m_scroll; }

    // Une colonne de texte ; renvoie la colonne pour la retoucher.
    Gtk::TreeViewColumn* add_text_column(const std::string& title,
                                         const Gtk::TreeModelColumn<Glib::ustring>& column,
                                         const ColumnOptions& options = ColumnOptions());
    // Une colonne de cases a cocher ; l'appelant recoit le chemin de la
    // ligne cliquee et bascule la valeur dans son modele.
    Gtk::TreeViewColumn* add_check_column(const Gtk::TreeModelColumn<bool>& column,
                                         const sigc::slot<void, const Glib::ustring&>& on_toggled);

    // Clic droit : la ligne visee est selectionnee (sans defaire une
    // selection multiple qui la contient deja), puis l'ecran est appele avec
    // son chemin et la colonne, pour construire le menu qu'il veut.
    sigc::signal<void, const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*, GdkEventButton*>&
    signal_context_menu() { return m_context_menu; }

private:
    bool on_button_press(GdkEventButton* event);
    Gtk::ScrolledWindow m_scroll;
    Gtk::TreeView       m_view;
    sigc::signal<void, const Gtk::TreeModel::Path&, Gtk::TreeViewColumn*, GdkEventButton*> m_context_menu;
};

/* Le journal d'un traitement : un texte monospace qui defile, avec, si
 * l'ecran les demande, ses trois commandes : exporter, effacer, suivre.
 * Chaque ligne porte un niveau, peint avec les couleurs d'etat de
 * l'application : lues dans la feuille de style, jamais ecrites ici.
 */
class LogPanel : public Gtk::Box {
public:
    enum Actions { None = 0, Export = 1, Clear = 2, AutoScroll = 4, All = 7 };
    enum class Level { Info, Ok, Warn, Error, Muted };

    LogPanel(const std::string& title, const std::string& subtitle, int actions = All);

    void append(const std::string& line, Level level = Level::Info);
    void clear();
    std::string text() const;
    void set_auto_scroll(bool on);
    bool auto_scroll() const;

private:
    void ensure_tags();
    void on_export();
    Gtk::ScrolledWindow  m_scroll;
    Gtk::TextView        m_view;
    Glib::RefPtr<Gtk::TextBuffer> m_buffer;
    Gtk::CheckButton*    m_follow = nullptr;
    bool                 m_follow_default = true;
    bool                 m_tags_ready = false;
};

/* Le panneau de detail au bas d'un ecran : une carte a hauteur fixe dont le
 * corps est rempli par l'ecran a chaque selection : une liste de ROMs, une
 * grille d'informations, ce qu'il veut. Vide, il le dit.
 */
class DetailPanel : public Gtk::Box {
public:
    DetailPanel(const std::string& icon_file, const std::string& title,
                const std::string& subtitle, int height);

    void set_title(const std::string& text);
    void set_subtitle(const std::string& text);
    // Remplace le contenu ; le panneau prend possession du widget.
    void set_content(Gtk::Widget* content);
    // Retire le contenu et affiche le message d'attente.
    void show_placeholder(const std::string& message);
    Gtk::Box& head() { return *m_card.head; }

private:
    Card                m_card;
    Gtk::Label*         m_title = nullptr;
    Gtk::Label*         m_subtitle = nullptr;
    Gtk::ScrolledWindow m_scroll;
    Gtk::Box            m_body{Gtk::ORIENTATION_VERTICAL, 0};
    Gtk::Widget*        m_content = nullptr;
};

}  // namespace SettingsUi
