// src/MameCatalog.cpp
#include "MameCatalog.h"

#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pugixml.hpp>
#include <zip.h>
#include <nlohmann/json.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <cstring>
#include <iostream>
#include <algorithm>
#include <cctype>

#include "AppContext.h"
#include "DatabaseManager.h"

namespace {

// Lance une commande et donne sa sortie standard a `sink`, par blocs, sans
// jamais la materialiser en entier : `mame -listxml` pese 320 Mo.
// Rend false si le processus n'a pas pu demarrer.
bool run_streaming(const std::vector<std::string>& args,
                   const std::function<bool(const char*, size_t)>& sink) {
    const std::vector<std::string> cmd = AppContext::host_command(args);

    int fds[2];
    if (pipe(fds) != 0) return false;

    pid_t pid = fork();
    if (pid < 0) { close(fds[0]); close(fds[1]); return false; }

    if (pid == 0) {
        close(fds[0]);
        dup2(fds[1], STDOUT_FILENO);
        close(fds[1]);
        // MAME ecrit ses avertissements sur stderr : on les laisse passer au
        // journal plutot que de les melanger au XML.
        std::vector<char*> argv;
        argv.reserve(cmd.size() + 1);
        for (const auto& a : cmd) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(fds[1]);
    std::array<char, 1 << 16> buf;
    bool keep = true;
    ssize_t n;
    while (keep && (n = read(fds[0], buf.data(), buf.size())) > 0)
        keep = sink(buf.data(), static_cast<size_t>(n));
    close(fds[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    return true;
}

// Toute la sortie d'une commande courte, sans passer par un shell.
std::string run_capture(const std::vector<std::string>& args) {
    std::string out;
    run_streaming(args, [&out](const char* p, size_t n) {
        out.append(p, n);
        return out.size() < (1u << 20);   // garde-fou : ces commandes sont breves
    });
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out;
}

// Les lignes qu'on jette avant de parser : ce sont les plus nombreuses
// (781 216 <device_ref>, 371 752 <rom>) et l'interface n'en montre rien.
//
// On ne jette que des elements AUTO-FERMANTS. Retirer la ligne ouvrante d'un
// element qui a des enfants — <device>, <port>, <dipswitch> — laisserait sa
// balise fermante orpheline, le fragment ne serait plus du XML valide, et la
// machine entiere serait perdue. C'est exactement ce qui a fait tomber un
// premier essai de 50 368 machines a 5 425.
bool is_noise(const char* line, size_t len) {
    static const char* const skip[] = {
        "<device_ref", "<rom ", "<disk ", "<sample", "<dipvalue", "<diplocation",
        "<confsetting", "<conflocation", "<slotoption", "<ramoption", "<chip ",
        // « <display> » n'est plus jete : il porte rotate, width et height,
        // d'ou se deduisent l'orientation et la definition des machines MAME.
        // Une ligne de plus par machine sur 371 752 <rom> jetees : le cout ne
        // se mesure pas, et sans elle trois filtres restaient vides.
        "<sound", "<biosset", "<feature", "<adjuster", "<analog",
        "<instance", "<extension", "<softwarelist", "<control",
    };
    size_t i = 0;
    while (i < len && (line[i] == ' ' || line[i] == '\t')) ++i;

    size_t e = len;
    while (e > i && (line[e - 1] == '\r' || line[e - 1] == ' ')) --e;
    if (e - i < 2 || line[e - 2] != '/' || line[e - 1] != '>') return false;

    for (const char* s : skip) {
        const size_t sl = std::strlen(s);
        if (e - i >= sl && std::memcmp(line + i, s, sl) == 0) return true;
    }
    return false;
}

}  // namespace

namespace MameCatalog {

std::string find_executable() {
    // Les emplacements usuels d'abord : Debian et Fedora ne rangent pas MAME
    // au meme endroit, et `which` coute un processus de plus.
    for (const char* p : {"/usr/games/mame", "/usr/bin/mame", "/usr/local/bin/mame"}) {
        if (access(p, X_OK) == 0) return p;
    }
    const std::string found = run_capture({"which", "mame"});
    return found.find('/') == std::string::npos ? std::string() : found;
}

std::string installed_build(const std::string& mame_exe) {
    if (mame_exe.empty()) return {};
    return run_capture({mame_exe, "-version"});
}

std::string version_number(const std::string& raw) {
    size_t b = raw.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    size_t e = b;
    while (e < raw.size() && (std::isdigit(static_cast<unsigned char>(raw[e])) ||
                              raw[e] == '.')) ++e;
    // Rien de numerique en tete : la sortie n'est pas celle qu'on attendait,
    // mieux vaut la rendre entiere que d'en inventer une lecture.
    if (e == b) return raw;
    std::string v = raw.substr(b, e - b);
    while (!v.empty() && v.back() == '.') v.pop_back();
    return v;
}

int compare_versions(const std::string& a, const std::string& b) {
    auto parts = [](const std::string& s) {
        std::vector<long> out;
        long cur = 0;
        bool any = false;
        for (char c : s) {
            if (std::isdigit(static_cast<unsigned char>(c))) { cur = cur * 10 + (c - '0'); any = true; }
            else if (c == '.') { out.push_back(any ? cur : 0); cur = 0; any = false; }
            else break;
        }
        if (any) out.push_back(cur);
        return out;
    };
    const std::vector<long> pa = parts(a), pb = parts(b);
    if (pa.empty() || pb.empty()) return 0;
    for (size_t i = 0; i < std::max(pa.size(), pb.size()); ++i) {
        const long x = i < pa.size() ? pa[i] : 0;
        const long y = i < pb.size() ? pb[i] : 0;
        if (x != y) return x < y ? -1 : 1;
    }
    return 0;
}

namespace {

size_t append_to_string(char* ptr, size_t size, size_t nmemb, void* userdata) {
    static_cast<std::string*>(userdata)->append(ptr, size * nmemb);
    return size * nmemb;
}

/* On ne s'accroche pas a UNE tournure de phrase, mais on ne ramasse pas non
 * plus n'importe quel numero.
 *
 * La page des sorties est ecrite a la main et change de forme a chaque
 * refonte du site : un seul point d'accroche, et la verification se met a
 * mentir le jour ou une balise bouge. On en garde donc deux, qui ne bougent
 * pas ensemble — la phrase d'annonce, et les noms d'archives « mame0289s.exe »
 * qui sont generes.
 *
 * Et on refuse tout le reste : la page porte, dans un commentaire HTML, un
 * « Version 1.11.0 » qui est celui de l'outil ayant fabrique le site. Pris
 * pour une version de MAME, il annoncait une mise a jour imaginaire — d'ou
 * les commentaires jetes d'abord, et la forme 0.NNN exigee ensuite.
 *
 * Aucun numero reconnu : on le DIT, au lieu de conclure que tout va bien.
 */
std::string strip_comments(const std::string& html) {
    std::string out;
    out.reserve(html.size());
    size_t i = 0;
    while (i < html.size()) {
        const size_t c = html.find("<!--", i);
        if (c == std::string::npos) { out.append(html, i, std::string::npos); break; }
        out.append(html, i, c - i);
        const size_t e = html.find("-->", c);
        if (e == std::string::npos) break;
        i = e + 3;
    }
    return out;
}

// La numerotation de MAME : 0 puis trois chiffres. Rien d'autre n'est une
// version de MAME, et s'en assurer coute moins cher qu'une fausse alerte.
bool looks_like_mame_version(const std::string& v) {
    return v.size() == 5 && v[0] == '0' && v[1] == '.' &&
           std::isdigit(static_cast<unsigned char>(v[2])) &&
           std::isdigit(static_cast<unsigned char>(v[3])) &&
           std::isdigit(static_cast<unsigned char>(v[4]));
}

std::string best_version_in(const std::string& html) {
    const std::string stripped = strip_comments(html);
    std::string low;
    low.reserve(stripped.size());
    for (char c : stripped) low += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string best;
    auto keep = [&best](const std::string& v) {
        if (!looks_like_mame_version(v)) return;
        if (best.empty() || compare_versions(best, v) < 0) best = v;
    };

    // « ... release is version 0.289. »
    for (size_t p = low.find("version "); p != std::string::npos;
         p = low.find("version ", p + 1)) {
        size_t i = p + 8, j = p + 8;
        while (j < low.size() && (std::isdigit(static_cast<unsigned char>(low[j])) ||
                                  low[j] == '.')) ++j;
        std::string v = low.substr(i, j - i);
        while (!v.empty() && v.back() == '.') v.pop_back();
        keep(v);
    }

    // « mame0289s.exe », « mame0289lx.zip » : 0289 se lit 0.289.
    for (size_t p = low.find("mame0"); p != std::string::npos;
         p = low.find("mame0", p + 1)) {
        const size_t i = p + 4;          // sur le '0'
        size_t j = i;
        while (j < low.size() && std::isdigit(static_cast<unsigned char>(low[j]))) ++j;
        if (j - i != 4) continue;
        keep(low.substr(i, 1) + "." + low.substr(i + 1, 3));
    }
    return best;
}

}  // namespace

LatestRelease fetch_latest_release() {
    LatestRelease r;

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    std::string body;
    struct curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "User-Agent: bootcade");

    curl_easy_setopt(curl, CURLOPT_URL, "https://www.mamedev.org/release.html");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_to_string);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) { r.error = curl_easy_strerror(res); return r; }
    if (status != 200)   { r.error = "HTTP " + std::to_string(status); return r; }

    r.version = best_version_in(body);
    r.ok = !r.version.empty();
    if (!r.ok) r.error = "no version found on the release page";
    return r;
}

std::string cached_build(const std::shared_ptr<DatabaseManager>& db) {
    return db ? db->getMetaString("mame_build") : std::string();
}

// Defini plus bas, avec le reste de la lecture de catver.ini : rebuild en a
// besoin pour reposer les genres qu'il vient d'effacer.
namespace { std::string configured_catver(); }

int rebuild(const std::shared_ptr<DatabaseManager>& db,
            const std::string& mame_exe,
            const std::function<void(int)>& progress) {
    if (!db || mame_exe.empty()) return -1;

    const std::string build = installed_build(mame_exe);
    if (build.empty()) {
        std::cerr << "[WARN] MAME did not report a version; catalog not rebuilt" << std::endl;
        return -1;
    }

    std::vector<MameMachine> batch;
    batch.reserve(4096);
    int total = 0;
    std::string pending;   // le fragment <machine>...</machine> en cours
    std::string carry;     // la ligne coupee entre deux blocs de lecture

    db->beginMameCatalogRebuild();

    // On decoupe le flux en lignes, on jette le bruit, et on accumule jusqu'a
    // </machine> : chaque fragment fait quelques kilo-octets et se parse seul,
    // ce qui evite un arbre DOM de 320 Mo.
    auto ends_machine = [](const std::string& s) {
        size_t e = s.find_last_not_of(" \t\r\n");
        return e != std::string::npos && e + 1 >= 10 &&
               s.compare(e + 1 - 10, 10, "</machine>") == 0;
    };

    auto handle_line = [&](const char* p, size_t n) {
        if (is_noise(p, n)) return;
        // Rien ne s'accumule avant la premiere <machine> : le flux commence par
        // la DTD et par <mame build=...>, et les coller devant le premier
        // fragment le rendait inexploitable — c'est ce qui faisait disparaitre
        // « 005 », la premiere machine du fichier.
        if (pending.empty() &&
            std::string(p, n).find("<machine ") == std::string::npos) return;
        pending.append(p, n);
        pending.push_back('\n');
        if (!ends_machine(pending)) return;

        pugi::xml_document doc;
        if (doc.load_buffer(pending.data(), pending.size())) {
            const pugi::xml_node m = doc.child("machine");
            if (m) {
                MameMachine mm;
                mm.name          = m.attribute("name").as_string();
                mm.sourcefile    = m.attribute("sourcefile").as_string();
                mm.cloneof       = m.attribute("cloneof").as_string();
                mm.romof         = m.attribute("romof").as_string();
                mm.is_bios       = m.attribute("isbios").as_bool(false);
                mm.is_device     = m.attribute("isdevice").as_bool(false);
                mm.is_mechanical = m.attribute("ismechanical").as_bool(false);
                mm.runnable      = m.attribute("runnable").as_bool(true);
                mm.description   = m.child_value("description");
                mm.year          = m.child_value("year");
                mm.manufacturer  = m.child_value("manufacturer");
                mm.driver_status = m.child("driver").attribute("status").as_string();
                /* Ce que MAME dit deja de lui-meme et qu'on jetait.
                 *
                 * `rotate` vaut 0, 90, 180 ou 270 : un quart de tour range la
                 * machine dans les verticales. On reprend le vocabulaire exact
                 * des DAT FinalBurn Neo, sinon le filtre « Orientation »
                 * afficherait deux listes de valeurs pour une seule notion.
                 *
                 * Les dimensions restent BRUTES, non tournees : elles servent
                 * a la fiche du jeu, qui doit montrer ce que MAME declare.
                 */
                if (const pugi::xml_node in = m.child("input"))
                    mm.players = in.attribute("players").as_int(0);
                if (const pugi::xml_node d = m.child("display")) {
                    const int rot = d.attribute("rotate").as_int(0);
                    mm.orientation = (rot == 90 || rot == 270) ? "vertical" : "horizontal";
                    mm.width  = d.attribute("width").as_string();
                    mm.height = d.attribute("height").as_string();
                }
                if (!mm.name.empty()) { batch.push_back(std::move(mm)); ++total; }
            }
        }
        pending.clear();

        if (batch.size() >= 4096) {
            db->insertMameMachines(batch);
            batch.clear();
            if (progress) progress(total);
        }
    };

    const bool started = run_streaming({mame_exe, "-listxml"},
                                       [&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            if (carry.empty()) {
                handle_line(p + start, i - start);
            } else {
                carry.append(p + start, i - start);
                handle_line(carry.data(), carry.size());
                carry.clear();
            }
            start = i + 1;
        }
        // Le bloc s'arrete au milieu d'une ligne : on la garde pour le suivant,
        // sinon une machine sur quelques milliers se retrouve tronquee.
        if (start < n) carry.append(p + start, n - start);
        return true;
    });

