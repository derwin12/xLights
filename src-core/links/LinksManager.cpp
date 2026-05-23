/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "links/LinksManager.h"

#include "settings/XLightsSettings.h"

static const std::string LINKS_SECTION = "links";
static const std::string LINKS_KEY = "entries";

LinksManager& LinksManager::Get()
{
    static LinksManager inst;
    return inst;
}

void LinksManager::Load()
{
    _links.clear();
    auto arr = XLightsSettings::Get().ReadJsonArray(LINKS_KEY, LINKS_SECTION);
    for (const auto& item : arr) {
        if (!item.is_object()) continue;
        LinkEntry e;
        e.label  = item.value("label",  "");
        e.target = item.value("target", "");
        e.isWeb  = item.value("isWeb",  true);
        if (!e.label.empty() && !e.target.empty())
            _links.push_back(std::move(e));
    }
}

void LinksManager::Save()
{
    auto arr = nlohmann::json::array();
    for (const auto& e : _links)
        arr.push_back({{"label", e.label}, {"target", e.target}, {"isWeb", e.isWeb}});
    XLightsSettings::Get().WriteJsonArray(LINKS_KEY, arr, LINKS_SECTION);
    XLightsSettings::Get().Flush();
}

void LinksManager::AddLink(const LinkEntry& entry)
{
    _links.push_back(entry);
    Save();
}

void LinksManager::RemoveLink(size_t index)
{
    if (index < _links.size()) {
        _links.erase(_links.begin() + static_cast<ptrdiff_t>(index));
        Save();
    }
}
