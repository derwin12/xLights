/***************************************************************
 * This source files comes from the xLights project
 * https://www.xlights.org
 * https://github.com/xLightsSequencer/xLights
 * See the github commit history for a record of contributing
 * developers.
 * Copyright claimed based on commit dates recorded in Github
 * License: https://github.com/xLightsSequencer/xLights/blob/master/License.txt
 **************************************************************/

#include "AutoMapper.h"

#include "ImportMappingNode.h"
#include "models/Model.h"
#include "models/ModelGroup.h"
#include "render/RenderContext.h"
#include "utils/string_utils.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <regex>
#include <set>
#include <string>
#include <unordered_map>

namespace {

// Strip the same set of separators / punctuation that the desktop dialog's
// AggressiveAutomap removed. Kept identical so behaviour matches the
// existing wx version.
std::string StripPunctuation(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    for (char c : in) {
        switch (c) {
        case ' ': case '-': case '_': case '(': case ')':
        case ':': case ';': case '\\': case '|': case '{': case '}':
        case '[': case ']': case '+': case '=': case '*': case '^':
        case '#': case ',': case '.':
            break;
        default:
            out.push_back(c);
            break;
        }
    }
    return out;
}

std::vector<std::string> SplitSlash(const std::string& s) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == '/') {
            parts.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    parts.push_back(cur);
    return parts;
}

