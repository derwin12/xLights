/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include <wx/progdlg.h>
#include <wx/msgdlg.h>
#include <wx/filefn.h>
#include <wx/regex.h>

#include "xLightsImportChannelMapDialog.h"
#include "xLightsMain.h"
#include "utils/string_utils.h"
#include "shared/utils/wxUtilities.h"
#include "ai/aiBase.h"
#include "ai/aiType.h"

#include <fstream>
#include <set>
#include <string>
#include <nlohmann/json.hpp>

#include <log.h>

std::string xLightsImportChannelMapDialog::GetAIPrompt(const std::string& promptFile) {


    std::string showFolderPromptFile = xlights->GetShowDirectory() + "/" + promptFile;
    std::string xlFolder = GetResourcesDirectory();
    std::string xLightsFolderPromptFile = xlFolder + "/prompts/" + promptFile;

    std::string fileToLoad;
    if (wxFileExists(showFolderPromptFile)) {
        spdlog::debug("Using prompt file from show folder: {}", showFolderPromptFile.c_str());
        fileToLoad = showFolderPromptFile;
    } else if (wxFileExists(xLightsFolderPromptFile)) {
        spdlog::debug("Using prompt file from xLights folder: {}", xLightsFolderPromptFile.c_str());
        fileToLoad = xLightsFolderPromptFile;
    } else {
        // This looks for a prompt without the aiEngine prefix
        std::string pf = AfterFirst(promptFile, '_');

        showFolderPromptFile = xlights->GetShowDirectory() + "/" + pf;
        xLightsFolderPromptFile = xlFolder + "/prompts/" + pf;

        if (wxFileExists(showFolderPromptFile)) {
            spdlog::debug("Using prompt file from show folder: {}", showFolderPromptFile.c_str());
            fileToLoad = showFolderPromptFile;
        } else if (wxFileExists(xLightsFolderPromptFile)) {
            spdlog::debug("Using prompt file from xLights folder: {}", xLightsFolderPromptFile.c_str());
            fileToLoad = xLightsFolderPromptFile;
        } else {
            spdlog::error("Prompt file not found: {}", promptFile.c_str());
            wxMessageBox("The prompt file could not be found " + promptFile, "Error", wxICON_ERROR | wxOK, this);
            return "";
        }
    }

    // read the prompt from the ./prompts/AIAutoMap.txt file relative to where this executable is
    std::string instructions;
    std::ifstream file(fileToLoad);
    if (file.is_open()) {
        std::string line;
        while (std::getline(file, line)) {
            if (line != "" && line[0] == '#') {
                // we skip over lines with a # as the first character so we can add comments into prompt files
            } else {
                instructions += line + "\\n";
            }
        }
        file.close();
    }

    if (instructions == "") {
        wxMessageBox("The prompt file contained no prompt. " + fileToLoad, "Error", wxICON_ERROR | wxOK, this);
    }

    return instructions;
}

std::string xLightsImportChannelMapDialog::BuildSourceModelPrompt(const std::list<ImportChannel*>& sourceModels, std::function<bool(const ImportChannel*)> filter) {


    std::string sourceDescription = "<sourceModels>\\n";
    for (const auto& it : sourceModels) {
        if (filter(it)) {
            std::string type = it->type;
            if (type == "") {
                if (it->isNode)
                    type = "pixel";
                else
                    type = "strand";
            }

            std::string groupModels = "";
            if (type == "ModelGroup") {
                groupModels = " groupModels='" + it->groupModels + "'";
            }

            sourceDescription += "  <model name='" + it->name + "' type='" + type + "'" + groupModels + " class='" + it->modelClass + "' effectCount='" + std::to_string(it->effectCount) + "'/>\\n";
        }
    }
    sourceDescription += "</sourceModels>";
    spdlog::debug("Source models: {}", sourceDescription.c_str());

    return sourceDescription;
}

std::string xLightsImportChannelMapDialog::BuildTargetModelPrompt(const std::list<xLightsImportModelNode*>& targetModels, std::function<bool(const xLightsImportModelNode*)> filter) {


    std::string targetDescription = "<targetModels>\\n";
    for (const auto& it : targetModels) {
        if (filter(it)) {
            std::string name = it->GetModelName();
            std::string type = it->_modelType;
            if (it->_strand != "") {
                if (it->_isSubmodel) {
                    type = "SubModel";
                } else {
                    type = "strand";
                }
            }
            if (it->_node != "") {
                type = "pixel";
            }
            std::string groupModels = "";
            if (it->_groupModels != "") {
                groupModels = " groupModels='" + it->_groupModels + "'";
            }
            targetDescription += "  <model name='" + name + "' type='" + type + "' class='" + it->_modelClass + "'" + groupModels + "/>\\n";
        }
    }
    targetDescription += "</targetModels>";
    spdlog::debug("Target models: {}", targetDescription.c_str());

    return targetDescription;
}

