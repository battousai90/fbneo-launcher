// src/RomAudit.cpp
#include "RomAudit.h"

#include <algorithm>
#include <filesystem>
#include <sstream>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomAudit {
namespace {

unsigned long parse_crc_hex(const std::string& hex) {
    if (hex.empty()) return 0;
    unsigned long crc = 0;
    std::stringstream ss;
    ss << std::hex << hex;
    ss >> crc;
    return crc;
}

void report(const RomInbox::Callbacks& cb, double pct, const std::string& msg) {
    if (cb.progress) cb.progress(pct, msg);
}
void log(const RomInbox::Callbacks& cb, const std::string& msg) {
    if (cb.log) cb.log(msg);
}
bool cancelled(const RomInbox::Callbacks& cb) { return cb.cancelled && cb.cancelled(); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return (char)std::tolower(c); });
    return s;
}

// One cached archive: its entries by name (raw *and* normalized, exactly like the
// scanner) plus the CRCs it holds.
struct ArchiveIndex {
    std::unordered_map<std::string, unsigned long> crc_by_name;
    std::unordered_set<unsigned long>              crcs;
};

} // namespace

Report audit(std::shared_ptr<DatabaseManager> db,
             const std::vector<std::string>& roms_paths,
             bool problems_only,
             const RomInbox::Callbacks& cb) {
    Report rep;
    std::error_code ec;

    // ── 1. Index the scan cache ──────────────────────────────────────────────
    report(cb, 2.0, "Reading the ROM cache…");
    std::vector<DatabaseManager::ZipContentRow> rows;
    db->getAllZipContents(rows);

    std::vector<fs::path> roots;
    for (const auto& r : roms_paths)
        if (!r.empty()) roots.push_back(fs::weakly_canonical(fs::path(r), ec));

    auto under_roots = [&](const fs::path& p) {
        if (roots.empty()) return true;   // no configured roots: accept everything
        for (const auto& root : roots) {
            auto it_r = root.begin(), end_r = root.end();
            auto it_p = p.begin(), end_p = p.end();
            bool ok = true;
            for (; it_r != end_r; ++it_r, ++it_p) {
                if (it_p == end_p || *it_p != *it_r) { ok = false; break; }
            }
            if (ok) return true;
        }
        return false;
    };

    std::unordered_map<std::string, ArchiveIndex> archives;   // path → index
    std::unordered_multimap<unsigned long, std::string> crc_to_archive;
    std::unordered_map<std::string, std::vector<std::string>> by_stem;
    // Raw entry names per archive, one per real zip entry (unlike ArchiveIndex.
    // crc_by_name, which stores each name twice : raw and normalized : and so
    // cannot be used to recover "what is actually in this zip").
    std::unordered_map<std::string, std::vector<std::string>> raw_entries_by_path;
    {
        std::unordered_map<std::string, bool> usable;
        for (const auto& r : rows) {
            auto u = usable.find(r.filepath);
            if (u == usable.end()) {
                fs::path p = fs::weakly_canonical(fs::path(r.filepath), ec);
                bool ok = fs::exists(p, ec) && under_roots(p);
                u = usable.emplace(r.filepath, ok).first;
                if (ok) by_stem[lower(fs::path(r.filepath).stem().string())].push_back(r.filepath);
            }
            if (!u->second) continue;
            auto& idx = archives[r.filepath];
            idx.crc_by_name[r.entry_name] = r.crc;
            // Second key under the scanner's normalization, so a name that differs
            // only by the ':'/'-' substitution is not reported as a mismatch here
            // while the scanner considers the set perfectly fine.
            idx.crc_by_name[RomScanner::normalize_name(r.entry_name)] = r.crc;
            idx.crcs.insert(r.crc);
            crc_to_archive.emplace(r.crc, r.filepath);
            raw_entries_by_path[r.filepath].push_back(r.entry_name);
        }
    }
    rep.pool_empty = archives.empty();
    log(cb, "Indexed " + std::to_string(archives.size()) + " archive(s) from the scan cache.");
    if (rep.pool_empty) {
        log(cb, "  ⚠ the cache is empty : run a ROM scan first.");
        report(cb, 100.0, "Nothing to audit.");
        return rep;
    }

    // ── 2. Walk every game in the database ───────────────────────────────────
    report(cb, 12.0, "Loading the game list…");
    std::vector<Game> games = db->getAllGames();
    rep.total = (int)games.size();

    // Every short name the current DAT knows about, regardless of which exact
    // archive ends up "claimed" for it. The same short name legitimately exists
    // under several systems (mslug is Arcade, Neo Geo *and* GBA's "Metal Slug
    // Advance"), and if two roots both happen to have a folder with the same
    // name : e.g. the library's own "…GBA Games" and an outbox someone also
    // added to roms_paths : the per-game archive picker below can only claim
    // one of the look-alikes. The other must not be reported as an orphan just
    // because it lost that coin flip; it is exactly as real a game.
    std::unordered_set<std::string> known_stems;
    for (const auto& g : games) known_stems.insert(lower(g.name));

    for (size_t gi = 0; gi < games.size(); ++gi) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        if ((gi % 512) == 0)
            report(cb, 15.0 + 84.0 * (double)gi / (double)games.size(),
                   "Auditing " + std::to_string(gi) + " / " + std::to_string(games.size()));

        const Game& g = games[gi];
        if (g.roms.empty()) continue;

        GameEntry e;
        e.name        = g.name;
        e.system      = g.system;
        e.description = g.description;
        e.cloneof     = g.cloneof;
        e.dat_header  = g.dat_header.empty()
                          ? ("FinalBurn Neo - " + g.system + " Games") : g.dat_header;

        // Which archive should hold this set? Several same-named archives can
        // legitimately coexist : the same short name exists under several
        // systems (tankbatt is both an Arcade and an MSX 1 set), and a folder
        // added separately to roms_paths (an outbox mirrors the library's own
        // "FinalBurn Neo - X Games" naming on purpose) can produce a second
        // candidate under an identically-named parent folder. Folder-name alone
        // cannot break that tie : a broken/incomplete duplicate sitting in an
        // outbox has the exact same parent folder name as the real, complete
        // one in the library, so picking "first folder-name match" can just as
        // easily choose the broken copy. Score every candidate by how many of
        // the set's own required ROMs it actually satisfies and take the best;
        // folder-name match only breaks a genuine tie.
        const ArchiveIndex* idx = nullptr;
        auto cand = by_stem.find(lower(g.name));
        if (cand != by_stem.end() && !cand->second.empty()) {
            std::string best = cand->second.front();
            int best_score = -1;
            bool best_dir_match = false;
            for (const auto& path : cand->second) {
                auto ait = archives.find(path);
                int score = 0;
                if (ait != archives.end()) {
                    for (const auto& rom : g.roms) {
                        if (rom.crc.empty()) continue;
                        unsigned long want = parse_crc_hex(rom.crc);
                        auto byname = ait->second.crc_by_name.find(rom.name);
                        if (byname == ait->second.crc_by_name.end())
                            byname = ait->second.crc_by_name.find(RomScanner::normalize_name(rom.name));
                        if (byname != ait->second.crc_by_name.end() && byname->second == want)
                            ++score;
                    }
                }
                bool dir_match = fs::path(path).parent_path().filename().string() == e.dat_header;
                if (score > best_score || (score == best_score && dir_match && !best_dir_match)) {
                    best = path;
                    best_score = score;
                    best_dir_match = dir_match;
                }
            }
            e.archive = best;
            e.archive_found = true;
            idx = &archives[best];
        }

        // No archive carries this set's name. The scanner falls back to matching on
        // content alone, so a correctly-dumped set inside a differently-named ZIP
        // still counts as available : mirror that, or the audit would invent
        // "missing" sets the scanner is happy with.
        if (!idx) {
            // A CRC shared across many archives (a BIOS, a common expansion ROM)
            // cannot tell one archive from another and must not count as
            // evidence : otherwise a game whose own data genuinely is not
            // anywhere gets pinned on whichever unrelated archive happens to
            // share its BIOS, misreporting that archive's real content as
            // "extra" and hiding that this game is simply absent. Same
            // reasoning, same threshold, as RomInbox's content-discovery axis.
            constexpr size_t kMaxArchivesPerDiscriminatingCrc = 32;
            std::unordered_map<std::string, int> hits;
            for (const auto& rom : g.roms) {
                if (rom.crc.empty()) continue;
                unsigned long want = parse_crc_hex(rom.crc);
                std::unordered_set<std::string> seen;
                auto range = crc_to_archive.equal_range(want);
                for (auto it = range.first; it != range.second; ++it)
                    seen.insert(it->second);
                if (seen.size() > kMaxArchivesPerDiscriminatingCrc) continue;
                for (const auto& path : seen) hits[path]++;
            }
            int best_hits = 0;
            for (const auto& [path, n] : hits)
                if (n > best_hits) { best_hits = n; e.archive = path; }
            if (best_hits > 0) {
                e.archive_found = true;
                idx = &archives[e.archive];
            }
        }

        for (const auto& rom : g.roms) {
            if (rom.crc.empty()) continue;   // nodump: nothing to verify against
            RomEntry r;
            r.name = rom.name;
            r.crc  = parse_crc_hex(rom.crc);
            r.size = (uint64_t)rom.size;

            if (idx) {
                auto byname = idx->crc_by_name.find(rom.name);
                if (byname == idx->crc_by_name.end())
                    byname = idx->crc_by_name.find(RomScanner::normalize_name(rom.name));
                bool name_present = (byname != idx->crc_by_name.end());
                if (name_present && byname->second == r.crc) {
                    r.state = RomState::Present;
                } else if (idx->crcs.count(r.crc)) {
                    // Right data, wrong filename : find which entry carries it.
                    r.state = RomState::WrongName;
                    for (const auto& [n, c] : idx->crc_by_name)
                        if (c == r.crc) { r.found_as = n; break; }
                } else if (name_present) {
                    // The file is in the archive under the expected name, but its
                    // content is not what the DAT describes: a bad dump or the
                    // wrong revision. The set counts as incorrect, not missing // matching how the scanner classifies it.
                    r.state = RomState::Corrupt;
                } else {
                    r.state = RomState::Absent;
                }
            }

            if (r.state == RomState::Corrupt) {
                e.corrupt++;
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
            } else if (r.state == RomState::Absent) {
                // Does a good copy exist anywhere else in the library? If so the set
                // can be repaired locally instead of re-downloaded.
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
                e.absent++;
            } else if (r.state == RomState::WrongName) {
                e.wrong++;
            }
            e.roms.push_back(std::move(r));
        }

        if (e.roms.empty()) continue;

        // Entries physically present in the archive that no rom above needs at
        // all (RomVault calls these Purple/Brown: "not needed here"). Harmless
        // for FBNeo : it only ever reads what it asks for by name : but worth
        // surfacing so they can be swept into quarantine like anything else.
        if (idx && !e.archive.empty()) {
            // DAT rom names are already canonical (e.g. "Spider-Man: Return…").
            // Archive entry names are what needs normalizing here : many were
            // saved with '-' where the DAT has ':' (filesystem-safe substitution)
            // : same direction the cache itself normalizes in when built above.
            std::unordered_set<std::string> required;
            for (const auto& r : e.roms) required.insert(r.name);
            auto raw = raw_entries_by_path.find(e.archive);
            if (raw != raw_entries_by_path.end())
                for (const auto& name : raw->second)
                    if (!required.count(name) && !required.count(RomScanner::normalize_name(name)))
                        e.extra_entries.push_back(name);
        }

        // Same rule as RomScanner::check_game_maps, so this audit can never
        // contradict the counters the main window shows.
        if (e.absent > 0)                       e.status = "missing";
        else if (e.wrong > 0 || e.corrupt > 0)  e.status = "incorrect";
        else                                    e.status = "available";

        if (e.status == "available")      rep.available++;
        else if (e.status == "incorrect") rep.incorrect++;
        else                              rep.missing++;

        // Repairable = nothing is truly gone. Every broken piece : absent or
        // corrupt : has a good copy in another library archive, so the set can be
        // rebuilt locally instead of re-downloaded.
        e.repairable = (e.status != "available");
        for (const auto& r : e.roms) {
            if ((r.state == RomState::Absent || r.state == RomState::Corrupt) && r.found_in.empty()) {
                e.repairable = false;
                break;
            }
        }
        if (e.repairable) rep.repairable++;

        if (!problems_only || e.status != "available" || !e.extra_entries.empty())
            rep.games.push_back(std::move(e));
    }

    std::sort(rep.games.begin(), rep.games.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return a.system < b.system;
        return a.name < b.name;
    });

    // ── 3. Whole archives matching no known game name at all ─────────────────
    // Same RomVault behaviour: an unrecognized zip is still opened and each of
    // its entries checked by CRC against the whole library, regardless of
    // whether the zip's own name matches anything. Checked against known_stems
    // rather than "did some GameEntry end up claiming this exact path" : a
    // duplicate copy of a real game (e.g. sitting in an outbox someone also
    // scans) is still a real game, just not the one instance a same-named
    // system folder in another root happened to win for its GameEntry.
    report(cb, 96.0, "Checking for orphan archives…");
    for (const auto& [path, idx] : archives) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        if (known_stems.count(lower(fs::path(path).stem().string()))) continue;

        auto raw = raw_entries_by_path.find(path);
        if (raw == raw_entries_by_path.end() || raw->second.empty()) continue;

        OrphanArchive orphan;
        orphan.path = path;
        for (const auto& name : raw->second) {
            auto crc_it = idx.crc_by_name.find(name);
            if (crc_it == idx.crc_by_name.end()) continue;

            OrphanEntry oe;
            oe.name = name;
            oe.crc  = crc_it->second;
            auto range = crc_to_archive.equal_range(oe.crc);
            for (auto it = range.first; it != range.second && !oe.copy_elsewhere; ++it)
                if (it->second != path) oe.copy_elsewhere = true;
            orphan.entries.push_back(std::move(oe));
        }
        rep.orphans.push_back(std::move(orphan));
    }
    std::sort(rep.orphans.begin(), rep.orphans.end(),
              [](const OrphanArchive& a, const OrphanArchive& b) { return a.path < b.path; });

    report(cb, 100.0, "Audit complete.");
    log(cb, "Library: " + std::to_string(rep.available) + " available, " +
                std::to_string(rep.incorrect) + " incorrect, " +
                std::to_string(rep.missing) + " missing (of " +
                std::to_string(rep.total) + " sets); " +
                std::to_string(rep.repairable) + " repairable from the library itself; " +
                std::to_string(rep.orphans.size()) + " orphan archive(s) matching no current DAT entry.");
    return rep;
}

} // namespace RomAudit
