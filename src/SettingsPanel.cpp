// src/SettingsPanel.cpp
//
// L'ecran des reglages : une coquille (barre de titre, quatre onglets, pied
// d'actions) et quatre pages baties avec les memes briques (SettingsUi).
//
// La MISE EN PAGE a ete entierement refaite d'apres les maquettes ; les
// COMPORTEMENTS, eux, sont ceux d'avant, widget pour widget et signal pour
// signal. C'est volontaire : une refonte visuelle qui reecrit au passage la
// lecture de la configuration, le balayage des ROMs ou la connexion au compte
// ne se debogue plus, parce qu'on ne sait plus ce qui a change.
#include "SettingsPanel.h"
#include "BootcadeAuth.h"
#include "IconManager.h"
#include "LoginDialog.h"
#include "SettingsUi.h"
#include "Countries.h"
#include <cctype>
#include "AppContext.h"
#include "ConfirmationDialog.h"
#include "DatabaseManager.h"
#include "DownloadDialog.h"
#include "GenerateDAT.h"
#include "FbneoUpdateCheck.h"
#include "HiscoreClient.h"
#include "i18n.h"
#include <map>
#include <gtkmm/filechooserdialog.h>
#include <gtkmm/messagedialog.h>
#include <fstream>
#include <iostream>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <cstdlib>
#include <ctime>
#include <iomanip>
#include <random>
#include <thread>
#include <unistd.h>

namespace {

namespace ui = SettingsUi;

// "player" followed by ten digits. Two things are wanted of it at once: that
// two fresh installs practically never collide, and that it reads as an
// obvious placeholder, so nobody mistakes it on the leaderboard for a name its
// owner actually chose.
std::string generated_player_name() {
    static std::mt19937_64 rng{std::random_device{}()};
    std::uniform_int_distribution<int> digit(0, 9);
    std::string name = "player";
    for (int i = 0; i < 10; ++i) name += char('0' + digit(rng));
    return name;
}

// The country the machine is already set to. Deliberately read from the
// locale and nowhere else: a geolocation lookup would hand someone's IP
// address to a third party purely to draw a flag next to their score, which is
// a far bigger imposition than the flag is worth. An unrecognised or absent
// locale simply yields no country, which the rest of the panel already treats
// as a valid state.
std::string locale_country_code() {
    for (const char* var : {"LC_ALL", "LC_MESSAGES", "LANG"}) {
        const char* value = std::getenv(var);
        if (!value) continue;
        std::string s = value;                       // e.g. "fr_FR.UTF-8"
        auto underscore = s.find('_');
        if (underscore == std::string::npos) continue;
        std::string code = s.substr(underscore + 1, 2);
        if (code.size() != 2) continue;
        for (auto& c : code) c = std::toupper((unsigned char)c);
        for (const auto& entry : kCountries)
            if (code == entry.code) return code;
    }
    return {};
}

/* ── Le registre des emulateurs ────────────────────────────────────────
 *
 * Bootcade lance FinalBurn Neo, et lui seul. La page n'est pourtant pas batie
 * AUTOUR de FBNeo : la liste de gauche et le panneau de droite se construisent
 * a partir de cette description, si bien qu'ajouter un emulateur plus tard
 * revient a ajouter une entree ici plus la poignee de gestes qui lui sont
 * propres, et non a redessiner l'ecran.
 *
 * Il n'y a donc qu'une entree. La maquette en montre six ; les cinq autres ne
 * sont pas supportees, et les afficher grisees ferait passer une absence de
 * fonctionnalite pour une panne.
 */
struct EmulatorEntry {
    const char* id;
    const char* logo;          // assets/icons/…
    const char* name;
    const char* kind;          // « Arcade emulator »
    const char* description;
};

const std::vector<EmulatorEntry>& emulator_registry() {
    static const std::vector<EmulatorEntry> kEntries = {
        {"fbneo", "bc-emu-fbneo.svg", "FinalBurn Neo", N_("Arcade emulator"),
         N_("Play arcade games from multiple systems with FinalBurn Neo.")},
    };
    return kEntries;
}

// La date d'un fichier, en AAAA-MM-JJ, ou vide s'il n'existe pas.
std::string file_date(const std::string& path) {
    struct stat st;
    if (path.empty() || ::stat(path.c_str(), &st) != 0) return {};
    char buf[16];
    std::tm tm{};
    localtime_r(&st.st_mtime, &tm);
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm)) return {};
    return buf;
}

std::string today_iso() {
    std::time_t now = std::time(nullptr);
    std::tm tm{};
    localtime_r(&now, &tm);
    char buf[16];
    if (!std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm)) return {};
    return buf;
}

// Une tuile de statistique du panneau emulateur : pictogramme, intitule,
// valeur. Trois cotes a cote forment la bande de la maquette.
Gtk::Widget* stat_tile(const std::string& icon_file, const std::string& label,
                       Gtk::Label& value) {
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 11);
    box->get_style_context()->add_class("cc-subcard");
    box->pack_start(*ui::tile(icon_file, ui::kIconRow, ui::kTileRow),
                    Gtk::PACK_SHRINK);
    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->pack_start(*ui::sub_label(label), Gtk::PACK_SHRINK);
    value.set_xalign(0.0f);
    value.get_style_context()->add_class("set-row-title");
    txt->pack_start(value, Gtk::PACK_SHRINK);
    box->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
    return box;
}

// Un champ de chemin suivi de ses boutons : la ligne que Library repete trois
// fois (previsualisations, titres, DAT).
Gtk::Widget* path_row(const std::string& title, const std::string& subtitle,
                      Gtk::Entry& entry, Gtk::Button& browse,
                      Gtk::Button& action) {
    auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    line->get_style_context()->add_class("set-row");

    auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 1);
    txt->set_valign(Gtk::ALIGN_CENTER);
    txt->set_size_request(240, -1);
    txt->pack_start(*ui::title_label(title), Gtk::PACK_SHRINK);
    txt->pack_start(*ui::sub_label(subtitle), Gtk::PACK_SHRINK);
    line->pack_start(*txt, Gtk::PACK_SHRINK);

    entry.set_hexpand(true);
    entry.set_valign(Gtk::ALIGN_CENTER);
    line->pack_start(entry, Gtk::PACK_EXPAND_WIDGET);
    browse.set_valign(Gtk::ALIGN_CENTER);
    action.set_valign(Gtk::ALIGN_CENTER);
    line->pack_start(browse, Gtk::PACK_SHRINK);
    line->pack_start(action, Gtk::PACK_SHRINK);
    return line;
}

}  // namespace

SettingsPanel::~SettingsPanel() {
    // Avant tout demontage : un fil de sonde reseau qui testerait le drapeau
    // puis emettrait sur un objet detruit attend ici, voit le drapeau tombe,
    // et renonce.
    std::lock_guard<std::mutex> lock(m_alive->mutex);
    m_alive->alive = false;
}

SettingsPanel::SettingsPanel() : Box(Gtk::ORIENTATION_VERTICAL, 0) {
    build_shell();
}

// ─────────────────────────────────────────────────────────────────────────
//  Coquille : barre de titre, onglets, pages, pied
// ─────────────────────────────────────────────────────────────────────────

void SettingsPanel::build_shell() {
    get_style_context()->add_class("cc-window");
    get_style_context()->add_class("set-window");

    // ── Barre de titre ───────────────────────────────────────────────────
    // Bootcade decore ses fenetres lui-meme, comme Controller Configuration :
    // une HeaderBar posee par la fenetre, la marque a gauche, et NOTRE croix a
    // droite plutot que la pastille du bureau, qui ne ressemble a rien d'autre
    // dans l'application.
    m_header.pack_start(*ui::tile("gear.svg", 21, 36, /*accent=*/true),
                        Gtk::PACK_SHRINK);
    m_header_title.set_text(_("Settings"));
    m_header_title.set_xalign(0.0f);
    m_header_title.get_style_context()->add_class("set-head-title");
    m_header_sub.set_text(_("Customize Bootcade to match your setup and preferences"));
    m_header_sub.set_xalign(0.0f);
    m_header_sub.get_style_context()->add_class("set-head-sub");
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 0);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    head_txt->pack_start(m_header_title, Gtk::PACK_SHRINK);
    head_txt->pack_start(m_header_sub,   Gtk::PACK_SHRINK);
    m_header.pack_start(*head_txt, Gtk::PACK_SHRINK);

    m_btn_close.set_image(*ui::image("bc-close.svg", 18));
    m_btn_close.set_tooltip_text(_("Close"));
    m_btn_close.get_style_context()->add_class("set-close");
    m_btn_close.set_valign(Gtk::ALIGN_CENTER);
    m_btn_close.signal_clicked().connect([this] { m_sig_close.emit(); });

    /* La barre de titre ne porte AUCUNE classe de fenetre.
     *
     * Elle en portait, pour que « .set-window .set-brand » atteigne la tuile
     * violette. Mais « .set-window » peint aussi un fond : le bandeau prenait
     * la couleur des pages et disparaissait, alors que la fenetre principale
     * et Controller Configuration laissent tous deux la barre au gris du
     * theme. Les elements de l'entete se stylent donc par leur propre classe,
     * sans exiger un ancetre, et le bandeau redevient gris comme partout
     * ailleurs. */
    m_headerbar.set_show_close_button(false);
    m_headerbar.pack_start(m_header);
    m_headerbar.pack_end(m_btn_close);
    // Titre personnalise vide : sans lui GTK dessine SON titre au centre en
    // plus de la marque, et l'ecran porte alors deux titres.
    m_headerbar.set_custom_title(*Gtk::make_managed<Gtk::Box>());
    m_headerbar.show_all();

    // ── Onglets ──────────────────────────────────────────────────────────
    m_tabbar.get_style_context()->add_class("set-tabbar");
    /* Aucune transition.
     *
     * Un fondu interpole la hauteur du Stack pendant toute sa duree : la
     * fenetre etait donc recalee sur une page a moitie affichee, et gardait
     * cette hauteur-la. Sur un ecran de reglages, l'effet ne valait pas ce
     * qu'il coutait.
     */
    m_pages.set_transition_type(Gtk::STACK_TRANSITION_TYPE_NONE);
    m_pages.set_transition_duration(0);
    /* Chaque page a SA hauteur.
     *
     * Un Gtk::Stack est homogene par defaut : les quatre pages prenaient donc
     * la hauteur de la plus haute, et les trois autres se terminaient par une
     * bande vide qui ne disait rien. La fenetre suit maintenant la page
     * affichee (voir fit_to_page).
     */
    m_pages.set_vhomogeneous(false);

    /* Les pages sont posees telles quelles, sans zone defilante.
     *
     * Un ecran de reglages qui defile cache la moitie de ses options derriere
     * un geste : on ne voit plus ce qui existe, et le pied d'actions flotte
     * au-dessus d'un contenu tronque. La fenetre prend donc la hauteur de sa
     * page la plus haute : Gtk::Stack est homogene par defaut, donc les
     * quatre pages partagent la meme taille et la fenetre ne saute plus d'un
     * onglet a l'autre. Seule la LISTE des dossiers de ROMs defile, parce
     * qu'elle est une liste : son contenu n'a pas de hauteur previsible. */
    m_pages.add(*build_page_general(),  "general");
    m_pages.add(*build_page_library(),  "library");
    m_pages.add(*build_page_emulator(), "emulator");
    m_pages.add(*build_page_online(),   "online");

    add_tab("general",  "gear.svg",          _("General"));
    add_tab("library",  "bc-folder.svg",     _("Library"));
    add_tab("emulator", "bc-controller.svg", _("Emulator"));
    add_tab("online",   "bc-globe.svg",      _("Online"));

    pack_start(m_tabbar, Gtk::PACK_SHRINK);
    pack_start(m_pages,  Gtk::PACK_EXPAND_WIDGET);

    // ── Pied ─────────────────────────────────────────────────────────────
    // Ce qui defait a gauche, ce qui valide a droite : c'est la disposition de
    // la maquette, et c'est aussi ce qui evite de cliquer « tout remettre a
    // zero » en visant « enregistrer ».
    m_btn_restore_defaults.set_label(_("Restore Default Settings"));
    m_btn_restore_defaults.set_image(*ui::image("bc-restore.svg", ui::kIconButton));
    m_btn_restore_defaults.set_always_show_image(true);
    m_btn_restore_defaults.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_restore_defaults_clicked));

    m_btn_cancel.set_label(_("Cancel"));
    m_btn_cancel.signal_clicked().connect([this] { m_sig_close.emit(); });

    m_btn_save.set_label(_("Save"));
    m_btn_save.set_image(*ui::image("bc-save.svg", ui::kIconButton));
    m_btn_save.set_always_show_image(true);
    m_btn_save.get_style_context()->add_class("accent-button");
    m_btn_save.signal_clicked().connect([this] { m_sig_save.emit(); });

    m_footer.get_style_context()->add_class("cc-footer");
    m_footer.pack_start(m_btn_restore_defaults, Gtk::PACK_SHRINK);
    m_footer.pack_end(m_btn_save,   Gtk::PACK_SHRINK);
    m_footer.pack_end(m_btn_cancel, Gtk::PACK_SHRINK);
    pack_start(m_footer, Gtk::PACK_SHRINK);

    m_net_done.connect([this] {
        set_network_state(m_net_state.load());
        // Reactive le bouton : sans cela il restait grise apres le premier
        // appui, et « Test Connection » n'etait cliquable qu'une seule fois
        // dans la vie de la fenetre : de l'exterieur, un bouton mort.
        m_btn_test_net.set_sensitive(true);
    });
    m_update_done.connect([this] {
        std::string tag;
        bool failed;
        {
            std::lock_guard<std::mutex> lock(m_update_mutex);
            tag = m_update_tag;
            failed = m_update_failed;
        }
        m_btn_check_updates.set_sensitive(true);
        if (failed)
            set_update_state(_("Could not check for updates."), "warn");
        else if (tag.empty())
            set_update_state(_("You are up to date"), "ok");
        else
            set_update_state(Glib::ustring::compose(_("Bootcade %1 is available"), tag),
                             "warn");
    });
    m_emu_update_done.connect([this] {
        std::string msg, tone;
        {
            std::lock_guard<std::mutex> lock(m_emu_mutex);
            msg = m_emu_update_msg;
            tone = m_emu_update_tone;
        }
        m_btn_emu_updates.set_sensitive(true);
        m_lbl_emu_note.set_text(msg);
        auto ctx = m_lbl_emu_note.get_style_context();
        for (const char* c : {"set-ok", "set-warn", "set-err", "set-sub"})
            ctx->remove_class(c);
        ctx->add_class(tone == "ok" ? "set-ok" : tone == "warn" ? "set-warn" : "set-sub");
        m_lbl_emu_note.show();
    });

    // Rien ne doit paraitre mis en avant par hasard : sans defaut declare, le
    // premier bouton de la page portait l'anneau de focus et se lisait comme
    // l'action recommandee.
    m_btn_save.set_can_default(true);
    /* Ce qui se montre selon l'etat ne doit PAS obeir a show_all().
     *
     * La fenetre qui accueille le panneau appelle show_all() apres coup, ce
     * qui remontrait d'un bloc le profil connecte et l'invitation a se
     * connecter en meme temps. Ces widgets-la decident eux-memes, et
     * refresh_account_row est seul juge. */
    /* L'ordre compte : on montre TOUT d'abord, ce qui donne a chaque enfant
     * son etat « visible », puis on pose le drapeau. Les blocs peuvent des
     * lors s'allumer et s'eteindre d'un seul show()/hide() sans que le
     * show_all() de la fenetre vienne tout rallumer par-dessus. Poser le
     * drapeau AVANT laissait leurs enfants jamais montres, et le bloc
     * s'affichait vide. */
    show_all_children();
    for (Gtk::Widget* w : {static_cast<Gtk::Widget*>(&m_profile_signed),
                           static_cast<Gtk::Widget*>(&m_profile_empty),
                           static_cast<Gtk::Widget*>(&m_account_avatar),
                           static_cast<Gtk::Widget*>(&m_hiscore_hint),
                           static_cast<Gtk::Widget*>(&m_lbl_emu_note)})
        w->set_no_show_all(true);
    m_lbl_emu_note.hide();   // rien a dire tant qu'on n'a pas verifie
    refresh_account_row();
    refresh_emulator_state();
}

