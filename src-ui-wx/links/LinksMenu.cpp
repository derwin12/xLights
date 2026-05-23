/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "xLightsMain.h"

#include "links/LinksManager.h"
#include "links/ManageLinksDialog.h"

#include <wx/utils.h>

// ---------------------------------------------------------------------------
// PopulateLinksMenu
// Layout:
//   [link 1]
//   [link 2]
//   ...
//   ─────────────  (only when links exist)
//   Manage Links...
// ---------------------------------------------------------------------------
void xLightsFrame::PopulateLinksMenu()
{
    for (auto* item : _linkMenuItems)
        Disconnect(item->GetId(), wxEVT_COMMAND_MENU_SELECTED,
                   (wxObjectEventFunction)&xLightsFrame::OnLinkItemClick);
    _linkMenuItems.clear();

    while (LinksMenu->GetMenuItemCount() > 0)
        LinksMenu->Delete(LinksMenu->FindItemByPosition(0));

    const auto& links = LinksManager::Get().GetLinks();

    for (size_t i = 0; i < links.size(); ++i) {
        int id = wxNewId();
        auto* item = new wxMenuItem(LinksMenu, id,
                                    wxString::FromUTF8(links[i].label));
        Connect(id, wxEVT_COMMAND_MENU_SELECTED,
                (wxObjectEventFunction)&xLightsFrame::OnLinkItemClick);
        LinksMenu->Append(item);
        _linkMenuItems.push_back(item);
    }

    if (!links.empty())
        LinksMenu->AppendSeparator();

    LinksMenu->Append(new wxMenuItem(LinksMenu, ID_MNU_LINKS_MANAGE,
                                     _("Manage Links..."), wxEmptyString, wxITEM_NORMAL));
}

// ---------------------------------------------------------------------------
// OnManageLinksClick  — opens the Manage Links dialog
// ---------------------------------------------------------------------------
void xLightsFrame::OnManageLinksClick(wxCommandEvent& /*event*/)
{
    ManageLinksDialog dlg(this);
    dlg.ShowModal();
    PopulateLinksMenu();
}

// ---------------------------------------------------------------------------
// OnLinkItemClick  — opens the link URL or launches the executable
// ---------------------------------------------------------------------------
void xLightsFrame::OnLinkItemClick(wxCommandEvent& event)
{
    int id = event.GetId();
    const auto& links = LinksManager::Get().GetLinks();
    for (size_t i = 0; i < _linkMenuItems.size(); ++i) {
        if (_linkMenuItems[i]->GetId() == id) {
            if (i >= links.size()) return;
            if (links[i].isWeb)
                wxLaunchDefaultBrowser(wxString::FromUTF8(links[i].target));
            else
                wxExecute(wxString::FromUTF8(links[i].target), wxEXEC_ASYNC);
            return;
        }
    }
}
