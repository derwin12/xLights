/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "links/ManageLinksDialog.h"

#include "links/AddLinkDialog.h"
#include "links/LinksManager.h"

#include <wx/intl.h>
#include <wx/string.h>
#include <wx/utils.h>

const long ManageLinksDialog::ID_LIST       = wxNewId();
const long ManageLinksDialog::ID_BTN_ADD    = wxNewId();
const long ManageLinksDialog::ID_BTN_REMOVE = wxNewId();

ManageLinksDialog::ManageLinksDialog(wxWindow* parent)
{
    Create(parent, wxID_ANY, _("Manage Links"), wxDefaultPosition, wxDefaultSize,
           wxCAPTION | wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, _T("wxID_ANY"));

    auto* outer = new wxBoxSizer(wxVERTICAL);

    // List
    _list = new wxListCtrl(this, ID_LIST, wxDefaultPosition, wxSize(520, 240),
                           wxLC_REPORT | wxLC_SINGLE_SEL | wxBORDER_SUNKEN);
    _list->InsertColumn(0, _("Label"),  wxLIST_FORMAT_LEFT, 160);
    _list->InsertColumn(1, _("Type"),   wxLIST_FORMAT_LEFT,  90);
    _list->InsertColumn(2, _("Target"), wxLIST_FORMAT_LEFT, 260);
    outer->Add(_list, 1, wxEXPAND | wxALL, 8);

    // Button row
    auto* btnRow = new wxBoxSizer(wxHORIZONTAL);
    _addBtn    = new wxButton(this, ID_BTN_ADD,    _("Add..."));
    _removeBtn = new wxButton(this, ID_BTN_REMOVE, _("Remove"));
    _removeBtn->Enable(false);
    btnRow->Add(_addBtn,    0, wxRIGHT, 6);
    btnRow->Add(_removeBtn, 0);
    btnRow->AddStretchSpacer();
    btnRow->Add(new wxButton(this, wxID_CLOSE, _("Close")), 0);
    outer->Add(btnRow, 0, wxEXPAND | wxLEFT | wxRIGHT | wxBOTTOM, 8);

    SetSizer(outer);
    outer->Fit(this);
    SetMinSize(wxSize(400, 200));
    Centre();

    RefreshList();

    _addBtn->Bind(wxEVT_BUTTON, &ManageLinksDialog::OnAddClick,    this);
    _removeBtn->Bind(wxEVT_BUTTON, &ManageLinksDialog::OnRemoveClick, this);
    _list->Bind(wxEVT_LIST_ITEM_SELECTED,   &ManageLinksDialog::OnListSelect,    this);
    _list->Bind(wxEVT_LIST_ITEM_DESELECTED, &ManageLinksDialog::OnListDeselect,  this);
    _list->Bind(wxEVT_LIST_ITEM_ACTIVATED,  &ManageLinksDialog::OnListActivated, this);
    Bind(wxEVT_BUTTON, [this](wxCommandEvent&) { EndModal(wxID_CLOSE); }, wxID_CLOSE);
}

void ManageLinksDialog::RefreshList()
{
    _list->DeleteAllItems();
    const auto& links = LinksManager::Get().GetLinks();
    for (long i = 0; i < static_cast<long>(links.size()); ++i) {
        _list->InsertItem(i, wxString::FromUTF8(links[i].label));
        _list->SetItem(i, 1, links[i].isWeb ? _("Web") : _("Executable"));
        _list->SetItem(i, 2, wxString::FromUTF8(links[i].target));
    }
    UpdateButtons();
}

void ManageLinksDialog::UpdateButtons()
{
    _removeBtn->Enable(_list->GetSelectedItemCount() > 0);
}

void ManageLinksDialog::OnAddClick(wxCommandEvent& /*event*/)
{
    AddLinkDialog dlg(this);
    if (dlg.ShowModal() != wxID_OK)
        return;

    LinkEntry entry;
    entry.label  = dlg.GetLinkLabel();
    entry.target = dlg.GetTarget();
    entry.isWeb  = dlg.IsWeb();
    LinksManager::Get().AddLink(entry);
    RefreshList();
}

void ManageLinksDialog::OnRemoveClick(wxCommandEvent& /*event*/)
{
    long idx = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx < 0) return;
    LinksManager::Get().RemoveLink(static_cast<size_t>(idx));
    RefreshList();
}

void ManageLinksDialog::OnListSelect(wxListEvent& /*event*/)
{
    UpdateButtons();
}

void ManageLinksDialog::OnListDeselect(wxListEvent& /*event*/)
{
    UpdateButtons();
}

void ManageLinksDialog::OnListActivated(wxListEvent& /*event*/)
{
    long idx = _list->GetNextItem(-1, wxLIST_NEXT_ALL, wxLIST_STATE_SELECTED);
    if (idx < 0) return;
    const auto& links = LinksManager::Get().GetLinks();
    if (static_cast<size_t>(idx) >= links.size()) return;
    if (links[idx].isWeb)
        wxLaunchDefaultBrowser(wxString::FromUTF8(links[idx].target));
    else
        wxExecute(wxString::FromUTF8(links[idx].target), wxEXEC_ASYNC);
}