/* Quatre pastilles a pictogramme, et une seule enfoncee.
 *
 * Un Gtk::Notebook aurait suffi a empiler les pages, mais ses onglets
 * n'acceptent pas proprement la geometrie de la maquette (48 px, pictogramme,
 * coins hauts arrondis). Des ToggleButton pilotant un Gtk::Stack donnent
 * exactement le dessin voulu, au prix d'une exclusion mutuelle a tenir : d'ou
 * le drapeau, sans lequel decocher l'onglet courant depuis le gestionnaire
 * rappellerait le gestionnaire.
 */
void SettingsPanel::add_tab(const std::string& id, const std::string& icon_file,
                            const std::string& label) {
    auto* btn = Gtk::make_managed<Gtk::ToggleButton>();
    auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    box->set_halign(Gtk::ALIGN_CENTER);
    box->pack_start(*ui::image(icon_file, 17), Gtk::PACK_SHRINK);
    box->pack_start(*Gtk::make_managed<Gtk::Label>(label), Gtk::PACK_SHRINK);
    btn->add(*box);
    btn->get_style_context()->add_class("set-tab");
    btn->set_active(m_tabs_buttons.empty());
    /* On ecoute « clicked », pas « toggled ».
     *
     * Avec « toggled » il fallait rattraper deux cas a la main : l'onglet
     * courant qu'un clic decochait, et les trois autres qu'il fallait
     * eteindre : chaque extinction rappelant le gestionnaire. Un clic dit
     * simplement « c'est celui-la », et set_active n'emet que « toggled »,
     * jamais « clicked » : il n'y a donc plus de reentrance a garder.
     */
    btn->signal_clicked().connect([this, btn, id] {
        if (m_tab_switching) return;
        m_tab_switching = true;
        for (auto* other : m_tabs_buttons) other->set_active(other == btn);
        m_pages.set_visible_child(id);
        m_tab_switching = false;
        fit_to_page();
    });
    m_tabs_buttons.push_back(btn);
    m_tabbar.pack_start(*btn, Gtk::PACK_SHRINK);
}

// ─────────────────────────────────────────────────────────────────────────
//  General
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_general() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    // ── Appearance & Startup ─────────────────────────────────────────────
    auto appearance = ui::card("bc-brush.svg", _("Appearance & Startup"),
                               _("Customize the look and behavior of Bootcade"));
    auto* app_rows = ui::rows();

    // Friendly names for known language codes; unknown codes show the raw code.
    // Each language is named in itself : someone looking for their own language
    // recognises "ไทย", not "th". Add an entry here whenever a locale/<code>.json
    // is added, otherwise the picker falls back to showing the bare code.
    static const std::map<std::string, std::string> lang_names = {
        {"en","English"}, {"fr","Français"}, {"es","Español"}, {"de","Deutsch"},
        {"pt","Português"}, {"zh","中文"}, {"ja","日本語"}, {"th","ไทย"}};
    m_combo_language.append("", _("System"));
    for (const auto& code : i18n::available_languages()) {
        auto it = lang_names.find(code);
        m_combo_language.append(code, it != lang_names.end() ? it->second : code);
    }
    m_combo_language.set_active_id("");
    m_combo_language.set_size_request(ui::kFieldWidth, -1);
    ui::add_row(app_rows, *ui::row("bc-globe.svg", _("Language"),
                                   _("Select the application language."),
                                   &m_combo_language));

    m_combo_theme.append("system", _("System"));
    m_combo_theme.append("dark",   _("Dark"));
    m_combo_theme.append("light",  _("Light"));
    m_combo_theme.set_active_id("dark");
    m_combo_theme.set_size_request(ui::kFieldWidth, -1);
    ui::add_row(app_rows, *ui::row("bc-palette.svg", _("Theme"),
                                   _("Choose the application theme."),
                                   &m_combo_theme));

    m_combo_startup.append("last_played",   _("Last played game"));
    m_combo_startup.append("most_played",   _("Most played game"));
    m_combo_startup.append("best_score",    _("Best personal highscore"));
    m_combo_startup.append("last_selected", _("Last selected game"));
    m_combo_startup.append("first",         _("First available game"));
    m_combo_startup.set_active_id("last_played");
    m_combo_startup.set_size_request(ui::kFieldWidth, -1);
    m_combo_startup.set_tooltip_text(
        _("If the chosen game cannot be found, the first available one is shown."));
    ui::add_row(app_rows, *ui::row("bc-power.svg", _("Game selected at startup"),
                                   _("Choose which game Bootcade selects when it starts."),
                                   &m_combo_startup));
    appearance.body->pack_start(*app_rows, Gtk::PACK_SHRINK);
    page->pack_start(*appearance.frame, Gtk::PACK_SHRINK);

    m_combo_theme.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_theme_changed.emit(m_combo_theme.get_active_id());
    });
    m_combo_language.signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_language_changed.emit(m_combo_language.get_active_id());
    });

    // ── Behavior ─────────────────────────────────────────────────────────
    // Deux interrupteurs, et deux seulement : ce sont les deux seuls gestes de
    // cette section qui commandent quelque chose que le lanceur fait deja.
    auto behavior = ui::card("gear.svg", _("Behavior"),
                             _("Configure optional application behavior."));
    auto* beh_rows = ui::rows();
    m_switch_window_state.set_active(true);
    m_switch_window_state.set_valign(Gtk::ALIGN_CENTER);
    ui::add_row(beh_rows, *ui::row("bc-window.svg", _("Restore last window state"),
                                   _("Remember window size and position."),
                                   &m_switch_window_state));
    m_switch_play_history.set_active(true);
    m_switch_play_history.set_valign(Gtk::ALIGN_CENTER);
    m_switch_play_history.set_tooltip_text(
        _("Off, Bootcade stops recording how long you play. Already recorded history is kept."));
    ui::add_row(beh_rows, *ui::row("bc-clock.svg", _("Keep play history"),
                                   _("Track recently played games."),
                                   &m_switch_play_history));
    behavior.body->pack_start(*beh_rows, Gtk::PACK_SHRINK);
    page->pack_start(*behavior.frame, Gtk::PACK_SHRINK);

    // ── Updates et Data & Storage, cote a cote ───────────────────────────
    auto* pair = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    pair->set_homogeneous(true);

    auto updates = ui::card("bc-sync.svg", _("Updates"),
                            _("Manage application updates."));
    auto* upd_rows = ui::rows();
    m_switch_auto_update.set_active(true);
    m_switch_auto_update.set_valign(Gtk::ALIGN_CENTER);
    ui::add_row(upd_rows, *ui::row("bc-download.svg",
                                   _("Check for updates automatically"),
                                   _("Notify when a new version is available."),
                                   &m_switch_auto_update));

    auto* upd_bottom = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 14);
    upd_bottom->get_style_context()->add_class("set-row");
    m_btn_check_updates.set_label(_("Check for updates now"));
    m_btn_check_updates.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_btn_check_updates.set_always_show_image(true);
    m_btn_check_updates.signal_clicked().connect([this] {
        m_btn_check_updates.set_sensitive(false);
        set_update_state(_("Checking…"), "muted");
        check_launcher_update_async();
    });
    upd_bottom->pack_start(m_btn_check_updates, Gtk::PACK_EXPAND_WIDGET);

    auto* ver_box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    ver_box->set_valign(Gtk::ALIGN_CENTER);
    ver_box->pack_start(*ui::sub_label(_("Current version")), Gtk::PACK_SHRINK);
#ifdef FBNEO_VERSION
    m_lbl_version.set_text(std::string("v") + FBNEO_VERSION);
#else
    m_lbl_version.set_text("—");
#endif
    m_lbl_version.set_xalign(0.0f);
    m_lbl_version.get_style_context()->add_class("set-row-title");
    ver_box->pack_start(m_lbl_version, Gtk::PACK_SHRINK);
    m_update_state_text.set_xalign(0.0f);
    m_update_state.pack_start(m_update_state_icon, Gtk::PACK_SHRINK);
    m_update_state.pack_start(m_update_state_text, Gtk::PACK_SHRINK);
    ver_box->pack_start(m_update_state, Gtk::PACK_SHRINK);
    upd_bottom->pack_start(*ver_box, Gtk::PACK_SHRINK);

    updates.body->pack_start(*upd_rows, Gtk::PACK_SHRINK);
    updates.body->pack_start(*upd_bottom, Gtk::PACK_SHRINK);
    pair->pack_start(*updates.frame, Gtk::PACK_EXPAND_WIDGET);

    auto storage = ui::card("database.svg", _("Data & Storage"),
                            _("Manage local application data."));
    auto* sto_rows = ui::rows();
    m_btn_clear_cache.set_label(_("Clear…"));
    m_btn_clear_cache.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_clear_cache_clicked));
    ui::add_row(sto_rows, *ui::row("bc-trash.svg", _("Clear local cache"),
                                   _("Remove cached images and temporary files."),
                                   &m_btn_clear_cache));
    m_btn_reset_settings.set_label(_("Reset…"));
    m_btn_reset_settings.get_style_context()->add_class("set-danger");
    m_btn_reset_settings.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_reset_settings_clicked));
    ui::add_row(sto_rows, *ui::row("bc-sync.svg", _("Reset all settings"),
                                   _("Restore all settings to their default values."),
                                   &m_btn_reset_settings));
    storage.body->pack_start(*sto_rows, Gtk::PACK_SHRINK);
    pair->pack_start(*storage.frame, Gtk::PACK_EXPAND_WIDGET);

    page->pack_start(*pair, Gtk::PACK_SHRINK);

    set_update_state(_("Not checked yet"), "muted");
    return page;
}

void SettingsPanel::set_update_state(const std::string& text, const std::string& tone) {
    m_update_state_text.set_text(text);
    auto ctx = m_update_state_text.get_style_context();
    for (const char* c : {"set-ok", "set-warn", "set-err", "set-sub"})
        ctx->remove_class(c);
    ctx->add_class(tone == "ok" ? "set-ok" : tone == "warn" ? "set-warn" : "set-sub");
    if (tone == "ok")
        m_update_state_icon.set(IconManager::load("icons/bc-detected.svg", 16, 16));
    else
        m_update_state_icon.set(IconManager::load("icons/bc-info.svg", 16, 16));
    m_update_state_icon.show();
}

/* La verification tourne HORS du fil graphique.
 *
 * fetch_launcher_latest interroge GitHub : sur un reseau lent elle bloque
 * plusieurs secondes, et la faire sur le fil principal figerait la fenetre
 * entiere le temps de l'appel.
 */