// Shared by the catch-all-style phases: once `model` has been mapped to a
// vendor model, fills any still-unmapped Strand/SubModel and Node children
// from not-yet-used `<mappedVendorModel>/...` available sources of the
// corresponding depth, ignoring names.
void FillMappedModelChildren(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              bool selectMapAvail,
                              std::unordered_set<std::string>& used,
                              const std::string& ruleLabel) {
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

// --- Fuzzy matcher helpers -------------------------------------------------
// Mirrors the tokenization / signature / family-compatibility rules from the
// xLightsMapper Python prototype's STRATEGIES.md, scoped down to a single
// last-resort matcher (MatchFuzzy).

const std::unordered_set<std::string> FUZZY_STOPWORDS = {
    "group", "model", "lights", "light", "the", "and", "all"
};

// Token -> canonical family name. A name can belong to multiple families.
const std::vector<std::pair<std::string, std::unordered_set<std::string>>> FUZZY_FAMILY_KEYWORDS = {
    { "cane", { "cane", "candycane" } },
    { "tree", { "tree", "mega", "flaketree" } },
    { "star", { "star", "flake", "snowflake", "stickstar", "stick" } },
    { "arch", { "arch", "arches" } },
    { "outline", { "outline", "eaves", "garage", "roof", "vertical", "window", "frame" } },
    { "matrix", { "matrix" } },
    { "spinner", { "spinner" } },
    { "bulb", { "bulb", "bigbulb", "c9" } },
    { "flood", { "flood", "floodhouse" } },
    { "wreath", { "wreath" } },
    { "stake", { "stake", "pixstake", "mickeystake" } },
};

// Families that should never be considered a match for each other, even
// when no family overlaps (used as the "incompatible pairs" guardrail).
const std::set<std::pair<std::string, std::string>> FUZZY_INCOMPATIBLE_FAMILIES = {
    { "bulb", "star" }, { "bulb", "flood" }, { "bulb", "tree" }, { "bulb", "arch" },
    { "cane", "outline" }, { "cane", "flood" },
    { "flood", "tree" }, { "flood", "star" },
};

// Lowercase, replace separators with spaces, strip anything that isn't
// alphanumeric/space, then collapse whitespace.
std::string FuzzyNormalize(const std::string& in) {
    std::string out;
    out.reserve(in.size());
    bool lastWasSpace = false;
    for (char c : in) {
        char lc = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lc == ' ' || lc == '_' || lc == '-' || lc == '/') {
            lc = ' ';
        } else if (!((lc >= 'a' && lc <= 'z') || (lc >= '0' && lc <= '9'))) {
            continue;
        }
        if (lc == ' ') {
            if (lastWasSpace || out.empty()) continue;
            lastWasSpace = true;
        } else {
            lastWasSpace = false;
        }
        out.push_back(lc);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    return out;
}

// Singular/plural + domain-synonym normalization for a single token.
std::string FuzzyCanonicalToken(const std::string& token) {
    std::string t = token;
    if (t.size() > 4) {
        if (t.size() > 2 && t.substr(t.size() - 2) == "es" &&
            (t == "canes" || t == "arches" || t == "trees" || t == "stars" || t == "flakes")) {
            t.pop_back();
        } else if (t.back() == 's') {
            t.pop_back();
        }
    }
    static const std::unordered_map<std::string, std::string> synonyms = {
        { "candycane", "cane" }, { "c9", "bulb" }, { "bulbs", "bulb" }, { "floodlights", "flood" },
    };
    auto it = synonyms.find(t);
    return it != synonyms.end() ? it->second : t;
}

// Tokenize a name: normalize, split on spaces, drop stopwords, canonicalize.
std::unordered_set<std::string> FuzzyTokens(const std::string& name) {
    std::unordered_set<std::string> out;
    std::string normalized = FuzzyNormalize(name);
    std::string cur;
    auto flush = [&]() {
        if (!cur.empty() && FUZZY_STOPWORDS.find(cur) == FUZZY_STOPWORDS.end()) {
            out.insert(FuzzyCanonicalToken(cur));
        }
        cur.clear();
    };
    for (char c : normalized) {
        if (c == ' ') flush();
        else cur.push_back(c);
    }
    flush();
    if (normalized.find("candy cane") != std::string::npos || normalized.find("candycane") != std::string::npos) {
        // "candy" is just a qualifier on "cane" here - drop it so e.g.
        // "Group - Candy Canes" reduces to the same token set as "Canes".
        out.erase("candy");
        out.insert("cane");
    }
    return out;
}

// Sequence of digit runs in the (un-normalized) name, e.g. "Arch-12" -> {"12"}.
std::vector<std::string> FuzzyNumericSignature(const std::string& name) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : name) {
        if (c >= '0' && c <= '9') {
            cur.push_back(c);
        } else if (!cur.empty()) {
            out.push_back(cur);
            cur.clear();
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

// Left/right/top/bottom side tags present in the normalized name, in a
// fixed order so two equivalent names produce identical signatures.
std::vector<std::string> FuzzySideSignature(const std::string& name) {
    std::vector<std::string> out;
    auto tokens = FuzzyTokens(name);
    for (const auto& tag : { "left", "right", "upper", "lower", "top", "bottom", "l", "r" }) {
        if (tokens.count(tag)) out.push_back(tag);
    }
    return out;
}

// FuzzyTokens with side tags (left/right/upper/lower/top/bottom/l/r) and
// purely-numeric tokens removed - the "family" tokens that should be shared
// by all members of a same-type group (e.g. "Arches-1-Left" and "Arches-6-R"
// both reduce to {"arch"}).
std::unordered_set<std::string> FuzzyBaseTokens(const std::string& name) {
    static const std::unordered_set<std::string> sideTags = {
        "left", "right", "upper", "lower", "top", "bottom", "l", "r"
    };
    std::unordered_set<std::string> base;
    for (const auto& tok : FuzzyTokens(name)) {
        if (sideTags.count(tok)) continue;
        if (std::all_of(tok.begin(), tok.end(), [](char c) { return c >= '0' && c <= '9'; })) continue;
        base.insert(tok);
    }
    return base;
}

std::unordered_set<std::string> FuzzyModelFamilies(const std::string& name) {
    auto tokens = FuzzyTokens(name);
    std::unordered_set<std::string> families;
    for (const auto& [family, keywords] : FUZZY_FAMILY_KEYWORDS) {
        bool found = false;
        for (const auto& tok : tokens) {
            if (keywords.count(tok)) {
                found = true;
                break;
            }
            // Compound-name fallback: a token like "eflake46", "chromaflake"
            // or "hspinner1" doesn't exactly equal a family keyword, but
            // contains one as a substring (e.g. "flake", "spinner",
            // "snowflake"). Only do this for keywords of 5+ chars to avoid
            // false positives from short keywords (e.g. "cane" inside
            // "hurricane").
            for (const auto& kw : keywords) {
                if (kw.size() >= 5 && tok.size() > kw.size() && tok.find(kw) != std::string::npos) {
                    found = true;
                    break;
                }
            }
            if (found) break;
        }
        if (found) families.insert(family);
    }
    return families;
}

// If either side has no recognized family, or the families overlap, they're
// considered compatible. Otherwise check the explicit incompatible-pairs
// table; spinner/star are special-cased as co-existing naming conventions.
bool FuzzyFamiliesCompatible(const std::string& a, const std::string& b) {
    auto fa = FuzzyModelFamilies(a);
    auto fb = FuzzyModelFamilies(b);
    if (fa.empty() || fb.empty()) return true;
    for (const auto& x : fa) {
        if (fb.count(x)) return true;
    }
    for (const auto& x : fa) {
        for (const auto& y : fb) {
            if (FUZZY_INCOMPATIBLE_FAMILIES.count({ x, y }) || FUZZY_INCOMPATIBLE_FAMILIES.count({ y, x })) {
                return false;
            }
        }
    }
    if ((fa.count("spinner") && fb.count("star")) || (fa.count("star") && fb.count("spinner"))) {
        return true;
    }
    return false;
}

// If every name in `memberNames` reduces to the same non-empty
// FuzzyBaseTokens set (e.g. a group made up entirely of "Cane-1".."Cane-4"
// all reduce to {"cane"}), returns that shared set - the group is "all one
// kind of prop" and that token can stand in for the group's own name when
// fuzzy-matching. Otherwise returns an empty set.
std::unordered_set<std::string> FuzzyHomogeneousGroupTokens(const std::vector<std::string>& memberNames) {
    std::unordered_set<std::string> common;
    for (const auto& name : memberNames) {
        auto base = FuzzyBaseTokens(name);
        if (base.empty()) return {};
        if (common.empty()) {
            common = base;
        } else if (base != common) {
            return {};
        }
    }
    return common;
}

double FuzzyJaccard(const std::unordered_set<std::string>& a, const std::unordered_set<std::string>& b) {
    if (a.empty() && b.empty()) return 0.0;
    size_t intersection = 0;
    for (const auto& tok : a) {
        if (b.count(tok)) intersection++;
    }
    size_t unionSize = a.size() + b.size() - intersection;
    return unionSize == 0 ? 0.0 : static_cast<double>(intersection) / static_cast<double>(unionSize);
}

// Shared by RunGroupMemberDimensionMatch/RunGroupMemberDimensionBackfill:
// lower is better. If both sides have a known node count, scores on relative
// node-count difference (plus a smaller weight for aspect-ratio difference);
// otherwise falls back to a coarse same-type-or-not score.
double GroupMemberDimensionScore(int targetNodes, int targetWidth, int targetHeight, double targetAspect,
                                  const std::string& targetType, const AvailableSource& src) {
    if (targetNodes > 0 && src.nodeCount > 0) {
        double nodeDiff = std::abs(src.nodeCount - targetNodes) / static_cast<double>(std::max(src.nodeCount, targetNodes));
        double aspectDiff = 0.0;
        if (targetAspect > 0.0 && src.width > 0 && src.height > 0) {
            double srcAspect = static_cast<double>(src.width) / static_cast<double>(src.height);
            aspectDiff = std::abs(srcAspect - targetAspect) / std::max(srcAspect, targetAspect);
        }
        return nodeDiff + (0.25 * aspectDiff);
    }
    return (Lower(Trim(src.displayType)) == targetType) ? 0.0 : 1.0;
}

} // namespace

namespace AutoMapper {

bool MatchNorm(const std::string& target, const std::string& candidate,
               const std::string&, const std::string&,
               const std::list<std::string>&) {
    return Lower(Trim(target)) == candidate;
}

bool MatchAggressive(const std::string& target, const std::string& candidate,
                     const std::string&, const std::string&,
                     const std::list<std::string>& aliases) {
    std::string strippedCandidate = StripPunctuation(candidate);
    if (StripPunctuation(Lower(Trim(target))) == strippedCandidate) {
        return true;
    }
    // OldName alias prefix — the rename history form. Plain string compare
    // (no punctuation strip) so users with renames don't get over-matched.
    for (const auto& it : aliases) {
        if (Lower(Trim(it)) == "oldname:" + candidate) {
            return true;
        }
    }
    // Plain alias match — try both punctuation-stripped and exact, so an
    // alias "Mega Tree (Vendor)" can match a vendor candidate
    // "megatreevendor" without the user needing to maintain a punctuation-
    // free duplicate alias.
    for (const auto& it : aliases) {
        std::string aliasNorm = Lower(Trim(it));
        if (aliasNorm == candidate) return true;
        if (StripPunctuation(aliasNorm) == strippedCandidate) return true;
    }
    return false;
}

bool MatchRegex(const std::string& target, const std::string& candidate,
                const std::string& pattern, const std::string& replacement,
                const std::list<std::string>&) {
    if (Lower(Trim(candidate)) != Lower(Trim(replacement))) {
        return false;
    }
    static std::regex r;
    static std::string lastPattern;
    static bool valid = false;
    try {
        if (pattern != lastPattern) {
            r = std::regex(pattern, std::regex::ECMAScript | std::regex::icase);
            lastPattern = pattern;
            valid = true;
        }
    } catch (const std::regex_error&) {
        valid = false;
        lastPattern = pattern;
    }
    if (!valid) {
        return false;
    }
    return std::regex_match(target, r);
}

namespace {

// Shared core for MatchFuzzy / RunGroupContentFuzzy: family-compatibility and
// numeric/side-signature guardrails on the raw names, then a token Jaccard
// overlap of >= 0.6 on the (possibly extra-token-augmented) token sets.
bool FuzzyTokenMatch(const std::string& target, const std::string& candidate,
                      std::unordered_set<std::string> targetTokens,
                      std::unordered_set<std::string> candTokens) {
    if (!FuzzyFamiliesCompatible(target, candidate)) {
        return false;
    }

    auto targetNum = FuzzyNumericSignature(target);
    auto candNum = FuzzyNumericSignature(candidate);
    if (!targetNum.empty() && !candNum.empty() && targetNum != candNum) {
        return false;
    }

    auto targetSide = FuzzySideSignature(target);
    auto candSide = FuzzySideSignature(candidate);
    if (!targetSide.empty() && !candSide.empty() && targetSide != candSide) {
        return false;
    }

    if (targetTokens.empty() || candTokens.empty()) {
        return false;
    }

    return FuzzyJaccard(targetTokens, candTokens) >= 0.6;
}

} // namespace

bool MatchFuzzy(const std::string& target, const std::string& candidate,
                const std::string&, const std::string&,
                const std::list<std::string>&) {
    return FuzzyTokenMatch(target, candidate, FuzzyTokens(target), FuzzyTokens(candidate));
}

void RunGroupContentFuzzy(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          RenderContext& renderContext,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped so this pass doesn't hand out
    // a source another phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
    }

    for (auto* model : roots) {
        if (model == nullptr || !model->IsGroup() || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;

        const std::string targetName = model->GetCoreModel();
        auto targetTokens = FuzzyTokens(targetName);

        // If the destination group is made up entirely of one kind of prop
        // (e.g. "Cane-1".."Cane-4"), fold that prop's family token into the
        // group's own tokens so e.g. "Group - Candy Canes" picks up "cane"
        // even if its own name didn't have enough overlap on its own.
        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());
        auto* grp = dynamic_cast<ModelGroup*>(layoutModel);
        if (grp != nullptr) {
            auto extra = FuzzyHomogeneousGroupTokens(grp->ModelNames());
            targetTokens.insert(extra.begin(), extra.end());
        }

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (src.modelType != "ModelGroup") continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;

            auto candTokens = FuzzyTokens(src.displayName);
            auto extra = FuzzyHomogeneousGroupTokens(src.groupMemberNames);
            candTokens.insert(extra.begin(), extra.end());

            if (!FuzzyTokenMatch(targetName, src.displayName, targetTokens, candTokens)) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }
}

void Run(const std::vector<ImportMappingNode*>& roots,
         const std::vector<AvailableSource>& available,
         RenderContext& renderContext,
         MatcherFn lambda_model, MatcherFn lambda_strand, MatcherFn lambda_node,
         const std::string& extra1, const std::string& extra2,
         const std::string& mg,
         bool selectOnly,
         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
         const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    for (auto* model : roots) {
        if (model == nullptr) {
            spdlog::warn("AutoMapper::Run: null root encountered, skipping");
            continue;
        }
        if (model->IsSkipped()) continue;
        bool isTargetSelected = selectedTargets.count(model) != 0;
        if (selectMapTarget && !isTargetSelected) {
            continue;
        }

        auto aliases = model->GetAliases();
        bool typeMatch = (model->IsGroup() && (mg == "B" || mg == "G")) ||
                         (!model->IsGroup() && (mg == "B" || mg == "M"));
        if (!typeMatch) continue;

        // Cache the layout model once per destination model so both the
        // slashed-path strand-alias lookup and the fallback can use it
        // without repeated map lookups.
        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            const std::string& availName = src.canonicalName;

            if (availName.find('/') != std::string::npos) {
                auto parts = SplitSlash(availName);
                if (lambda_model(model->GetCoreModel(), parts[0], extra1, extra2, aliases)) {
                    // matched the model name ... need to look at strands and submodels
                    for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
                        auto* strand = model->GetNthChild(k);
                        if (strand == nullptr) continue;
                        // Use the submodel's own aliases (from the layout) for
                        // strand matching so that e.g. a submodel aliased
                        // "15 spinners - all" correctly matches that part of
                        // "SS Spinner Left/15 Spinners - All".
                        std::list<std::string> strandAliases;
                        if (layoutModel != nullptr) {
                            Model* sm2 = layoutModel->GetSubModel(strand->GetCoreStrand());
                            if (sm2 != nullptr) {
                                strandAliases = sm2->GetAliases();
                            }
                        }
                        const auto& strandAliasesToUse = strandAliases.empty() ? aliases : strandAliases;
                        if (!lambda_strand(strand->GetCoreStrand(), parts[1], extra1, extra2, strandAliasesToUse)) {
                            continue;
                        }
                        if (parts.size() == 2) {
                            if (strand->GetMapping().empty()) {
                                strand->Map(src.displayName, "Strand");
                                strand->SetMappingRule(ruleLabel);
                            }
                        } else {
                            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                                auto* node = strand->GetNthChild(m);
                                if (node == nullptr) continue;
                                if (!node->GetMapping().empty()) continue;
                                if (lambda_node(node->GetCoreNode(), parts[2], extra1, extra2, aliases)) {
                                    if (parts.size() == 3) {
                                        node->Map(src.displayName, "Node");
                                        node->SetMappingRule(ruleLabel);
                                    }
                                }
                            }
                        }
                    }
                }
            } else {
                // match model to model
                if (model->GetMapping().empty() &&
                    lambda_model(model->GetCoreModel(), availName, extra1, extra2, aliases)) {
                    model->Map(src.displayName, src.modelType);
                    model->SetMappingRule(ruleLabel);
                }
            }
        }

    }

    // Process selected submodels independently
    if (selectMapTarget) {
        for (auto* model : roots) {
            if (model == nullptr) continue;
            for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
                auto* submodel = model->GetNthChild(k);
                if (submodel == nullptr) continue;
                bool isSubmodelSelected = selectedTargets.count(submodel) != 0;
                if (selectMapTarget && !isSubmodelSelected) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    const std::string& availName = src.canonicalName;
                    Model* layoutModel = renderContext.GetModel(model->GetCoreModel());
                    if (layoutModel == nullptr) continue;
                    const auto& mAliases = layoutModel->GetAliases();
                    Model* sm = layoutModel->GetSubModel(submodel->GetCoreStrand());
                    if (sm == nullptr) continue;
                    const auto& smAliases = sm->GetAliases();
                    if (!submodel->GetMapping().empty()) continue;

                    if (lambda_strand(submodel->GetModelName(), availName, extra1, extra2, smAliases)) {
                        submodel->Map(src.displayName, "SubModel");
                        submodel->SetMappingRule(ruleLabel);
                    } else {
                        for (const auto& modelAlias : mAliases) {
                            if (lambda_strand(modelAlias + "/" + submodel->GetCoreStrand(),
                                              availName, extra1, extra2, smAliases)) {
                                submodel->Map(src.displayName, "SubModel");
                                submodel->SetMappingRule(ruleLabel);
                                break;
                            }
                        }
                    }
                }
            }
        }
    }
}