    // Le flux peut s'arreter sans saut de ligne final : sans ceci, la derniere
    // machine reste dans le tampon et le catalogue en compte une de moins.
    if (!carry.empty()) { handle_line(carry.data(), carry.size()); carry.clear(); }

    if (!started) { db->abortMameCatalogRebuild(); return -1; }

    if (!batch.empty()) db->insertMameMachines(batch);
    db->commitMameCatalogRebuild(build);

    /* La regeneration vide la table, genres compris.
     *
     * Une mise a jour de MAME suffit a la declencher, et sans ceci le joueur
     * verrait le filtre « Genre » se vider tout seul un beau matin, sans
     * avoir rien fait. On repose donc ce que catver.ini sait encore dire des
     * machines qui viennent d'etre ecrites.
     */
    const std::string catver = catver_path(configured_catver());
    if (!catver.empty()) apply_catver(db, catver);

    if (progress) progress(total);
    std::cout << "[INFO] MAME catalog: " << total << " machines (build " << build << ")" << std::endl;
    return total;
}

int sync(const std::shared_ptr<DatabaseManager>& db,
         const std::string& mame_exe,
         const std::function<void(int)>& progress) {
    if (!db || mame_exe.empty()) return -1;
    const std::string installed = installed_build(mame_exe);
    if (!installed.empty() && installed == cached_build(db)) {
        return db->countMameMachines();
    }
    return rebuild(db, mame_exe, progress);
}