void SettingsPanel::check_launcher_update_async() {
    std::thread([this, alive = m_alive] {
        auto r = FbneoUpdateCheck::fetch_launcher_latest();
        std::string tag = r.tag;
        if (!tag.empty() && tag[0] == 'v') tag.erase(0, 1);
#ifdef FBNEO_VERSION
        const std::string me = FBNEO_VERSION;
#else
        const std::string me;
#endif
        {
            std::lock_guard<std::mutex> lock(m_update_mutex);
            m_update_failed = !r.ok;
            // Egale ou anterieure : rien a annoncer. La comparaison se fait sur
            // les seuls nombres, comme dans la fenetre principale : un build de
            // developpement porte « 1.0.20+7.g1a2b3c4 », dont std::stoi ne lit
            // que la partie publiee.
            m_update_tag.clear();
            if (r.ok && !tag.empty() && !me.empty()) {
                auto parts = [](const std::string& v) {
                    std::vector<int> out;
                    size_t i = 0;
                    while (i < v.size() && out.size() < 3) {
                        try { out.push_back(std::stoi(v.substr(i))); } catch (...) { break; }
                        size_t dot = v.find('.', i);
                        if (dot == std::string::npos) break;
                        i = dot + 1;
                    }
                    out.resize(3, 0);
                    return out;
                };
                auto a = parts(tag), b = parts(me);
                if (a > b) m_update_tag = tag;
            }
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_update_done.emit();
    }).detach();
}

// ─────────────────────────────────────────────────────────────────────────
//  Library
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_library() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    // ── ROM Directories ──────────────────────────────────────────────────
    auto roms = ui::card("bc-folder-plus.svg", _("ROM Directories"),
                         _("Add the folders that contain your ROMs. Bootcade will "
                           "scan these directories for supported games."));

    m_button_add_roms.set_label(_("Add Folder"));
    m_button_add_roms.set_image(*ui::image("folder-add.svg", ui::kIconButton));
    m_button_add_roms.set_always_show_image(true);
    m_button_add_roms.get_style_context()->add_class("accent-button");
    m_button_add_roms.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_add_roms_path_clicked));
    m_button_remove_roms.set_label(_("Remove"));
    m_button_remove_roms.set_image(*ui::image("bc-trash-red.svg", ui::kIconButton));
    m_button_remove_roms.set_always_show_image(true);
    m_button_remove_roms.get_style_context()->add_class("set-danger");
    m_button_remove_roms.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_remove_roms_path_clicked));
    auto* roms_actions = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    roms_actions->set_valign(Gtk::ALIGN_CENTER);
    roms_actions->pack_start(m_button_add_roms,    Gtk::PACK_SHRINK);
    roms_actions->pack_start(m_button_remove_roms, Gtk::PACK_SHRINK);
    roms.head->pack_end(*roms_actions, Gtk::PACK_SHRINK);

    /* La liste garde sa TreeView.
     *
     * Elle porte la selection dont depend « Remove », et l'index de la ligne
     * choisie est ce que remove_roms_path attend. La remplacer par une ListBox
     * aurait reecrit ce chemin-la pour un gain purement visuel : ce sont ses
     * COLONNES qui changent, pas elle.
     */
    m_model_roms = Gtk::ListStore::create(m_cols_roms);
    m_treeview_roms.set_model(m_model_roms);
    m_treeview_roms.set_headers_visible(false);
    m_treeview_roms.get_style_context()->add_class("set-romlist");
    m_treeview_roms.append_column("", m_cols_roms.icon);
    m_treeview_roms.append_column("", m_cols_roms.path);
    if (auto* col = m_treeview_roms.get_column(1)) col->set_expand(true);
    {
        // Pastille et mot dans UNE cellule : une colonne de plus les aurait
        // separes, et la pastille se serait retrouvee a distance de son texte.
        auto* cell = Gtk::manage(new Gtk::CellRendererText());
        cell->property_xalign() = 1.0f;
        auto* col = Gtk::manage(new Gtk::TreeViewColumn("", *cell));
        col->add_attribute(cell->property_markup(), m_cols_roms.status);
        m_treeview_roms.append_column(*col);
    }
    m_scrolled_roms.add(m_treeview_roms);
    m_scrolled_roms.set_policy(Gtk::POLICY_AUTOMATIC, Gtk::POLICY_AUTOMATIC);
    /* Hauteur EXACTE, pas un minimum.
     *
     * Avec un simple set_size_request la zone prenait sa hauteur naturelle,
     * c'est-a-dire celle de son contenu : dix dossiers donnaient dix lignes,
     * et la poignee ne commandait plus rien. Bornee des deux cotes, la liste
     * fait la hauteur demandee quel que soit le nombre de dossiers, et c'est
     * elle qui defile.
     */
    m_scrolled_roms.set_propagate_natural_height(false);
    apply_roms_list_height();
    m_scrolled_roms.get_style_context()->add_class("set-rows");
    roms.body->pack_start(m_scrolled_roms, Gtk::PACK_SHRINK);

    /* ── La poignee ──────────────────────────────────────────────────────
     *
     * Trois points sous la liste, centres : c'est la ou l'oeil cherche de
     * quoi tirer, et c'est la convention. Elle n'a l'air active qu'au survol,
     * pour ne pas se lire comme un separateur decoratif de plus.
     */
    auto* grip_icon = ui::image("bc-grip.svg", 20);
    m_roms_grip.add(*grip_icon);
    m_roms_grip.get_style_context()->add_class("set-grip");
    m_roms_grip.set_halign(Gtk::ALIGN_CENTER);
    m_roms_grip.set_tooltip_text(_("Drag to resize the list"));
    /* Le masque d'evenements, a poser NOUS-MEMES.
     *
     * Un Gtk::EventBox possede sa propre fenetre GDK, et GTK3 ne complete pas
     * son masque pour les gestes qu'on lui attache : sans ces trois bits, la
     * poignee recevait bien le survol (c'est pourquoi elle s'allumait) mais
     * jamais l'appui, et le geste ne demarrait pas. A poser avant que le
     * widget soit realise.
     */
    m_roms_grip.add_events(Gdk::BUTTON_PRESS_MASK | Gdk::BUTTON_RELEASE_MASK
                           | Gdk::BUTTON1_MOTION_MASK);
    // Le curseur dit ce que fait la poignee avant qu'on l'essaie.
    m_roms_grip.signal_realize().connect([this] {
        if (auto win = m_roms_grip.get_window())
            win->set_cursor(Gdk::Cursor::create(m_roms_grip.get_display(), "ns-resize"));
    });

    /* Un geste, pas des evenements bruts.
     *
     * Suivre soi-meme press / motion / release oblige a poser les bons masques
     * ET a tenir le grab du pointeur : sans grab, les deplacements partent au
     * widget survole des que le curseur quitte la poignee, c'est-a-dire des le
     * premier pixel, puisqu'elle bouge avec ce qu'elle redimensionne.
     * Gtk::GestureDrag s'en charge et rend directement le deplacement cumule
     * depuis le debut du geste : une valeur ABSOLUE, donc rien a accumuler.
     */
    m_grip_drag = Gtk::GestureDrag::create(m_roms_grip);
    m_grip_drag->signal_drag_begin().connect([this](double, double) {
        m_grip_start_height = m_roms_list_height;
        /* La borne haute se fige ICI, pas a chaque pixel.
         *
         * Elle depend de la place restante entre la fenetre et le bord de
         * l'ecran ; la recalculer pendant le glisser reviendrait a lire une
         * geometrie qui bouge. Une fenetre plus haute que la zone de travail
         * mettrait son pied d'actions hors de portee.
         */
        m_grip_max_height = 900;
        if (auto* win = dynamic_cast<Gtk::Window*>(get_toplevel())) {
            if (auto gdkwin = win->get_window()) {
                int win_w = 0, win_h = 0;
                win->get_size(win_w, win_h);
                Gdk::Rectangle work;
                auto monitor = win->get_display()->get_monitor_at_window(gdkwin);
                if (monitor) {
                    monitor->get_workarea(work);
                    m_grip_max_height =
                        m_roms_list_height + (work.get_height() - win_h);
                }
            }
        }
        if (m_grip_max_height < 120) m_grip_max_height = 120;
    });
    m_grip_drag->signal_drag_update().connect([this](double, double offset_y) {
        set_roms_list_height(m_grip_start_height + static_cast<int>(offset_y));
    });

    roms.body->pack_start(m_roms_grip, Gtk::PACK_SHRINK);
    page->pack_start(*roms.frame, Gtk::PACK_SHRINK);

    // ── Artwork & Media ──────────────────────────────────────────────────
    auto art = ui::card("bc-image.svg", _("Artwork & Media"),
                        _("Configure where Bootcade stores and downloads game "
                          "artwork, previews and titles."));
    auto* art_rows = ui::rows();

    m_button_browse_previews.set_label(_("Browse..."));
    m_button_browse_previews.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_previews.set_always_show_image(true);
    m_button_browse_previews.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_previews);
    });
    m_button_download_previews.set_label(_("Download All"));
    m_button_download_previews.set_image(*ui::image("download.svg", ui::kIconButton));
    m_button_download_previews.set_always_show_image(true);
    m_button_download_previews.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_previews_clicked));
    ui::add_row(art_rows, *path_row(_("Previews"),
                                    _("Path for game preview images (screenshots)."),
                                    m_entry_previews, m_button_browse_previews,
                                    m_button_download_previews));

    m_button_browse_titles.set_label(_("Browse..."));
    m_button_browse_titles.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_titles.set_always_show_image(true);
    m_button_browse_titles.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_titles);
    });
    m_button_download_titles.set_label(_("Download All"));
    m_button_download_titles.set_image(*ui::image("download.svg", ui::kIconButton));
    m_button_download_titles.set_always_show_image(true);
    m_button_download_titles.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_titles_clicked));
    ui::add_row(art_rows, *path_row(_("Titles"),
                                    _("Path for game title images (logos, marquees, etc)."),
                                    m_entry_titles, m_button_browse_titles,
                                    m_button_download_titles));
    art.body->pack_start(*art_rows, Gtk::PACK_SHRINK);

    // Les DAT partagent la carte des visuels : ce sont trois chemins de la
    // meme nature, et leur donner une carte a eux seuls pour une ligne aurait
    // ajoute un cadre sans ajouter de sens.
    auto* dat_rows = ui::rows();
    dat_rows->set_margin_top(12);
    m_button_browse_dat.set_label(_("Browse..."));
    m_button_browse_dat.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_dat.set_always_show_image(true);
    m_button_browse_dat.signal_clicked().connect([this] {
        on_folder_clicked(&m_entry_dat);
    });
    m_button_generate_dat.set_label(_("Generate DAT"));
    m_button_generate_dat.set_image(*ui::image("bc-file.svg", ui::kIconButton));
    m_button_generate_dat.set_always_show_image(true);
    m_button_generate_dat.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_generate_dat_clicked));
    ui::add_row(dat_rows, *path_row(_("DAT Files"),
                                    _("Configure the directory for DAT files (game lists)."),
                                    m_entry_dat, m_button_browse_dat,
                                    m_button_generate_dat));
    art.body->pack_start(*dat_rows, Gtk::PACK_SHRINK);
    page->pack_start(*art.frame, Gtk::PACK_SHRINK);

    // ── ROM Management : the folders the repair workflow writes into ──────
    // Environment, not one-shot inputs : that is why they live here and not
    // in the tabs that use them.
    auto mgmt = ui::card("database.svg", _("ROM Management"),
                         _("Folders used by the import and repair workflow."));
    auto* mgmt_rows = ui::rows();
    mgmt_rows->set_margin_top(12);
    auto open_folder = [this](Gtk::Entry* entry) {
        std::string path = entry->get_text();
        std::error_code ec;
        if (path.empty() || !std::filesystem::is_directory(path, ec)) return;
        try { Gio::AppInfo::launch_default_for_uri(Glib::filename_to_uri(path)); } catch (...) {}
    };
    m_button_browse_outbox.set_label(_("Browse..."));
    m_button_browse_outbox.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_outbox.set_always_show_image(true);
    m_button_browse_outbox.signal_clicked().connect([this] { on_folder_clicked(&m_entry_outbox); });
    m_button_open_outbox.set_label(_("Open"));
    m_button_open_outbox.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_button_open_outbox.set_always_show_image(true);
    m_button_open_outbox.signal_clicked().connect([this, open_folder] { open_folder(&m_entry_outbox); });
    ui::add_row(mgmt_rows, *path_row(_("Outbox"),
                                     _("Where repaired sets wait before being moved into your library."),
                                     m_entry_outbox, m_button_browse_outbox, m_button_open_outbox));
    m_button_browse_quarantine.set_label(_("Browse..."));
    m_button_browse_quarantine.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_quarantine.set_always_show_image(true);
    m_button_browse_quarantine.signal_clicked().connect([this] { on_folder_clicked(&m_entry_quarantine); });
    m_button_open_quarantine.set_label(_("Open"));
    m_button_open_quarantine.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_button_open_quarantine.set_always_show_image(true);
    m_button_open_quarantine.signal_clicked().connect([this, open_folder] { open_folder(&m_entry_quarantine); });
    ui::add_row(mgmt_rows, *path_row(_("Quarantine"),
                                     _("Where unusable, rejected or replaced files are kept, never deleted silently."),
                                     m_entry_quarantine, m_button_browse_quarantine, m_button_open_quarantine));
    m_button_manage_dats.set_label(_("Manage DATs in ROM Management"));
    m_button_manage_dats.set_image(*ui::image("bc-file.svg", ui::kIconButton));
    m_button_manage_dats.set_always_show_image(true);
    m_button_manage_dats.signal_clicked().connect([this] { m_sig_open_rom_manager.emit(); });
    ui::add_row(mgmt_rows, *ui::row("bc-file.svg", _("DAT files"),
                                    _("The DAT files your library is compared with are managed in ROM Management."),
                                    &m_button_manage_dats));
    mgmt.body->pack_start(*mgmt_rows, Gtk::PACK_SHRINK);
    page->pack_start(*mgmt.frame, Gtk::PACK_SHRINK);

    // ── Scan Options ─────────────────────────────────────────────────────
    auto scan = ui::card("bc-search.svg", _("Scan Options"),
                         _("Configure how Bootcade scans your ROM directories."));
    auto* scan_grid = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 18);
    scan_grid->set_homogeneous(true);

    auto check_block = [](Gtk::CheckButton& check, const std::string& title,
                          const std::string& subtitle) {
        check.set_label(title);
        check.set_active(true);
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
        box->pack_start(check, Gtk::PACK_SHRINK);
        auto* sub = ui::sub_label(subtitle);
        sub->set_margin_start(27);   // aligne sous le libelle, pas sous la case
        box->pack_start(*sub, Gtk::PACK_SHRINK);
        return box;
    };
    scan_grid->pack_start(*check_block(m_check_recursive,
                                       _("Scan directories recursively"),
                                       _("Include subfolders when scanning for games.")),
                          Gtk::PACK_EXPAND_WIDGET);
    scan_grid->pack_start(*check_block(m_check_loose_files,
                                       _("Include loose ROM files (non-zip)"),
                                       _("Also include individual ROM files, not only archives (zip, 7z, etc).")),
                          Gtk::PACK_EXPAND_WIDGET);
    scan.body->pack_start(*scan_grid, Gtk::PACK_SHRINK);
    page->pack_start(*scan.frame, Gtk::PACK_SHRINK);

    return page;
}

