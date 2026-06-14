/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "CommunityAliasPack.h"

#include "utils/CurlManager.h"

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <cctype>
#include <fstream>
#include <sstream>

namespace {

// Matches the server's normalization (app/pairs.py:_normalize): lowercase,
// trim, and collapse any internal whitespace runs to a single space. Unlike
// AutoMapper's fuzzy matcher, punctuation is intentionally left intact so
// pack entries round-trip exactly with what was submitted.
std::string NormalizeName(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool lastWasSpace = true; // trims leading whitespace
    for (char c : in) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        bool isSpace = (lc == ' ' || lc == '\t' || lc == '\n' || lc == '\r');
        if (isSpace) {
            if (lastWasSpace) continue;
            lastWasSpace = true;
            out.push_back(' ');
        } else {
            lastWasSpace = false;
            out.push_back(lc);
        }
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

} // namespace

bool CommunityAliasPack::LoadFromJson(const std::string& json) {
    std::unordered_map<std::string, std::unordered_set<std::string>> aliases;
    size_t count = 0;
    try {
        auto parsed = nlohmann::json::parse(json);
        if (!parsed.is_array()) {
            return false;
        }
        for (const auto& entry : parsed) {
            if (!entry.contains("vendor_name") || !entry.contains("user_name")) continue;
            std::string vendorName = entry["vendor_name"].get<std::string>();
            std::string userName = entry["user_name"].get<std::string>();
            aliases[NormalizeName(userName)].insert(NormalizeName(vendorName));
            count++;
        }
    } catch (const nlohmann::json::exception& e) {
        spdlog::warn("CommunityAliasPack::LoadFromJson: failed to parse pack: {}", e.what());
        return false;
    }

    _aliasesByTarget = std::move(aliases);
    _pairCount = count;
    return true;
}

bool CommunityAliasPack::LoadFromFile(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open()) {
        return false;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    return LoadFromJson(ss.str());
}

bool CommunityAliasPack::Matches(const std::string& target, const std::string& candidate) const {
    auto it = _aliasesByTarget.find(NormalizeName(target));
    if (it == _aliasesByTarget.end()) return false;
    return it->second.count(NormalizeName(candidate)) != 0;
}

bool CommunityAliasPack::Fetch(const std::string& baseUrl, const std::string& cachePath) {
    int rc = 0;
    std::string resp = CurlManager::HTTPSGet(baseUrl + "/xlights/pairs", "", "", 10, {}, &rc);
    if (rc < 200 || rc >= 300 || resp.empty()) {
        spdlog::info("CommunityAliasPack::Fetch: request to {} failed (rc={})", baseUrl, rc);
        return false;
    }

    if (!LoadFromJson(resp)) {
        return false;
    }

    std::ofstream out(cachePath, std::ios::binary | std::ios::trunc);
    if (out.is_open()) {
        out << resp;
    }
    return true;
}

bool CommunityAliasPack::Submit(const std::string& baseUrl,
                                 const std::string& vendorName,
                                 const std::string& vendorModelType,
                                 const std::string& userName) {
    nlohmann::json body;
    body["vendor_name"] = vendorName;
    body["vendor_model_type"] = vendorModelType;
    body["user_name"] = userName;

    int rc = 0;
    CurlManager::HTTPSPost(baseUrl + "/xlights/pairs", body.dump(), "", "", "application/json", 10, {}, &rc);
    return rc >= 200 && rc < 300;
}