std::vector<std::string> rompaths_from_mame_ini() {
    std::vector<std::string> out;
    const char* home = std::getenv("HOME");
    if (!home) return out;

    std::ifstream ini(std::string(home) + "/.mame/mame.ini");
    if (!ini) return out;

    std::string line;
    while (std::getline(ini, line)) {
        // « rompath   "a;b;c" » ou « rompath a;b;c »
        const auto key = line.find("rompath");
        if (key != 0) continue;
        auto rest = line.substr(7);
        const auto first = rest.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        rest = rest.substr(first);
        if (!rest.empty() && rest.front() == '"') {
            rest.erase(0, 1);
            const auto close = rest.rfind('"');
            if (close != std::string::npos) rest.erase(close);
        }
        while (!rest.empty() && (rest.back() == '\r' || rest.back() == ' ')) rest.pop_back();

        size_t start = 0;
        while (start <= rest.size()) {
            const auto sep = rest.find(';', start);
            auto piece = rest.substr(start, sep == std::string::npos ? std::string::npos
                                                                     : sep - start);
            if (!piece.empty()) out.push_back(piece);
            if (sep == std::string::npos) break;
            start = sep + 1;
        }
        break;   // une seule ligne rompath fait foi
    }
    return out;
}

AuditResult audit(const std::shared_ptr<DatabaseManager>& db,
                  const std::string& mame_exe,
                  const std::vector<std::string>& rompaths,
                  const std::function<bool(int)>& progress) {
    AuditResult res;
    if (!db || mame_exe.empty()) return res;

    db->resetMameStatuses();

    std::vector<std::string> args{mame_exe};
    if (!rompaths.empty()) {
        // On passe les dossiers explicitement : le mame.ini de l'utilisateur
        // peut parfaitement designer des chemins qui n'existent plus, et MAME
        // ne dirait alors rien d'autre que « rien trouve ».
        std::string joined;
        for (const auto& p : rompaths) {
            if (!joined.empty()) joined += ';';
            joined += p;
        }
        args.push_back("-rompath");
        args.push_back(joined);
    }
    args.push_back("-verifyroms");

    std::vector<std::pair<std::string, std::string>> verdicts;
    verdicts.reserve(8192);
    std::string carry;
    bool cancelled = false;

    // « romset <nom> [<parent>] is <verdict> ». Le parent est facultatif, et
    // les lignes de detail (un fichier par ligne) ne nous interessent pas ici :
    // le statut du set suffit a colorer la bibliotheque.
    auto handle = [&](const std::string& line) {
        if (line.compare(0, 7, "romset ") != 0) return;
        auto rest = line.substr(7);
        const auto sp = rest.find(' ');
        if (sp == std::string::npos) return;
        const std::string name = rest.substr(0, sp);
        const std::string tail = rest.substr(sp + 1);

        std::string status;
        if (tail.find("is good") != std::string::npos)                { status = "available"; ++res.good; }
        else if (tail.find("is best available") != std::string::npos) { status = "available"; ++res.playable; }
        else if (tail.find("is bad") != std::string::npos)            { status = "incorrect"; ++res.bad; }
        else return;

        verdicts.emplace_back(name, status);
        if (verdicts.size() % 2048 == 0 && progress && !progress((int)verdicts.size()))
            cancelled = true;
    };

    run_streaming(args, [&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            carry.append(p + start, i - start);
            while (!carry.empty() && carry.back() == '\r') carry.pop_back();
            handle(carry);
            carry.clear();
            start = i + 1;
        }
        if (start < n) carry.append(p + start, n - start);
        return !cancelled;
    });
    if (!carry.empty()) handle(carry);

    if (cancelled) return AuditResult{};

    db->setMameStatuses(verdicts);
    res.missing = db->countMameMachines() - (int)verdicts.size();
    std::cout << "[INFO] MAME audit: " << res.good << " good, " << res.playable
              << " playable, " << res.bad << " bad, " << res.missing << " missing"
              << std::endl;
    return res;
}