// ─────────────────────────────────────────────────────────────────────────
//  Emulator
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_emulator() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    const EmulatorEntry& emu = emulator_registry().front();

    // ── Colonne de gauche : le registre ──────────────────────────────────
    auto list_card = ui::card("bc-controller.svg", _("Emulators"),
                              _("Manage emulators available in Bootcade."));
    list_card.frame->set_size_request(300, -1);
    m_emu_list.set_selection_mode(Gtk::SELECTION_SINGLE);
    m_emu_list.get_style_context()->add_class("set-rows");

    for (const auto& entry : emulator_registry()) {
        auto* line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
        line->get_style_context()->add_class("set-listrow");
        line->pack_start(*ui::tile(entry.logo, 30, 44), Gtk::PACK_SHRINK);
        auto* txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
        txt->set_valign(Gtk::ALIGN_CENTER);
        txt->pack_start(*ui::title_label(entry.name), Gtk::PACK_SHRINK);
        // La pastille d'etat de l'entree choisie est celle du panneau de
        // droite : un seul calcul, donc jamais deux verdicts contradictoires
        // sur le meme binaire.
        m_emu_status_text.set_xalign(0.0f);
        auto* state = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 7);
        m_emu_status_pill.get_style_context()->add_class("set-dot");
        m_emu_status_pill.set_valign(Gtk::ALIGN_CENTER);
        state->pack_start(m_emu_status_pill, Gtk::PACK_SHRINK);
        state->pack_start(m_emu_status_text, Gtk::PACK_SHRINK);
        txt->pack_start(*state, Gtk::PACK_SHRINK);
        line->pack_start(*txt, Gtk::PACK_EXPAND_WIDGET);
        line->pack_start(*ui::image("bc-chevron-right.svg", 16), Gtk::PACK_SHRINK);
        auto* row = Gtk::make_managed<Gtk::ListBoxRow>();
        row->add(*line);
        m_emu_list.append(*row);
    }
    m_emu_list.select_row(*m_emu_list.get_row_at_index(0));
    list_card.body->pack_start(m_emu_list, Gtk::PACK_SHRINK);

    // Ce qui reste a venir se dit en clair plutot que sous forme d'entrees
    // grisees : une liste d'emulateurs non supportes ressemble a une panne.
    auto* soon = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    soon->get_style_context()->add_class("cc-subcard");
    soon->set_margin_top(12);
    auto* soon_head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 9);
    soon_head->set_halign(Gtk::ALIGN_CENTER);
    soon_head->pack_start(*ui::image("bc-plus.svg", 17), Gtk::PACK_SHRINK);
    soon_head->pack_start(*ui::title_label(_("Add Emulator")), Gtk::PACK_SHRINK);
    soon->pack_start(*soon_head, Gtk::PACK_SHRINK);
    auto* soon_sub = ui::sub_label(_("Support for more emulators is coming."));
    soon_sub->set_justify(Gtk::JUSTIFY_CENTER);
    soon_sub->set_xalign(0.5f);
    soon->pack_start(*soon_sub, Gtk::PACK_SHRINK);
    list_card.body->pack_start(*soon, Gtk::PACK_SHRINK);
    page->pack_start(*list_card.frame, Gtk::PACK_SHRINK);

    // ── Colonne de droite : la configuration de l'emulateur choisi ───────
    auto* right = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                              ui::kCardSpacing);

    auto* head_card = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 16);
    head_card->get_style_context()->add_class("cc-card");
    auto* head = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 16);
    head->pack_start(*ui::tile(emu.logo, 42, 62), Gtk::PACK_SHRINK);
    auto* head_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 3);
    head_txt->set_valign(Gtk::ALIGN_CENTER);
    head_txt->pack_start(*ui::card_title_label(emu.name), Gtk::PACK_SHRINK);
    head_txt->pack_start(*ui::sub_label(_(emu.kind)), Gtk::PACK_SHRINK);
    head_txt->pack_start(*ui::sub_label(_(emu.description)), Gtk::PACK_SHRINK);
    head->pack_start(*head_txt, Gtk::PACK_EXPAND_WIDGET);
    m_emu_head_pill.get_style_context()->add_class("set-pill");
    m_emu_head_pill.set_valign(Gtk::ALIGN_CENTER);
    m_emu_head_dot.get_style_context()->add_class("set-dot");
    m_emu_head_dot.set_valign(Gtk::ALIGN_CENTER);
    m_emu_head_pill.pack_start(m_emu_head_dot,  Gtk::PACK_SHRINK);
    m_emu_head_pill.pack_start(m_emu_head_text, Gtk::PACK_SHRINK);
    head->pack_start(m_emu_head_pill, Gtk::PACK_SHRINK);
    head_card->pack_start(*head, Gtk::PACK_SHRINK);

    // Bande de trois tuiles : ce que le lanceur SAIT du binaire installe.
    auto* stats = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    stats->set_homogeneous(true);
    stats->pack_start(*stat_tile("bc-package.svg", _("Build"), m_lbl_emu_version),
                      Gtk::PACK_EXPAND_WIDGET);
    stats->pack_start(*stat_tile("database.svg", _("Installed"), m_lbl_emu_systems),
                      Gtk::PACK_EXPAND_WIDGET);
    stats->pack_start(*stat_tile("bc-clock.svg", _("Last checked"), m_lbl_emu_checked),
                      Gtk::PACK_EXPAND_WIDGET);
    head_card->pack_start(*stats, Gtk::PACK_SHRINK);

    auto* upd_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    m_btn_emu_updates.set_label(_("Check for updates"));
    m_btn_emu_updates.set_image(*ui::image("bc-restore.svg", ui::kIconButton));
    m_btn_emu_updates.set_always_show_image(true);
    m_btn_emu_updates.signal_clicked().connect([this] {
        m_btn_emu_updates.set_sensitive(false);
        m_lbl_emu_note.set_text(_("Checking…"));
        m_lbl_emu_note.show();
        check_emulator_update_async();
    });
    upd_line->pack_start(m_btn_emu_updates, Gtk::PACK_SHRINK);
    m_lbl_emu_note.set_xalign(0.0f);
    m_lbl_emu_note.set_valign(Gtk::ALIGN_CENTER);
    upd_line->pack_start(m_lbl_emu_note, Gtk::PACK_SHRINK);
    head_card->pack_start(*upd_line, Gtk::PACK_SHRINK);
    right->pack_start(*head_card, Gtk::PACK_SHRINK);

    // ── L'executable ─────────────────────────────────────────────────────
    auto exe = ui::card("bc-folder.svg", _("Executable"),
                        _("Select the FinalBurn Neo executable used to launch games."));
    auto* exe_line = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    m_entry_fbneo.set_hexpand(true);
    exe_line->pack_start(m_entry_fbneo, Gtk::PACK_EXPAND_WIDGET);

    m_button_browse_fbneo.set_label(_("Browse..."));
    m_button_browse_fbneo.set_image(*ui::image("folder-browse.svg", ui::kIconButton));
    m_button_browse_fbneo.set_always_show_image(true);
    m_button_browse_fbneo.signal_clicked().connect([this] {
        auto dialog = Gtk::FileChooserDialog(_("Select FBNeo Executable"), Gtk::FILE_CHOOSER_ACTION_OPEN);
        dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
        dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

        auto filter = Gtk::FileFilter::create();
        filter->set_name("Executable");
        filter->add_pattern("*fbneo*");
        dialog.add_filter(filter);

        if (!m_entry_fbneo.get_text().empty()) {
            dialog.set_filename(m_entry_fbneo.get_text());
        }

        if (dialog.run() == Gtk::RESPONSE_OK) {
            m_entry_fbneo.set_text(dialog.get_filename());
        }
    });
    exe_line->pack_start(m_button_browse_fbneo, Gtk::PACK_SHRINK);

    m_button_download_fbneo.set_label(_("Download"));
    m_button_download_fbneo.set_image(*ui::image("download.svg", ui::kIconButton));
    m_button_download_fbneo.set_always_show_image(true);
    m_button_download_fbneo.signal_clicked().connect(
        sigc::mem_fun(*this, &SettingsPanel::on_download_fbneo_clicked));
    exe_line->pack_start(m_button_download_fbneo, Gtk::PACK_SHRINK);
    exe.body->pack_start(*exe_line, Gtk::PACK_SHRINK);

    auto* exe_foot = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 12);
    exe_foot->set_margin_top(12);
    m_exe_state_text.set_xalign(0.0f);
    m_exe_state.set_valign(Gtk::ALIGN_CENTER);
    m_exe_state.pack_start(m_exe_state_icon, Gtk::PACK_SHRINK);
    m_exe_state.pack_start(m_exe_state_text, Gtk::PACK_SHRINK);
    exe_foot->pack_start(m_exe_state, Gtk::PACK_EXPAND_WIDGET);

    m_btn_test_emu.set_label(_("Test Emulator"));
    m_btn_test_emu.set_image(*ui::image("play.svg", ui::kIconButton));
    m_btn_test_emu.set_always_show_image(true);
    m_btn_test_emu.signal_clicked().connect([this] {
        const std::string path = m_entry_fbneo.get_text();
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (path.empty() || ::access(path.c_str(), X_OK) != 0) {
            refresh_emulator_state();
            if (win)
                ui::notice(*win, _("The emulator cannot be run."),
                           _("Set a valid FinalBurn Neo executable, or download one."),
                           "bc-info.svg");
            return;
        }
        // Le vrai test, c'est de le LANCER : un fichier executable qui refuse
        // de demarrer (bibliotheque manquante, architecture) passe tous les
        // controles de permissions et echoue quand meme au premier jeu.
        try {
            Glib::spawn_async("", AppContext::host_command({path}),
                              Glib::SPAWN_SEARCH_PATH | Glib::SPAWN_DO_NOT_REAP_CHILD);
            m_exe_state_text.set_text(_("Emulator started. Close its window to come back."));
        } catch (const Glib::Error& e) {
            m_exe_state_text.set_text(
                Glib::ustring::compose(_("Could not start the emulator: %1"), e.what()));
        }
    });
    exe_foot->pack_start(m_btn_test_emu, Gtk::PACK_SHRINK);
    exe.body->pack_start(*exe_foot, Gtk::PACK_SHRINK);
    right->pack_start(*exe.frame, Gtk::PACK_SHRINK);

    // ── Options propres a l'emulateur ────────────────────────────────────
    /* Elles vivent ICI et non dans General : ce sont des options de FBNeo,
     * pas de Bootcade. Le plein ecran et la mise a l'echelle entiere sont
     * exactement les reglages qu'offrait deja le menu « Launch » ; ils ne
     * sont pas dupliques, ils ont demenage, et le menu reste en phase parce
     * que les deux ecrivent la meme cle et s'ecoutent l'un l'autre.
     */
    auto options = ui::card("gear.svg", _("Options"),
                            _("Configure emulator specific options."));
    auto* opt_rows = ui::rows();

    m_switch_fullscreen.set_valign(Gtk::ALIGN_CENTER);
    m_switch_fullscreen.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_launch_options.emit();
    });
    ui::add_row(opt_rows, *ui::row("bc-window.svg", _("Launch games fullscreen"),
                                   _("Start FinalBurn Neo in fullscreen instead of a window."),
                                   &m_switch_fullscreen));

    m_switch_integerscale.set_valign(Gtk::ALIGN_CENTER);
    m_switch_integerscale.set_tooltip_text(
        _("Scales the picture by whole pixels only: sharper, with black borders."));
    m_switch_integerscale.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals) m_sig_launch_options.emit();
    });
    ui::add_row(opt_rows, *ui::row("bc-image.svg", _("Integer scaling"),
                                   _("Avoid blurry scaling by using whole pixel multiples."),
                                   &m_switch_integerscale));

    m_entry_emu_args.set_size_request(ui::kFieldWidth, -1);
    m_entry_emu_args.set_placeholder_text(_("e.g. -nohiscores"));
    m_entry_emu_args.set_tooltip_text(
        _("Passed to FinalBurn Neo before the game name, separated by spaces."));
    ui::add_row(opt_rows, *ui::row("bc-sliders.svg",
                                   _("Additional command line arguments"),
                                   _("Extra arguments added to every game launch."),
                                   &m_entry_emu_args));
    options.body->pack_start(*opt_rows, Gtk::PACK_SHRINK);
    // Meme regle qu'a l'onglet Online : la derniere carte de la colonne
    // descend jusqu'en bas pour s'aligner sur le cadre d'en face.
    right->pack_start(*options.frame, Gtk::PACK_EXPAND_WIDGET);

    page->pack_start(*right, Gtk::PACK_EXPAND_WIDGET);
    return page;
}

void SettingsPanel::set_launch_flags(bool fullscreen, bool integerscale) {
    // Pose sans emettre : appele par la fenetre principale quand SON menu a
    // bouge. Reemettre ferait revenir le signal a son emetteur.
    m_suppress_appearance_signals = true;
    m_switch_fullscreen.set_active(fullscreen);
    m_switch_integerscale.set_active(integerscale);
    m_suppress_appearance_signals = false;
}

