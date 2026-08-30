// src/RomInbox.cpp
#include "RomInbox.h"
#include "RomArchive.h"
#include "RomScanner.h"

#include <zip.h>
#include <zlib.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomInbox {
namespace {

unsigned long parse_crc_hex(const std::string& hex) {
    if (hex.empty()) return 0;
    unsigned long crc = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

bool is_cancelled(const Callbacks& cb) { return cb.cancelled && cb.cancelled(); }

void report(const Callbacks& cb, double pct, const std::string& msg) {
    if (cb.progress) cb.progress(pct, msg);
}

void log(const Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// A ROM dumped loose, with no archive around it at all, is just as valid an
// inbox source as one already zipped : DAT roms are matched by name/CRC either
// way. Only sidecar files that are clearly never ROM data are excluded, rather
// than trying to enumerate every possible ROM extension across every system.
bool is_junk_sidecar(const std::string& path) {
    static const std::unordered_set<std::string> exts = {
        ".txt", ".nfo", ".diz", ".dsc", ".cue", ".ccd", ".sub", ".m3u",
        ".ips", ".bps", ".ups", ".xdelta", ".dat", ".xml", ".json",
        ".jpg", ".jpeg", ".png", ".gif", ".bmp", ".pdf",
        ".md5", ".sha1", ".sfv", ".cfg", ".ini", ".url", ".log",
    };
    auto p = fs::path(path);
    return exts.count(lower(p.extension().string())) > 0;
}

// CRC32 of a whole file : the "archive" reading path for a loose ROM, which
// has exactly one entry: itself.
bool compute_file_crc(const std::string& path, unsigned long& crc, uint64_t& size) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    crc = crc32(0L, Z_NULL, 0);
    size = 0;
    std::vector<char> buf(65536);
    while (f.read(buf.data(), buf.size()) || f.gcount() > 0) {
        auto n = (uInt)f.gcount();
        crc = crc32(crc, (const Bytef*)buf.data(), n);
        size += n;
    }
    return true;
}

// Replace anything that would escape the intended directory. DAT headers and game
// names are tame in practice, but they end up as filesystem paths so they get
// sanitized rather than trusted.
std::string sanitize_component(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) out += (c == '/' || c == '\\') ? '_' : c;
    if (out == "." || out == "..") out = "_";
    return out.empty() ? std::string("_") : out;
}

// True when `path` sits inside `root`. Both are expected already canonical-ish;
// used to keep the inbox and the outbox out of the "existing library" pool.
bool is_under(const fs::path& path, const fs::path& root) {
    if (root.empty()) return false;
    auto it_r = root.begin(), end_r = root.end();
    auto it_p = path.begin(), end_p = path.end();
    for (; it_r != end_r; ++it_r, ++it_p) {
        if (it_p == end_p || *it_p != *it_r) return false;
    }
    return true;
}

fs::path weak_canonical(const std::string& p) {
    std::error_code ec;
    fs::path c = fs::weakly_canonical(fs::path(p), ec);
    return ec ? fs::path(p) : c;
}

// A single ZIP sitting in the inbox, with its recomputed contents.
struct InboxArchive {
    std::string path;
    std::vector<RomArchive::Entry> entries;
};

// CRC → every place that CRC can be read from.
using PoolIndex = std::unordered_multimap<unsigned long, PieceSource>;

int action_rank(Action a) {
    switch (a) {
        case Action::Move:             return 4;
        case Action::Rebuild:          return 3;
        case Action::AlreadyInLibrary: return 2;
        case Action::Incomplete:       return 1;
    }
    return 0;
}

// ── Rebuild helpers ──────────────────────────────────────────────────────────

// Keeps every source archive open for the lifetime of one rebuild: libzip streams
// from the source when the destination is written, so the handles must outlive
// zip_close() on the target.
class SourceArchives {
public:
    ~SourceArchives() {
        for (auto& [_, z] : m_open) if (z) zip_close(z);
    }