void RunSubModelFallback(const std::vector<ImportMappingNode*>& roots,
                         const std::vector<AvailableSource>& available,
                         RenderContext& renderContext,
                         bool selectOnly,
                         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                         const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    for (auto* model : roots) {
        if (model == nullptr) continue;
        // Submodels only exist on regular models, and only non-slashed
        // bare-model sources are matched below (groups never appear there
        // since group entries are excluded by findModelType/modelType
        // checks at the call site), so skip group roots entirely.
        if (model->IsGroup()) continue;
        if (model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;

        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());

        // Step 1: match unmapped submodels by name against non-slashed,
        // non-group sources (submodels only come from models, not groups).
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr || !sm->GetMapping().empty()) continue;
            const std::string smName = Lower(Trim(sm->GetCoreStrand()));
            for (const auto& src : available) {
                if (selectMapAvail && !src.selected) continue;
                if (!sm->GetMapping().empty()) break;
                const std::string& availName = src.canonicalName;
                if (availName.find('/') != std::string::npos) continue;
                if (src.modelType == "ModelGroup") continue;
                // Singing props are reserved for Phase 30/32 - a vendor
                // singing prop should never get claimed as a generic
                // submodel match.
                if (src.isSingingProp) continue;
                if (smName == availName) {
                    sm->Map(src.displayName, "Unknown");
                    sm->SetMappingRule(ruleLabel);
                }
            }
        }

        // Step 2: match still-unmapped submodels by their layout aliases.
        if (layoutModel == nullptr) continue;
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr || !sm->GetMapping().empty()) continue;
            Model* sm2 = layoutModel->GetSubModel(sm->GetCoreStrand());
            if (sm2 == nullptr) continue;
            const auto& smAliases = sm2->GetAliases();
            if (smAliases.empty()) continue;
            for (const auto& src : available) {
                if (selectMapAvail && !src.selected) continue;
                if (!sm->GetMapping().empty()) break;
                const std::string& availName = src.canonicalName;
                if (availName.find('/') != std::string::npos) continue;
                if (src.modelType == "ModelGroup") continue;
                // Singing props are reserved for Phase 30/32 - a vendor
                // singing prop should never get claimed as a generic
                // submodel match.
                if (src.isSingingProp) continue;
                const std::string strippedAvail = StripPunctuation(availName);
                for (const auto& alias : smAliases) {
                    std::string aliasNorm = Lower(Trim(alias));
                    if (aliasNorm == availName || StripPunctuation(aliasNorm) == strippedAvail) {
                        sm->Map(src.displayName, "Unknown");
                        sm->SetMappingRule(ruleLabel);
                        break;
                    }
                }
            }
        }
    }
}