void SettingsPanel::check_emulator_update_async() {
    std::thread([this, alive = m_alive] {
        auto r = FbneoUpdateCheck::fetch_latest();
        std::string known;
        {
            nlohmann::json j;
            std::ifstream fi(AppContext::get_config_path());
            if (fi) { try { fi >> j; } catch (...) {} }
            known = j.value("fbneo_release_sha", std::string());
        }
        {
            std::lock_guard<std::mutex> lock(m_emu_mutex);
            if (!r.ok) {
                m_emu_update_msg  = _("Could not reach the release server.");
                m_emu_update_tone = "warn";
            } else if (known.empty()) {
                // Rien a comparer : le binaire configure n'a pas ete telecharge
                // par le lanceur. Le dire est plus honnete que d'annoncer une
                // mise a jour dont on ne sait rien.
                m_emu_update_msg  = _("No baseline recorded: download through Bootcade to track updates.");
                m_emu_update_tone = "muted";
            } else if (known != r.sha) {
                m_emu_update_msg  = _("A new FinalBurn Neo build is available.");
                m_emu_update_tone = "warn";
            } else {
                m_emu_update_msg  = _("FinalBurn Neo is up to date.");
                m_emu_update_tone = "ok";
            }
        }
        // La date de verification est un fait, pas un affichage : elle est
        // ecrite pour que la tuile la retrouve au prochain lancement.
        if (r.ok) {
            nlohmann::json j;
            const std::string path = AppContext::get_config_path();
            { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) {} } }
            j["fbneo_checked_at"] = today_iso();
            std::ofstream fo(path);
            if (fo) fo << j.dump(4);
        }
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_emu_update_done.emit();
    }).detach();
}

/* Pose la hauteur exacte de la liste, dans le BON ORDRE.
 *
 * GTK refuse un minimum superieur au maximum en vigueur, et inversement :
 * poser les deux dans un ordre fixe echouait donc une fois sur deux, en
 * agrandissant ou en retrecissant selon l'ordre choisi. L'assertion sautait,
 * le minimum n'etait pas applique, et la fenetre ne suivait pas la poignee.
 * Relacher la contrainte d'abord rend les deux affectations toujours valides.
 */
void SettingsPanel::apply_roms_list_height() {
    m_scrolled_roms.set_min_content_height(-1);
    m_scrolled_roms.set_max_content_height(m_roms_list_height);
    m_scrolled_roms.set_min_content_height(m_roms_list_height);
}

/* Regle la hauteur de la liste. La fenetre se recale ensuite, en ABSOLU.
 *
 * Elle grandissait auparavant de la difference a chaque evenement de souris,
 * a partir d'une taille relue chez GTK : or un redimensionnement est
 * asynchrone, la taille relue etait donc celle d'avant, et l'erreur
 * s'additionnait a chaque pixel de deplacement. Au bout d'un glisser la
 * fenetre depassait de plusieurs centaines de pixels la hauteur de son
 * contenu : l'enorme bande vide.
 *
 * fit_to_page ne calcule pas de difference : il redemande la hauteur
 * MINIMALE de la page. Rien ne s'accumule, et tirer vers le haut retrecit la
 * fenetre aussi bien que tirer vers le bas l'agrandit.
 */
void SettingsPanel::set_roms_list_height(int height) {
    static constexpr int kMinHeight = 120;   // trois lignes : en dessous ce
                                             // n'est plus une liste
    if (height < kMinHeight)        height = kMinHeight;
    if (height > m_grip_max_height) height = m_grip_max_height;
    if (height == m_roms_list_height) return;

    m_roms_list_height = height;
    apply_roms_list_height();
    fit_to_page();
}

/* Recale la fenetre sur la hauteur de la page affichee.
 *
 * La hauteur est CALCULEE, pas devinee. On a d'abord essaye de redemander
 * 1 px en comptant sur GTK pour remonter au minimum : il ne le fait pas. Une
 * fenetre GTK3 redimensionnable accepte d'etre plus petite que le minimum de
 * son contenu, qu'elle se contente alors de rogner. La fenetre restait donc
 * a la hauteur de la premiere page ouverte, quelle que soit la page affichee
 * ensuite, et la liste agrandie a la poignee etait comprimee sans que rien ne
 * bouge : exactement le symptome constate.
 *
 * On demande donc la hauteur NATURELLE du panneau, pas la minimale : c'est
 * celle a laquelle rien n'est comprime, et c'est tout l'objet d'un ecran qui
 * ne defile pas.
 *
 * SANS la barre de titre : gtk_window_resize compte deja la barre posee par
 * set_titlebar. L'ajouter faisait une fenetre trop haute d'exactement une
 * barre, d'ou la bande vide au-dessus du pied, sur les quatre pages.
 *
 * Differe en BASSE priorite : au moment du clic la nouvelle page n'a pas
 * encore negocie sa taille, et a l'ouverture la fenetre n'est meme pas encore
 * affichee.
 */
void SettingsPanel::fit_to_page() {
    // Une seule en attente : un glisser emet des dizaines d'evenements, et
    // autant de redimensionnements empiles se marcheraient dessus.
    m_fit_conn.disconnect();
    m_fit_conn = Glib::signal_idle().connect([this] {
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        if (win) {
            int panel_min = 0, panel_nat = 0;
            get_preferred_height(panel_min, panel_nat);
            int w = 0, h = 0;
            win->get_size(w, h);
            const int wanted = panel_nat;
            if (wanted > 0 && wanted != h) win->resize(w, wanted);
        }
        return false;           // une seule fois
    }, Glib::PRIORITY_LOW);
}

void SettingsPanel::refresh_emulator_state() {
    const std::string exe = get_fbneo_executable();
    const bool ready = !exe.empty() && ::access(exe.c_str(), X_OK) == 0;

    m_emu_status_text.set_text(ready ? _("Active") : _("Not configured"));
    m_emu_head_text.set_text(ready ? _("Active") : _("Not configured"));
    for (auto* w : {static_cast<Gtk::Widget*>(&m_emu_head_dot),
                    static_cast<Gtk::Widget*>(&m_emu_head_text),
                    static_cast<Gtk::Widget*>(&m_emu_head_pill)}) {
        auto c = w->get_style_context();
        c->remove_class("set-ok");
        c->remove_class("set-off");
        c->add_class(ready ? "set-ok" : "set-off");
    }
    auto pill = m_emu_status_pill.get_style_context();
    pill->remove_class("set-ok");
    pill->remove_class("set-err");
    pill->add_class(ready ? "set-ok" : "set-err");
    auto txt = m_emu_status_text.get_style_context();
    txt->remove_class("set-ok");
    txt->remove_class("set-sub");
    txt->add_class(ready ? "set-ok" : "set-sub");

    if (ready) {
        m_exe_state_icon.set(IconManager::load("icons/bc-detected.svg", 16, 16));
        m_exe_state_text.set_text(_("Executable found and working."));
        m_exe_state_text.get_style_context()->remove_class("set-err");
        m_exe_state_text.get_style_context()->add_class("set-ok");
    } else {
        m_exe_state_icon.set(IconManager::load("icons/bc-info.svg", 16, 16));
        m_exe_state_text.set_text(exe.empty()
            ? std::string(_("No executable selected yet."))
            : std::string(_("This path is not an executable Bootcade can run.")));
        m_exe_state_text.get_style_context()->remove_class("set-ok");
        m_exe_state_text.get_style_context()->add_class("set-err");
    }

    nlohmann::json j;
    { std::ifstream fi(AppContext::get_config_path()); if (fi) { try { fi >> j; } catch (...) {} } }
    // « Build » : l'empreinte du commit que le lanceur a telecharge. C'est la
    // seule identite de version que cette distribution de FBNeo expose ; en
    // inventer un numero serait plus lisible et faux.
    std::string sha = j.value("fbneo_release_sha", std::string());
    m_lbl_emu_version.set_text(sha.empty() ? std::string(_("Unknown"))
                                           : sha.substr(0, 7));
    const std::string installed = file_date(exe);
    m_lbl_emu_systems.set_text(installed.empty() ? "—" : installed);
    const std::string checked = j.value("fbneo_checked_at", std::string());
    m_lbl_emu_checked.set_text(checked.empty() ? std::string(_("Never")) : checked);
}

// ─────────────────────────────────────────────────────────────────────────
//  Online
// ─────────────────────────────────────────────────────────────────────────

