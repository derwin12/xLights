/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "links/AddLinkDialog.h"

#include <wx/filedlg.h>
#include <wx/intl.h>
#include <wx/msgdlg.h>
#include <wx/string.h>

const long AddLinkDialog::ID_LABEL_TEXT  = wxNewId();
const long AddLinkDialog::ID_TARGET_TEXT = wxNewId();
const long AddLinkDialog::ID_TYPE_RADIO  = wxNewId();
const long AddLinkDialog::ID_BROWSE_BTN  = wxNewId();

AddLinkDialog::AddLinkDialog(wxWindow* parent)
{
    Create(parent, wxID_ANY, _("Add New Link"), wxDefaultPosition, wxDefaultSize,
           wxCAPTION | wxDEFAULT_DIALOG_STYLE | wxRESIZE_BORDER, _T("wxID_ANY"));

    auto* outer = new wxFlexGridSizer(0, 1, 5, 5);
    outer->AddGrowableCol(0);

    // Label row
    auto* labelRow = new wxFlexGridSizer(1, 2, 5, 5);
    labelRow->AddGrowableCol(1);
    labelRow->Add(new wxStaticText(this, wxID_ANY, _("Label:")),
                  0, wxALIGN_CENTER_VERTICAL);
    _labelCtrl = new wxTextCtrl(this, ID_LABEL_TEXT, wxEmptyString,
                                wxDefaultPosition, wxSize(320, -1));
    labelRow->Add(_labelCtrl, 1, wxEXPAND);
    outer->Add(labelRow, 1, wxEXPAND | wxALL, 5);

    // Type radio
    wxString types[] = { _("Web URL"), _("Executable") };
    _typeRadio = new wxRadioBox(this, ID_TYPE_RADIO, _("Type"), wxDefaultPosition,
                                wxDefaultSize, 2, types, 2, wxRA_SPECIFY_COLS);
    outer->Add(_typeRadio, 0, wxEXPAND | wxALL, 5);

    // Target row
    auto* targetRow = new wxFlexGridSizer(1, 3, 5, 5);
    targetRow->AddGrowableCol(1);
    targetRow->Add(new wxStaticText(this, wxID_ANY, _("Target:")),
                   0, wxALIGN_CENTER_VERTICAL);
    _targetCtrl = new wxTextCtrl(this, ID_TARGET_TEXT, wxEmptyString,
                                 wxDefaultPosition, wxSize(320, -1));
    targetRow->Add(_targetCtrl, 1, wxEXPAND);
    _browseBtn = new wxButton(this, ID_BROWSE_BTN, _("Browse..."),
                              wxDefaultPosition, wxDefaultSize);
    _browseBtn->Enable(false);
    targetRow->Add(_browseBtn, 0);
    outer->Add(targetRow, 1, wxEXPAND | wxALL, 5);

    // Standard OK / Cancel buttons
    auto* btnSizer = CreateStdDialogButtonSizer(wxOK | wxCANCEL);
    outer->Add(btnSizer, 0, wxALIGN_RIGHT | wxALL, 5);

    SetSizer(outer);
    outer->Fit(this);
    SetMinSize(GetSize());
    Centre();

    _typeRadio->Bind(wxEVT_RADIOBOX, &AddLinkDialog::OnTypeChanged, this);
    _browseBtn->Bind(wxEVT_BUTTON,   &AddLinkDialog::OnBrowseClick, this);
    Bind(wxEVT_BUTTON, &AddLinkDialog::OnOK, this, wxID_OK);
}

std::string AddLinkDialog::GetLinkLabel() const
{
    return _labelCtrl->GetValue().ToStdString();
}

std::string AddLinkDialog::GetTarget() const
{
    return _targetCtrl->GetValue().ToStdString();
}

bool AddLinkDialog::IsWeb() const
{
    return _typeRadio->GetSelection() == 0;
}

void AddLinkDialog::OnTypeChanged(wxCommandEvent& /*event*/)
{
    _browseBtn->Enable(_typeRadio->GetSelection() == 1);
}

void AddLinkDialog::OnBrowseClick(wxCommandEvent& /*event*/)
{
#if defined(_WIN32) || defined(__WXMSW__)
    const wxString filter = _("Executables (*.exe;*.bat;*.cmd)|*.exe;*.bat;*.cmd|All files (*.*)|*.*");
#elif defined(__APPLE__)
    const wxString filter = _("Applications (*.app)|*.app|All files (*.*)|*.*");
#else
    const wxString filter = _("All files (*.*)|*.*");
#endif
    wxFileDialog dlg(this, _("Select Executable"), wxEmptyString, wxEmptyString,
                     filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    if (dlg.ShowModal() == wxID_OK)
        _targetCtrl->SetValue(dlg.GetPath());
}

void AddLinkDialog::OnOK(wxCommandEvent& event)
{
    if (_labelCtrl->GetValue().IsEmpty()) {
        wxMessageBox(_("Please enter a label for the link."), _("Missing Label"),
                     wxICON_WARNING | wxOK, this);
        return;
    }
    if (_targetCtrl->GetValue().IsEmpty()) {
        wxMessageBox(_("Please enter a target URL or path."), _("Missing Target"),
                     wxICON_WARNING | wxOK, this);
        return;
    }
    event.Skip();
}