void RunSingingProp(const std::vector<ImportMappingNode*>& roots,
                    const std::vector<AvailableSource>& available,
                    bool selectOnly,
                    const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                    const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // this pass doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Top-level pass: model<->model only, matched purely on both sides being
    // a real singing prop (a Custom model with at least one populated
    // faceInfo NodeRange). Groups are not considered singing props.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;
        if (model->IsGroup()) continue;
        if (!model->IsSingingProp()) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (src.modelType == "ModelGroup") continue;
            if (!src.isSingingProp) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    // Strand/Node pass: for any root that ended up mapped (here or earlier),
    // fill its still-unmapped Strand/Node children from `<vendorModel>/...`
    // sources of the same depth, ignoring names.
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

void RunSingingPropBackfill(const std::vector<ImportMappingNode*>& roots,
                            const std::vector<AvailableSource>& available,
                            bool selectOnly,
                            const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                            const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Vendor singing-prop sources, reused round-robin - unlike Phase 30,
    // these are not removed from consideration once used, since by this
    // point there are more singing-face destinations than vendor singing
    // models to pair them with 1:1.
    std::vector<const AvailableSource*> singingSources;
    for (const auto& src : available) {
        if (selectMapAvail && !src.selected) continue;
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType == "ModelGroup") continue;
        if (!src.isSingingProp) continue;
        singingSources.push_back(&src);
    }
    if (singingSources.empty()) return;

    size_t nextSource = 0;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;
        if (model->IsGroup()) continue;
        if (!model->IsSingingProp()) continue;

        const AvailableSource* src = singingSources[nextSource % singingSources.size()];
        ++nextSource;

        model->Map(src->displayName, src->modelType);
        model->SetMappingRule(ruleLabel);

        // Fill still-unmapped Strand/Node children from the reused vendor
        // model's `<vendorModel>/...` sources of the same depth, ignoring
        // names (and allowing reuse, same as the top-level mapping).
        const std::string vendorPrefix = Lower(Trim(src->displayName)) + "/";
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& s : available) {
                    if (selectMapAvail && !s.selected) continue;
                    if (s.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(s.canonicalName).size() != 2) continue;

                    strand->Map(s.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& s : available) {
                    if (selectMapAvail && !s.selected) continue;
                    if (s.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(s.canonicalName).size() != 3) continue;

                    node->Map(s.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    break;
                }
            }
        }
    }
}

void RunFloodlight(const std::vector<ImportMappingNode*>& roots,
                   const std::vector<AvailableSource>& available,
                   bool selectOnly,
                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                   const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Step 1: flood group <-> flood group.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;
        if (!model->IsGroup()) continue;
        if (!model->IsFloodGroup()) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (src.modelType != "ModelGroup") continue;
            if (!src.isFloodGroup) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    // Step 2: individual floodlight <-> individual floodlight.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;
        if (model->IsGroup()) continue;
        if (!model->IsFloodlight()) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (src.modelType == "ModelGroup") continue;
            if (!src.isFloodlight) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }
}

void RunFloodlightBackfill(const std::vector<ImportMappingNode*>& roots,
                           const std::vector<AvailableSource>& available,
                           bool selectOnly,
                           const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                           const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Vendor flood-group sources, reused round-robin.
    std::vector<const AvailableSource*> floodGroupSources;
    for (const auto& src : available) {
        if (selectMapAvail && !src.selected) continue;
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType != "ModelGroup") continue;
        if (!src.isFloodGroup) continue;
        floodGroupSources.push_back(&src);
    }

    // Vendor individual-floodlight sources, reused round-robin.
    std::vector<const AvailableSource*> floodlightSources;
    for (const auto& src : available) {
        if (selectMapAvail && !src.selected) continue;
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType == "ModelGroup") continue;
        if (!src.isFloodlight) continue;
        floodlightSources.push_back(&src);
    }

    // Step 1: flood-group backfill.
    if (!floodGroupSources.empty()) {
        size_t nextSource = 0;
        for (auto* model : roots) {
            if (model == nullptr) continue;
            if (selectMapAvail || selectMapTarget) {
                bool isTargetSelected = selectedTargets.count(model) != 0;
                if (selectMapTarget && !isTargetSelected) continue;
            }
            if (!model->GetMapping().empty()) continue;
            if (model->IsSkipped()) continue;
            if (!model->IsGroup()) continue;
            if (!model->IsFloodGroup()) continue;

            const AvailableSource* src = floodGroupSources[nextSource % floodGroupSources.size()];
            ++nextSource;

            model->Map(src->displayName, src->modelType);
            model->SetMappingRule(ruleLabel);
        }
    }

    // Step 2: individual-floodlight backfill.
    if (!floodlightSources.empty()) {
        size_t nextSource = 0;
        for (auto* model : roots) {
            if (model == nullptr) continue;
            if (selectMapAvail || selectMapTarget) {
                bool isTargetSelected = selectedTargets.count(model) != 0;
                if (selectMapTarget && !isTargetSelected) continue;
            }
            if (!model->GetMapping().empty()) continue;
            if (model->IsSkipped()) continue;
            if (model->IsGroup()) continue;
            if (!model->IsFloodlight()) continue;

            const AvailableSource* src = floodlightSources[nextSource % floodlightSources.size()];
            ++nextSource;

            model->Map(src->displayName, src->modelType);
            model->SetMappingRule(ruleLabel);
        }
    }
}

