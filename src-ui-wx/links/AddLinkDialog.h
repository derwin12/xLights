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
#include <wx/radiobox.h>
#include <wx/sizer.h>
#include <wx/stattext.h>
#include <wx/textctrl.h>

#include <string>

class AddLinkDialog : public wxDialog {
public:
    AddLinkDialog(wxWindow* parent);
    virtual ~AddLinkDialog() = default;

    std::string GetLinkLabel() const;
    std::string GetTarget()    const;
    bool        IsWeb()        const;

private:
    static const long ID_LABEL_TEXT;
    static const long ID_TARGET_TEXT;
    static const long ID_TYPE_RADIO;
    static const long ID_BROWSE_BTN;

    wxTextCtrl* _labelCtrl  = nullptr;
    wxTextCtrl* _targetCtrl = nullptr;
    wxRadioBox* _typeRadio  = nullptr;
    wxButton*   _browseBtn  = nullptr;

    void OnTypeChanged(wxCommandEvent& event);
    void OnBrowseClick(wxCommandEvent& event);
    void OnOK(wxCommandEvent& event);
};