namespace {

// Echappement XML minimal. Les descriptions MAME contiennent des esperluettes
// (« Bally & Midway »), des apostrophes et des chevrons (« <unknown> ») : sans
// cela le DAT produit ne serait pas relisible par notre propre parseur.
std::string xml_escape(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 16);
    for (char c : in) {
        switch (c) {
        case '&':  out += "&amp;";  break;
        case '<':  out += "&lt;";   break;
        case '>':  out += "&gt;";   break;
        case '"':  out += "&quot;"; break;
        case '\'': out += "&apos;"; break;
        default:   out += c;
        }
    }
    return out;
}

// Un fichier DAT en cours d'ecriture, avec son en-tete Logiqx.
struct DatWriter {
    std::ofstream out;
    int games = 0;

    bool open(const std::string& path, const std::string& header_name,
              const std::string& build) {
        out.open(path, std::ios::binary);
        if (!out) return false;
        // L'en-tete respecte le gabarit « <marque> - <systeme> Games » :
        // DatParser::extractSystemFromHeader en tire le systeme, et c'est lui
        // qui classe ensuite les jeux dans la colonne des filtres.
        out << "<?xml version=\"1.0\"?>\n"
               "<!DOCTYPE datafile PUBLIC \"-//Logiqx//DTD ROM Management Datafile//EN\""
               " \"http://www.logiqx.com/Dats/datafile.dtd\">\n\n"
               "<datafile>\n\t<header>\n"
               "\t\t<name>" << xml_escape(header_name) << "</name>\n"
               "\t\t<description>" << xml_escape(header_name) << " " << xml_escape(build)
            << "</description>\n"
               "\t\t<category>Standard DatFile</category>\n"
               "\t\t<version>" << xml_escape(build) << "</version>\n"
               "\t\t<author>MAME</author>\n"
               "\t\t<homepage>https://www.mamedev.org/</homepage>\n"
               "\t\t<url>https://www.mamedev.org/</url>\n"
               "\t</header>\n";
        return true;
    }