std::string xLightsImportChannelMapDialog::BuildAlreadyMappedPrompt(const std::list<xLightsImportModelNode*>& targetModels, std::function<bool(const xLightsImportModelNode*)> filter) {


    std::string exampleMappings = "<exampleMappings>\\n";
    for (const auto& it : targetModels) {
        if (filter(it)) {
            std::string name = it->GetModelName();
            std::string type = it->_modelType;
            if (it->IsStrand())
                type = "strand";
            else if (it->IsNode())
                type = "pixel";
            else if (it->IsSubModel())
                type = "SubModel";
            exampleMappings += "  <model name='" + name + "' mappedTo='" + it->_mapping + "'/>\\n";
        }
    }
    exampleMappings += "</exampleMappings>";
    spdlog::debug("Example mappings: {}", exampleMappings.c_str());

    return exampleMappings;
}

bool xLightsImportChannelMapDialog::RunAIPrompt(wxProgressDialog* dlg, const std::string& prompt, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {


    spdlog::debug("Prompt: {}", prompt.c_str());

    auto ai = xlights->GetAIService();
    if (ai == nullptr)
        return false;

   auto const[ response, worked] = ai->CallLLM(prompt);
    if (!worked) {
        return false;
    }

    std::string possibleSources = "";
    for (const auto& it : sourceModels) {
        possibleSources += it->name + ", ";
    }

    spdlog::debug("Response: {}", response.c_str());

    bool mapped = false;

    try {
        nlohmann::json root = nlohmann::json::parse(response);

        spdlog::debug("Parsed response");

        nlohmann::json mappings = root["mappings"];
        if (mappings.is_null()) {
            spdlog::error("No mappings found in response");
        } else {
            // now go through all the targets
            for (const auto& it : targetModels) {
                if (!it->HasMapping()) {
                    auto mn = it->GetModelName();
                    // find the model in the mappings sourceModel
                    for (size_t i = 0; i < mappings.size(); ++i) {
                        std::string targetModel = mappings[i]["targetModel"].get<std::string>();
                        std::string mappingSource = mappings[i]["sourceModel"].get<std::string>();
                        if (targetModel == mn && possibleSources.find(mappingSource) != std::string::npos) {
                            it->Map(mappingSource,"Unknown");
                            mapped = true;
                            break;
                        }
                    }
                }
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Error parsing response: {}", e.what());
    }

    return mapped;
}

bool xLightsImportChannelMapDialog::DoStructuredAIMapping(const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {


    auto ai = xlights->GetAIService(aiType::TYPE::MAPPING);
    if (ai == nullptr)
        return false;

    // Build source model info
    std::vector<aiBase::MappingModelInfo> sourceInfo;
    for (const auto& m : sourceModels) {
        aiBase::MappingModelInfo info;
        info.name = m->name;
        info.type = m->type;
        info.modelClass = m->modelClass;
        info.effectCount = m->effectCount;
        info.groupModels = m->groupModels;
        info.isSubModel = m->IsSubModel();
        info.isStrand = m->IsStrand();
        info.isNode = m->IsNode();
        info.nodeCount = m->nodeCount;
        info.strandCount = m->strandCount;
        info.width = m->width;
        info.height = m->height;
        info.subModelNames = m->subModelNames;
        info.aliases = m->aliases;
        sourceInfo.push_back(info);
    }

    // Build target model info and collect existing mappings
    std::vector<aiBase::MappingModelInfo> targetInfo;
    std::map<std::string, std::string> existingMappings;
    for (const auto& m : targetModels) {
        std::string name = m->GetModelName();
        if (m->HasMapping() && !m->_mapping.empty()) {
            existingMappings[name] = m->_mapping;
            continue;
        }

        aiBase::MappingModelInfo info;
        info.name = name;
        info.type = m->_modelType;
        if (m->IsSubModel()) {
            info.type = "SubModel";
            info.isSubModel = true;
        } else if (m->IsStrand()) {
            info.type = "strand";
            info.isStrand = true;
        } else if (m->IsNode()) {
            info.type = "pixel";
            info.isNode = true;
        }
        info.modelClass = m->_modelClass;
        info.groupModels = m->_groupModels;
        info.nodeCount = m->_nodeCount;
        info.strandCount = m->_strandCount;
        info.width = m->_width;
        info.height = m->_height;
        info.aliases.assign(m->_aliases.begin(), m->_aliases.end());
        // Collect submodel names from children
        for (unsigned int c = 0; c < m->GetChildCount(); ++c) {
            auto child = m->GetNthChild(c);
            if (child != nullptr && child->_isSubmodel) {
                info.subModelNames.push_back(child->_strand);
            }
        }
        targetInfo.push_back(info);
    }

    if (targetInfo.empty()) {
        spdlog::debug("No unmapped targets for structured AI mapping");
        return false;
    }

    spdlog::debug("Structured AI mapping: {} sources, {} targets, {} existing",
                       sourceInfo.size(), targetInfo.size(), existingMappings.size());

    auto result = ai->GenerateModelMapping(sourceInfo, targetInfo, existingMappings);

    if (!result.error.empty()) {
        spdlog::error("Structured AI mapping error: {}", result.error.c_str());
        wxMessageBox(result.error, "AI Mapping Error", wxICON_ERROR);
        return false;
    }

    if (result.mappings.empty()) {
        spdlog::debug("Structured AI mapping returned no mappings");
        return false;
    }

    // Build a quick lookup for source model validation
    std::set<std::string> validSources;
    for (const auto& m : sourceModels) {
        validSources.insert(m->name);
    }

    // Apply the mappings
    bool mapped = false;
    size_t appliedCount = 0;
    for (const auto& target : targetModels) {
        if (target->HasMapping())
            continue;

        std::string targetName = target->GetModelName();
        auto it = result.mappings.find(targetName);
        if (it != result.mappings.end() && validSources.count(it->second) > 0) {
            target->Map(it->second, findModelType(it->second));
            mapped = true;
            ++appliedCount;
        }
    }

    spdlog::debug("Structured AI mapping: applied {} of {} returned mappings",
                       appliedCount, result.mappings.size());

    return mapped;
}

bool xLightsImportChannelMapDialog::AIModelMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {
    // we only model map if there are models in target
    if (targetModels.size() == 0)
        return false;

    auto llm = xlights->GetAIService();
    if (llm == nullptr)
        return false;
    std::string prompt = GetAIPrompt(llm->GetLLMName() + "_AI_Model_AutoMap.txt");

    // exclude pixels and strands
    std::string sourceModelsPrompt = BuildSourceModelPrompt(sourceModels, [](const ImportChannel* m) { return !m->IsNode() && !m->IsStrand(); });
    // exlude already mapped models and Submodels, strands and pixels
    std::string targetModelsPrompt = BuildTargetModelPrompt(targetModels, [](const xLightsImportModelNode* m) { return !m->IsMapped() && !m->IsSubModel() && !m->IsStrand() && !m->IsNode(); });
    // include already mapped models and exclude Submodels, strands and pixels
    std::string altreadyMappedPrompt = BuildAlreadyMappedPrompt(targetModels, [](const xLightsImportModelNode* m) { return m->IsMapped() && !m->IsSubModel() && !m->IsStrand() && m->IsNode(); });

    if (prompt.find("{sourcemodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{sourcemodels}"), 14, sourceModelsPrompt);
    if (prompt.find("{targetmodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{targetmodels}"), 14, targetModelsPrompt);
    if (prompt.find("{examplemapping}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{examplemapping}"), 16, altreadyMappedPrompt);

    bool res = RunAIPrompt(dlg, prompt, sourceModels, targetModels);
    dlg->Update(25, "Models mapped");

    return res;
}

bool xLightsImportChannelMapDialog::AISubModelMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {
    // we only submodel map if there are submodels in target
    int submodelCount = 0;
    for (const auto& it : targetModels) {
        if (it->IsSubModel()) {
            ++submodelCount;
            break;
        }
    }
    if (submodelCount == 0)
        return false;

    auto llm = xlights->GetAIService();
    if (llm == nullptr)
        return false;
    std::string prompt = GetAIPrompt(llm->GetLLMName() + "_AI_SubModel_AutoMap.txt");

    // exclude pixels and strands
    std::string sourceModelsPrompt = BuildSourceModelPrompt(sourceModels, [](const ImportChannel* m) { return !m->IsNode() && !m->IsStrand(); });
    // exlude already mapped submodels and only include submodels
    std::string targetModelsPrompt = BuildTargetModelPrompt(targetModels, [](const xLightsImportModelNode* m) { return !m->_mappingExists && m->IsSubModel(); });
    // include already mapped sub models only
    std::string altreadyMappedPrompt = BuildAlreadyMappedPrompt(targetModels, [](const xLightsImportModelNode* m) { return m->_mappingExists && m->IsSubModel(); });

    if (prompt.find("{sourcemodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{sourcemodels}"), 14, sourceModelsPrompt);
    if (prompt.find("{targetmodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{targetmodels}"), 14, targetModelsPrompt);
    if (prompt.find("{examplemapping}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{examplemapping}"), 16, altreadyMappedPrompt);

    bool res = RunAIPrompt(dlg, prompt, sourceModels, targetModels);
    dlg->Update(50, "SubModels mapped");

    return res;
}

bool xLightsImportChannelMapDialog::AIStrandMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {
    // we only strand map if there source models which are strands
    int strandCount = 0;
    for (const auto& it : sourceModels) {
        if (it->IsStrand()) {
            ++strandCount;
            break;
        }
    }
    if (strandCount == 0)
        return false;

    auto llm = xlights->GetAIService();
    if (llm == nullptr)
		return false;
    std::string prompt = GetAIPrompt(llm->GetLLMName() + "_AI_Strand_AutoMap.txt");

    // only include strands
    std::string sourceModelsPrompt = BuildSourceModelPrompt(sourceModels, [](const ImportChannel* m) { return m->IsStrand(); });
    // only include strands
    std::string targetModelsPrompt = BuildTargetModelPrompt(targetModels, [](const xLightsImportModelNode* m) { return !m->IsMapped() && m->IsStrand(); });
    // include already mapped strands only
    std::string altreadyMappedPrompt = BuildAlreadyMappedPrompt(targetModels, [](const xLightsImportModelNode* m) { return m->IsMapped() && m->IsStrand(); });

    if (prompt.find("{sourcemodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{sourcemodels}"), 14, sourceModelsPrompt);
    if (prompt.find("{targetmodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{targetmodels}"), 14, targetModelsPrompt);
    if (prompt.find("{examplemapping}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{examplemapping}"), 16, altreadyMappedPrompt);

    bool res = RunAIPrompt(dlg, prompt, sourceModels, targetModels);
    dlg->Update(75, "Strands mapped");

    return res;
}

#define AI_NODE_COUNT_LIMIT 16

bool xLightsImportChannelMapDialog::AINodeMap(wxProgressDialog* dlg, const std::list<ImportChannel*>& sourceModels, const std::list<xLightsImportModelNode*>& targetModels) {
    // we only node map if there are > 0 models in target with < 16 nodes and there is some node level sequencing
    int nodeModelCount = 0;
    for (const auto& it : sourceModels) {
        if (it->IsNode()) {
            ++nodeModelCount;
            break;
        }
    }
    if (nodeModelCount == 0) {
        for (const auto& it : targetModels) {
            if (it->_nodeCount < AI_NODE_COUNT_LIMIT) {
                ++nodeModelCount;
                break;
            }
        }
    }
    if (nodeModelCount == 0)
        return false;

    auto llm = xlights->GetAIService();
    if (llm == nullptr)
        return false;
    std::string prompt = GetAIPrompt(llm->GetLLMName() + "_AI_Node_AutoMap.txt");

    // include all node level sequencing
    std::string sourceModelsPrompt = BuildSourceModelPrompt(sourceModels, [](const ImportChannel* m) { return m->IsNode(); });
    // only include nodes on models with less than AI_NODE_COUNT_LIMIT nodes
    std::string targetModelsPrompt = BuildTargetModelPrompt(targetModels, [](const xLightsImportModelNode* m) { return !m->IsMapped() && m->IsNode() && m->_nodeCount < AI_NODE_COUNT_LIMIT; });
    // include already mapped nodes
    std::string altreadyMappedPrompt = BuildAlreadyMappedPrompt(targetModels, [](const xLightsImportModelNode* m) { return m->IsMapped() && m->IsNode(); });

    if (prompt.find("{sourcemodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{sourcemodels}"), 14, sourceModelsPrompt);
    if (prompt.find("{targetmodels}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{targetmodels}"), 14, targetModelsPrompt);
    if (prompt.find("{examplemapping}") != std::string::npos)
        prompt = prompt.replace(prompt.find("{examplemapping}"), 16, altreadyMappedPrompt);

    bool res = RunAIPrompt(dlg, prompt, sourceModels, targetModels);
    dlg->Update(100, "Nodes mapped");

    return res;
}

void xLightsImportChannelMapDialog::DoAIAutoMap(bool select) {
    // Ideas for future improvement

    // - We could try to represent the location of the models against a normalised coordinate system to encourage left/right etc to map better
    // - We could build some sort of asset with special hints for really common custom models
    // - We could include aliases in the information as further hints
    // - We could drop the models list in model groups to save space as I dont think it helps that much
    // - we could include strand and node names where they exist (although that may already be there ... i have not checked)

    // I welcome other developers experimenting with the prompt and trying to improve it.

    bool selectMapAvail = (ListCtrl_Available->GetSelectedItemCount() != 0) && select;
    bool selectMapTarget = (TreeListCtrl_Mapping->GetSelectedItemsCount() != 0) && select;

    // build a list of possible sources .. this is the selected items in the list or all items
    bool sourceContainsNodes = false;
    std::list<ImportChannel*> sourceModels;
    for (int j = 0; j < ListCtrl_Available->GetItemCount(); ++j) {
        ImportChannel* m = (ImportChannel*)ListCtrl_Available->GetItemData(j);
        if (selectMapAvail) {
            bool isSourceSelected = ListCtrl_Available->GetItemState(j, wxLIST_STATE_SELECTED) == wxLIST_STATE_SELECTED;
            if (isSourceSelected) {
                sourceModels.push_back(m);
                sourceContainsNodes |= m->isNode;
            }
        } else {
            sourceModels.push_back(m);
            sourceContainsNodes |= m->isNode;
        }
    }

    std::list<xLightsImportModelNode*> targetModels;

    // build a list of possible targets .. this is the selected items in the tree or all items but only if they are not already mapped
    wxDataViewItemArray targetSelectedItems;
    TreeListCtrl_Mapping->GetSelections(targetSelectedItems);
    for (unsigned int i = 0; i < _dataModel->GetChildCount(); ++i) {
        auto model = _dataModel->GetNthChild(i);
        if (model != nullptr) {
            if (selectMapTarget) {
                auto index = (wxDataViewItem)model;
                for (const wxDataViewItem& selectedItem : targetSelectedItems) {
                    if (index.GetID() == selectedItem.GetID()) {
                        targetModels.push_back(model);
                        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
                            auto strand = model->GetNthChild(k);
                            if (strand != nullptr) {
                                targetModels.push_back(strand);
                                // we only add in nodes if the source sequence contains node level effects
                                if (sourceContainsNodes) {
                                    for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                                        auto node = strand->GetNthChild(m);
                                        if (node != nullptr) {
                                            targetModels.push_back(node);
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                targetModels.push_back(model);
                for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
                    auto strand = model->GetNthChild(k);
                    if (strand != nullptr) {
                        targetModels.push_back(strand);
                        // we only add in nodes if the source sequence contains node level effects
                        if (sourceContainsNodes) {
                            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                                auto node = strand->GetNthChild(m);
                                if (node != nullptr) {
                                    targetModels.push_back(node);
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    wxProgressDialog* dlg = new wxProgressDialog("Generating mapping", "Please give me some time to map your models. This can take a minute or two.", 100, this, wxPD_APP_MODAL | wxPD_SMOOTH | wxPD_AUTO_HIDE);
    dlg->Show();

    // Try structured AI mapping first (e.g., Claude with GenerateModelMapping)
    auto mappingService = xlights->GetAIService(aiType::TYPE::MAPPING);
    bool mapped = false;
    if (mappingService != nullptr) {
        dlg->Update(10, "Using structured AI mapping...");
        mapped = DoStructuredAIMapping(sourceModels, targetModels);
        TreeListCtrl_Mapping->Refresh();
        dlg->Update(50, "Structured mapping complete");
    }

    // Fall back to prompt-based mapping for any remaining unmapped models
    if (xlights->GetAIService() != nullptr) {
        mapped = AIModelMap(dlg, sourceModels, targetModels) || mapped;
        TreeListCtrl_Mapping->Refresh();
        mapped = AISubModelMap(dlg, sourceModels, targetModels) || mapped;
        TreeListCtrl_Mapping->Refresh();
        mapped = AIStrandMap(dlg, sourceModels, targetModels) || mapped;
        TreeListCtrl_Mapping->Refresh();
        mapped = AINodeMap(dlg, sourceModels, targetModels) || mapped;
    }

    delete dlg;

    if (!mapped) {
        wxMessageBox("Unable to generate mappings. Check log file for details.", "Mapping Failed", 5);
    }
}