void RunBestGuess(const std::vector<ImportMappingNode*>& roots,
                  const std::vector<AvailableSource>& available,
                  bool selectOnly,
                  const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                  const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // this pass doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Top-level pass: model<->model, group<->group, matched purely on a
    // shared non-empty model class (e.g. both "SingingFace").
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;

        std::string targetClass = model->GetModelClass();
        if (targetClass.empty() || targetClass == "SingingFace") continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            bool srcIsGroup = (src.modelType == "ModelGroup");
            if (srcIsGroup != model->IsGroup()) continue;
            if (src.modelClass != targetClass) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    // Strand/Node pass: for any root that ended up mapped (here or earlier),
    // fill its still-unmapped Strand/Node children from `<vendorModel>/...`
    // sources of the same depth, ignoring names.
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

void RunLikeModelBackfill(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped so this phase doesn't hand out
    // a vendor source another phase (or this phase, for an earlier root)
    // already claimed.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
    }

    for (auto* model : roots) {
        if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;

        const std::string targetName = model->GetCoreModel();
        auto targetBase = FuzzyBaseTokens(targetName);
        auto targetSide = FuzzySideSignature(targetName);
        auto targetNum = FuzzyNumericSignature(targetName);
        if (targetBase.empty() || targetNum.size() != 1) continue;

        for (auto* sibling : roots) {
            if (sibling == nullptr || sibling == model || sibling->IsGroup()) continue;
            if (sibling->GetMapping().empty()) continue;

            const std::string siblingName = sibling->GetCoreModel();
            if (FuzzyBaseTokens(siblingName) != targetBase) continue;
            if (FuzzySideSignature(siblingName) != targetSide) continue;

            auto siblingNum = FuzzyNumericSignature(siblingName);
            if (siblingNum.size() != 1 || siblingNum[0] == targetNum[0]) continue;

            // Substitute the sibling's number for ours in the sibling's
            // mapped vendor name, e.g. mapped "Cane 1" -> candidate "Cane 4".
            const std::string& mapping = sibling->GetMapping();
            size_t pos = mapping.find(siblingNum[0]);
            if (pos == std::string::npos) continue;
            std::string candidate = mapping.substr(0, pos) + targetNum[0] + mapping.substr(pos + siblingNum[0].size());
            std::string candidateLower = Lower(Trim(candidate));
            if (used.count(candidateLower) != 0) continue;

            for (const auto& src : available) {
                if (selectMapAvail && !src.selected) continue;
                if (src.canonicalName.find('/') != std::string::npos) continue;
                if (src.modelType == "ModelGroup") continue;
                if (Lower(Trim(src.displayName)) != candidateLower) continue;

                model->Map(src.displayName, src.modelType);
                model->SetMappingRule(ruleLabel);
                used.insert(candidateLower);
                break;
            }
            if (!model->GetMapping().empty()) break;
        }
    }

    // Dimension-reuse fallback: for a still-unmapped numbered sibling (e.g.
    // "Cane-4") whose analogous vendor name (e.g. "Cane 4") doesn't exist -
    // vendor only has 3 canes - if it's a Custom model whose node count and
    // grid shape exactly match an already-mapped same-family sibling's (e.g.
    // "Cane-3", same 100x51 shape), reuse that sibling's mapped vendor source.
    // This keeps a genuinely like-shaped destination paired with its closest
    // relative instead of falling through to Phase 105's blind same-type
    // (e.g. Custom<->Custom) pairing with an unrelated prop.
    for (auto* model : roots) {
        if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (Lower(Trim(model->GetModelType())) != "custom") continue;

        const int targetNodes = model->GetNodeCount();
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        if (targetNodes <= 0) continue;

        const std::string targetName = model->GetCoreModel();
        auto targetBase = FuzzyBaseTokens(targetName);
        auto targetSide = FuzzySideSignature(targetName);
        if (targetBase.empty()) continue;

        for (auto* sibling : roots) {
            if (sibling == nullptr || sibling == model || sibling->IsGroup()) continue;
            if (sibling->GetMapping().empty()) continue;
            if (Lower(Trim(sibling->GetModelType())) != "custom") continue;
            if (sibling->GetNodeCount() != targetNodes) continue;
            if (sibling->GetWidth() != targetWidth || sibling->GetHeight() != targetHeight) continue;

            const std::string siblingName = sibling->GetCoreModel();
            if (FuzzyBaseTokens(siblingName) != targetBase) continue;
            if (FuzzySideSignature(siblingName) != targetSide) continue;

            model->Map(sibling->GetMapping(), "Model");
            model->SetMappingRule(ruleLabel);
            break;
        }
    }
}

void RunCustomExactDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                                   const std::vector<AvailableSource>& available,
                                   bool selectOnly,
                                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                   const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    for (auto* model : roots) {
        if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!model->GetMapping().empty()) continue;
        if (Lower(Trim(model->GetModelType())) != "custom") continue;

        const int targetNodes = model->GetNodeCount();
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        if (targetNodes <= 0) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (src.modelType == "ModelGroup") continue;
            if (Lower(Trim(src.displayType)) != "custom") continue;
            if (src.nodeCount != targetNodes || src.width != targetWidth || src.height != targetHeight) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

void RunCustomSubmodelOverlapMatch(const std::vector<ImportMappingNode*>& roots,
                                    const std::vector<AvailableSource>& available,
                                    bool selectOnly,
                                    const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                    const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Pre-build, per still-unmapped non-group Custom vendor model, the set of
    // its submodel names from `<vendorModel>/<submodel>` entries.
    struct CustomCandidate {
        const AvailableSource* src;
        std::unordered_set<std::string> submodels;
    };
    std::vector<CustomCandidate> candidates;
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType == "ModelGroup") continue;
        if (Lower(Trim(src.displayType)) != "custom") continue;
        if (used.count(Lower(Trim(src.displayName))) != 0) continue;

        CustomCandidate cand;
        cand.src = &src;
        const std::string prefix = src.canonicalName + "/";
        for (const auto& sub : available) {
            if (sub.canonicalName.rfind(prefix, 0) != 0) continue;
            auto parts = SplitSlash(sub.canonicalName);
            if (parts.size() != 2) continue;
            cand.submodels.insert(Lower(Trim(parts[1])));
        }
        if (!cand.submodels.empty()) candidates.push_back(std::move(cand));
    }

    for (auto* model : roots) {
        if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!model->GetMapping().empty()) continue;
        if (Lower(Trim(model->GetModelType())) != "custom") continue;

        std::unordered_set<std::string> targetSubmodels;
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            const std::string& strand = sm->GetCoreStrand();
            if (!strand.empty()) targetSubmodels.insert(Lower(Trim(strand)));
        }
        if (targetSubmodels.size() < 3) continue;

        for (const auto& cand : candidates) {
            if (selectMapAvail && !cand.src->selected) continue;
            if (used.count(Lower(Trim(cand.src->displayName))) != 0) continue;

            int overlap = 0;
            for (const auto& name : targetSubmodels) {
                if (cand.submodels.count(name) != 0) ++overlap;
            }
            if (overlap < 3) continue;

            model->Map(cand.src->displayName, cand.src->modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(cand.src->displayName)));
            break;
        }
    }

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