    void close() {
        if (out.is_open()) { out << "</datafile>\n"; out.close(); }
    }
};

}  // namespace

int generate_dats(const std::string& mame_exe,
                  const std::string& dat_dir,
                  const std::function<bool(int)>& progress) {
    if (mame_exe.empty() || dat_dir.empty()) return -1;

    const std::string build = installed_build(mame_exe);
    std::error_code ec;
    std::filesystem::create_directories(dat_dir, ec);

    // Deux fichiers seulement : ce qui se joue, et ce qui se regarde tourner.
    // Les machines internes (isdevice) n'ont rien a faire dans un DAT destine
    // a l'utilisateur, mais leurs ROMs servent a resoudre les sets splits, donc
    // elles vont dans le fichier arcade avec les BIOS.
    DatWriter arcade, mech;
    if (!arcade.open(dat_dir + "/MAME_-_Arcade.dat", "MAME - Arcade Games", build))
        return -1;
    if (!mech.open(dat_dir + "/MAME_-_Mechanical.dat", "MAME - Mechanical Games", build)) {
        arcade.close();
        return -1;
    }

    std::string pending, carry;
    int seen = 0;
    bool cancelled = false;

    auto flush_machine = [&]() {
        pugi::xml_document doc;
        if (!doc.load_buffer(pending.data(), pending.size())) { pending.clear(); return; }
        const pugi::xml_node m = doc.child("machine");
        if (!m) { pending.clear(); return; }

        const std::string name = m.attribute("name").as_string();
        if (name.empty()) { pending.clear(); return; }
        const bool is_device = m.attribute("isdevice").as_bool(false);
        const bool is_mech   = m.attribute("ismechanical").as_bool(false);

        DatWriter& w = (is_mech && !is_device) ? mech : arcade;
        std::ostringstream g;
        g << "\t<game name=\"" << xml_escape(name) << "\"";
        const std::string cloneof = m.attribute("cloneof").as_string();
        const std::string romof   = m.attribute("romof").as_string();
        const std::string srcfile = m.attribute("sourcefile").as_string();
        if (!cloneof.empty()) g << " cloneof=\"" << xml_escape(cloneof) << "\"";
        if (!romof.empty())   g << " romof=\""   << xml_escape(romof)   << "\"";
        if (!srcfile.empty()) g << " sourcefile=\"" << xml_escape(srcfile) << "\"";
        if (m.attribute("isbios").as_bool(false)) g << " isbios=\"yes\"";
        g << ">\n";
        g << "\t\t<description>" << xml_escape(m.child_value("description")) << "</description>\n";
        const std::string year = m.child_value("year");
        const std::string manu = m.child_value("manufacturer");
        if (!year.empty()) g << "\t\t<year>" << xml_escape(year) << "</year>\n";
        if (!manu.empty()) g << "\t\t<manufacturer>" << xml_escape(manu) << "</manufacturer>\n";

        int roms = 0;
        for (pugi::xml_node r = m.child("rom"); r; r = r.next_sibling("rom")) {
            const std::string rn = r.attribute("name").as_string();
            if (rn.empty()) continue;
            // Une ROM sans CRC n'est pas verifiable : RomResolve l'ignore de
            // toute facon, autant ne pas l'ecrire.
            const std::string crc = r.attribute("crc").as_string();
            if (crc.empty()) continue;
            g << "\t\t<rom name=\"" << xml_escape(rn) << "\"";
            const std::string merge = r.attribute("merge").as_string();
            if (!merge.empty()) g << " merge=\"" << xml_escape(merge) << "\"";
            g << " size=\"" << r.attribute("size").as_ullong() << "\""
              << " crc=\"" << xml_escape(crc) << "\"";
            const std::string sha1 = r.attribute("sha1").as_string();
            if (!sha1.empty()) g << " sha1=\"" << xml_escape(sha1) << "\"";
            g << "/>\n";
            ++roms;
        }
        const pugi::xml_node drv = m.child("driver");
        if (drv) g << "\t\t<driver status=\"" << xml_escape(drv.attribute("status").as_string())
                   << "\"/>\n";
        g << "\t</game>\n";

        // Une machine sans aucune ROM verifiable ne dit rien au gestionnaire :
        // l'ecrire ne ferait qu'alourdir le fichier et la liste.
        if (roms > 0) { w.out << g.str(); ++w.games; }

        pending.clear();
        if (++seen % 2048 == 0 && progress && !progress(seen)) cancelled = true;
    };

    auto handle_line = [&](const char* p, size_t n) {
        if (pending.empty() &&
            std::string(p, n).find("<machine ") == std::string::npos) return;
        pending.append(p, n);
        pending.push_back('\n');
        size_t e = pending.find_last_not_of(" \t\r\n");
        if (e == std::string::npos || e + 1 < 10) return;
        if (pending.compare(e + 1 - 10, 10, "</machine>") != 0) return;
        flush_machine();
    };

    run_streaming({mame_exe, "-listxml"}, [&](const char* p, size_t n) {
        size_t start = 0;
        for (size_t i = 0; i < n; ++i) {
            if (p[i] != '\n') continue;
            if (carry.empty()) {
                handle_line(p + start, i - start);
            } else {
                carry.append(p + start, i - start);
                handle_line(carry.data(), carry.size());
                carry.clear();
            }
            start = i + 1;
        }
        if (start < n) carry.append(p + start, n - start);
        return !cancelled;
    });
    if (!carry.empty()) handle_line(carry.data(), carry.size());

    const int games_arcade = arcade.games, games_mech = mech.games;
    arcade.close();
    mech.close();
    if (cancelled) return -1;

    std::cout << "[INFO] MAME DAT: " << games_arcade << " arcade, "
              << games_mech << " mechanical (build " << build << ")" << std::endl;
    if (progress) progress(seen);
    return 2;
}



