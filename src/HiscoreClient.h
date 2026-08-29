// src/HiscoreClient.h
//
// Read-only client for the Bootcade score service. Submitting a score is a
// separate concern and lands later; everything here is safe to call without
// the player having any account, pseudonym or opinion on the matter.
//
// Every call is blocking and MUST run off the GTK thread. The service lives
// on the network, so a slow or absent one would otherwise freeze the game
// list — and an unreachable service is the normal case for anyone running
// the launcher without the homelab.
#pragma once
#include <set>
#include <string>
#include <vector>

namespace HiscoreClient {

// Base URL of the service, e.g. "http://192.168.1.21". Read from
// config.json ("hiscore_url"); an empty value disables every call, which is
// what a fresh install with no server configured should do.
std::string base_url();
void        set_base_url(const std::string& url);

// "<system>\n<game>" keys for the games the service can rank. Sent as one
// list rather than asked per game: the alternative is 29 000 requests to
// paint one column.
std::set<std::string> fetch_supported();

struct Entry {
    std::string player;
    std::string country;   // ISO code, may be empty
    long long   score = 0;
    std::string since;     // ISO-8601 UTC, when the score was validated
};

// Leaderboard for one game, best first. Empty on any failure — a missing
// leaderboard and an unreachable server look the same to the player, and
// neither is worth an error dialog over a game they were merely browsing.
std::vector<Entry> fetch_top(const std::string& system,
                             const std::string& game,
                             int limit = 50);

// Outcome of a submission. `reached` distinguishes "the service answered"
// from "we never got there": an unreachable server is not the player's
// problem and must not be reported to them as a rejected score.
struct SubmitResult {
    bool        reached = false;
    bool        accepted = false;   // published straight away
    bool        pending  = false;   // queued for an administrator
    long long   score = 0;
    bool        has_score = false;
    std::string reason;             // why it was queued, or why it was refused
    std::string error;              // transport-level failure
};

// Send one session. `hi_before` may be empty — a first-ever game on a title
// has no baseline — and the service will queue rather than publish it.
SubmitResult submit(const std::string& system,
                    const std::string& game,
                    const std::string& player,
                    const std::string& country,   // ISO code, may be empty
                    const std::string& hi_before,
                    const std::string& hi_after);

// Key used in the supported-games set.
inline std::string key(const std::string& system, const std::string& game) {
    return system + "\n" + game;
}

} // namespace HiscoreClient