    // Returns the open archive and the index of `entry` inside it, or false.
    bool locate(const std::string& container, const std::string& entry,
                zip_t*& out_zip, zip_int64_t& out_index) {
        zip_t* z = open(container);
        if (!z) return false;
        zip_int64_t idx = zip_name_locate(z, entry.c_str(), 0);
        if (idx < 0) return false;
        out_zip = z;
        out_index = idx;
        return true;
    }

private:
    zip_t* open(const std::string& path) {
        auto it = m_open.find(path);
        if (it != m_open.end()) return it->second;
        int err = 0;
        zip_t* z = zip_open(path.c_str(), ZIP_RDONLY, &err);
        m_open[path] = z;
        return z;
    }
    std::unordered_map<std::string, zip_t*> m_open;
};

// Re-read a freshly written archive and check every piece against the DAT.
// This is the only thing standing between a silent libzip/IO failure and a
// corrupt set landing in the outbox, so a rebuild is not considered done until
// this passes.
bool verify_against_plan(const std::string& zip_path, const SetPlan& plan,
                         std::string& error) {
    std::vector<RomScanner::ZipEntry> produced;
    if (!RomScanner::read_zip_entries(zip_path, produced)) {
        error = "cannot reopen the rebuilt archive";
        return false;
    }
    std::unordered_map<std::string, const RomScanner::ZipEntry*> by_name;
    for (const auto& e : produced) by_name[e.name] = &e;

    for (const auto& p : plan.pieces) {
        auto it = by_name.find(p.target_name);
        if (it == by_name.end()) {
            error = "missing entry after rebuild: " + p.target_name;
            return false;
        }
        if (it->second->crc != p.crc) {
            error = "CRC mismatch after rebuild: " + p.target_name;
            return false;
        }
        if (p.size && it->second->size != p.size) {
            error = "size mismatch after rebuild: " + p.target_name;
            return false;
        }
    }
    if (produced.size() != plan.pieces.size()) {
        error = "unexpected entry count after rebuild";
        return false;
    }
    return true;
}

// Stream one entry of an already-open ZIP straight into the archive being written,
// without ever holding it in memory.
//
// libzip renamed this call: zip_source_zip() gained a password argument and became
// zip_source_zip_file(), the old name surviving as a deprecated alias. Distributions
// are all over the place on which one they ship, so CMake probes for the symbol at
// configure time (HAVE_ZIP_SOURCE_ZIP_FILE) rather than us guessing from version
// numbers. Using the modern spelling where it exists also avoids the deprecation
// warning.
#ifdef HAVE_ZIP_SOURCE_ZIP_FILE
#  define FBNEO_ZIP_SOURCE_FROM_ZIP(dst, src, idx) \
       zip_source_zip_file((dst), (src), (idx), 0, 0, -1, nullptr)
#else
#  define FBNEO_ZIP_SOURCE_FROM_ZIP(dst, src, idx) \
       zip_source_zip((dst), (src), (idx), 0, 0, -1)
#endif

// Maps an inbox path a Move has already relocated to its new outbox path.
using Relocations = std::unordered_map<std::string, std::string>;

// Temporary directory holding entries pulled out of non-ZIP sources, removed
// whatever the outcome. libzip needs the files to survive until zip_close().
struct Staging {
    fs::path dir;
    explicit Staging(const std::string& base) : dir(base + ".staging") {}
    ~Staging() { std::error_code ec; fs::remove_all(dir, ec); }
    Staging(const Staging&) = delete;
    Staging& operator=(const Staging&) = delete;
};

bool rebuild_set(const SetPlan& plan, const Relocations& relocated, std::string& error) {
    const std::string tmp_path = plan.dest_path + ".tmp";

    std::error_code ec;
    fs::remove(tmp_path, ec);

    // Resolve each piece's container up front: a Move may already have relocated
    // the archive a clone borrows from.
    auto container_of = [&](const PiecePlan& p) {
        auto moved = relocated.find(p.src.container);
        return moved != relocated.end() ? moved->second : p.src.container;
    };

    // 7z and rar cannot be streamed entry-by-entry into a ZIP the way libzip
    // streams from another ZIP, and 7z's solid blocks make random access
    // pathological : pulling N entries one at a time re-decompresses the whole
    // block N times. So each non-ZIP source is drained once, in a single pass.
    Staging staging(plan.dest_path);
    std::unordered_map<std::string, std::string> staged;   // "container\x1fentry" → file
    {
        std::unordered_map<std::string, std::vector<std::string>> by_container;
        for (const auto& p : plan.pieces) {
            std::string c = container_of(p);
            // A loose file needs no extraction step at all : it *is* its own
            // one and only piece, read straight off disk further down.
            if (!RomArchive::is_zip(c) && RomArchive::looks_like_archive(c))
                by_container[c].push_back(p.src.entry);
        }
        // Each source gets its own staging subdirectory: extract() numbers its
        // output files from zero, so sharing one directory between two archives
        // would silently overwrite the first one's pieces with the second's.
        size_t slot = 0;
        for (const auto& [container, entries] : by_container) {
            std::string dir = (staging.dir / std::to_string(slot++)).string();
            std::unordered_map<std::string, std::string> files;
            if (!RomArchive::extract(container, entries, dir, files, error))
                return false;
            for (auto& [entry, file] : files)
                staged[container + '\x1f' + entry] = file;
        }
    }

    SourceArchives sources;

    int zerr = 0;
    zip_t* out = zip_open(tmp_path.c_str(), ZIP_CREATE | ZIP_TRUNCATE, &zerr);
    if (!out) {
        error = "cannot create " + tmp_path;
        return false;
    }

    for (const auto& p : plan.pieces) {
        const std::string container = container_of(p);
        zip_source_t* zs = nullptr;

        if (RomArchive::is_zip(container)) {
            zip_t* src = nullptr;
            zip_int64_t src_idx = 0;
            if (!sources.locate(container, p.src.entry, src, src_idx)) {
                error = "source vanished: " + p.src.entry + " in " + container;
                zip_discard(out);
                fs::remove(tmp_path, ec);
                return false;
            }
            // flags = 0 → libzip decompresses the source and recompresses on write,
            // so the output is uniformly deflated whatever the source archive used.
            // It streams, so a 700 MB Neo Geo set never has to fit in memory.
            zs = FBNEO_ZIP_SOURCE_FROM_ZIP(out, src, (zip_uint64_t)src_idx);
        } else if (!RomArchive::looks_like_archive(container)) {
            // Loose file: the piece's bytes are the whole file, nothing to stage.
            zs = zip_source_file(out, container.c_str(), 0, -1);
        } else {
            auto it = staged.find(container + '\x1f' + p.src.entry);
            if (it != staged.end())
                zs = zip_source_file(out, it->second.c_str(), 0, -1);
        }

        if (!zs) {
            error = "cannot read " + p.src.entry + " from " + container;
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }

        zip_int64_t added = zip_file_add(out, p.target_name.c_str(), zs, ZIP_FL_ENC_UTF_8 | ZIP_FL_OVERWRITE);
        if (added < 0) {
            zip_source_free(zs);
            error = "cannot add " + p.target_name + ": " + zip_strerror(out);
            zip_discard(out);
            fs::remove(tmp_path, ec);
            return false;
        }
        zip_set_file_compression(out, (zip_uint64_t)added, ZIP_CM_DEFLATE, 0);
    }

    if (zip_close(out) != 0) {
        error = std::string("cannot finalize archive: ") + zip_strerror(out);
        zip_discard(out);
        fs::remove(tmp_path, ec);
        return false;
    }

    if (!verify_against_plan(tmp_path, plan, error)) {
        fs::remove(tmp_path, ec);
        return false;
    }

    // Only now is the real destination touched.
    fs::rename(tmp_path, plan.dest_path, ec);
    if (ec) {
        fs::remove(plan.dest_path, ec);
        fs::rename(tmp_path, plan.dest_path, ec);
        if (ec) {
            error = "cannot move the rebuilt archive into place: " + ec.message();
            fs::remove(tmp_path, ec);
            return false;
        }
    }
    return true;
}

// rename(), falling back to copy+delete when inbox and outbox live on different
// filesystems : which is the normal case when ROMs sit on an external drive.
bool move_file(const std::string& from, const std::string& to, std::string& error) {
    std::error_code ec;
    fs::rename(from, to, ec);
    if (!ec) return true;

    ec.clear();
    fs::copy_file(from, to, fs::copy_options::overwrite_existing, ec);
    if (ec) {
        error = "cannot copy to " + to + ": " + ec.message();
        return false;
    }
    fs::remove(from, ec);
    if (ec) {
        error = "copied to " + to + " but the source could not be removed: " + ec.message();
        return false;
    }
    return true;
}

} // namespace