// ── catver.ini : le genre des machines MAME ─────────────────────────────────

namespace {

/* SQLite n'accepte que de l'UTF-8 valide.
 *
 * catver.ini est ecrit par des contributeurs sur trois systemes differents :
 * la plupart des lignes sont en ASCII pur, mais certaines portent des octets
 * hauts qui ne forment pas de l'UTF-8. Les inserer tels quels ferait echouer
 * l'UPDATE, et la machine concernee resterait sans genre sans qu'on sache
 * pourquoi. On valide donc, et a defaut on relit les octets comme du
 * Latin-1 : c'est ce que ces fichiers contiennent quand ils ne sont pas en
 * UTF-8, et cela ne peut pas echouer.
 */
std::string to_utf8(const std::string& in) {
    size_t i = 0;
    bool valid = true;
    while (i < in.size() && valid) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t extra = 0;
        if      (c < 0x80)                 extra = 0;
        else if ((c & 0xE0) == 0xC0)       extra = 1;
        else if ((c & 0xF0) == 0xE0)       extra = 2;
        else if ((c & 0xF8) == 0xF0)       extra = 3;
        else                               { valid = false; break; }
        if (i + extra >= in.size()) { valid = false; break; }
        for (size_t k = 1; k <= extra; ++k)
            if ((static_cast<unsigned char>(in[i + k]) & 0xC0) != 0x80) { valid = false; break; }
        i += extra + 1;
    }
    if (valid) return in;

    std::string out;
    out.reserve(in.size() + 8);
    for (char ch : in) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (c < 0x80) out += static_cast<char>(c);
        else { out += static_cast<char>(0xC0 | (c >> 6));
               out += static_cast<char>(0x80 | (c & 0x3F)); }
    }
    return out;
}

std::string trim(const std::string& s) {
    const size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return {};
    const size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

// Le chemin que les reglages ont enregistre. MameCatalog le relit lui-meme
// parce que rebuild() doit reposer les genres sans qu'aucun ecran ne soit
// ouvert : une mise a jour de MAME vide le catalogue, genres compris.
std::string configured_catver() {
    nlohmann::json j;
    std::ifstream in(AppContext::get_config_path());
    if (!in) return {};
    try { in >> j; } catch (...) { return {}; }
    return j.value("mame_catver_path", std::string());
}

// L'ecriture de curl, vers un fichier ouvert.
size_t write_to_file(void* ptr, size_t size, size_t nmemb, void* stream) {
    return std::fwrite(ptr, size, nmemb, static_cast<FILE*>(stream));
}

}  // namespace

std::string catver_url(const std::string& mame_version) {
    // « 0.289 » -> « pS_CatVer_289.zip ». On exige exactement trois chiffres
    // apres le point : rien d'autre n'est une version de MAME, et fabriquer
    // une adresse a partir d'autre chose ramenerait le fichier d'une version
    // qui n'est pas celle installee ici.
    const size_t dot = mame_version.find('.');
    if (dot == std::string::npos) return {};
    std::string digits;
    for (size_t i = dot + 1; i < mame_version.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(mame_version[i]))) break;
        digits += mame_version[i];
    }
    if (digits.size() != 3) return {};
    return "https://www.progettosnaps.net/download/?tipo=catver&file=pS_CatVer_"
           + digits + ".zip";
}

std::string catver_app_path() {
    return AppContext::get_user_config_dir() + "/catver.ini";
}

std::string catver_path(const std::string& configured) {
    // Le choix de l'utilisateur d'abord : s'il a deja un catver.ini, rien ne
    // justifie d'aller en telecharger un second.
    std::error_code ec;
    if (!configured.empty() && std::filesystem::is_regular_file(configured, ec))
        return configured;
    const std::string mine = catver_app_path();
    if (std::filesystem::is_regular_file(mine, ec)) return mine;
    return {};
}