// Shared by RunSpecialKeywordGroupMatch (Phase 17) and RunCatchAll
// (Phase 120) - true if `name` contains (case-insensitive) one of the
// special-sequencer-meaning group keywords ("last", "override", "bottom").
bool IsSpecialKeywordGroupName(const std::string& name) {
    static const std::vector<std::string> keywords = { "last", "override", "bottom" };
    const std::string lowered = Lower(Trim(name));
    for (const auto& kw : keywords) {
        if (lowered.find(kw) != std::string::npos) return true;
    }
    return false;
}

void RunSpecialKeywordGroupMatch(const std::vector<ImportMappingNode*>& roots,
                                  const std::vector<AvailableSource>& available,
                                  bool selectOnly,
                                  const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                  const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    auto containsKeyword = IsSpecialKeywordGroupName;

    for (auto* model : roots) {
        if (model == nullptr || !model->IsGroup() || model->IsSkipped()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!model->GetMapping().empty()) continue;
        bool matches = containsKeyword(model->GetModelName());
        if (!matches) {
            for (const auto& alias : model->GetAliases()) {
                if (containsKeyword(alias)) { matches = true; break; }
            }
        }
        if (!matches) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (src.modelType != "ModelGroup") continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (!containsKeyword(src.displayName)) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

// Returns true if `name`, once normalized into whitespace-separated tokens,
// contains a whole-word "everything" or "all" token (so "Waterfall"/"Hallway"
// don't match, but "Group - All Props" and "Everything" do).
bool ContainsEverythingOrAllToken(const std::string& name) {
    const std::string normalized = FuzzyNormalize(name);
    std::string cur;
    auto isMatch = [&]() { return cur == "everything" || cur == "all"; };
    for (char c : normalized) {
        if (c == ' ') {
            if (isMatch()) return true;
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    return isMatch();
}

void RunEverythingGroupMatch(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              RenderContext& renderContext,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    auto matchesEverything = [](const ImportMappingNode* model) {
        if (ContainsEverythingOrAllToken(model->GetModelName())) return true;
        for (const auto& alias : model->GetAliases()) {
            if (ContainsEverythingOrAllToken(alias)) return true;
        }
        return false;
    };

    // Find the still-unmapped destination group with an "everything"/"all"
    // name or alias and the highest member count.
    ImportMappingNode* bestDest = nullptr;
    size_t bestDestCount = 0;
    for (auto* model : roots) {
        if (model == nullptr || !model->IsGroup() || model->IsSkipped()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!model->GetMapping().empty()) continue;
        if (!matchesEverything(model)) continue;

        size_t count = 0;
        if (auto* grp = dynamic_cast<ModelGroup*>(renderContext.GetModel(model->GetCoreModel()))) {
            count = grp->ModelNames().size();
        }
        if (bestDest == nullptr || count > bestDestCount) {
            bestDest = model;
            bestDestCount = count;
        }
    }
    if (bestDest == nullptr) return;

    // Find the still-unmapped vendor ModelGroup with an "everything"/"all"
    // name and the highest member count.
    const AvailableSource* bestSrc = nullptr;
    size_t bestSrcCount = 0;
    for (const auto& src : available) {
        if (selectMapAvail && !src.selected) continue;
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType != "ModelGroup") continue;
        if (used.count(Lower(Trim(src.displayName))) != 0) continue;
        if (!ContainsEverythingOrAllToken(src.displayName)) continue;

        const size_t count = src.groupMemberNames.size();
        if (bestSrc == nullptr || count > bestSrcCount) {
            bestSrc = &src;
            bestSrcCount = count;
        }
    }
    if (bestSrc == nullptr) return;

    bestDest->Map(bestSrc->displayName, bestSrc->modelType);
    bestDest->SetMappingRule(ruleLabel);
    used.insert(Lower(Trim(bestSrc->displayName)));

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

void RunCustomDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // this pass doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Top-level pass: model<->model, group<->group, both Custom type, picking
    // the candidate whose node count / grid shape is the closest match.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;

        const std::string targetType = Lower(Trim(model->GetModelType()));
        if (targetType != "custom") continue;

        const std::string targetName = model->GetCoreModel();
        const int targetNodes = model->GetNodeCount();
        if (targetNodes <= 0) continue;
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        const double targetAspect = (targetWidth > 0 && targetHeight > 0)
            ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
            : 0.0;

        const AvailableSource* best = nullptr;
        double bestScore = 0.0;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            bool srcIsGroup = (src.modelType == "ModelGroup");
            if (srcIsGroup != model->IsGroup()) continue;
            if (Lower(Trim(src.displayType)) != "custom") continue;
            if (src.nodeCount <= 0) continue;
            // Family guardrail: e.g. don't let a "Snowflake"-family vendor
            // model (recognized via FuzzyModelFamilies, including compound
            // names like "EFlake46"/"ChromaFlake...") match a destination
            // Custom model that is recognizably a different family (and
            // vice versa), even if their node counts happen to be close.
            if (!FuzzyFamiliesCompatible(targetName, src.displayName)) continue;

            // Relative difference in node count - the dominant factor since
            // it best reflects overall prop "size".
            double nodeDiff = std::abs(src.nodeCount - targetNodes) / static_cast<double>(std::max(src.nodeCount, targetNodes));

            // Aspect-ratio difference (only considered when both sides have
            // known dimensions); weighted lower than node count.
            double aspectDiff = 0.0;
            if (targetAspect > 0.0 && src.width > 0 && src.height > 0) {
                double srcAspect = static_cast<double>(src.width) / static_cast<double>(src.height);
                aspectDiff = std::abs(srcAspect - targetAspect) / std::max(srcAspect, targetAspect);
            }

            double score = nodeDiff + (0.25 * aspectDiff);
            if (best == nullptr || score < bestScore) {
                best = &src;
                bestScore = score;
            }
        }

        // Only accept a close match - node counts within ~25% of each other.
        // Anything looser is left for Phase 105/120 to pair by type alone.
        if (best != nullptr && bestScore <= 0.25) {
            model->Map(best->displayName, best->modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(best->displayName)));
        }
    }

    // Strand/Node pass: for any root that ended up mapped (here or earlier),
    // fill its still-unmapped Strand/Node children from `<vendorModel>/...`
    // sources of the same depth, ignoring names.
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

void RunGroupMemberDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                                   const std::vector<AvailableSource>& available,
                                   RenderContext& renderContext,
                                   bool selectOnly,
                                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                   const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // this pass doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    for (auto* group : roots) {
        if (group == nullptr || !group->IsGroup()) continue;
        if (group->GetMapping().empty()) continue;

        // Only trust group<->group pairings that came from a name-based
        // match (Exact/Alias/Community/Fuzzy/GroupContent etc). A
        // catch-all-style group<->group pairing (Phase 105/120, e.g. a huge
        // "01 Everything" group matched purely on type) carries no real
        // correspondence between the two groups' members, so using it here
        // would let an unrelated destination model (e.g. "3D Cube-2") steal
        // a vendor source intended for a member of a *different*,
        // meaningfully-matched group (e.g. "Group - Snowflakes").
        if (group->GetMappingRule().find("Catchall") != std::string::npos) continue;

        Model* layoutModel = renderContext.GetModel(group->GetCoreModel());
        auto* grp = dynamic_cast<ModelGroup*>(layoutModel);
        if (grp == nullptr) continue;

        std::set<std::string> destMembers;
        for (const auto& name : grp->ModelNames()) destMembers.insert(Lower(Trim(name)));
        if (destMembers.empty()) continue;

        // Find the vendor ModelGroup this destination group was mapped to,
        // and its member-name list.
        const std::string vendorGroupName = Lower(Trim(group->GetMapping()));
        const AvailableSource* vendorGroup = nullptr;
        for (const auto& src : available) {
            if (src.modelType != "ModelGroup") continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (Lower(Trim(src.displayName)) == vendorGroupName) { vendorGroup = &src; break; }
        }
        if (vendorGroup == nullptr || vendorGroup->groupMemberNames.empty()) continue;

        std::set<std::string> vendorMembers;
        for (const auto& name : vendorGroup->groupMemberNames) vendorMembers.insert(Lower(Trim(name)));

        for (auto* model : roots) {
            if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
            if (!model->GetMapping().empty()) continue;
            if (selectMapTarget && selectedTargets.count(model) == 0) continue;
            if (destMembers.count(Lower(Trim(model->GetCoreModel()))) == 0) continue;

            const int targetNodes = model->GetNodeCount();
            const int targetWidth = model->GetWidth();
            const int targetHeight = model->GetHeight();
            const double targetAspect = (targetWidth > 0 && targetHeight > 0)
                ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
                : 0.0;
            const std::string targetType = Lower(Trim(model->GetModelType()));

            const AvailableSource* best = nullptr;
            double bestScore = 0.0;
            for (const auto& src : available) {
                if (selectMapAvail && !src.selected) continue;
                if (src.canonicalName.find('/') != std::string::npos) continue;
                if (src.modelType == "ModelGroup") continue;
                if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                if (vendorMembers.count(Lower(Trim(src.displayName))) == 0) continue;

                double score = GroupMemberDimensionScore(targetNodes, targetWidth, targetHeight, targetAspect, targetType, src);

                if (best == nullptr || score < bestScore) {
                    best = &src;
                    bestScore = score;
                }
            }

            if (best != nullptr) {
                model->Map(best->displayName, best->modelType);
                model->SetMappingRule(ruleLabel);
                used.insert(Lower(Trim(best->displayName)));
            }
        }
    }

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

