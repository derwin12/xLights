#pragma once

/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include <string>
#include <unordered_map>
#include <unordered_set>

// Loads and queries the community alias pack served from
// mapper.xlights.info's /xlights/pairs endpoint: a crowdsourced set of
// (vendor model/group name -> user model/group name) pairs that other users
// have accepted often enough to be promoted to "approved".
//
// Used by QuikMap as "Phase 2.5": after exact-name and per-user-alias
// matching, but before submodel fallback, names are checked against this
// pack as an extra community-sourced alias source.
class CommunityAliasPack {
public:
    // Loads a previously-cached copy of the pack from `path` (the JSON
    // array returned by GET /xlights/pairs). Returns false if the file
    // doesn't exist or can't be parsed; the pack is left empty in that case.
    bool LoadFromFile(const std::string& path);

    // Parses the JSON array directly (used by LoadFromFile and by Fetch
    // after a successful download).
    bool LoadFromJson(const std::string& json);

    // True if `candidate` is a known community alias for `target` (both
    // compared after lowercasing/whitespace-normalization).
    bool Matches(const std::string& target, const std::string& candidate) const;

    bool Empty() const { return _aliasesByTarget.empty(); }
    size_t Size() const { return _pairCount; }

    // Downloads the current approved pack from `baseUrl` (e.g.
    // "https://mapper.xlights.info") and writes it to `cachePath`,
    // replacing this instance's in-memory pack on success.
    // Returns false (leaving any previously-loaded pack untouched) on
    // network failure or invalid response - callers should treat this as
    // "community aliases unavailable" rather than an error.
    bool Fetch(const std::string& baseUrl, const std::string& cachePath);

    // Submits a single accepted (vendor name -> user name) mapping to the
    // community pack at `baseUrl`. Best-effort / fire-and-forget: returns
    // false on any failure, callers should not surface this to the user.
    static bool Submit(const std::string& baseUrl,
                        const std::string& vendorName,
                        const std::string& vendorModelType,
                        const std::string& userName);

private:
    // userNameNorm -> set of vendorNameNorm aliases for that target.
    std::unordered_map<std::string, std::unordered_set<std::string>> _aliasesByTarget;
    size_t _pairCount = 0;
};
