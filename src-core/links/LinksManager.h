/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#pragma once

#include <string>
#include <vector>

struct LinkEntry {
    std::string label;
    std::string target;
    bool isWeb = true;
};

class LinksManager {
public:
    static LinksManager& Get();

    void Load();
    void Save();

    const std::vector<LinkEntry>& GetLinks() const { return _links; }
    void AddLink(const LinkEntry& entry);
    void RemoveLink(size_t index);

private:
    LinksManager() = default;
    LinksManager(const LinksManager&) = delete;
    LinksManager& operator=(const LinksManager&) = delete;

    std::vector<LinkEntry> _links;
};