// ── Public helpers ───────────────────────────────────────────────────────────

std::string outbox_subdir_for(const Game& game) {
    if (!game.dat_header.empty()) return sanitize_component(game.dat_header);
    // Rows imported before dat_header existed: rebuild the header from the trimmed
    // system name. This reproduces all 17 real FBNeo DAT names exactly.
    if (game.system.empty() || game.system == "Unknown") return "Unknown";
    return sanitize_component("FinalBurn Neo - " + game.system + " Games");
}

const char* action_label(Action a) {
    switch (a) {
        case Action::Move:             return "Complete";
        case Action::Rebuild:          return "Rebuild";
        case Action::Incomplete:       return "Incomplete";
        case Action::AlreadyInLibrary: return "In library";
    }
    return "?";
}

// ── analyze ──────────────────────────────────────────────────────────────────

Report analyze(const std::string& inbox_dir,
               const std::string& outbox_dir,
               std::shared_ptr<DatabaseManager> db,
               bool recursive,
               const Callbacks& cb) {
    Report rep;

    std::error_code ec;
    if (inbox_dir.empty() || !fs::is_directory(inbox_dir, ec)) {
        log(cb, "Inbox directory is not readable: " + inbox_dir);
        return rep;
    }

    // ── 1. Enumerate the inbox ───────────────────────────────────────────────
    report(cb, 2.0, "Listing inbox…");
    std::vector<std::string> zip_paths;
    auto classify = [&](const fs::directory_entry& de) {
        if (!de.is_regular_file(ec)) return;
        std::string p = de.path().string();
        // Any archive format libarchive can open is accepted; whether it can
        // actually be read is settled when we try, below. A file that is not
        // an archive at all is still accepted as a loose, single-ROM source // only the handful of extensions that are clearly never ROM data
        // (readmes, cue sheets, patches, checksums…) are skipped.
        if (RomArchive::looks_like_archive(p) || !is_junk_sidecar(p))
            zip_paths.push_back(p);
    };
    if (recursive) {
        for (auto it = fs::recursive_directory_iterator(inbox_dir, ec);
             it != fs::recursive_directory_iterator(); ++it) {
            if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
            classify(*it);
        }
    } else {
        for (auto it = fs::directory_iterator(inbox_dir, ec);
             it != fs::directory_iterator(); ++it) {
            if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
            classify(*it);
        }
    }
    std::sort(zip_paths.begin(), zip_paths.end());

    log(cb, "Inbox: " + std::to_string(zip_paths.size()) + " archive(s).");

    if (zip_paths.empty()) {
        report(cb, 100.0, "Nothing to analyze.");
        return rep;
    }

    // ── 2. Index the inbox ───────────────────────────────────────────────────
    std::vector<InboxArchive> archives;
    PoolIndex inbox_pool;
    archives.reserve(zip_paths.size());
    for (size_t i = 0; i < zip_paths.size(); ++i) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        double pct = 5.0 + 35.0 * (double)i / (double)zip_paths.size();
        report(cb, pct, "Reading " + fs::path(zip_paths[i]).filename().string());

        InboxArchive a;
        a.path = zip_paths[i];

        if (RomArchive::looks_like_archive(a.path)) {
            // An archive with no readable entry is either corrupt or a format
            // libarchive was not built for; either way it is not something we can
            // act on, and saying so is more useful than "no DAT entry matches".
            if (!RomArchive::read_entries(a.path, a.entries) || a.entries.empty()) {
                log(cb, "  cannot read (corrupt or unsupported format): " +
                            fs::path(a.path).filename().string());
                rep.unsupported.push_back(a.path);
                continue;
            }
        } else {
            // Loose file: its one and only "entry" is itself.
            unsigned long crc = 0; uint64_t size = 0;
            if (!compute_file_crc(a.path, crc, size)) {
                log(cb, "  cannot read: " + fs::path(a.path).filename().string());
                rep.unsupported.push_back(a.path);
                continue;
            }
            a.entries.push_back({fs::path(a.path).filename().string(), crc, size});
        }
        for (const auto& e : a.entries)
            inbox_pool.emplace(e.crc, PieceSource{a.path, e.name, true});
        archives.push_back(std::move(a));
    }

    // ── 3. Index the existing library (read-only pool) ───────────────────────
    report(cb, 42.0, "Indexing the existing library…");
    PoolIndex lib_pool;
    {
        std::vector<DatabaseManager::ZipContentRow> rows;
        db->getAllZipContents(rows);

        const fs::path inbox_c  = weak_canonical(inbox_dir);
        const fs::path outbox_c = outbox_dir.empty() ? fs::path() : weak_canonical(outbox_dir);

        std::unordered_map<std::string, bool> usable;  // per distinct archive path
        for (const auto& r : rows) {
            auto it = usable.find(r.filepath);
            if (it == usable.end()) {
                fs::path p = weak_canonical(r.filepath);
                // Stale cache rows, plus anything already inside the inbox or the
                // outbox : neither is "the library".
                bool ok = fs::exists(p, ec) && !is_under(p, inbox_c) &&
                          (outbox_c.empty() || !is_under(p, outbox_c));
                it = usable.emplace(r.filepath, ok).first;
            }
            if (it->second)
                lib_pool.emplace(r.crc, PieceSource{r.filepath, r.entry_name, false});
        }
    }
    rep.library_pool_empty = lib_pool.empty();
    log(cb, "Library pool: " + std::to_string(lib_pool.size()) + " indexed piece(s).");
    if (rep.library_pool_empty)
        log(cb, "  ⚠ empty pool : run a ROM scan first, or sets will look incomplete.");

    // ── 4. Resolve candidates ────────────────────────────────────────────────
    std::unordered_map<std::string, Game> game_cache;  // "name\x1fsystem" → full set
    auto fetch_game = [&](const std::string& name, const std::string& system) -> const Game& {
        std::string key = name + '\x1f' + system;
        auto it = game_cache.find(key);
        if (it == game_cache.end())
            it = game_cache.emplace(key, db->getGame(name, system)).first;
        return it->second;
    };

    std::unordered_map<std::string, SetPlan> best;  // "name\x1fsystem" → best plan

    // Memo for the content-discovery axis: shared BIOS and PROM dumps make the same
    // CRC come up for archive after archive, and each miss would otherwise be a
    // fresh database round-trip.
    std::unordered_map<unsigned long, std::vector<std::pair<std::string, std::string>>> crc_games;
    auto games_for_crc = [&](unsigned long crc)
        -> const std::vector<std::pair<std::string, std::string>>& {
        auto it = crc_games.find(crc);
        if (it == crc_games.end()) {
            std::vector<std::pair<std::string, std::string>> v;
            for (const auto& g : db->getGamesByRomCrc(crc)) v.emplace_back(g.name, g.system);
            it = crc_games.emplace(crc, std::move(v)).first;
        }
        return it->second;
    };

    for (size_t ai = 0; ai < archives.size(); ++ai) {
        if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
        const InboxArchive& arc = archives[ai];
        double pct = 45.0 + 53.0 * (double)ai / (double)archives.size();
        report(cb, pct, "Matching " + fs::path(arc.path).filename().string());

        // Entries of this archive, by name and by CRC.
        std::unordered_map<std::string, const RomArchive::Entry*> arc_by_name;
        std::unordered_multimap<unsigned long, const RomArchive::Entry*> arc_by_crc;
        for (const auto& e : arc.entries) {
            arc_by_name[e.name] = &e;
            arc_by_crc.emplace(e.crc, &e);
        }

        // Candidate sets, along two axes.
        struct Candidate { std::string name, system; bool by_name; };
        std::vector<Candidate> candidates;
        std::unordered_set<std::string> seen;
        auto add_candidate = [&](const std::string& n, const std::string& s, bool by_name) {
            if (seen.insert(n + '\x1f' + s).second) candidates.push_back({n, s, by_name});
        };

        // Axis 1 : the archive's own name.
        std::string stem = fs::path(arc.path).stem().string();
        for (const auto& g : db->getAllGamesWithName(stem)) add_candidate(g.name, g.system, true);

        // Axis 2 : the archive's content, ALWAYS, not merely as a fallback. An
        // archive is not only "the set it is named after": a ZIP called
        // sxevious.zip may also carry the six hack ROMs that its clone hyxevious
        // is short of, and stopping at the name match would silently leave that
        // set broken. It is also what rescues "Metal Slug (1996).zip".
        //
        // FBNeo DATs inline every shared BIOS ROM into each game (all 656
        // ColecoVision sets carry coleco.rom, every Neo Geo set carries the full
        // BIOS), so a CRC common to hundreds of games identifies nothing and would
        // otherwise turn one archive into the entire catalogue. Only CRCs that
        // actually discriminate are used as seeds.
        {
            constexpr size_t kMaxGamesPerSeedCrc = 32;
            std::unordered_set<unsigned long> distinct_crcs;
            for (const auto& e : arc.entries) distinct_crcs.insert(e.crc);
            for (unsigned long crc : distinct_crcs) {
                if (is_cancelled(cb)) { rep.cancelled = true; return rep; }
                const auto& games = games_for_crc(crc);
                if (games.size() > kMaxGamesPerSeedCrc) continue;  // shared BIOS dump
                for (const auto& [gn, gs] : games) add_candidate(gn, gs, false);
            }
        }

        if (candidates.empty()) {
            {
                std::unordered_set<unsigned long> distinct_crcs;
                for (const auto& e : arc.entries) distinct_crcs.insert(e.crc);
                std::string dbg = "[INBOX-DEBUG] unrecognized " + fs::path(arc.path).filename().string()
                                 + " stem=\"" + stem + "\" entries=" + std::to_string(arc.entries.size());
                for (unsigned long crc : distinct_crcs) {
                    char hex[16]; snprintf(hex, sizeof(hex), "%08lx", crc);
                    dbg += " | crc=" + std::string(hex) + " games=" + std::to_string(games_for_crc(crc).size());
                }
                log(cb, dbg);
            }
            rep.unrecognized.push_back(arc.path);
            continue;
        }

        bool produced_any = false;
        bool saw_already_in_library = false; // a content-only match that turned out redundant
        std::string dbg_trace; // built only when this archive ends up unrecognized

        for (const auto& [cand_name, cand_system, by_name] : candidates) {
            const Game& game = fetch_game(cand_name, cand_system);
            if (game.roms.empty()) {
                dbg_trace += " | candidate " + cand_name + "/" + cand_system + " by_name=" +
                             (by_name ? "1" : "0") + " -> fetch_game returned EMPTY roms";
                continue;
            }

            SetPlan plan;
            plan.game_name       = game.name;
            plan.system          = game.system;
            plan.dat_header      = outbox_subdir_for(game);
            plan.description     = game.description;
            plan.trigger_archive = arc.path;

            bool all_resolved       = true;
            bool all_from_trigger   = true;
            std::unordered_set<std::string> used_from_trigger;

            // Does the library already hold this set? Answered on content rather
            // than on the stored status flag, which can be stale. It is not enough
            // for the pieces to exist *somewhere* in the library: they must all sit
            // in one archive, otherwise the user has scattered ROMs, not a set.
            std::unordered_map<std::string, int> lib_container_hits;
            int verifiable_roms = 0;

            for (const auto& rom : game.roms) {
                if (rom.crc.empty()) continue;  // nodump entry, unverifiable
                unsigned long want_crc = parse_crc_hex(rom.crc);
                uint64_t      want_size = (uint64_t)rom.size;
                verifiable_roms++;

                {
                    // Right CRC is not enough here: a library archive that holds the
                    // right data under the wrong entry name is exactly the case RomAudit
                    // flags as "incorrect" (WrongName), so it must not count as already
                    // correct here either : only a name+CRC match proves the set is fine
                    // as it sits. A CRC-only match still resolves as a repair *source*
                    // for `piece` below; this check only gates the "nothing to do" verdict.
                    std::unordered_set<std::string> seen_containers;
                    auto range = lib_pool.equal_range(want_crc);
                    for (auto it = range.first; it != range.second; ++it)
                        if (it->second.entry == rom.name &&
                            seen_containers.insert(it->second.container).second)
                            lib_container_hits[it->second.container]++;
                }

                PiecePlan piece;
                piece.target_name = rom.name;
                piece.crc         = want_crc;
                piece.size        = want_size;

                auto size_ok = [&](uint64_t got) { return want_size == 0 || got == want_size; };

                // 1. right name, right content, already in the trigger archive
                auto byname = arc_by_name.find(rom.name);
                if (byname != arc_by_name.end() && byname->second->crc == want_crc &&
                    size_ok(byname->second->size)) {
                    piece.src = {arc.path, rom.name, true};
                    piece.resolved = true;
                    used_from_trigger.insert(rom.name);
                }
                // 2. right content under another name in the trigger archive
                if (!piece.resolved) {
                    auto range = arc_by_crc.equal_range(want_crc);
                    for (auto it = range.first; it != range.second; ++it) {
                        if (!size_ok(it->second->size)) continue;
                        piece.src = {arc.path, it->second->name, true};
                        piece.resolved = true;
                        plan.renamed_entries++;
                        used_from_trigger.insert(it->second->name);
                        break;
                    }
                }
                // 3. elsewhere in the inbox
                if (!piece.resolved) {
                    auto range = inbox_pool.equal_range(want_crc);
                    if (range.first != range.second) {
                        piece.src = range.first->second;
                        piece.resolved = true;
                        all_from_trigger = false;
                    }
                }
                // 4. the existing library, read-only
                if (!piece.resolved) {
                    auto range = lib_pool.equal_range(want_crc);
                    if (range.first != range.second) {
                        piece.src = range.first->second;
                        piece.resolved = true;
                        all_from_trigger = false;
                        plan.pieces_from_library++;
                    }
                }

                if (!piece.resolved) {
                    all_resolved = false;
                    plan.missing.push_back({rom.name, want_crc, want_size});
                }
                plan.pieces.push_back(std::move(piece));
            }

            if (plan.pieces.empty()) {
                dbg_trace += " | candidate " + cand_name + "/" + cand_system + " -> plan.pieces EMPTY (every rom.crc was blank/nodump?)";
                continue;
            }

            for (const auto& e : arc.entries)
                if (!used_from_trigger.count(e.name)) plan.extra_entries.push_back(e.name);

            bool library_has_set = false;
            for (const auto& [_c, hits] : lib_container_hits)
                if (verifiable_roms > 0 && hits == verifiable_roms) { library_has_set = true; break; }

            if (library_has_set) {
                plan.action = Action::AlreadyInLibrary;
            } else if (!all_resolved) {
                plan.action = Action::Incomplete;
            } else if (all_from_trigger && plan.renamed_entries == 0 && plan.extra_entries.empty()
                       && RomArchive::is_zip(arc.path)) {
                // A perfect set can only be relocated as-is when it already is a
                // ZIP. A 7z or rar always has to be rewritten, because that is the
                // only container FinalBurn Neo loads.
                plan.action = Action::Move;
            } else {
                plan.action = Action::Rebuild;
            }

            // Content discovery casts a wide net: a single shared PROM links an
            // archive to dozens of unrelated sets. Those are only worth reporting
            // when this archive actually improves them : otherwise the list fills
            // with "already in library" noise about games the user never asked
            // about. Sets matched by filename are always reported, since that is
            // the archive the user deliberately downloaded.
            bool actionable = (plan.action == Action::Move || plan.action == Action::Rebuild);
            if (!by_name && !actionable) {
                const char* an = plan.action == Action::AlreadyInLibrary ? "AlreadyInLibrary"
                                : plan.action == Action::Incomplete       ? "Incomplete" : "?";
                dbg_trace += " | candidate " + cand_name + "/" + cand_system +
                             " -> action=" + an + " by_name=0, skipped as non-actionable content-only match";
                if (plan.action == Action::AlreadyInLibrary) saw_already_in_library = true;
                continue;
            }

            plan.dest_path = (fs::path(outbox_dir) / plan.dat_header /
                              (sanitize_component(plan.game_name) + ".zip")).string();
            plan.selected = actionable;

            produced_any = true;
            std::string key = plan.game_name + '\x1f' + plan.system;
            auto it = best.find(key);
            if (it == best.end() || action_rank(plan.action) > action_rank(it->second.action))
                best[key] = std::move(plan);
        }

        if (!produced_any) {
            if (saw_already_in_library) {
                rep.already_have.push_back(arc.path);
            } else {
                std::string cand_list;
                for (const auto& [n, s, bn] : candidates)
                    cand_list += " " + n + "/" + s + (bn ? "(name)" : "(crc)");
                log(cb, "[INBOX-DEBUG] unrecognized " + fs::path(arc.path).filename().string() +
                        " stem=\"" + stem + "\" candidates:" + cand_list + dbg_trace);
                rep.unrecognized.push_back(arc.path);
            }
        }
    }

    for (auto& [_, plan] : best) rep.sets.push_back(std::move(plan));
    std::sort(rep.sets.begin(), rep.sets.end(), [](const SetPlan& a, const SetPlan& b) {
        if (a.system != b.system) return a.system < b.system;
        return a.game_name < b.game_name;
    });

    for (const auto& s : rep.sets) {
        switch (s.action) {
            case Action::Move:             rep.complete++;   break;
            case Action::Rebuild:          rep.fixable++;    break;
            case Action::Incomplete:       rep.incomplete++; break;
            case Action::AlreadyInLibrary: rep.already++;    break;
        }
    }

    report(cb, 100.0, "Analysis complete.");
    log(cb, "Result: " + std::to_string(rep.complete) + " complete, " +
                std::to_string(rep.fixable) + " to rebuild, " +
                std::to_string(rep.incomplete) + " incomplete, " +
                std::to_string(rep.already) + " already in library, " +
                std::to_string(rep.unrecognized.size()) + " unrecognized.");
    return rep;
}