void RunGroupMemberDimensionBackfill(const std::vector<ImportMappingNode*>& roots,
                                      const std::vector<AvailableSource>& available,
                                      RenderContext& renderContext,
                                      bool selectOnly,
                                      const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                      const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    for (auto* group : roots) {
        if (group == nullptr || !group->IsGroup()) continue;
        if (group->GetMapping().empty()) continue;
        // Same restriction as RunGroupMemberDimensionMatch - only trust
        // name-based group<->group pairings as a member-reuse pool.
        if (group->GetMappingRule().find("Catchall") != std::string::npos) continue;

        Model* layoutModel = renderContext.GetModel(group->GetCoreModel());
        auto* grp = dynamic_cast<ModelGroup*>(layoutModel);
        if (grp == nullptr) continue;

        std::set<std::string> destMembers;
        for (const auto& name : grp->ModelNames()) destMembers.insert(Lower(Trim(name)));
        if (destMembers.empty()) continue;

        const std::string vendorGroupName = Lower(Trim(group->GetMapping()));
        const AvailableSource* vendorGroup = nullptr;
        for (const auto& src : available) {
            if (src.modelType != "ModelGroup") continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (Lower(Trim(src.displayName)) == vendorGroupName) { vendorGroup = &src; break; }
        }
        if (vendorGroup == nullptr || vendorGroup->groupMemberNames.empty()) continue;

        std::set<std::string> vendorMembers;
        for (const auto& name : vendorGroup->groupMemberNames) vendorMembers.insert(Lower(Trim(name)));

        // Reusable pool - sources already claimed by Phase 110 (or an earlier
        // root in this loop) remain candidates here, since by this point
        // there may be more destination group members than vendor ones.
        std::vector<const AvailableSource*> pool;
        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (src.modelType == "ModelGroup") continue;
            if (vendorMembers.count(Lower(Trim(src.displayName))) == 0) continue;
            pool.push_back(&src);
        }
        if (pool.empty()) continue;

        for (auto* model : roots) {
            if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
            if (!model->GetMapping().empty()) continue;
            if (selectMapTarget && selectedTargets.count(model) == 0) continue;
            if (destMembers.count(Lower(Trim(model->GetCoreModel()))) == 0) continue;

            const int targetNodes = model->GetNodeCount();
            const int targetWidth = model->GetWidth();
            const int targetHeight = model->GetHeight();
            const double targetAspect = (targetWidth > 0 && targetHeight > 0)
                ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
                : 0.0;
            const std::string targetType = Lower(Trim(model->GetModelType()));

            const AvailableSource* best = nullptr;
            double bestScore = 0.0;
            for (const auto* src : pool) {
                double score = GroupMemberDimensionScore(targetNodes, targetWidth, targetHeight, targetAspect, targetType, *src);
                if (best == nullptr || score < bestScore) {
                    best = src;
                    bestScore = score;
                }
            }

            if (best != nullptr) {
                model->Map(best->displayName, best->modelType);
                model->SetMappingRule(ruleLabel);
            }
        }
    }
}

