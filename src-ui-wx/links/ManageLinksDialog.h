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

#include <wx/button.h>
#include <wx/dialog.h>
#include <wx/listctrl.h>
#include <wx/sizer.h>

class ManageLinksDialog : public wxDialog {
public:
    ManageLinksDialog(wxWindow* parent);
    virtual ~ManageLinksDialog() = default;

private:
    static const long ID_LIST;
    static const long ID_BTN_ADD;
    static const long ID_BTN_REMOVE;

    wxListCtrl* _list    = nullptr;
    wxButton*   _addBtn  = nullptr;
    wxButton*   _removeBtn = nullptr;

    void RefreshList();
    void UpdateButtons();

    void OnAddClick(wxCommandEvent& event);
    void OnRemoveClick(wxCommandEvent& event);
    void OnListSelect(wxListEvent& event);
    void OnListDeselect(wxListEvent& event);
    void OnListActivated(wxListEvent& event);
};