// ── apply ────────────────────────────────────────────────────────────────────

ApplyResult apply(const Report& report_in, const Callbacks& cb) {
    ApplyResult res;
    std::error_code ec;

    std::vector<const SetPlan*> todo;
    for (const auto& s : report_in.sets) {
        if (!s.selected) continue;
        if (s.action != Action::Move && s.action != Action::Rebuild) continue;
        todo.push_back(&s);
    }

    // Rebuilds run before moves. A parent set is frequently a perfect Move while
    // its clones are Rebuilds sourcing pieces from that same archive; moving the
    // parent out of the inbox first would pull the ground from under them.
    // (Relocations are also tracked below, so either order stays correct.)
    std::stable_partition(todo.begin(), todo.end(),
                          [](const SetPlan* s) { return s->action == Action::Rebuild; });
    if (todo.empty()) {
        report(cb, 100.0, "Nothing to do.");
        return res;
    }

    // (archive, entry) pairs actually consumed, so an inbox archive is only deleted
    // once every single one of its entries has landed somewhere. Anything holding
    // unclaimed data is left alone.
    std::unordered_set<std::string> consumed_entries;
    std::unordered_set<std::string> moved_archives;
    Relocations relocated;

    for (size_t i = 0; i < todo.size(); ++i) {
        if (is_cancelled(cb)) { res.cancelled = true; break; }
        const SetPlan& plan = *todo[i];
        double pct = 100.0 * (double)i / (double)todo.size();
        report(cb, pct, plan.game_name + " (" + plan.system + ")");

        fs::create_directories(fs::path(plan.dest_path).parent_path(), ec);
        if (ec) {
            res.failed++;
            res.errors.push_back(plan.game_name + ": cannot create the destination folder: " + ec.message());
            ec.clear();
            continue;
        }

        std::string error;
        if (plan.action == Action::Move) {
            if (move_file(plan.trigger_archive, plan.dest_path, error)) {
                res.moved++;
                moved_archives.insert(plan.trigger_archive);
                relocated[plan.trigger_archive] = plan.dest_path;
                log(cb, "moved   " + plan.game_name + " → " + plan.dat_header);
            } else {
                res.failed++;
                res.errors.push_back(plan.game_name + ": " + error);
                log(cb, "FAILED  " + plan.game_name + ": " + error);
            }
        } else {
            if (rebuild_set(plan, relocated, error)) {
                res.rebuilt++;
                for (const auto& p : plan.pieces)
                    consumed_entries.insert(p.src.container + '\x1f' + p.src.entry);
                std::string extra = plan.pieces_from_library
                                        ? " (" + std::to_string(plan.pieces_from_library) + " from library)"
                                        : "";
                log(cb, "rebuilt " + plan.game_name + " → " + plan.dat_header + extra);
            } else {
                res.failed++;
                res.errors.push_back(plan.game_name + ": " + error);
                log(cb, "FAILED  " + plan.game_name + ": " + error);
            }
        }
    }

    // Delete inbox archives whose every entry was consumed by a successful rebuild.
    if (!res.cancelled) {
        report(cb, 99.0, "Cleaning up the inbox…");
        std::unordered_map<std::string, std::vector<std::string>> inbox_entries;
        for (const auto& s : report_in.sets) {
            if (moved_archives.count(s.trigger_archive)) continue;
            if (inbox_entries.count(s.trigger_archive)) continue;
            std::vector<std::string> names;
            if (!RomArchive::looks_like_archive(s.trigger_archive))
                names.push_back(fs::path(s.trigger_archive).filename().string());
            else if (!RomArchive::list_names(s.trigger_archive, names))
                continue;
            inbox_entries[s.trigger_archive] = std::move(names);
        }
        for (const auto& [archive, names] : inbox_entries) {
            bool fully_consumed = !names.empty();
            for (const auto& n : names) {
                if (!consumed_entries.count(archive + '\x1f' + n)) { fully_consumed = false; break; }
            }
            if (!fully_consumed) continue;
            fs::remove(archive, ec);
            if (!ec) {
                res.consumed_archives++;
                log(cb, "consumed " + fs::path(archive).filename().string());
            }
            ec.clear();
        }
    }

    report(cb, 100.0, "Done.");
    log(cb, "Applied: " + std::to_string(res.moved) + " moved, " +
                std::to_string(res.rebuilt) + " rebuilt, " +
                std::to_string(res.failed) + " failed, " +
                std::to_string(res.consumed_archives) + " source archive(s) consumed.");
    return res;
}

} // namespace RomInbox