void RunModelTypeCatchAll(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // this pass doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Top-level pass: model<->model, group<->group, requiring matching model
    // "type" (e.g. "Arches", "Tree 360", "Matrix"), ignoring names entirely.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;

        const std::string targetType = Lower(Trim(model->GetModelType()));
        if (targetType.empty()) continue;

        // "Custom" covers wildly different shapes (a 3D Cube and a Snowflake
        // are both "Custom"), so a blind type match here is meaningless.
        // Custom models that didn't get a dimension-based match in Phase 100
        // are left for Phase 120 instead.
        if (targetType == "custom") continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            bool srcIsGroup = (src.modelType == "ModelGroup");
            if (srcIsGroup != model->IsGroup()) continue;
            if (Lower(Trim(src.displayType)) != targetType) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    // Strand/Node pass: for any root that ended up mapped (here or earlier),
    // fill its still-unmapped Strand/Node children from `<vendorModel>/...`
    // sources of the same depth, ignoring names.
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

void RunCatchAll(const std::vector<ImportMappingNode*>& roots,
                 const std::vector<AvailableSource>& available,
                 bool selectOnly,
                 const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                 const std::string& ruleLabel) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Seed "used" with anything already mapped (by name, lowered/trimmed) so
    // the catch-all doesn't hand out a source a previous phase already used.
    std::unordered_set<std::string> used;
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty()) used.insert(Lower(Trim(model->GetMapping())));
        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* sm = model->GetNthChild(k);
            if (sm == nullptr) continue;
            if (!sm->GetMapping().empty()) used.insert(Lower(Trim(sm->GetMapping())));
            for (unsigned int m = 0; m < sm->GetChildCount(); ++m) {
                auto* node = sm->GetNthChild(m);
                if (node == nullptr) continue;
                if (!node->GetMapping().empty()) used.insert(Lower(Trim(node->GetMapping())));
            }
        }
    }

    // Top-level pass: model<->model, group<->group, ignoring names entirely.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            bool srcIsGroup = (src.modelType == "ModelGroup");
            if (srcIsGroup != model->IsGroup()) continue;

            // Vendor ModelGroups carrying "Last"/"Override"/"Bottom" special
            // sequencer-meaning names (see RunSpecialKeywordGroupMatch /
            // Phase 17) are reserved for a name-based match. If they weren't
            // claimed by an earlier phase, leave them - and the destination
            // group under consideration - unmapped rather than pairing them
            // blindly here.
            if (srcIsGroup && IsSpecialKeywordGroupName(src.displayName)) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }

    // Strand/Node pass: for any root that ended up mapped (here or earlier),
    // fill its still-unmapped Strand/Node children from `<vendorModel>/...`
    // sources of the same depth, ignoring names.
    for (auto* model : roots) {
        if (model == nullptr || model->GetMapping().empty()) continue;
        const std::string vendorPrefix = Lower(Trim(model->GetMapping())) + "/";

        for (unsigned int k = 0; k < model->GetChildCount(); ++k) {
            auto* strand = model->GetNthChild(k);
            if (strand == nullptr) continue;

            if (strand->GetMapping().empty()) {
                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 2) continue;

                    strand->Map(src.displayName, "Strand");
                    strand->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }

            for (unsigned int m = 0; m < strand->GetChildCount(); ++m) {
                auto* node = strand->GetNthChild(m);
                if (node == nullptr || !node->GetMapping().empty()) continue;

                for (const auto& src : available) {
                    if (selectMapAvail && !src.selected) continue;
                    if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                    if (src.canonicalName.rfind(vendorPrefix, 0) != 0) continue;
                    if (SplitSlash(src.canonicalName).size() != 3) continue;

                    node->Map(src.displayName, "Node");
                    node->SetMappingRule(ruleLabel);
                    used.insert(Lower(Trim(src.displayName)));
                    break;
                }
            }
        }
    }
}

void RunSiblingReuseBackfill(const std::vector<ImportMappingNode*>& roots,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel) {
    bool selectMapTarget = selectOnly && !selectedTargets.empty();

    for (auto* model : roots) {
        if (model == nullptr || model->IsGroup() || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;

        const std::string targetType = Lower(Trim(model->GetModelType()));
        if (targetType.empty()) continue;

        const std::string targetName = model->GetCoreModel();
        auto targetBase = FuzzyBaseTokens(targetName);
        auto targetSide = FuzzySideSignature(targetName);
        if (targetBase.empty()) continue;

        for (auto* sibling : roots) {
            if (sibling == nullptr || sibling == model || sibling->IsGroup()) continue;
            if (sibling->GetMapping().empty()) continue;
            if (Lower(Trim(sibling->GetModelType())) != targetType) continue;

            const std::string siblingName = sibling->GetCoreModel();
            if (FuzzyBaseTokens(siblingName) != targetBase) continue;
            if (FuzzySideSignature(siblingName) != targetSide) continue;

            model->Map(sibling->GetMapping(), "Model");
            model->SetMappingRule(ruleLabel);
            break;
        }
    }
}

namespace {

// True if `m` is a DMX model, or a ModelGroup that (recursively) contains
// at least one DMX model. `visited` guards against group cycles.
bool ContainsDMXProp(Model* m, RenderContext& renderContext, std::set<std::string>& visited) {
    if (m == nullptr) return false;
    if (m->IsDMXModel()) return true;
    if (m->GetDisplayAs() != DisplayAsType::ModelGroup) return false;
    if (!visited.insert(m->GetName()).second) return false;

    auto* grp = dynamic_cast<ModelGroup*>(m);
    if (grp == nullptr) return false;
    for (const auto& name : grp->ModelNames()) {
        if (ContainsDMXProp(renderContext.GetModel(name), renderContext, visited)) return true;
    }
    return false;
}

} // anonymous namespace

void RunSkipDMX(const std::vector<ImportMappingNode*>& roots,
                RenderContext& renderContext,
                const std::string& ruleLabel) {
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty() || model->IsSkipped()) continue;

        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());
        if (layoutModel == nullptr) continue;

        std::set<std::string> visited;
        if (ContainsDMXProp(layoutModel, renderContext, visited)) {
            model->SetSkipped(true);
            model->SetMappingRule(ruleLabel);
        }
    }
}

void RunGroupCoverageSkip(const std::vector<ImportMappingNode*>& roots,
                          RenderContext& renderContext,
                          const std::string& ruleLabel) {
    for (auto* group : roots) {
        if (group == nullptr || !group->IsGroup()) continue;
        if (group->GetMapping().empty()) continue;

        Model* layoutModel = renderContext.GetModel(group->GetCoreModel());
        auto* grp = dynamic_cast<ModelGroup*>(layoutModel);
        if (grp == nullptr) continue;

        auto memberNames = grp->ModelNames();
        if (memberNames.size() < 2) continue;

        // The group counts as "all one model type" if every member's fuzzy
        // base tokens (family, ignoring side/number) are identical and
        // non-empty.
        std::unordered_set<std::string> commonBase;
        bool homogeneous = true;
        for (const auto& name : memberNames) {
            auto base = FuzzyBaseTokens(name);
            if (base.empty()) { homogeneous = false; break; }
            if (commonBase.empty()) {
                commonBase = base;
            } else if (base != commonBase) {
                homogeneous = false;
                break;
            }
        }
        if (!homogeneous) continue;

        std::set<std::string> memberSet;
        for (const auto& name : memberNames) memberSet.insert(Lower(Trim(name)));

        for (auto* root : roots) {
            if (root == nullptr || root == group || root->IsGroup()) continue;
            if (!root->GetMapping().empty() || root->IsSkipped()) continue;
            if (memberSet.count(Lower(Trim(root->GetCoreModel()))) != 0) {
                root->SetSkipped(true);
                root->SetMappingRule(ruleLabel);
            }
        }
    }
}

} // namespace AutoMapper
