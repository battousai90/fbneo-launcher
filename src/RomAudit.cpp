// src/RomAudit.cpp
#include "RomAudit.h"

#include <algorithm>
#include <filesystem>
#include <unordered_map>
#include <unordered_set>

namespace fs = std::filesystem;

namespace RomAudit {
namespace {

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

} // namespace

Report audit(std::shared_ptr<DatabaseManager> db,
             const std::vector<std::string>& roms_paths,
             bool problems_only,
             const RomInbox::Callbacks& cb) {
    Report rep;
    rep.style = RomResolve::load_style();

    // ── 1. Index the scan cache ──────────────────────────────────────────────
    // The same index the scanner's split pass reads, so the two pick the same
    // archive for a set and judge it by the same rule.
    report(cb, 2.0, "Reading the ROM cache…");
    RomResolve::CacheIndex index(db, roms_paths);

    // Which archives hold a given CRC : the repair-source axis ("a good copy
    // exists in mslug2.zip") and the orphan report's "copy elsewhere" flag.
    std::unordered_multimap<unsigned long, std::string> crc_to_archive;
    for (const auto& [path, a] : index.all())
        for (const auto& [crc, name] : a.name_by_crc)
            crc_to_archive.emplace(crc, path);

    rep.pool_empty = index.empty();
    log(cb, "Indexed " + std::to_string(index.size()) + " archive(s) from the scan cache; collection style: "
            + RomResolve::to_string(rep.style) + ".");
    if (rep.pool_empty) {
        log(cb, "  ⚠ the cache is empty : run a ROM scan first.");
        report(cb, 100.0, "Nothing to audit.");
        return rep;
    }

    // ── 2. Walk every game in the database ───────────────────────────────────
    report(cb, 12.0, "Loading the game list…");
    std::vector<Game> games = db->getAllGames();
    rep.total = (int)games.size();

    // Sets the user asked not to hear about again (see DatabaseManager::ignoreSet).
    std::unordered_set<std::string> ignored;
    for (const auto& ig : db->getIgnoredSets()) ignored.insert(ig.name + '\x1f' + ig.system);

    // Every short name the current DAT knows about, regardless of which exact
    // archive ends up "claimed" for it. The same short name legitimately exists
    // under several systems (mslug is Arcade, Neo Geo *and* GBA's "Metal Slug
    // Advance"), and if two roots both happen to have a folder with the same
    // name : e.g. the library's own "…GBA Games" and an outbox someone also
    // added to roms_paths : the per-game archive picker can only claim one of
    // the look-alikes. The other must not be reported as an orphan just
    // because it lost that coin flip; it is exactly as real a game.
    std::unordered_set<std::string> known_stems;
    std::unordered_map<std::string, size_t> by_key;
    by_key.reserve(games.size());
    for (size_t i = 0; i < games.size(); ++i) {
        known_stems.insert(lower(games[i].name));
        by_key[games[i].name + '\x1f' + games[i].system] = i;
    }

    // The romof chain is walked through this in-memory list, never the database.
    RomResolve::GameLookup game_for = [&](const std::string& name, const std::string& system) -> Game {
        auto it = by_key.find(name + '\x1f' + system);
        return it == by_key.end() ? Game{} : games[it->second];
    };
    RomResolve::ArchiveLookup archive_for = [&](const Game& g) { return index.for_game(g); };

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

        // Which archive should hold this set? By name first (the index scores
        // same-named candidates and breaks ties by folder, see CacheIndex).
        const RomResolve::Archive* own = index.for_game(g);
        if (own) {
            e.archive = own->path;
            e.archive_found = true;
        }

        // No archive carries this set's name. The scanner falls back to matching on
        // content alone, so a correctly-dumped set inside a differently-named ZIP
        // still counts as available : mirror that, or the audit would invent
        // "missing" sets the scanner is happy with.
        if (!own) {
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
                unsigned long want = strtoul(rom.crc.c_str(), nullptr, 16);
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
                own = index.by_path(e.archive);
            }
        }

        // The verdict, ROM by ROM, by the one shared rule. In a split
        // collection an inherited ROM the set's own zip lacks is looked for in
        // the parent's, then the BIOS's, and the verdict says which one had it.
        RomResolve::Verdict verdict = RomResolve::evaluate(g, own, rep.style, archive_for, game_for);
        if (verdict.roms.empty()) continue;

        for (const auto& v : verdict.roms) {
            RomEntry r;
            r.name           = v.name;
            r.crc            = v.crc;
            r.size           = v.size;
            r.state          = v.state;
            r.found_as       = v.found_as;
            r.found_in       = v.found_in;
            r.inherited      = v.inherited;
            r.inherited_from = v.inherited_from;

            if (r.state == RomState::Corrupt) {
                e.corrupt++;
                // Does a good copy exist anywhere else in the library? If so the
                // set can be repaired locally instead of re-downloaded.
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
            } else if (r.state == RomState::Absent) {
                auto other = crc_to_archive.find(r.crc);
                if (other != crc_to_archive.end()) r.found_in = other->second;
                e.absent++;
            } else if (r.state == RomState::WrongName) {
                e.wrong++;
            }
            e.roms.push_back(std::move(r));
        }

        // Entries physically present in the archive that no rom above needs at
        // all (RomVault calls these Purple/Brown: "not needed here"). Harmless
        // for FBNeo : it only ever reads what it asks for by name : but worth
        // surfacing so they can be swept into quarantine like anything else.
        // An inherited ROM a split zip still carries is required-but-optional,
        // not extra : it is in e.roms, so it never lands here.
        if (own && !e.archive.empty()) {
            // DAT rom names are already canonical (e.g. "Spider-Man: Return…").
            // Archive entry names are what needs normalizing here : many were
            // saved with '-' where the DAT has ':' (filesystem-safe substitution)
            // : same direction the cache itself normalizes in when built.
            std::unordered_set<std::string> required;
            for (const auto& r : e.roms) required.insert(r.name);
            for (const auto& name : own->entries)
                if (!required.count(name) && !required.count(RomScanner::normalize_name(name)))
                    e.extra_entries.push_back(name);
        }

        e.status  = verdict.status;
        e.ignored = ignored.count(g.name + '\x1f' + g.system) > 0;

        // An ignored set keeps its real status (so "why did I ignore this?"
        // still has an answer) but is a bucket of its own: not a problem to
        // count, not a repair to offer.
        if (e.ignored) {
            rep.ignored++;
            if (!problems_only || e.status != "available" || !e.extra_entries.empty())
                rep.games.push_back(std::move(e));
            continue;
        }

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
    for (const auto& [path, a] : index.all()) {
        if (cancelled(cb)) { rep.cancelled = true; return rep; }
        if (known_stems.count(lower(fs::path(path).stem().string()))) continue;
        if (a.entries.empty()) continue;

        OrphanArchive orphan;
        orphan.path = path;
        for (const auto& name : a.entries) {
            auto crc_it = a.crc_by_name.find(name);
            if (crc_it == a.crc_by_name.end()) continue;

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
                std::to_string(rep.ignored) + " ignored; " +
                std::to_string(rep.orphans.size()) + " orphan archive(s) matching no current DAT entry.");
    return rep;
}

} // namespace RomAudit
