// src/MameCatalog.cpp
#include "MameCatalog.h"

#include <sys/wait.h>
#include <unistd.h>

#include <curl/curl.h>
#include <pugixml.hpp>

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
        "<display", "<sound", "<biosset", "<feature", "<adjuster", "<analog",
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


std::vector<Game> load(const std::shared_ptr<DatabaseManager>& db,
                       bool include_mechanical) {
    return db ? db->getMameCatalog(include_mechanical) : std::vector<Game>{};
}

}  // namespace MameCatalog