Gtk::Widget* SettingsPanel::build_page_online() {
    auto* page = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL,
                                             ui::kCardSpacing);
    page->get_style_context()->add_class("set-page");

    auto* left = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL,
                                             ui::kCardSpacing);

    // ── Compte ───────────────────────────────────────────────────────────
    // Ne pas avoir de compte est un ETAT NORMAL, pas une erreur : Bootcade se
    // joue entierement hors ligne. La carte ne porte donc ni avertissement ni
    // couleur d'alerte quand personne n'est connecte.
    auto account = ui::card("bc-account.svg", _("Account"),
                            _("Sign in to access Bootcade online features and "
                              "manage your profile."));
    m_account_row.get_style_context()->add_class("cc-subcard");
    m_account_name.set_xalign(0.0f);
    m_account_sub.set_xalign(0.0f);
    m_account_text.set_valign(Gtk::ALIGN_CENTER);
    m_account_text.pack_start(m_account_name, Gtk::PACK_SHRINK);
    m_account_text.pack_start(m_account_sub,  Gtk::PACK_SHRINK);
    m_account_avatar.set_valign(Gtk::ALIGN_CENTER);
    m_account_row.pack_start(m_account_avatar, Gtk::PACK_SHRINK);
    m_account_row.pack_start(m_account_text,   Gtk::PACK_EXPAND_WIDGET);

    auto* acc_buttons = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 9);
    acc_buttons->set_valign(Gtk::ALIGN_CENTER);
    m_button_manage_account.set_label(_("Manage Account"));
    m_button_manage_account.set_image(*ui::image("bc-account-manage.svg", ui::kIconButton));
    m_button_manage_account.set_always_show_image(true);
    m_button_manage_account.signal_clicked().connect([this] {
        /* Le COMPTE, pas le profil : ce sont deux choses differentes.
         *
         * Le profil est la vitrine publique sur le site : pseudo, avatar,
         * scores. Le compte est ce que gere Keycloak : mot de passe, adresse
         * de courriel, double authentification, sessions ouvertes, suppression
         * du compte. « Manage Account » doit mener la, et nulle part ailleurs.
         */
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        const std::string url = BootcadeAuth::account_console_url();
        gtk_show_uri_on_window(win ? GTK_WINDOW(win->gobj()) : nullptr,
                               url.c_str(), GDK_CURRENT_TIME, nullptr);
    });
    acc_buttons->pack_start(m_button_manage_account, Gtk::PACK_SHRINK);

    m_button_account.signal_clicked().connect([this] {
        if (BootcadeAuth::signed_in()) {
            BootcadeAuth::sign_out();
        } else {
            auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
            if (!win) return;
            LoginDialog dlg(*win);
            dlg.run();
        }
        refresh_account_row();
        m_sig_account_changed.emit();
    });
    acc_buttons->pack_start(m_button_account, Gtk::PACK_SHRINK);
    m_account_row.pack_start(*acc_buttons, Gtk::PACK_SHRINK);
    account.body->pack_start(m_account_row, Gtk::PACK_SHRINK);
    left->pack_start(*account.frame, Gtk::PACK_SHRINK);

    // ── Fonctions en ligne ───────────────────────────────────────────────
    auto features = ui::card("bc-globe.svg", _("Online Features"),
                             _("Configure Bootcade's online services."));
    auto* feat_rows = ui::rows();
    m_switch_hiscore.set_active(false);
    m_switch_hiscore.set_valign(Gtk::ALIGN_CENTER);
    m_switch_hiscore.set_tooltip_text(
        _("On, your scores and play time are published to the leaderboard. Off, no badge is shown and the launcher never contacts the service."));
    m_switch_hiscore.property_active().signal_changed().connect([this] {
        if (!m_suppress_appearance_signals)
            m_sig_hiscore_toggled.emit(m_switch_hiscore.get_active());
    });
    ui::add_row(feat_rows, *ui::row("bc-trophy.svg", _("Online highscores"),
                                    _("Publish your scores and use Bootcade leaderboards."),
                                    &m_switch_hiscore));

    /* Les trois reglages suivants commandent chacun quelque chose de precis.
     *
     * Ils ne sont pas trois nuances du meme interrupteur : le premier decide
     * si l'on TELECHARGE les classements des autres, le deuxieme QUAND on
     * synchronise, le troisieme ce qu'on JOINT a un score envoye. On peut
     * vouloir publier ses scores sans rapatrier ceux du monde entier, ou
     * publier un score sans annoncer combien d'heures on y a passe.
     */
    m_switch_community.set_valign(Gtk::ALIGN_CENTER);
    m_switch_community.set_active(true);
    m_switch_community.set_tooltip_text(
        _("Off, Bootcade never downloads global leaderboards; your own scores are still published."));
    ui::add_row(feat_rows, *ui::row("bc-users.svg", _("Community features"),
                                    _("Access global leaderboards and player rankings."),
                                    &m_switch_community));

    m_switch_autosync.set_valign(Gtk::ALIGN_CENTER);
    m_switch_autosync.set_active(true);
    m_switch_autosync.set_tooltip_text(
        _("Off, scores stay queued until you sync from the account menu. Nothing is ever lost."));
    ui::add_row(feat_rows, *ui::row("bc-cloud.svg", _("Automatic sync"),
                                    _("Send queued scores and refresh leaderboards at startup."),
                                    &m_switch_autosync));

    m_switch_playstats.set_valign(Gtk::ALIGN_CENTER);
    m_switch_playstats.set_active(true);
    m_switch_playstats.set_tooltip_text(
        _("Off, your scores are still published, but without how long you played."));
    ui::add_row(feat_rows, *ui::row("bc-chart.svg", _("Share play statistics"),
                                    _("Include your play time alongside a published score."),
                                    &m_switch_playstats));

    m_hiscore_hint.set_xalign(0.0f);
    m_hiscore_hint.get_style_context()->add_class("set-sub");
    m_hiscore_hint.set_margin_start(14);
    m_hiscore_hint.set_margin_top(8);

    // Le pays reste ici : il accompagne le score publie, et c'est le seul
    // endroit ou un joueur sans compte peut le choisir.
    m_entry_hiscore_country.set_placeholder_text(_("start typing, e.g. France"));
    m_entry_hiscore_country.set_size_request(ui::kFieldWidth, -1);
    build_country_completion();
    ui::add_row(feat_rows, *ui::row("bc-globe.svg", _("Country"),
                                    _("Shown next to your score on the leaderboard."),
                                    &m_entry_hiscore_country));
    features.body->pack_start(*feat_rows, Gtk::PACK_SHRINK);
    features.body->pack_start(m_hiscore_hint, Gtk::PACK_SHRINK);
    left->pack_start(*features.frame, Gtk::PACK_SHRINK);

    // ── Etat du reseau ───────────────────────────────────────────────────
    /* NET est INDEPENDANT du compte et des classements.
     *
     * Il vient de la sonde de joignabilite et d'elle seule : une reponse HTTP,
     * quel qu'en soit le code, prouve que le service repond. Le deduire de
     * l'authentification afficherait « hors ligne » a tout joueur sans compte,
     * sur une machine parfaitement connectee.
     */
    auto network = ui::card("bc-wifi.svg", _("Network Status"),
                            _("Current connection status to Bootcade services."));
    m_net_row.get_style_context()->add_class("cc-subcard");
    m_net_dot.get_style_context()->add_class("set-dot");
    m_net_dot.set_valign(Gtk::ALIGN_CENTER);
    m_net_row.pack_start(m_net_dot, Gtk::PACK_SHRINK);
    auto* net_txt = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 2);
    net_txt->set_valign(Gtk::ALIGN_CENTER);
    m_net_title.set_xalign(0.0f);
    m_net_title.get_style_context()->add_class("set-row-title");
    m_net_sub.set_xalign(0.0f);
    m_net_sub.get_style_context()->add_class("set-sub");
    net_txt->pack_start(m_net_title, Gtk::PACK_SHRINK);
    net_txt->pack_start(m_net_sub,   Gtk::PACK_SHRINK);
    m_net_row.pack_start(*net_txt, Gtk::PACK_EXPAND_WIDGET);
    m_btn_test_net.set_label(_("Test Connection"));
    m_btn_test_net.set_image(*ui::image("bc-wifi.svg", ui::kIconButton));
    m_btn_test_net.set_always_show_image(true);
    m_btn_test_net.signal_clicked().connect([this] { probe_network_async(); });
    m_net_row.pack_start(m_btn_test_net, Gtk::PACK_SHRINK);
    network.body->pack_start(m_net_row, Gtk::PACK_SHRINK);
    /* La DERNIERE carte de la colonne absorbe la place restante.
     *
     * Sans cela la colonne s'arrete a la hauteur de son contenu tandis que la
     * carte d'en face, seule dans sa colonne, descend jusqu'en bas : les deux
     * colonnes se terminaient a des hauteurs differentes. C'est la derniere
     * carte qui s'etire, pas les intervalles entre les cartes, sinon la page
     * se disloquerait au lieu de s'aligner.
     */
    left->pack_start(*network.frame, Gtk::PACK_EXPAND_WIDGET);
    page->pack_start(*left, Gtk::PACK_EXPAND_WIDGET);
    set_network_state(0);

    // ── Colonne de droite : le profil ────────────────────────────────────
    auto profile = ui::card("bc-trophy.svg", _("Your Profile"),
                            _("Your Bootcade identity, as the launcher knows it."));
    profile.frame->set_size_request(360, -1);

    m_profile_avatar.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_avatar, Gtk::PACK_SHRINK);
    m_profile_name.get_style_context()->add_class("set-card-title");
    m_profile_name.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_name, Gtk::PACK_SHRINK);
    m_profile_country.get_style_context()->add_class("set-sub");
    m_profile_country.set_halign(Gtk::ALIGN_CENTER);
    m_profile_signed.pack_start(m_profile_country, Gtk::PACK_SHRINK);
    m_profile_dot.get_style_context()->add_class("set-dot");
    m_profile_dot.set_valign(Gtk::ALIGN_CENTER);
    m_profile_state.set_halign(Gtk::ALIGN_CENTER);
    m_profile_state.pack_start(m_profile_dot, Gtk::PACK_SHRINK);
    m_profile_state.pack_start(m_profile_state_text, Gtk::PACK_SHRINK);
    m_profile_signed.pack_start(m_profile_state, Gtk::PACK_SHRINK);
    m_profile_signed.pack_start(*ui::hairline(), Gtk::PACK_SHRINK);

    /* Trois compteurs, tous LOCAUX.
     *
     * Les classements personnels viennent du cache que la couche de scores
     * tient deja ; les jeux joues et les favoris, de la base. Aucun n'est
     * demande au serveur : la carte doit s'afficher hors ligne, et un
     * compteur qui disparait quand le reseau tombe ressemble a une perte de
     * donnees.
     */
    auto* stats_row = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_HORIZONTAL, 10);
    stats_row->set_homogeneous(true);
    auto stat = [](const std::string& icon, Gtk::Label& value,
                   const std::string& label) {
        auto* box = Gtk::make_managed<Gtk::Box>(Gtk::ORIENTATION_VERTICAL, 4);
        box->get_style_context()->add_class("cc-subcard");
        box->pack_start(*ui::image(icon, 20), Gtk::PACK_SHRINK);
        value.set_halign(Gtk::ALIGN_CENTER);
        value.get_style_context()->add_class("set-stat-value");
        box->pack_start(value, Gtk::PACK_SHRINK);
        auto* l = ui::sub_label(label);
        l->set_halign(Gtk::ALIGN_CENTER);
        l->set_xalign(0.5f);
        box->pack_start(*l, Gtk::PACK_SHRINK);
        return box;
    };
    stats_row->pack_start(*stat("bc-trophy.svg", m_stat_hiscores, _("Highscores")),
                          Gtk::PACK_EXPAND_WIDGET);
    stats_row->pack_start(*stat("bc-controller.svg", m_stat_played, _("Games played")),
                          Gtk::PACK_EXPAND_WIDGET);
    stats_row->pack_start(*stat("star-gold.svg", m_stat_favorites, _("Favorites")),
                          Gtk::PACK_EXPAND_WIDGET);
    m_profile_signed.pack_start(*stats_row, Gtk::PACK_SHRINK);
    // Le seul compteur que le lanceur detient VRAIMENT : ce qui attend d'etre
    // envoye. Le service n'expose ni succes ni nombre de parties, et les
    // dessiner quand meme ferait de cette carte une vitrine.
    m_profile_queued.set_halign(Gtk::ALIGN_CENTER);
    m_profile_queued.get_style_context()->add_class("set-sub");
    m_profile_signed.pack_start(m_profile_queued, Gtk::PACK_SHRINK);
    m_btn_view_profile.set_label(_("View full profile"));
    m_btn_view_profile.set_image(*ui::image("bc-external.svg", ui::kIconButton));
    m_btn_view_profile.set_always_show_image(true);
    m_btn_view_profile.signal_clicked().connect([this] {
        auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
        std::string url = "https://bootcade.netlify.app/profile/";
        if (BootcadeAuth::signed_in()) url += "?sso=1";
        gtk_show_uri_on_window(win ? GTK_WINDOW(win->gobj()) : nullptr,
                               url.c_str(), GDK_CURRENT_TIME, nullptr);
    });
    m_profile_signed.pack_start(m_btn_view_profile, Gtk::PACK_SHRINK);
    profile.body->pack_start(m_profile_signed, Gtk::PACK_SHRINK);

    // Sans compte, la carte explique ce qu'un compte apporte : elle n'annonce
    // pas un probleme, parce qu'il n'y en a pas.
    m_profile_empty.pack_start(*ui::tile("bc-account.svg", 26, 54), Gtk::PACK_SHRINK);
    auto* empty_title = ui::title_label(_("No account"));
    empty_title->set_halign(Gtk::ALIGN_CENTER);
    m_profile_empty.pack_start(*empty_title, Gtk::PACK_SHRINK);
    auto* empty_sub = ui::sub_label(
        _("Bootcade works fully offline. Sign in only if you want your scores "
          "to appear on the online leaderboards."));
    empty_sub->set_justify(Gtk::JUSTIFY_CENTER);
    empty_sub->set_xalign(0.5f);
    empty_sub->set_max_width_chars(34);
    m_profile_empty.pack_start(*empty_sub, Gtk::PACK_SHRINK);
    profile.body->pack_start(m_profile_empty, Gtk::PACK_SHRINK);

    page->pack_start(*profile.frame, Gtk::PACK_SHRINK);
    return page;
}

void SettingsPanel::set_network_state(int state) {
    static const char* kTones[] = {"set-sub", "set-ok", "set-err"};
    m_net_title.set_text(state == 1 ? _("Connected")
                       : state == 2 ? _("Offline")
                                    : _("Checking…"));
    m_net_sub.set_text(state == 1 ? _("Bootcade services are reachable.")
                     : state == 2 ? _("Bootcade works normally; online features wait.")
                                  : _("Testing the connection to Bootcade services."));
    for (auto* w : {static_cast<Gtk::Widget*>(&m_net_dot),
                    static_cast<Gtk::Widget*>(&m_net_title)}) {
        auto ctx = w->get_style_context();
        for (const char* c : kTones) ctx->remove_class(c);
        ctx->add_class(kTones[state < 0 || state > 2 ? 0 : state]);
    }
    // Le profil affiche le MEME etat reseau : deux pastilles contradictoires
    // dans la meme page ont deja ete un bug, on ne le refait pas.
    auto pctx = m_profile_dot.get_style_context();
    for (const char* c : kTones) pctx->remove_class(c);
    pctx->add_class(kTones[state < 0 || state > 2 ? 0 : state]);
    m_profile_state_text.set_text(state == 1 ? _("Online") : _("Offline"));
}

void SettingsPanel::probe_network_async() {
    m_btn_test_net.set_sensitive(false);
    set_network_state(0);
    std::thread([this, alive = m_alive] {
        m_net_state.store(HiscoreClient::probe_reachable() ? 1 : 2);
        std::lock_guard<std::mutex> live(alive->mutex);
        if (alive->alive) m_net_done.emit();
    }).detach();
}

void SettingsPanel::refresh_profile_stats() {
    // Les classements personnels sont un cache local : ils restent lisibles
    // hors ligne, ce qui est precisement le cas ou l'on veut encore savoir ou
    // l'on en est.
    m_stat_hiscores.set_text(
        std::to_string(HiscoreClient::cached_personal_ranks().size()));
    if (m_database) {
        m_stat_played.set_text(std::to_string(m_database->countPlayedGames()));
        m_stat_favorites.set_text(std::to_string(m_database->countFavorites()));
    } else {
        // Sans base, on n'affiche pas zero : zero est une affirmation, et
        // celle-ci serait fausse.
        m_stat_played.set_text("—");
        m_stat_favorites.set_text("—");
    }
}

void SettingsPanel::on_window_shown() {
    // Ce que la fenetre principale a pu changer pendant que l'ecran etait
    // ferme : la session, l'emulateur telecharge depuis un menu.
    refresh_account_row();
    refresh_emulator_state();
    refresh_roms_list();
    refresh_profile_stats();
    // A l'ouverture aussi : la fenetre est creee avant que les pages aient
    // negocie leur taille, et resterait sur une hauteur d'avance.
    fit_to_page();
    m_btn_test_net.set_sensitive(true);
    probe_network_async();
}

// ─────────────────────────────────────────────────────────────────────────
//  Pied : valeurs par defaut, cache, remise a zero
// ─────────────────────────────────────────────────────────────────────────

/* Remet les REGLAGES a leur valeur d'usine, sans toucher aux donnees.
 *
 * Les chemins, la bibliotheque et le compte restent : ce bouton repond a
 * « j'ai bricole trois options et je ne sais plus lesquelles », pas a
 * « efface tout ». La remise a zero complete, elle, a son propre bouton et sa
 * propre confirmation dans Data & Storage.
 */
void SettingsPanel::apply_defaults() {
    m_suppress_appearance_signals = true;
    m_combo_theme.set_active_id("dark");
    m_combo_language.set_active_id("");
    m_suppress_appearance_signals = false;
    // Emis explicitement : le theme et la langue doivent suivre tout de suite,
    // sinon l'ecran annonce des valeurs que l'application n'applique pas.
    m_sig_theme_changed.emit(m_combo_theme.get_active_id());
    m_sig_language_changed.emit(m_combo_language.get_active_id());

    m_combo_startup.set_active_id("last_played");
    m_switch_window_state.set_active(true);
    m_switch_play_history.set_active(true);
    m_switch_auto_update.set_active(true);
    m_check_recursive.set_active(true);
    m_check_loose_files.set_active(true);
    m_switch_community.set_active(true);
    m_switch_autosync.set_active(true);
    m_switch_playstats.set_active(true);
    m_switch_fullscreen.set_active(false);
    m_switch_integerscale.set_active(false);
    m_entry_emu_args.set_text("");
    // Le menu « Launch » de la fenetre principale doit suivre : deux endroits
    // qui affichent le meme reglage ne doivent jamais diverger.
    m_sig_launch_options.emit();
}