CatverResult apply_catver(const std::shared_ptr<DatabaseManager>& db,
                          const std::string& path) {
    CatverResult r;
    r.path = path;
    if (!db)          { r.error = "no database"; return r; }
    if (path.empty()) { r.error = "no catver.ini selected"; return r; }

    std::ifstream in(path, std::ios::binary);
    if (!in) { r.error = "cannot read " + path; return r; }

    std::vector<std::pair<std::string, std::string>> genres;
    genres.reserve(60000);

    /* UNE seule section compte.
     *
     * Le fichier en porte quatre. [VerAdded] associe les MEMES noms de
     * machines a un numero de version (« pacman=0.1 ») : lue comme la
     * precedente, elle remplacerait chaque genre par un numero, et le filtre
     * « Genre » afficherait la liste des versions de MAME.
     */
    bool in_category = false;
    std::string line;
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
        const size_t b = line.find_first_not_of(" \t");
        if (b == std::string::npos) continue;
        if (line[b] == ';' || line[b] == '#') continue;
        if (line[b] == '[') {
            in_category = line.compare(b, 10, "[Category]") == 0;
            continue;
        }
        if (!in_category) continue;

        const size_t eq = line.find('=', b);
        if (eq == std::string::npos) continue;          // ligne sans « = » : ignoree
        const std::string name = trim(line.substr(b, eq - b));

        /* Le genre PRINCIPAL, pas le sous-genre.
         *
         * catver.ini ecrit « Maze / Collect » : la colonne des filtres
         * affiche deja « Maze » pour FinalBurn Neo, et garder le sous-genre
         * donnerait des centaines de categories a une machine pour une a
         * l'autre — deux vocabulaires dans une seule liste.
         */
        std::string genre = line.substr(eq + 1);

        /* « TTL * Shooter / Gallery » : le prefixe n'est pas un genre.
         *
         * catver.ini marque ainsi les machines a logique cablee, d'avant les
         * microprocesseurs. C'est une technologie, pas une categorie de jeu :
         * garde, il coupait 63 machines de leur genre et ajoutait six entrees
         * en double a la colonne des filtres (« Shooter » et « TTL * Shooter »).
         */
        if (genre.compare(0, 6, "TTL * ") == 0) genre = genre.substr(6);

        const size_t slash = genre.find('/');
        if (slash != std::string::npos) genre = genre.substr(0, slash);
        genre = trim(genre);

        if (name.empty() || genre.empty()) continue;
        genres.emplace_back(to_utf8(name), to_utf8(genre));
        ++r.entries;
    }

    if (genres.empty()) {
        r.error = "no [Category] section in " + path;
        return r;
    }

    const int applied = db->setMameGenres(genres);
    if (applied < 0) { r.error = "could not write the genres"; return r; }
    r.applied = applied;
    r.ok = true;
    std::cout << "[INFO] catver.ini: " << r.entries << " entries, "
              << r.applied << " machines classified" << std::endl;
    return r;
}

CatverResult download_catver(const std::string& url,
                             const std::string& dest_dir,
                             const std::function<bool(double)>& progress) {
    CatverResult r;
    if (url.empty())      { r.error = "no download address"; return r; }
    if (dest_dir.empty()) { r.error = "no destination folder"; return r; }

    std::error_code ec;
    std::filesystem::create_directories(dest_dir, ec);

    const std::string zip_path = dest_dir + "/catver-download.zip";
    const std::string out_path = dest_dir + "/catver.ini";

    CURL* curl = curl_easy_init();
    if (!curl) { r.error = "curl_easy_init failed"; return r; }

    FILE* fp = std::fopen(zip_path.c_str(), "wb");
    if (!fp) { curl_easy_cleanup(curl); r.error = "cannot write " + zip_path; return r; }

    // Le rappel de progression sert aussi d'interruption : rendre autre chose
    // que 0 abandonne le transfert au lieu de le laisser courir.
    struct Xfer { const std::function<bool(double)>* cb; } xfer{&progress};
    auto on_progress = [](void* p, curl_off_t dltotal, curl_off_t dlnow,
                          curl_off_t, curl_off_t) -> int {
        auto* x = static_cast<Xfer*>(p);
        if (!*x->cb) return 0;
        const double f = dltotal > 0 ? static_cast<double>(dlnow) / static_cast<double>(dltotal)
                                     : 0.0;
        return (*x->cb)(f) ? 0 : 1;
    };

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_to_file);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, fp);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade/1.0");
    curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                     static_cast<curl_xferinfo_callback>(on_progress));
    curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &xfer);
    curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);
    std::fclose(fp);

    if (res != CURLE_OK) {
        std::filesystem::remove(zip_path, ec);
        r.error = curl_easy_strerror(res);
        return r;
    }
    if (status != 200) {
        // progetto-SNAPS rend une page d'erreur en 200 pour une version qu'il
        // ne connait pas ; c'est l'ouverture du zip, plus bas, qui tranche.
        std::filesystem::remove(zip_path, ec);
        r.error = "HTTP " + std::to_string(status);
        return r;
    }

    /* L'archive porte aussi un dossier UI_files/ (genre.ini, catlist.ini,
     * mature.ini) et un mode d'emploi. On n'extrait QUE catver.ini : le
     * reste ne sert a rien ici, et deverser une arborescence entiere dans le
     * dossier de configuration ferait du desordre que personne n'a demande.
     */
    int err = 0;
    zip_t* z = zip_open(zip_path.c_str(), ZIP_RDONLY, &err);
    if (!z) {
        std::filesystem::remove(zip_path, ec);
        r.error = "the downloaded file is not a zip archive";
        return r;
    }

    bool written = false;
    const zip_int64_t n = zip_get_num_entries(z, 0);
    for (zip_int64_t i = 0; i < n && !written; ++i) {
        const char* name = zip_get_name(z, i, 0);
        if (!name) continue;
        std::string base(name);
        const size_t slash = base.find_last_of('/');
        if (slash != std::string::npos) base = base.substr(slash + 1);
        if (base != "catver.ini") continue;

        zip_file_t* f = zip_fopen_index(z, i, 0);
        if (!f) break;
        std::ofstream out(out_path, std::ios::binary);
        if (out) {
            char buf[1 << 16];
            zip_int64_t got;
            while ((got = zip_fread(f, buf, sizeof(buf))) > 0)
                out.write(buf, got);
            written = out.good();
        }
        zip_fclose(f);
    }
    zip_close(z);
    std::filesystem::remove(zip_path, ec);

    if (!written) { r.error = "no catver.ini inside the archive"; return r; }
    r.ok   = true;
    r.path = out_path;
    return r;
}

