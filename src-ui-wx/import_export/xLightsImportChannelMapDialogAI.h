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

// Declarations for the AI-driven model mapping helpers used by
// xLightsImportChannelMapDialog. Implemented in
// xLightsImportChannelMapDialogAI.cpp.
//
// This file is included directly into the body of the
// xLightsImportChannelMapDialog class declaration in
// xLightsImportChannelMapDialog.h, so the methods declared here are private
// members of that class.

bool AIModelMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
bool AISubModelMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
bool AIStrandMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
bool AINodeMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
std::string GetAIPrompt(const std::string& promptType);
std::string BuildSourceModelPrompt(const std::list<ImportChannel*>& sourceModels, std::function<bool(const ImportChannel*)> filter);
std::string BuildTargetModelPrompt(const std::list<xLightsImportModelNode*>& targetModels, std::function<bool(const xLightsImportModelNode*)> filter);
std::string BuildAlreadyMappedPrompt(const std::list<xLightsImportModelNode*>& targetModels, std::function<bool(const xLightsImportModelNode*)> filter);
bool RunAIPrompt(wxProgressDialog* dlg, const std::string& prompt, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
bool DoStructuredAIMapping(const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels);
void DoAIAutoMap(bool select);