void SettingsPanel::on_restore_defaults_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;
    ConfirmationDialog dlg(
        *win, _("Restore default settings?"),
        _("Appearance, startup, behavior, update and scan options go back to "
          "their default values.\n\nYour ROM folders, artwork paths, emulator "
          "and account are left untouched."),
        "↺", false);
    if (!dlg.show_and_confirm()) return;
    apply_defaults();
}

void SettingsPanel::on_clear_cache_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;

    /* Ce qui est efface est EXACTEMENT ce qui se reconstruit tout seul.
     *
     * Ni games.db (elle porte les favoris et l'historique de jeu, que rien ne
     * regenere), ni la file d'attente des scores (un score gare est du travail
     * du joueur, pas un fichier temporaire).
     */
    const std::string dir = AppContext::get_user_config_dir();
    const std::vector<std::string> files = {
        dir + "/filter_cache.json",
        dir + "/hiscore-cache.json",
        dir + "/debug.log",
    };
    uintmax_t total = 0;
    for (const auto& f : files) {
        std::error_code ec;
        auto size = std::filesystem::file_size(f, ec);
        if (!ec) total += size;
    }
    if (total == 0) {
        ui::notice(*win, _("Nothing to clear."),
                   _("The local cache is already empty."));
        return;
    }

    /* En kilo-octets sous le mega-octet.
     *
     * Un cache de 15 Ko s'annoncait « 0.0 MB », ce qui dit exactement le
     * contraire de ce que fait le bouton : rien a effacer. Un ordre de
     * grandeur faux est pire qu'une unite inhabituelle.
     */
    const Glib::ustring size_text =
        total >= 1024 * 1024
            ? Glib::ustring::format(std::fixed, std::setprecision(1),
                                    double(total) / (1024.0 * 1024.0)) + " MB"
            : Glib::ustring::format(std::max<uintmax_t>(1, total / 1024)) + " kB";
    ConfirmationDialog dlg(
        *win, _("Clear local cache?"),
        Glib::ustring::compose(
            _("Bootcade will delete %1 of cached filters, cached leaderboards "
              "and the debug log.\n\nYour games, favourites, play history and "
              "pending scores are not touched. Everything deleted here is "
              "rebuilt automatically."),
            size_text),
        "🧹", true);
    if (!dlg.show_and_confirm()) return;

    for (const auto& f : files) {
        std::error_code ec;
        std::filesystem::remove(f, ec);
    }
    ui::notice(*win, _("Local cache cleared."),
               _("The deleted files are rebuilt automatically as you use Bootcade."),
               "bc-detected.svg");
}

void SettingsPanel::on_reset_settings_clicked() {
    auto* win = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!win) return;
    ConfirmationDialog dlg(
        *win, _("Reset all settings?"),
        _("Every setting goes back to its default value, including your ROM "
          "folders, artwork paths and emulator path. Bootcade will need to be "
          "configured again.\n\nYour games, favourites and play history stay "
          "where they are, and you stay signed in."),
        "⚠️", true);
    if (!dlg.show_and_confirm()) return;

    apply_defaults();
    set_roms_paths({});
    set_dat_path("");
    set_previews_path("");
    set_titles_path("");
    set_fbneo_executable("");
    refresh_emulator_state();
    // Ecrit tout de suite : une remise a zero confirmee qu'un Cancel annulerait
    // laisserait le joueur croire qu'elle a eu lieu.
    save_to_file(AppContext::get_config_path());
}

// ─────────────────────────────────────────────────────────────────────────
//  Scores en ligne : identite et pays (comportement inchange)
// ─────────────────────────────────────────────────────────────────────────

std::string SettingsPanel::get_hiscore_player() const {
    return std::string(m_entry_hiscore_player.get_text());
}

void SettingsPanel::ensure_hiscore_identity() {
    // Both are filled only when missing: a name or a country the player has
    // set, in any past version, is never overwritten.
    if (get_hiscore_player().empty())
        m_entry_hiscore_player.set_text(generated_player_name());
    if (get_hiscore_country().empty())
        set_hiscore_country(locale_country_code());
}

void SettingsPanel::record_hiscore_answer(bool publish) {
    m_hiscore_asked = true;
    // Assigned through the switch rather than to a separate flag, so the
    // answer travels the same signal as a later change made in Settings: the
    // badges appear or vanish immediately, with no second code path to keep
    // in step.
    m_switch_hiscore.set_active(publish);
}

bool SettingsPanel::is_hiscore_enabled() const {
    return m_switch_hiscore.get_active();
}

// -> the ISO code, or "" when the field is empty or holds something that is
// not a country. Resolved on read rather than pinned when a completion is
// accepted, so a name typed in full without touching the popup still counts.
std::string SettingsPanel::get_hiscore_country() const {
    std::string text = m_entry_hiscore_country.get_text();
    while (!text.empty() && std::isspace((unsigned char)text.front())) text.erase(text.begin());
    while (!text.empty() && std::isspace((unsigned char)text.back()))  text.pop_back();
    if (text.empty()) return {};

    std::string upper = text;
    for (auto& c : upper) c = std::toupper((unsigned char)c);
    if (upper.size() == 2)
        for (const auto& c : kCountries)
            if (upper == c.code) return c.code;

    // Case-insensitive, and against BOTH the translated name and the English
    // one. The translation is what the field shows, but a config written under
    // another language : or a name typed in English : must still resolve.
    Glib::ustring folded = Glib::ustring(text).lowercase();
    for (const auto& c : kCountries)
        if (folded == Glib::ustring(_(c.name)).lowercase()
         || folded == Glib::ustring(c.name).lowercase())
            return c.code;

    // Typed but unrecognised: no country rather than a guess. A wrong flag on
    // a leaderboard is worse than none, and the field is optional anyway.
    return {};
}

void SettingsPanel::set_hiscore_country(const std::string& code) {
    for (const auto& c : kCountries)
        if (code == c.code) { m_entry_hiscore_country.set_text(_(c.name)); return; }
    m_entry_hiscore_country.set_text("");
}

void SettingsPanel::build_country_completion() {
    m_country_model = Gtk::ListStore::create(m_country_cols);
    // Sorted on the TRANSLATED name: kCountries is alphabetical in English, so
    // a French list would run Afghanistan, Albanie, Algérie… then jump about.
    std::vector<std::pair<std::string, std::string>> sorted;
    sorted.reserve(sizeof(kCountries) / sizeof(kCountries[0]));
    for (const auto& c : kCountries) sorted.emplace_back(_(c.name), c.code);
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) {
                  return g_utf8_collate(a.first.c_str(), b.first.c_str()) < 0;
              });
    for (const auto& [name, code] : sorted) {
        auto row = *m_country_model->append();
        row[m_country_cols.name] = name;
        row[m_country_cols.code] = code;
    }
    m_country_completion = Gtk::EntryCompletion::create();
    m_country_completion->set_model(m_country_model);
    m_country_completion->set_text_column(m_country_cols.name);
    m_country_completion->set_minimum_key_length(1);
    m_country_completion->set_popup_completion(true);
    // No inline completion: it would type ahead of the user inside the entry,
    // and "FR" would become "France" mid-keystroke while they meant "FRO".
    m_country_completion->set_inline_completion(false);

    // Matches a prefix of the name OR the two-letter code, so both "fra" and
    // "fr" find France. GTK hands `key` already folded to lower case.
    m_country_completion->set_match_func(
        [this](const Glib::ustring& key, const Gtk::TreeModel::const_iterator& iter) {
            if (!iter) return false;
            Glib::ustring name = (*iter)[m_country_cols.name];
            Glib::ustring code = (*iter)[m_country_cols.code];
            return name.lowercase().find(key) == 0 || code.lowercase().find(key) == 0;
        });
    m_entry_hiscore_country.set_completion(m_country_completion);

    // Unrecognised text is marked as you type. Without this the field fails
    // silently : a typo simply means no flag, discovered days later on the
    // leaderboard, with nothing on screen to explain it.
    m_entry_hiscore_country.signal_changed().connect([this] {
        bool typed = !m_entry_hiscore_country.get_text().empty();
        bool known = !get_hiscore_country().empty();
        auto ctx = m_entry_hiscore_country.get_style_context();
        if (typed && !known) ctx->add_class("error");
        else                 ctx->remove_class("error");
    });
}

std::string SettingsPanel::get_theme() const {
    auto id = m_combo_theme.get_active_id();
    return id.empty() ? "system" : std::string(id);
}
void SettingsPanel::set_theme(const std::string& mode) {
    m_suppress_appearance_signals = true;
    m_combo_theme.set_active_id(mode.empty() ? "system" : mode);
    m_suppress_appearance_signals = false;
}
std::string SettingsPanel::get_language() const {
    return std::string(m_combo_language.get_active_id());
}
void SettingsPanel::set_language(const std::string& code) {
    m_suppress_appearance_signals = true;
    m_combo_language.set_active_id(code);
    m_suppress_appearance_signals = false;
}