std::string catver_file_version(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    /* L'en-tete tient dans les toutes premieres lignes ; lire le fichier
     * entier (2,5 Mo) pour y trouver un commentaire serait payer cher une
     * information qui est toujours en tete. */
    std::string line;
    for (int i = 0; i < 40 && std::getline(in, line); ++i) {
        const size_t at = line.find("catver.ini");
        if (at == std::string::npos) continue;
        size_t p = at;
        while (p < line.size() && !std::isdigit(static_cast<unsigned char>(line[p]))) ++p;
        std::string v;
        while (p < line.size() &&
               (std::isdigit(static_cast<unsigned char>(line[p])) || line[p] == '.'))
            v += line[p++];
        while (!v.empty() && v.back() == '.') v.pop_back();
        if (v.find('.') != std::string::npos) return v;
    }
    return {};
}

namespace {

/* Ou se trouve le numero de version dans une adresse.
 *
 * Le DERNIER groupe d'exactement trois chiffres : l'adresse de
 * progetto-SNAPS en contient un seul, et une adresse personnalisee qui en
 * porterait plusieurs designe presque toujours sa version en dernier
 * (.../catver/289.zip). Rend false quand il n'y en a aucun, seul cas ou
 * l'on ne peut rien dire.
 */
bool url_version_span(const std::string& url, size_t& begin, size_t& len) {
    bool found = false;
    size_t i = 0;
    while (i < url.size()) {
        if (!std::isdigit(static_cast<unsigned char>(url[i]))) { ++i; continue; }
        size_t j = i;
        while (j < url.size() && std::isdigit(static_cast<unsigned char>(url[j]))) ++j;
        if (j - i == 3) { begin = i; len = 3; found = true; }
        i = j;
    }
    return found;
}

}  // namespace

int catver_url_version(const std::string& url) {
    size_t b = 0, n = 0;
    if (!url_version_span(url, b, n)) return -1;
    return std::stoi(url.substr(b, n));
}

std::string catver_url_with_version(const std::string& url, int version) {
    size_t b = 0, n = 0;
    if (!url_version_span(url, b, n)) return {};
    if (version < 0 || version > 999) return {};
    char buf[4];
    std::snprintf(buf, sizeof(buf), "%03d", version);
    return url.substr(0, b) + buf + url.substr(b + n);
}

UrlProbe probe_catver_url(const std::string& url) {
    if (url.empty()) return UrlProbe::Unreachable;

    CURL* curl = curl_easy_init();
    if (!curl) return UrlProbe::Unreachable;

    /* Quatre octets, pas une requete HEAD.
     *
     * Le serveur repond 200 avec une page d'erreur HTML pour certaines
     * versions qu'il ne connait pas : le code HTTP seul ment donc. Les
     * quatre premiers octets d'un zip valent « PK\x03\x04 » et tranchent
     * sans telecharger les 900 Ko.
     */
    std::string head;
    auto sink = +[](void* ptr, size_t size, size_t nmemb, void* user) -> size_t {
        auto* out = static_cast<std::string*>(user);
        out->append(static_cast<char*>(ptr), size * nmemb);
        return size * nmemb;
    };
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_RANGE, "0-3");
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &head);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_USERAGENT, "bootcade/1.0");
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 20L);
    curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 10L);

    const CURLcode res = curl_easy_perform(curl);
    long status = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) return UrlProbe::Unreachable;
    if (status == 404 || status == 410) return UrlProbe::Absent;
    if (status != 200 && status != 206) return UrlProbe::Unreachable;
    return head.compare(0, 4, "PK\x03\x04") == 0 ? UrlProbe::Present
                                                 : UrlProbe::Absent;
}

CatverCheck check_catver(const std::string& url, const std::string& local_file) {
    CatverCheck c;
    if (url.empty()) { c.error = "no download address"; return c; }

    const std::string have = catver_file_version(local_file);
    if (!have.empty()) {
        const std::string digits = have.substr(have.find('.') + 1);
        if (digits.size() == 3) c.local = std::stoi(digits);
    }

    // A defaut de fichier lisible, l'adresse configuree sert de point de
    // depart : elle designe ce que le joueur considere comme sa version.
    const int from = c.local >= 0 ? c.local : catver_url_version(url);
    if (from < 0) { c.error = "no version number in the address"; return c; }
    c.newest = c.local;

    int misses = 0;
    for (int v = from + 1; v <= from + 12 && misses < 2; ++v) {
        const std::string candidate = catver_url_with_version(url, v);
        if (candidate.empty()) { c.error = "no version number in the address"; return c; }
        const UrlProbe p = probe_catver_url(candidate);
        if (p == UrlProbe::Unreachable) {
            // Une seule panne de reseau suffit a rendre la reponse fausse :
            // on ne conclut donc rien de ce qui a ete vu jusque-la.
            if (!c.asked) { c.error = "could not reach the server"; return c; }
            break;
        }
        c.asked = true;
        c.probed_to = v;
        if (p == UrlProbe::Present) { c.newest = v; c.url = candidate; misses = 0; }
        else                        { ++misses; }
    }
    if (!c.asked) c.error = "could not reach the server";
    return c;
}

std::vector<Game> load(const std::shared_ptr<DatabaseManager>& db,
                       bool include_mechanical) {
    return db ? db->getMameCatalog(include_mechanical) : std::vector<Game>{};
}

}  // namespace MameCatalog
