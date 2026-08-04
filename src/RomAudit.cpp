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
        }
    }
    rep.pool_empty = archives.empty();
    log(cb, "Indexed " + std::to_string(archives.size()) + " archive(s) from the scan cache.");
    if (rep.pool_empty) {
        log(cb, "  ⚠ the cache is empty — run a ROM scan first.");
        report(cb, 100.0, "Nothing to audit.");
        return rep;
    }

    // ── 2. Walk every game in the database ───────────────────────────────────
    report(cb, 12.0, "Loading the game list…");
    std::vector<Game> games = db->getAllGames();
    rep.total = (int)games.size();

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

        // Which archive should hold this set? Among same-named archives, prefer the
        // one sitting in this system's own folder — the same short name exists under
        // several systems (tankbatt is both an Arcade and an MSX 1 set).
        const ArchiveIndex* idx = nullptr;
        auto cand = by_stem.find(lower(g.name));
        if (cand != by_stem.end() && !cand->second.empty()) {
            std::string best = cand->second.front();
            for (const auto& path : cand->second) {
                std::string dir = fs::path(path).parent_path().filename().string();
                if (dir == e.dat_header) { best = path; break; }
            }
            e.archive = best;
            e.archive_found = true;
            idx = &archives[best];
        }

        // No archive carries this set's name. The scanner falls back to matching on
        // content alone, so a correctly-dumped set inside a differently-named ZIP
        // still counts as available — mirror that, or the audit would invent
        // "missing" sets the scanner is happy with.
        if (!idx) {
            std::unordered_map<std::string, int> hits;
            for (const auto& rom : g.roms) {
                if (rom.crc.empty()) continue;
                unsigned long want = parse_crc_hex(rom.crc);
                std::unordered_set<std::string> seen;
                auto range = crc_to_archive.equal_range(want);
                for (auto it = range.first; it != range.second; ++it)
                    if (seen.insert(it->second).second) hits[it->second]++;
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
                    // Right data, wrong filename — find which entry carries it.
                    r.state = RomState::WrongName;
                    for (const auto& [n, c] : idx->crc_by_name)
                        if (c == r.crc) { r.found_as = n; break; }
                } else if (name_present) {
                    // The file is in the archive under the expected name, but its
                    // content is not what the DAT describes: a bad dump or the
                    // wrong revision. The set counts as incorrect, not missing —
                    // matching how the scanner classifies it.
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

        // Same rule as RomScanner::check_game_maps, so this audit can never
        // contradict the counters the main window shows.
        if (e.absent > 0)                       e.status = "missing";
        else if (e.wrong > 0 || e.corrupt > 0)  e.status = "incorrect";
        else                                    e.status = "available";

        if (e.status == "available")      rep.available++;
        else if (e.status == "incorrect") rep.incorrect++;
        else                              rep.missing++;

        // Repairable = nothing is truly gone. Every broken piece — absent or
        // corrupt — has a good copy in another library archive, so the set can be
        // rebuilt locally instead of re-downloaded.
        e.repairable = (e.status != "available");
        for (const auto& r : e.roms) {
            if ((r.state == RomState::Absent || r.state == RomState::Corrupt) && r.found_in.empty()) {
                e.repairable = false;
                break;
            }
        }
        if (e.repairable) rep.repairable++;

        if (!problems_only || e.status != "available")
            rep.games.push_back(std::move(e));
    }

    std::sort(rep.games.begin(), rep.games.end(), [](const GameEntry& a, const GameEntry& b) {
        if (a.system != b.system) return a.system < b.system;
        return a.name < b.name;
    });

    report(cb, 100.0, "Audit complete.");
    log(cb, "Library: " + std::to_string(rep.available) + " available, " +
                std::to_string(rep.incorrect) + " incorrect, " +
                std::to_string(rep.missing) + " missing (of " +
                std::to_string(rep.total) + " sets); " +
                std::to_string(rep.repairable) + " repairable from the library itself.");
    return rep;
}

} // namespace RomAudit