void SettingsPanel::on_folder_clicked(Gtk::Entry* entry) {
    auto dialog = Gtk::FileChooserDialog(_("Select Folder"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

    auto current = entry->get_text();
    if (!current.empty()) {
        dialog.set_current_folder(Glib::path_get_dirname(current));
    }

    if (dialog.run() == Gtk::RESPONSE_OK) {
        entry->set_text(dialog.get_filename());
    }
}

// --- Getters ---
std::string SettingsPanel::get_roms_path() const {
    // Deprecated: return first path for compatibility
    return m_roms_paths.empty() ? "" : m_roms_paths[0];
}

std::vector<std::string> SettingsPanel::get_roms_paths() const {
    return m_roms_paths;
}

std::string SettingsPanel::get_dat_path() const { return m_entry_dat.get_text(); }
std::string SettingsPanel::get_previews_path() const { return m_entry_previews.get_text(); }
std::string SettingsPanel::get_outbox_path() const { return m_entry_outbox.get_text(); }
std::string SettingsPanel::get_quarantine_path() const { return m_entry_quarantine.get_text(); }
void SettingsPanel::set_outbox_path(const std::string& path) { m_entry_outbox.set_text(path); }
void SettingsPanel::set_quarantine_path(const std::string& path) { m_entry_quarantine.set_text(path); }
std::string SettingsPanel::get_titles_path() const { return m_entry_titles.get_text(); }
std::string SettingsPanel::get_fbneo_executable() const { return m_entry_fbneo.get_text(); }

// --- Setters ---
void SettingsPanel::set_roms_path(const std::string& path) {
    // Deprecated: set as first path for compatibility
    if (m_roms_paths.empty()) {
        m_roms_paths.push_back(path);
    } else {
        m_roms_paths[0] = path;
    }
    refresh_roms_list();
}

void SettingsPanel::set_roms_paths(const std::vector<std::string>& paths) {
    m_roms_paths = paths;
    refresh_roms_list();
}

void SettingsPanel::add_roms_path(const std::string& path) {
    m_roms_paths.push_back(path);
    refresh_roms_list();
}

void SettingsPanel::remove_roms_path(int index) {
    if (index >= 0 && index < static_cast<int>(m_roms_paths.size())) {
        m_roms_paths.erase(m_roms_paths.begin() + index);
        refresh_roms_list();
    }
}

void SettingsPanel::set_dat_path(const std::string& path) { m_entry_dat.set_text(path); }
void SettingsPanel::set_previews_path(const std::string& path) { m_entry_previews.set_text(path); }
void SettingsPanel::set_titles_path(const std::string& path) { m_entry_titles.set_text(path); }
void SettingsPanel::set_fbneo_executable(const std::string& path) {
    m_entry_fbneo.set_text(path);
}

// --- Load / Save ---
bool SettingsPanel::load_from_file(const std::string& filename) {
    std::ifstream file(filename);
    if (!file.is_open()) return false;

    try {
        nlohmann::json j;
        file >> j;

        // Load multiple ROMs paths if available, fallback to single path for compatibility
        if (j.contains("roms_paths") && j["roms_paths"].is_array()) {
            std::vector<std::string> paths;
            for (const auto& path : j["roms_paths"]) {
                paths.push_back(path.get<std::string>());
            }
            set_roms_paths(paths);
        } else if (j.contains("roms_path")) {
            // Legacy single path support
            set_roms_path(j["roms_path"]);
        }

        if (j.contains("dat_path")) set_dat_path(j["dat_path"]);
        if (j.contains("previews_path")) set_previews_path(j["previews_path"]);
        if (j.contains("rom_manager") && j["rom_manager"].is_object()) {
            const auto& rm = j["rom_manager"];
            if (rm.contains("outbox_path") && rm["outbox_path"].is_string())         set_outbox_path(rm["outbox_path"]);
            if (rm.contains("quarantine_path") && rm["quarantine_path"].is_string()) set_quarantine_path(rm["quarantine_path"]);
        }
        if (j.contains("titles_path")) set_titles_path(j["titles_path"]);
        if (j.contains("scan_recursive")) m_check_recursive.set_active(j["scan_recursive"].get<bool>());
        if (j.contains("scan_loose_files")) m_check_loose_files.set_active(j["scan_loose_files"].get<bool>());

        // Legacy compatibility for thumbnails_path
        if (j.contains("thumbnails_path") && !j.contains("previews_path")) {
            set_previews_path(j["thumbnails_path"]);
        }

        if (j.contains("fbneo_executable")) set_fbneo_executable(j["fbneo_executable"]);
        if (j.contains("theme")) set_theme(j["theme"].get<std::string>());
        if (j.contains("language")) set_language(j["language"].get<std::string>());
        if (j.contains("hiscore_player"))
            m_entry_hiscore_player.set_text(j["hiscore_player"].get<std::string>());
        if (j.contains("startup_selection"))
            m_combo_startup.set_active_id(j["startup_selection"].get<std::string>());
        // Absent means off. Opting in has to be a deliberate act, so a config
        // written before this option existed must not switch it on.
        m_switch_hiscore.set_active(j.value("hiscore_enabled", false));
        // Absent means nobody has ever been asked : except on a config that
        // already carries a choice under the old always-on default, whose
        // owner has been publishing for weeks and must not be interrogated
        // about a decision they already live with.
        m_hiscore_asked = j.value("hiscore_asked", j.contains("hiscore_enabled"));
        if (j.contains("hiscore_country"))
            set_hiscore_country(j["hiscore_country"].get<std::string>());
        // Les trois reglages de comportement. Absents, ils valent « oui » :
        // c'est ce que le lanceur faisait avant qu'ils soient reglables, et
        // une mise a jour ne doit pas eteindre en silence ce qui marchait.
        // La hauteur choisie a la poignee. Relue avant que la fenetre existe,
        // donc posee directement sur le widget : set_roms_list_height
        // voudrait redimensionner une fenetre qui n'est pas encore la.
        m_roms_list_height = j.value("roms_list_height", 200);
        if (m_roms_list_height < 120)  m_roms_list_height = 120;
        if (m_roms_list_height > 1200) m_roms_list_height = 1200;
        apply_roms_list_height();
        m_switch_window_state.set_active(j.value("restore_window_state", true));
        m_switch_play_history.set_active(j.value("keep_play_history", true));
        m_switch_auto_update.set_active(j.value("check_updates_auto", true));
        // Les fonctions en ligne : allumees par defaut, parce que c'est ce que
        // le lanceur faisait deja quand les classements etaient actifs. Les
        // eteindre en silence a la premiere mise a jour serait une regression.
        m_switch_community.set_active(j.value("hiscore_community", true));
        m_switch_autosync.set_active(j.value("hiscore_auto_sync", true));
        m_switch_playstats.set_active(j.value("hiscore_share_playtime", true));
        // Options de l'emulateur. Le plein ecran et la mise a l'echelle
        // entiere partagent leurs cles avec le menu « Launch » : ce sont les
        // memes reglages, pas des copies.
        set_launch_flags(j.value("launch_fullscreen", false),
                         j.value("launch_integerscale", false));
        m_entry_emu_args.set_text(j.value("fbneo_extra_args", std::string()));
    } catch (...) {
        return false;
    }

    // A relative dat_path/fbneo_executable resolves against whatever directory
    // the process happened to be launched from, which silently differs between
    // a desktop launcher, a terminal, and a dev checkout : leading to reads and
    // writes quietly landing in two unrelated folders across runs. Pin any
    // legacy relative value to an absolute one and persist it immediately so
    // this migration only ever has to happen once.
    //
    // Anchor at $HOME, not the executable's own directory: the Flatpak build
    // runs from a read-only /app/bin, so get_executable_dir() would point
    // somewhere the app can never write to. $HOME is what a bare "./support/…"
    // has always actually resolved to for a normally-launched (desktop/Flatpak)
    // session in practice, since that is the working directory it starts in.
    bool migrated = false;
    const char* home_env = std::getenv("HOME");
    std::string home = home_env ? home_env : ".";
    auto pin_absolute = [&](std::string current, auto setter) {
        if (current.empty() || std::filesystem::path(current).is_absolute()) return;
        std::string absolute = (std::filesystem::path(home) / current).lexically_normal().string();
        (this->*setter)(absolute);
        migrated = true;
    };
    pin_absolute(get_dat_path(), &SettingsPanel::set_dat_path);
    pin_absolute(get_fbneo_executable(), &SettingsPanel::set_fbneo_executable);
    if (migrated) save_to_file(filename);

    refresh_emulator_state();
    refresh_roms_list();
    return true;
}

bool SettingsPanel::save_to_file(const std::string& filename) {
    // Read existing file first to preserve keys written by other parts of the app
    // (e.g. launch_fullscreen, launch_integerscale, controllers)
    nlohmann::json j;
    {
        std::ifstream fi(filename);
        if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } }
    }

    // Save multiple ROMs paths as array
    j["roms_paths"] = nlohmann::json::array();
    for (const auto& path : m_roms_paths) {
        j["roms_paths"].push_back(path);
    }

    // Keep legacy single path for compatibility (first path)
    j["roms_path"] = get_roms_path();

    j["dat_path"] = get_dat_path();
    j["previews_path"] = get_previews_path();
    j["rom_manager"]["outbox_path"]     = get_outbox_path();
    j["rom_manager"]["quarantine_path"] = get_quarantine_path();
    j["titles_path"] = get_titles_path();
    j["fbneo_executable"] = get_fbneo_executable();
    j["scan_recursive"] = m_check_recursive.get_active();
    j["scan_loose_files"] = m_check_loose_files.get_active();
    j["theme"] = get_theme();
    j["language"] = get_language();
    j["hiscore_player"] = get_hiscore_player();
    j["startup_selection"] = get_startup_selection();
    j["hiscore_enabled"] = m_switch_hiscore.get_active();
    j["hiscore_asked"] = m_hiscore_asked;
    j["hiscore_country"] = get_hiscore_country();
    j["roms_list_height"]     = m_roms_list_height;
    j["restore_window_state"] = m_switch_window_state.get_active();
    j["keep_play_history"]    = m_switch_play_history.get_active();
    j["check_updates_auto"]   = m_switch_auto_update.get_active();
    j["hiscore_community"]      = m_switch_community.get_active();
    j["hiscore_auto_sync"]      = m_switch_autosync.get_active();
    j["hiscore_share_playtime"] = m_switch_playstats.get_active();
    j["launch_fullscreen"]      = m_switch_fullscreen.get_active();
    j["launch_integerscale"]    = m_switch_integerscale.get_active();
    j["fbneo_extra_args"]       = get_emulator_extra_args();
    j["window_width"] = 1000;
    j["window_height"] = 600;

    std::ofstream file(filename);
    if (!file.is_open()) return false;

    file << j.dump(4); // Pretty print with 4 spaces
    return true;
}

// --- New methods for ROMs path management ---
void SettingsPanel::on_add_roms_path_clicked() {
    auto dialog = Gtk::FileChooserDialog(_("Select ROMs Directory"), Gtk::FILE_CHOOSER_ACTION_SELECT_FOLDER);
    dialog.add_button(_("Cancel"), Gtk::RESPONSE_CANCEL);
    dialog.add_button(_("Select"), Gtk::RESPONSE_OK);

    if (dialog.run() == Gtk::RESPONSE_OK) {
        std::string path = dialog.get_filename();
        // Check if path already exists
        auto it = std::find(m_roms_paths.begin(), m_roms_paths.end(), path);
        if (it == m_roms_paths.end()) {
            add_roms_path(path);
        }
    }
}

void SettingsPanel::on_remove_roms_path_clicked() {
    auto selection = m_treeview_roms.get_selection();
    auto iter = selection->get_selected();
    if (iter) {
        // Find the index of the selected item
        auto path = m_model_roms->get_path(iter);
        int index = path[0];  // Get the first (and only) index
        remove_roms_path(index);
    }
}

void SettingsPanel::refresh_roms_list() {
    if (!m_model_roms) return;
    m_model_roms->clear();
    auto folder = IconManager::load("icons/bc-folder.svg", 18, 18);
    for (const auto& path : m_roms_paths) {
        // Un dossier configure mais introuvable est la premiere cause de
        // « mes jeux ont disparu » : le dire dans la liste evite d'aller le
        // chercher dans un journal.
        std::error_code ec;
        const bool present = std::filesystem::is_directory(path, ec);
        auto row = *m_model_roms->append();
        row[m_cols_roms.icon]   = folder;
        row[m_cols_roms.path]   = path;
        row[m_cols_roms.status] = present
            ? Glib::ustring("<span foreground='#41d08a'>● ") + _("Active") + "</span>"
            : Glib::ustring("<span foreground='#e5484d'>● ") + _("Missing") + "</span>";
    }
}

void SettingsPanel::on_download_fbneo_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;

    // $HOME, not current_path(): the working directory a launch happens to
    // start in is not stable across a desktop icon vs. a terminal vs. a dev
    // checkout, so this could silently extract into a different folder each time.
    const char* home_env = std::getenv("HOME");
    auto download_dialog = std::make_unique<DownloadDialog>(
        *parent_window,
        // finalburnneo/FBNeo stopped maintaining the SDL/Linux build (missing DAT
        // export calls for several systems, upstream declined the fix) : this fork
        // tracks their master daily and carries just that fix. See website FAQ.
        "https://github.com/battousai90/FBNeo/releases/download/latest/linux-sdl2-x86_64.zip",
        home_env ? std::string(home_env) : std::filesystem::current_path().string()
    );

    download_dialog->set_settings_entry(&m_entry_fbneo);
    download_dialog->start_download();
    download_dialog->run();

    // Record what "latest" pointed at just now, so a future startup check has
    // a baseline to notice the next time our fork moves past this build.
    auto r = FbneoUpdateCheck::fetch_latest();
    if (r.ok) {
        nlohmann::json j;
        const std::string path = AppContext::get_config_path();
        { std::ifstream fi(path); if (fi) { try { fi >> j; } catch (...) { j = nlohmann::json{}; } } }
        j["fbneo_release_sha"] = r.sha;
        std::ofstream fo(path);
        if (fo) fo << j.dump(4);
    }
    refresh_emulator_state();
}

void SettingsPanel::on_generate_dat_clicked() {
    auto parent_window = dynamic_cast<Gtk::Window*>(get_toplevel());
    if (!parent_window) return;

    GenerateDAT::execute(*parent_window, get_fbneo_executable(), get_dat_path(), &m_entry_dat);
}

void SettingsPanel::on_download_previews_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download previews button clicked" << std::endl;
}

void SettingsPanel::on_download_titles_clicked() {
    // Cette méthode sera connectée depuis MainWindow
    // pour avoir accès aux jeux et aux méthodes de progression
    std::cout << "[INFO] Download titles button clicked" << std::endl;
}

void SettingsPanel::refresh_account_row() {
    const bool in = BootcadeAuth::signed_in();

    // Le drapeau se derive du code ISO en deux points de code Unicode :
    // aucune image a embarquer, aucune liste a tenir a jour.
    auto flag_for = [](const std::string& cc) -> Glib::ustring {
        if (cc.size() != 2) return {};
        gunichar a = 0x1F1E6 + (g_ascii_toupper(cc[0]) - 'A');
        gunichar b = 0x1F1E6 + (g_ascii_toupper(cc[1]) - 'A');
        return Glib::ustring(1, a) + Glib::ustring(1, b);
    };

    if (in) {
        // L'avatar choisi, sinon celui par defaut : un joueur qui n'en a pas
        // pris doit quand meme voir une image, pas un trou.
        std::string id = BootcadeAuth::avatar_id();
        if (id.empty()) id = "joystick";
        m_account_avatar.set(IconManager::load("avatars/" + id + ".svg", 52, 52));
        m_account_avatar.show();
        m_profile_avatar.set(IconManager::load("avatars/" + id + ".svg", 96, 96));

        const std::string cc = BootcadeAuth::country();
        Glib::ustring flag = flag_for(cc);

        m_account_name.set_markup(
            "<b>" + Glib::Markup::escape_text(BootcadeAuth::username()) + "</b>"
            + (flag.empty() ? Glib::ustring() : Glib::ustring(" ") + flag));
        m_account_sub.set_markup("<span size='small' alpha='70%'>"
                                 + Glib::Markup::escape_text(_("Signed in")) + "</span>");
        m_button_account.set_label(_("Sign out"));
        m_button_account.set_image(*SettingsUi::image("bc-signout.svg", SettingsUi::kIconButton));
        m_button_account.set_always_show_image(true);
        m_button_account.get_style_context()->add_class("set-danger");

        m_profile_name.set_text(BootcadeAuth::username());
        Glib::ustring country_line;
        for (const auto& c : kCountries)
            if (cc == c.code) { country_line = _(c.name); break; }
        m_profile_country.set_text(flag.empty() ? country_line
                                                : flag + " " + country_line);
        const int queued = HiscoreClient::outbox_size();
        m_profile_queued.set_text(
            queued == 0 ? Glib::ustring(_("No score waiting to be sent."))
                        : Glib::ustring::compose(
                              _("%1 score(s) waiting to be sent."), queued));
        refresh_profile_stats();
        m_profile_signed.show();
        m_profile_empty.hide();
    } else {
        m_account_avatar.hide();
        m_account_name.set_markup("<span alpha='70%'>"
                                  + Glib::Markup::escape_text(_("Not signed in"))
                                  + "</span>");
        m_account_sub.set_markup("<span size='small' alpha='70%'>"
                                 + Glib::Markup::escape_text(
                                       _("Bootcade works fully offline. An account is optional."))
                                 + "</span>");
        m_button_account.set_label(_("Sign in"));
        m_button_account.set_image(*SettingsUi::image("bc-account.svg", SettingsUi::kIconButton));
        m_button_account.set_always_show_image(true);
        m_button_account.get_style_context()->remove_class("set-danger");

        m_profile_signed.hide();
        m_profile_empty.show();
    }
    // « Manage Account » n'a de sens qu'avec un compte : la page du site
    // renverrait sinon vers un formulaire de connexion.
    m_button_manage_account.set_sensitive(in);

    // L'interrupteur suit la connexion : publier sans compte est impossible
    // depuis que les scores sont rattaches a un compte, et laisser le reglage
    // actif ferait croire le contraire. Connecte, le joueur reste libre de
    // refuser la publication.
    m_switch_hiscore.set_sensitive(in);
    m_hiscore_hint.set_text(in ? "" : _("Sign in to publish your scores."));
    m_hiscore_hint.set_visible(!in);

    // Le pays vient du compte : le laisser modifiable ici ferait croire au
    // joueur qu'il agit sur quelque chose, alors que le serveur prend celui du
    // jeton. Il reste lisible, et modifiable pour qui n'a pas de compte.
    m_entry_hiscore_country.set_sensitive(!in);
    if (in && !BootcadeAuth::country().empty())
        set_hiscore_country(BootcadeAuth::country());
}
