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
#include <spdlog/fmt/fmt.h>

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
    { "tree", { "tree", "mega", "flaketree", "megatree" } },
    { "star", { "star", "stickstar", "stick" } },
    { "snowflake", { "flake", "snowflake" } },
    { "arch", { "arch", "arches" } },
    { "outline", { "outline", "eaves", "garage", "roof", "vertical", "window", "frame" } },
    { "matrix", { "matrix" } },
    { "spinner", { "spinner" } },
    { "bulb", { "bulb", "bigbulb", "c9" } },
    { "flood", { "flood", "floodhouse" } },
    { "wreath", { "wreath" } },
    { "stake", { "stake", "pixstake", "mickeystake" } },
    // The remaining families below are primarily populated via [T:...]
    // type-hint tags (see ParseTypeHintAliases) rather than common naming
    // conventions, but are still recognized if they appear as plain tokens.
    { "cube", { "cube" } },
    { "cross", { "cross" } },
    { "icicles", { "icicles" } },
    { "line", { "line" } },
    { "sphere", { "sphere" } },
    { "tunetosign", { "tune" } },
};

// [T:Xxx] / [T:Xxx_Yyy] type-hint tag (lowercased, matched case-insensitively)
// -> alias-like text. A "[T:Matrix]" in a model's Description is treated
// exactly as if the model also had an alias of "Matrix" - it feeds the same
// alias-based (Phase 10) and family-based (Fuzzy/GroupMemberDimension)
// matching as a real <alias> entry. Multi-word values use spaces so
// FuzzyTokens splits them into the individual family keywords (e.g.
// "Matrix Column" -> {"matrix", "column"}, matching the "matrix" family on
// the "matrix" token). Unrecognized tags are ignored.
const std::unordered_map<std::string, std::string> TYPE_HINT_ALIAS_TEXT = {
    { "arch", "Arch" },
    { "candycane", "Candy Cane" },
    { "cross", "Cross" },
    { "cube", "Cube" },
    { "flood", "Flood" },
    { "icicles", "Icicles" },
    { "line", "Line" },
    { "matrix", "Matrix" },
    { "matrix_horizontal", "Matrix Horizontal" },
    { "matrix_column", "Matrix Column" },
    { "matrix_pole", "Matrix Pole" },
    { "megatree", "Mega Tree" },
    { "snowflake", "Snowflake" },
    { "sphere", "Sphere" },
    { "spinner", "Spinner" },
    { "star", "Star" },
    { "tuneto", "Tune To" },
    { "tree", "Tree" },
    { "windowframe", "Window Frame" },
};

// Families that should never be considered a match for each other, even
// when no family overlaps (used as the "incompatible pairs" guardrail).
const std::set<std::pair<std::string, std::string>> FUZZY_INCOMPATIBLE_FAMILIES = {
    { "bulb", "star" }, { "bulb", "snowflake" }, { "bulb", "flood" }, { "bulb", "tree" }, { "bulb", "arch" },
    { "cane", "outline" }, { "cane", "flood" },
    { "flood", "tree" }, { "flood", "star" }, { "flood", "snowflake" },
};

// Lowercase, replace separators with spaces, strip anything that isn't
// alphanumeric/space, then collapse whitespace. Also splits a camelCase word
// boundary (an uppercase letter immediately following a lowercase letter or
// digit) into a space, so a compound name with no real separator - e.g.
// "MegaTree", "MiniTree1Star", "GarageSnowFlakeLeft" - still tokenizes into
// its component words ("mega tree", "mini tree1 star", "garage snow flake
// left"). Without this, family keywords too short for the substring-fallback
// below (e.g. "star"/"tree"/"mega", all under 5 chars) can never match inside
// such a compound, and side tags like "left"/"l" never get recognized either.
std::string FuzzyNormalize(const std::string& in) {
    std::string out;
    out.reserve(in.size() + 8);
    bool lastWasSpace = false;
    char prevOrig = '\0';
    for (char c : in) {
        bool isUpper = (c >= 'A' && c <= 'Z');
        bool prevLowerOrDigit = (prevOrig >= 'a' && prevOrig <= 'z') || (prevOrig >= '0' && prevOrig <= '9');
        if (isUpper && prevLowerOrDigit && !out.empty() && !lastWasSpace) {
            out.push_back(' ');
            lastWasSpace = true;
        }
        prevOrig = c;

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
// table; spinner/star and spinner/snowflake are special-cased as
// co-existing naming conventions (e.g. "Spinner Star"). Any other
// non-overlapping pair of recognized families is incompatible.
bool FamiliesCompatible(const std::unordered_set<std::string>& fa, const std::unordered_set<std::string>& fb) {
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
    if ((fa.count("spinner") && (fb.count("star") || fb.count("snowflake"))) ||
        (fb.count("spinner") && (fa.count("star") || fa.count("snowflake")))) {
        return true;
    }
    return false;
}

// Union of FuzzyModelFamilies(name) and FuzzyModelFamilies(alias) for each
// alias - lets [T:...] type-hint tags (carried as alias-like strings, see
// ParseTypeHintAliases) and real aliases broaden a model's recognized
// families for family-compatibility checks, exactly as if the model had
// been named after the hint/alias.
std::unordered_set<std::string> EffectiveModelFamilies(const std::string& name, const std::vector<std::string>& aliases,
                                                         const std::string& displayType = "") {
    auto families = FuzzyModelFamilies(name);
    for (const auto& alias : aliases) {
        auto aliasFamilies = FuzzyModelFamilies(alias);
        families.insert(aliasFamilies.begin(), aliasFamilies.end());
    }
    // Structural fallback: a name-token miss (e.g. "MegaTree" with no space,
    // which doesn't exactly match the "tree"/"mega" keywords) shouldn't leave
    // a real spinning tree with no recognized family at all - that makes
    // FamiliesCompatible's permissive-when-empty rule treat it as compatible
    // with literally anything. AutoMapper::IsTreeLikeModel checks the actual
    // DisplayAs value, independent of name tokenization.
    if (AutoMapper::IsTreeLikeModel(displayType)) families.insert("tree");
    return families;
}

std::unordered_set<std::string> EffectiveModelFamilies(const std::string& name, const std::list<std::string>& aliases,
                                                         const std::string& displayType = "") {
    auto families = FuzzyModelFamilies(name);
    for (const auto& alias : aliases) {
        auto aliasFamilies = FuzzyModelFamilies(alias);
        families.insert(aliasFamilies.begin(), aliasFamilies.end());
    }
    if (AutoMapper::IsTreeLikeModel(displayType)) families.insert("tree");
    return families;
}

} // namespace

namespace AutoMapper {

std::vector<std::string> ParseTypeHintAliases(const std::string& description) {
    std::vector<std::string> result;
    static const std::regex tagRe(R"(\[T:([A-Za-z_]+)\])");
    for (auto it = std::sregex_iterator(description.begin(), description.end(), tagRe); it != std::sregex_iterator(); ++it) {
        auto found = TYPE_HINT_ALIAS_TEXT.find(Lower((*it)[1].str()));
        if (found != TYPE_HINT_ALIAS_TEXT.end()) result.push_back(found->second);
    }
    return result;
}

} // namespace AutoMapper

namespace {

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

bool IsAStarModel(const std::string& name, bool isGroup) {
    // Unlike a spinning tree or matrix panel, a star prop has no dedicated
    // DisplayAs/modelClass - it's just a generic Custom/PolyLine model with
    // an arbitrary node layout, so there's no structural signal to key off
    // of (see IsTreeLikeModel/IsMatrixLikeModel). The only available signal
    // is the name itself, via the same "star" family keywords (star,
    // stickstar, stick) already used by FamiliesCompatible for fuzzy-match
    // gating. A ModelGroup is never itself a star model, same exclusion
    // reasoning as IsMatrixLikeModel(isGroup).
    if (isGroup) return false;
    return FuzzyModelFamilies(name).count("star") != 0;
}

bool IsASnowflakeModel(const std::string& name, bool isGroup) {
    // Split out from IsAStarModel - "star" and "snowflake" are related but
    // distinct naming families (see FUZZY_FAMILY_KEYWORDS), so a model
    // named "Snowflake 1" should not be reported as a star candidate and
    // vice versa. Same name-only/no-structural-signal reasoning as
    // IsAStarModel, and same group exclusion as IsMatrixLikeModel(isGroup).
    if (isGroup) return false;
    return FuzzyModelFamilies(name).count("snowflake") != 0;
}

bool IsTreeLikeModel(const std::string& displayType) {
    // A spinning tree shows up in two DisplayAs formats: the old combined
    // form ("Tree 180"/"Tree 360", shape+rotation baked into the string -
    // StartsWith "Tree ") and the new form (bare "Tree", with the
    // shape/rotation broken out into a separate attribute). Both must be
    // recognized here since Model::DetermineClass only handles the old
    // format (grouping it under the same "Matrix" modelClass as a real
    // Horiz/Vert Matrix panel) and leaves the new bare "Tree" unclassified.
    return displayType == "Tree" || StartsWith(displayType, "Tree ");
}

bool IsMegaTreeModel(const std::string& displayType, int nodeCount) {
    // A spinning "mega tree" (e.g. 16+ strands x hundreds of nodes each) is a
    // fundamentally different physical prop from a small decorative tree
    // (e.g. a single-strand "flat"/ribbon tree with the same or even longer
    // per-strand pixel run) - matching one to the other produces a nonsense
    // effect mapping even though both pass IsTreeLikeModel. Total node count
    // (NumStrings x NodesPerString) is the reliable discriminator here, not
    // strand length alone: a "FlatTree" can have NodesPerString=80 (over a
    // naive per-strand-length threshold) but only 1 strand, 80 nodes total,
    // while a real mega tree's per-strand length is similar but multiplied
    // across many strands into the thousands.
    return IsTreeLikeModel(displayType) && nodeCount >= 1000;
}

bool IsMatrixLikeModel(const std::string& modelClass, const std::string& displayType, int width, int height, int nodeCount, int depth, bool isGroup) {
    // A ModelGroup is never matrix-like, even one whose own modelClass or
    // bounding-box dimensions happen to look right - a group's
    // width/height/nodeCount are aggregated/bounding-box values across all
    // its members (e.g. an "everything" group like "02 All No Forest No
    // MT"), not a real grid, and can coincidentally satisfy the
    // dimension-based ratio check below for completely unrelated reasons.
    if (isGroup) return false;
    // A spinning tree (see IsTreeLikeModel), a Cube model (DisplayAs
    // "Cube"), and a Sphere model (DisplayAs "Sphere") are never a matrix,
    // checked by displayType directly rather than modelClass, because
    // Model::DetermineClass doesn't classify a bare "Tree", "Cube", or
    // "Sphere" at all - all three fall through to modelClass "", which
    // would otherwise let them slip into the dimension-based fallback below
    // (e.g. a Cube's CubeWidth x CubeHeight x its node count, or a Sphere's
    // similarly-derived width/height x node count, is numerically
    // indistinguishable from a flat matrix grid).
    if (IsTreeLikeModel(displayType)) return false;
    if (displayType == "Cube" || StartsWith(displayType, "Cube ")) return false;
    if (displayType == "Sphere" || StartsWith(displayType, "Sphere ")) return false;

    if (modelClass == "Matrix") return true;
    // Any other recognized class (e.g. "Line" for Arches/Single Line/Poly
    // Line/Candy Canes/Circle/Window Frame, "SingingFace", "SpiralTree") is
    // definitively NOT a matrix, regardless of its width/height/nodeCount -
    // a "Triple Arches" model is laid out as 3 strings x N pixels, which is
    // numerically indistinguishable from a 3xN matrix grid by dimensions
    // alone. The dimension-based fallback below is only meaningful for
    // models with no recognized class at all (plain "Custom" models, or a
    // bare DisplayAs="Matrix" which Model::DetermineClass doesn't classify).
    // TODO: Model::DetermineClass has no dedicated classification for a
    // generic Custom model (it falls through to "" unless it's a singing
    // face) - revisit giving plain Custom models their own recognized class
    // (or at least a sub-classification, e.g. "Custom:Matrix" vs.
    // "Custom:Outline" vs. "Custom:Sparse") so this dimension-based guess
    // isn't the only signal available for them.
    if (!modelClass.empty()) return false;
    // A volumetric model (e.g. a 3D Cube/Cylinder, depth > 1) is never a
    // flat matrix, regardless of its width/height/nodeCount - same
    // depth-guard spirit as the dimension-based checks in Phase 105/110/115/120.
    if (depth > 1) return false;
    if (width <= 1 || height <= 1 || nodeCount <= 0) return false;
    double full = static_cast<double>(width) * static_cast<double>(height);
    double ratio = static_cast<double>(nodeCount) / full;
    return ratio >= 0.5 && ratio <= 1.0;
}

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
                      std::unordered_set<std::string> candTokens,
                      const std::list<std::string>& targetAliases = {},
                      const std::vector<std::string>& candAliases = {}) {
    if (!FamiliesCompatible(EffectiveModelFamilies(target, targetAliases), EffectiveModelFamilies(candidate, candAliases))) {
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
                const std::list<std::string>& aliases) {
    return FuzzyTokenMatch(target, candidate, FuzzyTokens(target), FuzzyTokens(candidate), aliases);
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

    // Debug dump: every star-family vendor source and destination root this
    // phase can see (by name, since stars have no DisplayAs/modelClass
    // signal - see IsAStarModel), regardless of mapped/skipped/selected
    // state, so a QuikMap run's spdlog log shows what was/wasn't recognized
    // as a star on each side when reviewing a group-content fuzzy match.
    std::vector<std::string> starSourceLines, starDestinationLines;
    DescribeStarCandidates(roots, available, starSourceLines, starDestinationLines);
    for (const auto& line : starSourceLines) spdlog::info("QuikMap {}: source star candidate {}", ruleLabel, line);
    for (const auto& line : starDestinationLines) spdlog::info("QuikMap {}: destination star candidate {}", ruleLabel, line);

    // Same debug dump for the related-but-distinct snowflake family (see
    // IsASnowflakeModel) - split out from star so a "Snowflake 1" model
    // isn't reported as a star candidate or vice versa.
    std::vector<std::string> snowflakeSourceLines, snowflakeDestinationLines;
    DescribeSnowflakeCandidates(roots, available, snowflakeSourceLines, snowflakeDestinationLines);
    for (const auto& line : snowflakeSourceLines) spdlog::info("QuikMap {}: source snowflake candidate {}", ruleLabel, line);
    for (const auto& line : snowflakeDestinationLines) spdlog::info("QuikMap {}: destination snowflake candidate {}", ruleLabel, line);

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

            if (!FuzzyTokenMatch(targetName, src.displayName, targetTokens, candTokens, model->GetAliases(), src.aliases)) continue;

            model->Map(src.displayName, src.modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(src.displayName)));
            break;
        }
    }
}

void RunFamilyAnchoredFuzzy(const std::vector<ImportMappingNode*>& roots,
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

    // Debug dump: every tree-like vendor source and destination root this
    // phase can see (by name/type, regardless of mapped/skipped/selected
    // state), with its node count and IsMegaTreeModel verdict, so a QuikMap
    // run's spdlog log shows exactly which trees were/weren't classified as
    // a mega tree when reviewing a family-anchored fuzzy match.
    std::vector<std::string> megaTreeSourceLines, megaTreeDestinationLines;
    DescribeMegaTreeCandidates(roots, available, megaTreeSourceLines, megaTreeDestinationLines);
    for (const auto& line : megaTreeSourceLines) spdlog::info("QuikMap {}: source mega-tree candidate {}", ruleLabel, line);
    for (const auto& line : megaTreeDestinationLines) spdlog::info("QuikMap {}: destination mega-tree candidate {}", ruleLabel, line);

    // Seed "used" with anything already mapped so this pass doesn't hand out
    // a source another phase already used.
    std::unordered_set<std::string> used;
    for (auto* r : roots) {
        if (r != nullptr && !r->GetMapping().empty()) used.insert(Lower(Trim(r->GetMapping())));
    }

    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;

        const std::string targetName = model->GetCoreModel();
        auto targetFamilies = EffectiveModelFamilies(targetName, model->GetAliases(), model->GetModelType());
        // No recognized family token on the destination - nothing to anchor
        // a relaxed-Jaccard match on, so leave it for the catch-all phases.
        if (targetFamilies.empty()) continue;

        auto targetNum = FuzzyNumericSignature(targetName);
        auto targetSide = FuzzySideSignature(targetName);
        const int targetNodes = model->GetNodeCount();
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        const double targetAspect = (targetWidth > 0 && targetHeight > 0)
            ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
            : 0.0;
        const std::string targetType = Lower(Trim(model->GetModelType()));
        const bool targetIsSinging = model->IsSingingProp();

        const AvailableSource* best = nullptr;
        double bestScore = 0.0;
        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            bool srcIsGroup = (src.modelType == "ModelGroup");
            if (srcIsGroup != model->IsGroup()) continue;
            // A singing prop's vendor mapping carries face/mouth-movement
            // effect data that's meaningless on a non-singing destination
            // (and vice versa) - e.g. "FlatTree-3" must not match vendor
            // "Singing Tree Female" just because both share the "tree"
            // family anchor token. Same guard as the sibling-mapping-reuse
            // pass below (RunLikeModelBackfill).
            if (src.isSingingProp != targetIsSinging) continue;

            // A "mega tree" (IsMegaTreeModel, total node count >= 1000) must
            // not match a small decorative tree and vice versa, even though
            // both anchor on the shared "tree" family token below - e.g.
            // "FlatTree-1" (1 strand x 80 nodes) must not match vendor
            // "MegaTree" (32 strands x 400 nodes) just because both are
            // IsTreeLikeModel.
            if (IsTreeLikeModel(model->GetModelType()) && IsTreeLikeModel(src.displayType) &&
                IsMegaTreeModel(model->GetModelType(), model->GetNodeCount()) != IsMegaTreeModel(src.displayType, src.nodeCount)) continue;

            // A native Star model (DisplayAs="Star", e.g. a tree's star-
            // topper "FlatTreeStar-N") must never match a Tree-classified
            // vendor source, or vice versa - the destination name often
            // contains "Tree" (it's named after its parent tree), which
            // gives it the "tree" family token too and lets it anchor
            // against a vendor tree below, but DisplayAs is unambiguous and
            // always wins over an incidental name token.
            if ((model->GetModelType() == "Star" && IsTreeLikeModel(src.displayType)) ||
                (IsTreeLikeModel(model->GetModelType()) && src.displayType == "Star")) continue;

            // The anchor: a genuinely shared recognized family token (e.g.
            // "matrix"), not just FamiliesCompatible's permissive-when-empty
            // rule - this is what lets this phase skip the strict overall
            // token-Jaccard requirement that Phase 25/26/27 still enforce.
            auto srcFamilies = EffectiveModelFamilies(src.displayName, src.aliases, src.displayType);
            bool sharedFamily = false;
            for (const auto& f : targetFamilies) {
                if (srcFamilies.count(f)) { sharedFamily = true; break; }
            }
            if (!sharedFamily) continue;

            // Same numeric/side guardrails as MatchFuzzy - a shared family
            // token doesn't override an explicit conflicting number/side
            // (e.g. "Arch 1" must still not match a vendor "Arch 5").
            auto srcNum = FuzzyNumericSignature(src.displayName);
            if (!targetNum.empty() && !srcNum.empty() && targetNum != srcNum) continue;
            auto srcSide = FuzzySideSignature(src.displayName);
            if (!targetSide.empty() && !srcSide.empty() && targetSide != srcSide) continue;

            // Among family-anchored candidates, prefer the one that's
            // dimensionally closest rather than the one whose number happens
            // to textually coincide - a vendor's own enumeration (e.g.
            // "Matrix 2") has no real relationship to the destination's
            // numbering scheme, but node-count/aspect similarity does
            // reflect whether they're actually the same physical prop.
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

void Run(const std::vector<ImportMappingNode*>& roots,
         const std::vector<AvailableSource>& available,
         RenderContext& renderContext,
         MatcherFn lambda_model, MatcherFn lambda_strand, MatcherFn lambda_node,
         const std::string& extra1, const std::string& extra2,
         const std::string& mg,
         bool selectOnly,
         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
         const std::string& ruleLabel,
         AvailableKindFilter kindFilter,
         bool allowSharedSource) {
    bool selectMapAvail = false;
    bool selectMapTarget = false;
    if (selectOnly) {
        for (const auto& a : available) {
            if (a.selected) { selectMapAvail = true; break; }
        }
        selectMapTarget = !selectedTargets.empty();
    }

    // Vendor model/group names already claimed by a destination root, so
    // this pass (and later destinations within this same pass) doesn't hand
    // out the same bare-model source twice - e.g. once "Mega Tree" is mapped
    // to "AA Mega Tree", it shouldn't also get claimed by "Group - Mega
    // Trees" later in this phase or in a later phase. Seeded from whatever
    // is already mapped (by an earlier phase), then updated as this pass
    // maps things. Not consulted at all when allowSharedSource is set (see
    // Run()'s doc comment - Phase 10/Alias deliberately allows fan-out).
    std::unordered_set<std::string> usedModelSources;
    if (!allowSharedSource) {
        for (auto* r : roots) {
            if (r != nullptr && !r->GetMapping().empty()) {
                usedModelSources.insert(Lower(Trim(r->GetMapping())));
            }
        }
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
                if (kindFilter != AvailableKindFilter::Any) {
                    bool srcIsGroup = (src.modelType == "ModelGroup");
                    bool sameKind = (model->IsGroup() == srcIsGroup);
                    if (kindFilter == AvailableKindFilter::SameKindOnly && !sameKind) continue;
                    if (kindFilter == AvailableKindFilter::CrossKindOnly && sameKind) continue;
                }
                if (!allowSharedSource && usedModelSources.count(availName) != 0) continue;
                if (model->GetMapping().empty() &&
                    lambda_model(model->GetCoreModel(), availName, extra1, extra2, aliases)) {
                    model->Map(src.displayName, src.modelType);
                    model->SetMappingRule(ruleLabel);
                    if (!allowSharedSource) usedModelSources.insert(availName);
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
            // 3D models must not donate mappings to flat models and vice versa.
            if (sibling->GetDepth() > 1 || model->GetDepth() > 1) {
                if (sibling->GetDepth() != model->GetDepth()) continue;
            }

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
            if (sibling->GetDepth() > 1 || model->GetDepth() > 1) {
                if (sibling->GetDepth() != model->GetDepth()) continue;
            }

            const std::string siblingName = sibling->GetCoreModel();
            if (FuzzyBaseTokens(siblingName) != targetBase) continue;
            if (FuzzySideSignature(siblingName) != targetSide) continue;

            model->Map(sibling->GetMapping(), "Model");
            model->SetMappingRule(ruleLabel);
            break;
        }
    }
}

void DescribeMatrixCandidates(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              std::vector<std::string>& outSourceLines,
                              std::vector<std::string>& outDestinationLines) {
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (!IsMatrixLikeModel(src.modelClass, src.displayType, src.width, src.height, src.nodeCount, src.depth, src.modelType == "ModelGroup")) continue;
        outSourceLines.push_back(fmt::format("'{}' (type={}, class={}, {}x{}, {} nodes, group={})",
                                              src.displayName, src.displayType, src.modelClass, src.width, src.height,
                                              src.nodeCount, src.modelType == "ModelGroup"));
    }
    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!IsMatrixLikeModel(model->GetModelClass(), model->GetModelType(), model->GetWidth(), model->GetHeight(), model->GetNodeCount(), model->GetDepth(), model->IsGroup())) continue;
        outDestinationLines.push_back(fmt::format("'{}' (type={}, class={}, {}x{}, {} nodes, group={}, mapped={}, skipped={})",
                                                    model->GetCoreModel(), model->GetModelType(), model->GetModelClass(),
                                                    model->GetWidth(), model->GetHeight(), model->GetNodeCount(), model->IsGroup(),
                                                    !model->GetMapping().empty(), model->IsSkipped()));
    }
}

void DescribeTreeCandidates(const std::vector<ImportMappingNode*>& roots,
                            const std::vector<AvailableSource>& available,
                            std::vector<std::string>& outSourceLines,
                            std::vector<std::string>& outDestinationLines) {
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (!IsTreeLikeModel(src.displayType)) continue;
        outSourceLines.push_back(fmt::format("'{}' (type={}, class={}, {}x{}, {} nodes, group={})",
                                              src.displayName, src.displayType, src.modelClass, src.width, src.height,
                                              src.nodeCount, src.modelType == "ModelGroup"));
    }
    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!IsTreeLikeModel(model->GetModelType())) continue;
        outDestinationLines.push_back(fmt::format("'{}' (type={}, class={}, {}x{}, {} nodes, group={}, mapped={}, skipped={})",
                                                    model->GetCoreModel(), model->GetModelType(), model->GetModelClass(),
                                                    model->GetWidth(), model->GetHeight(), model->GetNodeCount(), model->IsGroup(),
                                                    !model->GetMapping().empty(), model->IsSkipped()));
    }
}

void DescribeStarCandidates(const std::vector<ImportMappingNode*>& roots,
                            const std::vector<AvailableSource>& available,
                            std::vector<std::string>& outSourceLines,
                            std::vector<std::string>& outDestinationLines) {
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (!IsAStarModel(src.displayName, src.modelType == "ModelGroup")) continue;
        outSourceLines.push_back(fmt::format("'{}' (type={}, group={})",
                                              src.displayName, src.displayType.empty() ? src.modelType : src.displayType,
                                              src.modelType == "ModelGroup"));
    }
    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!IsAStarModel(model->GetCoreModel(), model->IsGroup())) continue;
        outDestinationLines.push_back(fmt::format("'{}' (type={}, group={}, mapped={}, skipped={})",
                                                    model->GetCoreModel(), model->GetModelType(), model->IsGroup(),
                                                    !model->GetMapping().empty(), model->IsSkipped()));
    }
}

void DescribeSnowflakeCandidates(const std::vector<ImportMappingNode*>& roots,
                                 const std::vector<AvailableSource>& available,
                                 std::vector<std::string>& outSourceLines,
                                 std::vector<std::string>& outDestinationLines) {
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (!IsASnowflakeModel(src.displayName, src.modelType == "ModelGroup")) continue;
        outSourceLines.push_back(fmt::format("'{}' (type={}, group={})",
                                              src.displayName, src.displayType.empty() ? src.modelType : src.displayType,
                                              src.modelType == "ModelGroup"));
    }
    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!IsASnowflakeModel(model->GetCoreModel(), model->IsGroup())) continue;
        outDestinationLines.push_back(fmt::format("'{}' (type={}, group={}, mapped={}, skipped={})",
                                                    model->GetCoreModel(), model->GetModelType(), model->IsGroup(),
                                                    !model->GetMapping().empty(), model->IsSkipped()));
    }
}

void DescribeMegaTreeCandidates(const std::vector<ImportMappingNode*>& roots,
                                const std::vector<AvailableSource>& available,
                                std::vector<std::string>& outSourceLines,
                                std::vector<std::string>& outDestinationLines) {
    for (const auto& src : available) {
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (src.modelType == "ModelGroup") continue;
        if (!IsTreeLikeModel(src.displayType)) continue;
        bool mega = IsMegaTreeModel(src.displayType, src.nodeCount);
        outSourceLines.push_back(fmt::format("{}'{}' (type={}, nodeCount={}, mega={})",
                                              mega ? "**MEGA TREE** " : "", src.displayName, src.displayType,
                                              src.nodeCount, mega));
    }
    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped() || model->IsGroup()) continue;
        if (!IsTreeLikeModel(model->GetModelType())) continue;
        bool mega = IsMegaTreeModel(model->GetModelType(), model->GetNodeCount());
        outDestinationLines.push_back(fmt::format("{}'{}' (type={}, nodeCount={}, mega={}, mapped={}, skipped={})",
                                                    mega ? "**MEGA TREE** " : "", model->GetCoreModel(), model->GetModelType(),
                                                    model->GetNodeCount(), mega,
                                                    !model->GetMapping().empty(), model->IsSkipped()));
    }
}

void RunMatrixBackfill(const std::vector<ImportMappingNode*>& roots,
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

    // Debug dump: every matrix-like vendor source and destination root this
    // phase can see, regardless of mapped/skipped/selected state, so a
    // QuikMap run's spdlog log shows exactly what was/wasn't identified as a
    // matrix on each side. Same data is surfaced in the QuikMap detail
    // report via DescribeMatrixCandidates - see
    // xLightsImportChannelMapDialog::GenerateQuikMapMatrixCandidateReport().
    std::vector<std::string> sourceLines, destinationLines;
    DescribeMatrixCandidates(roots, available, sourceLines, destinationLines);
    for (const auto& line : sourceLines) spdlog::info("QuikMap Phase 93: source matrix candidate {}", line);
    for (const auto& line : destinationLines) spdlog::info("QuikMap Phase 93: destination matrix candidate {}", line);

    // Same debug dump for tree-like sources/destinations - these are
    // explicitly excluded from matrix classification (see IsTreeLikeModel),
    // logged here too so a run's spdlog log confirms a spinning tree wasn't
    // accidentally swept into the matrix pool above.
    std::vector<std::string> treeSourceLines, treeDestinationLines;
    DescribeTreeCandidates(roots, available, treeSourceLines, treeDestinationLines);
    for (const auto& line : treeSourceLines) spdlog::info("QuikMap Phase 93: source tree candidate (excluded from matrix) {}", line);
    for (const auto& line : treeDestinationLines) spdlog::info("QuikMap Phase 93: destination tree candidate (excluded from matrix) {}", line);

    // Pool of matrix-like vendor sources (model<->model only - groups are
    // never matrix-like, see IsMatrixLikeModel), used round-robin once every
    // still-unused one has been claimed.
    std::vector<const AvailableSource*> pool;
    for (const auto& src : available) {
        if (selectMapAvail && !src.selected) continue;
        if (src.canonicalName.find('/') != std::string::npos) continue;
        if (!IsMatrixLikeModel(src.modelClass, src.displayType, src.width, src.height, src.nodeCount, src.depth, src.modelType == "ModelGroup")) continue;
        pool.push_back(&src);
    }
    if (pool.empty()) return;

    // Names already claimed by an earlier phase - those pool members start
    // "used" so an actually-unclaimed matrix source is preferred first.
    std::unordered_map<const AvailableSource*, int> useCount;
    for (auto* src : pool) {
        bool alreadyUsed = false;
        for (auto* model : roots) {
            if (model == nullptr || model->GetMapping().empty()) continue;
            if (Lower(Trim(model->GetMapping())) == Lower(Trim(src->displayName))) { alreadyUsed = true; break; }
        }
        useCount[src] = alreadyUsed ? 1 : 0;
    }

    for (auto* model : roots) {
        if (model == nullptr || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!IsMatrixLikeModel(model->GetModelClass(), model->GetModelType(), model->GetWidth(), model->GetHeight(), model->GetNodeCount(), model->GetDepth(), model->IsGroup())) continue;

        const int targetNodes = model->GetNodeCount();
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        const double targetAspect = (targetWidth > 0 && targetHeight > 0)
            ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
            : 0.0;
        const std::string targetType = Lower(Trim(model->GetModelType()));

        const AvailableSource* best = nullptr;
        double bestScore = 0.0;
        const AvailableSource* bestUnused = nullptr;
        double bestUnusedScore = 0.0;
        for (const auto* src : pool) {
            // Both model and src are guaranteed non-group here -
            // IsMatrixLikeModel(isGroup=true) always returns false, so the
            // pool never contains a ModelGroup and this loop never reaches a
            // group destination either.
            double score = GroupMemberDimensionScore(targetNodes, targetWidth, targetHeight, targetAspect, targetType, *src);
            if (best == nullptr || score < bestScore) {
                best = src;
                bestScore = score;
            }
            if (useCount[src] == 0 && (bestUnused == nullptr || score < bestUnusedScore)) {
                bestUnused = src;
                bestUnusedScore = score;
            }
        }

        const AvailableSource* chosen = bestUnused != nullptr ? bestUnused : best;
        if (chosen != nullptr) {
            model->Map(chosen->displayName, chosen->modelType);
            model->SetMappingRule(ruleLabel);
            ++useCount[chosen];
        }
    }
}

namespace {

// Classifies a destination root into one of the three structural/name
// families recognized elsewhere in QuikMap, or "" if it's none of them.
std::string ClassifyDestinationFamily(ImportMappingNode* node) {
    if (node == nullptr) return std::string();
    if (IsMatrixLikeModel(node->GetModelClass(), node->GetModelType(), node->GetWidth(), node->GetHeight(), node->GetNodeCount(), node->GetDepth(), node->IsGroup())) return "matrix";
    if (IsAStarModel(node->GetCoreModel(), node->IsGroup())) return "star";
    if (IsASnowflakeModel(node->GetCoreModel(), node->IsGroup())) return "snowflake";
    return std::string();
}

// Same classification for a vendor AvailableSource.
std::string ClassifySourceFamily(const AvailableSource& src) {
    bool isGroup = (src.modelType == "ModelGroup");
    if (IsMatrixLikeModel(src.modelClass, src.displayType, src.width, src.height, src.nodeCount, src.depth, isGroup)) return "matrix";
    if (IsAStarModel(src.displayName, isGroup)) return "star";
    if (IsASnowflakeModel(src.displayName, isGroup)) return "snowflake";
    return std::string();
}

// Classifies one member of a live ModelGroup by name. Prefers the member's
// own ImportMappingNode root (if it's tracked as one in `roots`) so matrix
// classification - which needs structural data, not just a name - is
// possible; falls back to a name-only star/snowflake check (the only
// families with a name-only signal) when the member isn't a tracked root.
std::string ClassifyGroupMemberFamily(const std::string& memberName,
                                       const std::unordered_map<std::string, ImportMappingNode*>& rootByName) {
    auto it = rootByName.find(Lower(Trim(memberName)));
    if (it != rootByName.end()) {
        std::string f = ClassifyDestinationFamily(it->second);
        if (!f.empty()) return f;
    }
    if (IsAStarModel(memberName, false)) return "star";
    if (IsASnowflakeModel(memberName, false)) return "snowflake";
    return std::string();
}

} // namespace

void RunFamilyGroupBackfill(const std::vector<ImportMappingNode*>& roots,
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

    std::unordered_map<std::string, ImportMappingNode*> rootByName;
    for (auto* r : roots) {
        if (r != nullptr) rootByName[Lower(Trim(r->GetCoreModel()))] = r;
    }

    // Confirm a family is actually present in this layout by finding at
    // least one still-unmapped destination group whose live members are all
    // the same family - a stronger signal than any single name/structure
    // guess, used to gate the broader backfill below.
    std::unordered_set<std::string> confirmedFamilies;
    for (auto* model : roots) {
        if (model == nullptr || !model->IsGroup() || model->IsSkipped()) continue;
        if (!model->GetMapping().empty()) continue;

        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());
        auto* grp = dynamic_cast<ModelGroup*>(layoutModel);
        if (grp == nullptr) continue;
        auto memberNames = grp->ModelNames();
        if (memberNames.size() < 2) continue;

        std::string family;
        bool homogeneous = true;
        for (const auto& memberName : memberNames) {
            std::string f = ClassifyGroupMemberFamily(memberName, rootByName);
            if (f.empty()) { homogeneous = false; break; }
            if (family.empty()) family = f;
            else if (family != f) { homogeneous = false; break; }
        }
        if (homogeneous && !family.empty()) confirmedFamilies.insert(family);
    }
    if (confirmedFamilies.empty()) return;

    for (const auto& family : confirmedFamilies) {
        std::vector<const AvailableSource*> pool;
        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (ClassifySourceFamily(src) != family) continue;
            pool.push_back(&src);
        }
        if (pool.empty()) continue;

        // Names already claimed by an earlier phase start "used" so an
        // actually-unclaimed source is preferred first, same pattern as
        // Phase 93's matrix pool.
        std::unordered_map<const AvailableSource*, int> useCount;
        for (auto* src : pool) {
            bool alreadyUsed = false;
            for (auto* m : roots) {
                if (m == nullptr || m->GetMapping().empty()) continue;
                if (Lower(Trim(m->GetMapping())) == Lower(Trim(src->displayName))) { alreadyUsed = true; break; }
            }
            useCount[src] = alreadyUsed ? 1 : 0;
        }

        for (auto* model : roots) {
            if (model == nullptr || model->IsSkipped()) continue;
            if (!model->GetMapping().empty()) continue;
            if (selectMapTarget && selectedTargets.count(model) == 0) continue;
            if (ClassifyDestinationFamily(model) != family) continue;

            const int targetNodes = model->GetNodeCount();
            const int targetWidth = model->GetWidth();
            const int targetHeight = model->GetHeight();
            const double targetAspect = (targetWidth > 0 && targetHeight > 0)
                ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
                : 0.0;
            const std::string targetType = Lower(Trim(model->GetModelType()));

            const AvailableSource* best = nullptr;
            double bestScore = 0.0;
            const AvailableSource* bestUnused = nullptr;
            double bestUnusedScore = 0.0;
            for (const auto* src : pool) {
                bool srcIsGroup = (src->modelType == "ModelGroup");
                if (srcIsGroup != model->IsGroup()) continue;
                double score = GroupMemberDimensionScore(targetNodes, targetWidth, targetHeight, targetAspect, targetType, *src);
                if (best == nullptr || score < bestScore) {
                    best = src;
                    bestScore = score;
                }
                if (useCount[src] == 0 && (bestUnused == nullptr || score < bestUnusedScore)) {
                    bestUnused = src;
                    bestUnusedScore = score;
                }
            }

            const AvailableSource* chosen = bestUnused != nullptr ? bestUnused : best;
            if (chosen != nullptr) {
                model->Map(chosen->displayName, chosen->modelType);
                model->SetMappingRule(ruleLabel);
                ++useCount[chosen];
            }
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
        const int targetDepth = model->GetDepth();
        if (targetNodes <= 0) continue;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (src.modelType == "ModelGroup") continue;
            if (Lower(Trim(src.displayType)) != "custom") continue;
            if (src.nodeCount != targetNodes || src.width != targetWidth || src.height != targetHeight) continue;
            if (targetDepth > 1 || src.depth > 1) {
                if (src.depth != targetDepth) continue;
            }

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

        const AvailableSource* best = nullptr;
        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (src.modelType != "ModelGroup") continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (!containsKeyword(src.displayName)) continue;

            if (best == nullptr) {
                best = &src;
                continue;
            }
            bool srcHasEffects = src.effectCount > 0;
            bool bestHasEffects = best->effectCount > 0;
            if (srcHasEffects && !bestHasEffects) {
                best = &src;
            } else if (srcHasEffects == bestHasEffects &&
                       src.groupMemberNames.size() > best->groupMemberNames.size()) {
                best = &src;
            }
        }
        if (best != nullptr) {
            model->Map(best->displayName, best->modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(best->displayName)));
        }
    }

    FillMappedModelChildren(roots, available, selectMapAvail, used, ruleLabel);
}

// Detects whether a model name implies a horizontal or vertical orientation.
// Returns 1 for "horiz…", -1 for "vert…", 0 for neither.
// Matching is on whole-word token prefixes after FuzzyNormalize so that
// "converts" (contains "vert" as a substring, not a token) does not fire.
static int DetectHVOrientation(const std::string& name) {
    const std::string norm = FuzzyNormalize(name);
    std::string tok;
    for (char c : norm + " ") {
        if (c == ' ') {
            if (!tok.empty()) {
                if (tok.starts_with("horiz")) return 1;
                if (tok.starts_with("vert"))  return -1;
                tok.clear();
            }
        } else {
            tok += c;
        }
    }
    return 0;
}

void RunHVGroupMatch(const std::vector<ImportMappingNode*>& roots,
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
        if (model == nullptr || !model->IsGroup() || model->IsSkipped()) continue;
        if (selectMapTarget && selectedTargets.count(model) == 0) continue;
        if (!model->GetMapping().empty()) continue;

        // Detect orientation from name, then aliases.
        int destOri = DetectHVOrientation(model->GetModelName());
        if (destOri == 0) {
            for (const auto& alias : model->GetAliases()) {
                destOri = DetectHVOrientation(alias);
                if (destOri != 0) break;
            }
        }
        if (destOri == 0) continue;

        // Corroborate the name-token guess against the destination group's
        // own members' geometry (see ImportMappingNode::GetGroupGeometricOrientation).
        // When the members' actual layout confidently disagrees with the
        // name (e.g. a stale/mislabeled "...Vertical..." group whose members
        // are really laid out horizontally), don't trust the name at all -
        // skip this destination rather than risk a wrong-orientation match.
        int destGeomOri = model->GetGroupGeometricOrientation();
        if (destGeomOri != 0 && destGeomOri != destOri) continue;

        // Build destination token set for Jaccard scoring.
        auto destTokens = FuzzyTokens(model->GetModelName());

        const AvailableSource* best = nullptr;
        double bestScore = -1.0;
        bool bestGeomConfirmed = false;

        for (const auto& src : available) {
            if (selectMapAvail && !src.selected) continue;
            if (src.canonicalName.find('/') != std::string::npos) continue;
            if (src.modelType != "ModelGroup") continue;
            if (used.count(Lower(Trim(src.displayName))) != 0) continue;
            if (DetectHVOrientation(src.displayName) != destOri) continue;
            // Same constraint on the vendor side: if its members' geometry
            // confidently disagrees with its own name-token orientation,
            // it's not a trustworthy same-orientation candidate.
            if (src.groupGeomOrientation != 0 && src.groupGeomOrientation != destOri) continue;

            // Score by Jaccard similarity of name tokens.
            auto srcTokens = FuzzyTokens(src.displayName);
            int inter = 0;
            for (const auto& t : destTokens) {
                if (srcTokens.count(t)) ++inter;
            }
            int uni = static_cast<int>(destTokens.size() + srcTokens.size()) - inter;
            double score = (uni > 0) ? static_cast<double>(inter) / static_cast<double>(uni) : 0.0;

            // Geometry-confirmed candidates (both sides' members agree with
            // destOri) win ties over ones where geometry is merely unknown.
            bool geomConfirmed = (destGeomOri == destOri) && (src.groupGeomOrientation == destOri);
            if (best == nullptr || score > bestScore ||
                (score == bestScore && geomConfirmed && !bestGeomConfirmed)) {
                best = &src;
                bestScore = score;
                bestGeomConfirmed = geomConfirmed;
            }
        }

        if (best != nullptr) {
            model->Map(best->displayName, best->modelType);
            model->SetMappingRule(ruleLabel);
            used.insert(Lower(Trim(best->displayName)));
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
        const int targetDepth = model->GetDepth();
        const double targetAspect = (targetWidth > 0 && targetHeight > 0)
            ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
            : 0.0;
        const bool targetIsSinging = model->IsSingingProp();

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
            // A singing prop's vendor mapping carries face/mouth-movement
            // effect data that's meaningless on a non-singing destination
            // (and vice versa) - same guard as Phase 28's
            // RunFamilyAnchoredFuzzy. Without this, a singing vendor model
            // (e.g. "SingingSnowman") could be picked for a non-singing
            // destination (e.g. "SpinnerStarFlake") purely because their
            // Custom-model node counts happen to be close.
            if (src.isSingingProp != targetIsSinging) continue;
            // 3D models (depth > 1) must not match flat models — require depths to agree.
            if (targetDepth > 1 || src.depth > 1) {
                if (src.depth != targetDepth) continue;
            }
            // Family guardrail: e.g. don't let a "Snowflake"-family vendor
            // model (recognized via FuzzyModelFamilies, including compound
            // names like "EFlake46"/"ChromaFlake...") match a destination
            // Custom model that is recognizably a different family (and
            // vice versa), even if their node counts happen to be close.
            if (!FamiliesCompatible(EffectiveModelFamilies(targetName, model->GetAliases()), EffectiveModelFamilies(src.displayName, src.aliases))) continue;

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
        // match (Exact/Alias/Community/Fuzzy/GroupContent etc). Show-wide
        // aggregation groups matched by EverythingGroup (Phase 16),
        // SpecialKeyword (Phase 17), or Catchall (Phase 105/120) carry no
        // real per-member correspondence between the two groups' members —
        // using them here would let an unrelated destination model (e.g.
        // "3D Cube-2") steal a vendor source intended for a member of a
        // *different*, meaningfully-matched group (e.g. "Group - Snowflakes").
        if (group->GetMappingRule().find("Catchall") != std::string::npos) continue;
        if (group->GetMappingRule().find("EverythingGroup") != std::string::npos) continue;
        if (group->GetMappingRule().find("SpecialKeywordGroup") != std::string::npos) continue;

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

        // If the vendor group is much larger than the destination group, its
        // members have no real per-member correspondence to the destination
        // group's members (e.g. a name/fuzzy match landed on a huge "All"
        // container group) - dimension-only scoring across such a pool tends
        // to pick wildly unrelated prop types just because node counts happen
        // to be close. Skip this group rather than guess.
        std::set<std::string> vendorMembers;
        for (const auto& name : vendorGroup->groupMemberNames) vendorMembers.insert(Lower(Trim(name)));
        if (vendorMembers.size() > destMembers.size() * 3 + 2) continue;

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
            const auto targetFamilies = EffectiveModelFamilies(model->GetCoreModel(), model->GetAliases(), model->GetModelType());

            const AvailableSource* best = nullptr;
            double bestScore = 0.0;
            for (const auto& src : available) {
                if (selectMapAvail && !src.selected) continue;
                if (src.canonicalName.find('/') != std::string::npos) continue;
                if (src.modelType == "ModelGroup") continue;
                if (used.count(Lower(Trim(src.displayName))) != 0) continue;
                if (vendorMembers.count(Lower(Trim(src.displayName))) == 0) continue;

                // 3D models (depth > 1) must not match flat models and vice
                // versa - same guard as Phase 120's RunCatchAllFallback.
                // Without this, a volumetric Custom model (e.g. a Cube-shaped
                // "MediumPresent-N", CubeDepth > 1) has no recognized
                // modelClass (Cube falls through to "" - see IsMatrixLikeModel)
                // and isn't a "Line", so neither the family guard nor the
                // line-vs-grid/modelClass guards below reject it against a
                // flat grid-shaped vendor source like "MegaTree" just because
                // both happen to be width/height > 1.
                if (model->GetDepth() > 1 || src.depth > 1) {
                    if (model->GetDepth() != src.depth) continue;
                }

                // A "mega tree" (IsMegaTreeModel, total node count >= 1000)
                // must not match a small decorative tree and vice versa, even
                // though both are IsTreeLikeModel and the small tree's own
                // NodesPerString can coincidentally be just as long (see
                // IsMegaTreeModel) - this is what previously let e.g.
                // "FlatTree-1" (1 strand x 80 nodes) be scored against
                // "MegaTree" (32 strands x 400 nodes) purely on dimension
                // closeness.
                if (IsTreeLikeModel(model->GetModelType()) && IsTreeLikeModel(src.displayType) &&
                    IsMegaTreeModel(model->GetModelType(), model->GetNodeCount()) != IsMegaTreeModel(src.displayType, src.nodeCount)) continue;

                // A native Star model (DisplayAs="Star", e.g. a tree's star-
                // topper "FlatTreeStar-N") must never match a Tree-classified
                // vendor source, or vice versa - same guard as Phase 28.
                if ((model->GetModelType() == "Star" && IsTreeLikeModel(src.displayType)) ||
                    (IsTreeLikeModel(model->GetModelType()) && src.displayType == "Star")) continue;

                // A vendor source whose recognized family (including
                // [T:Xxx]-hint/alias families) is incompatible with the
                // destination model's is never a real correspondence within
                // this group, regardless of how close its dimensions are -
                // this is what previously let e.g. "3D Cube-1" be scored
                // against "Matrix 2"/"Snowflake 1" purely on node count.
                if (!FamiliesCompatible(targetFamilies, EffectiveModelFamilies(src.displayName, src.aliases, src.displayType))) continue;

                // Neither side has a recognized family for most outline/line
                // props (e.g. "Driveway - 01L", "FrontYard - Left"), so the
                // family guard above is permissive (empty-vs-anything) and
                // gives them no protection at all - this is what previously
                // let e.g. "FrontYard - Left" or "Driveway - 01L" be scored
                // against "MegaTree" (modelClass "Matrix") purely because
                // both had unknown/zero node counts. Same line-vs-grid and
                // modelClass guards as Phase 120's RunCatchAllFallback.
                bool modelIsGrid = targetWidth > 1 && targetHeight > 1;
                bool srcIsGrid = src.width > 1 && src.height > 1;
                bool modelHasDims = targetWidth > 0 && targetHeight > 0;
                bool srcHasDims = src.width > 0 && src.height > 0;
                bool modelIsLine = model->GetModelClass() == "Line" || (modelHasDims && !modelIsGrid);
                bool srcIsLine = src.modelClass == "Line" || (srcHasDims && !srcIsGrid);
                if ((modelIsLine && srcIsGrid) || (srcIsLine && modelIsGrid)) continue;
                if (!model->GetModelClass().empty() && !src.modelClass.empty() && model->GetModelClass() != src.modelClass) continue;

                // General sanity backstop: when both node counts are known,
                // a >10x size mismatch is never a real per-member
                // correspondence, regardless of what dimension score it
                // happens to produce - this is what previously let e.g. a
                // 50-node "Spiral mini-N" (a plain Custom model with no
                // recognized class - DetermineClass has no branch for a
                // non-spiral, non-singing Custom model) be picked as the
                // "least-bad" remaining candidate against a 12,800-node
                // "MegaTree" once nothing else was left in a small group's
                // vendor pool. Catches this whole class of mismatch without
                // needing a dedicated structural guard per case.
                if (targetNodes > 0 && src.nodeCount > 0) {
                    int lo = std::min(targetNodes, src.nodeCount);
                    int hi = std::max(targetNodes, src.nodeCount);
                    if (hi > lo * 10) continue;
                }

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
        // Same restriction as RunGroupMemberDimensionMatch — only trust
        // name-based group<->group pairings as a member-reuse pool. Show-wide
        // aggregation groups matched by EverythingGroup (Phase 16) or
        // SpecialKeyword (Phase 17) have no real per-member correspondence
        // and must not be used as backfill anchors.
        if (group->GetMappingRule().find("Catchall") != std::string::npos) continue;
        if (group->GetMappingRule().find("EverythingGroup") != std::string::npos) continue;
        if (group->GetMappingRule().find("SpecialKeywordGroup") != std::string::npos) continue;

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

        // Same large-vendor-group guard as RunGroupMemberDimensionMatch - a
        // vendor group much bigger than the destination group has no real
        // per-member correspondence to use as a reuse pool.
        std::set<std::string> vendorMembers;
        for (const auto& name : vendorGroup->groupMemberNames) vendorMembers.insert(Lower(Trim(name)));
        if (vendorMembers.size() > destMembers.size() * 3 + 2) continue;

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

        // Tracks how many destinations this backfill pass has already handed
        // each pool source to, so e.g. "HFlake4"/"HFlake5" don't both pile
        // onto "Snowflake 1" while "Snowflake 2"/"Snowflake 3" sit unused -
        // a never-yet-used, family-compatible candidate is always preferred
        // over reusing one already claimed in this pass, no matter its
        // dimension score. Only once every family-compatible candidate has
        // been used at least once does reuse (picking the best score) kick
        // in, so a destination still gets mapped rather than left unmapped.
        std::unordered_map<const AvailableSource*, int> useCount;
        for (auto* src : pool) useCount[src] = 0;

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
            const auto targetFamilies = EffectiveModelFamilies(model->GetCoreModel(), model->GetAliases(), model->GetModelType());

            const AvailableSource* best = nullptr;
            double bestScore = 0.0;
            const AvailableSource* bestUnused = nullptr;
            double bestUnusedScore = 0.0;
            for (const auto* src : pool) {
                // 3D models (depth > 1) must not match flat models and vice
                // versa - same guard as RunGroupMemberDimensionMatch/Phase 120.
                if (model->GetDepth() > 1 || src->depth > 1) {
                    if (model->GetDepth() != src->depth) continue;
                }

                // Same mega-tree-vs-small-tree guard as RunGroupMemberDimensionMatch.
                if (IsTreeLikeModel(model->GetModelType()) && IsTreeLikeModel(src->displayType) &&
                    IsMegaTreeModel(model->GetModelType(), model->GetNodeCount()) != IsMegaTreeModel(src->displayType, src->nodeCount)) continue;

                // A native Star model (DisplayAs="Star", e.g. a tree's star-
                // topper "FlatTreeStar-N") must never match a Tree-classified
                // vendor source, or vice versa - same guard as Phase 28/110.
                if ((model->GetModelType() == "Star" && IsTreeLikeModel(src->displayType)) ||
                    (IsTreeLikeModel(model->GetModelType()) && src->displayType == "Star")) continue;

                // Same family-compatibility gate as RunGroupMemberDimensionMatch.
                if (!FamiliesCompatible(targetFamilies, EffectiveModelFamilies(src->displayName, src->aliases, src->displayType))) continue;

                // Same line-vs-grid/modelClass guards as RunGroupMemberDimensionMatch
                // and Phase 120's RunCatchAllFallback - the family guard above gives
                // no protection to outline/line props with no recognized family
                // (e.g. "Driveway - 01L", "FrontYard - Left"), which previously let
                // them be scored against "MegaTree" (modelClass "Matrix") purely on
                // unknown/zero node counts.
                bool modelIsGrid = targetWidth > 1 && targetHeight > 1;
                bool srcIsGrid = src->width > 1 && src->height > 1;
                bool modelHasDims = targetWidth > 0 && targetHeight > 0;
                bool srcHasDims = src->width > 0 && src->height > 0;
                bool modelIsLine = model->GetModelClass() == "Line" || (modelHasDims && !modelIsGrid);
                bool srcIsLine = src->modelClass == "Line" || (srcHasDims && !srcIsGrid);
                if ((modelIsLine && srcIsGrid) || (srcIsLine && modelIsGrid)) continue;
                if (!model->GetModelClass().empty() && !src->modelClass.empty() && model->GetModelClass() != src->modelClass) continue;

                // General sanity backstop, same as RunGroupMemberDimensionMatch -
                // a >10x node-count mismatch is never a real correspondence,
                // no matter how "least-bad" it scores against everything else
                // left in the pool.
                if (targetNodes > 0 && src->nodeCount > 0) {
                    int lo = std::min(targetNodes, src->nodeCount);
                    int hi = std::max(targetNodes, src->nodeCount);
                    if (hi > lo * 10) continue;
                }

                double score = GroupMemberDimensionScore(targetNodes, targetWidth, targetHeight, targetAspect, targetType, *src);
                if (best == nullptr || score < bestScore) {
                    best = src;
                    bestScore = score;
                }
                if (useCount[src] == 0 && (bestUnused == nullptr || score < bestUnusedScore)) {
                    bestUnused = src;
                    bestUnusedScore = score;
                }
            }

            const AvailableSource* chosen = bestUnused != nullptr ? bestUnused : best;
            if (chosen != nullptr) {
                model->Map(chosen->displayName, chosen->modelType);
                model->SetMappingRule(ruleLabel);
                ++useCount[chosen];
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
    // Among candidates that pass the kind/depth/family/keyword gates, picks
    // the dimensionally-closest one (via GroupMemberDimensionScore, same
    // scoring Phase 110/115 use) rather than just the first one found in
    // `available`'s order - e.g. so "Matrix Seeds" reliably gets "Matrix 2"
    // over some other equally-arbitrary unmapped Custom model.
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (selectMapAvail || selectMapTarget) {
            bool isTargetSelected = selectedTargets.count(model) != 0;
            if (selectMapTarget && !isTargetSelected) continue;
        }
        if (!model->GetMapping().empty()) continue;
        if (model->IsSkipped()) continue;

        const int targetNodes = model->GetNodeCount();
        const int targetWidth = model->GetWidth();
        const int targetHeight = model->GetHeight();
        const double targetAspect = (targetWidth > 0 && targetHeight > 0)
            ? static_cast<double>(targetWidth) / static_cast<double>(targetHeight)
            : 0.0;
        const std::string targetType = Lower(Trim(model->GetModelType()));
        const auto targetFamilies = EffectiveModelFamilies(model->GetCoreModel(), model->GetAliases(), model->GetModelType());

        const AvailableSource* best = nullptr;
        double bestScore = 0.0;
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

            // 3D models (depth > 1) must not match flat models and vice versa.
            // Applies to Custom 3D models (Depth="N" in XML), CubeModel types, etc.
            if (model->GetDepth() > 1 || src.depth > 1) {
                if (model->GetDepth() != src.depth) continue;
            }

            // A line-like prop (a 1D string/path of nodes) must never be
            // paired with a genuine 2D grid (both width and height > 1) and
            // vice versa - e.g. "Driveway - 01L" must not steal "Matrix 2"
            // (a grid) just because this catch-all otherwise ignores
            // type/name. "Line-like" is modelClass == "Line" (Single Line,
            // Poly Line, Arches, Candy Canes, Circle, Window Frame - see
            // Model::DetermineClass) OR - since many outline/edge props
            // (e.g. a driveway strip) are actually built as a "Custom" model
            // with no recognized class - any model with known dimensions
            // that isn't itself a 2D grid (e.g. a 1-pixel-tall, N-pixel-wide
            // Custom strip). Permissive (neither line-like nor grid) when
            // dimensions are unknown on a side with no recognized class, same
            // as the family guard above.
            bool modelIsGrid = targetWidth > 1 && targetHeight > 1;
            bool srcIsGrid = src.width > 1 && src.height > 1;
            bool modelHasDims = targetWidth > 0 && targetHeight > 0;
            bool srcHasDims = src.width > 0 && src.height > 0;
            bool modelIsLine = model->GetModelClass() == "Line" || (modelHasDims && !modelIsGrid);
            bool srcIsLine = src.modelClass == "Line" || (srcHasDims && !srcIsGrid);
            if ((modelIsLine && srcIsGrid) || (srcIsLine && modelIsGrid)) continue;

            // Different recognized model classes (e.g. "Line" vs "Matrix")
            // are never compatible, regardless of reported width/height -
            // the dimension-based check above can be fooled by a vendor
            // "Horiz Matrix"/"Vert Matrix" configured with NumStrings=1
            // (e.g. "Matrix 2": parm1=1, parm2=300 -> width=1, height=300),
            // which is numerically a 1xN strip indistinguishable from a
            // Single Line model even though it's a different *kind* of prop.
            // Permissive when either side has no recognized class (most
            // Custom models), same spirit as the family guard below.
            if (!model->GetModelClass().empty() && !src.modelClass.empty() && model->GetModelClass() != src.modelClass) continue;

            // Family guardrail - same as Phase 100/110/115: e.g. a "star"-
            // family vendor model (recognized via FuzzyModelFamilies) must
            // not steal a vendor source from a "matrix"-family destination
            // it has no real business matching, even though this catch-all
            // otherwise ignores names. Permissive when either side has no
            // recognized family (most props), so it only blocks genuine
            // cross-family mismatches.
            if (!FamiliesCompatible(targetFamilies, EffectiveModelFamilies(src.displayName, src.aliases, src.displayType))) continue;

            // A "mega tree" (IsMegaTreeModel, total node count >= 1000) must
            // not match a small decorative tree and vice versa - same guard
            // as Phase 28/110/115.
            if (IsTreeLikeModel(model->GetModelType()) && IsTreeLikeModel(src.displayType) &&
                IsMegaTreeModel(model->GetModelType(), model->GetNodeCount()) != IsMegaTreeModel(src.displayType, src.nodeCount)) continue;

            // A native Star model (DisplayAs="Star", e.g. a tree's star-
            // topper "FlatTreeStar-N") must never match a Tree-classified
            // vendor source, or vice versa - same guard as Phase 28/110/115.
            if ((model->GetModelType() == "Star" && IsTreeLikeModel(src.displayType)) ||
                (IsTreeLikeModel(model->GetModelType()) && src.displayType == "Star")) continue;

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
                              const std::vector<AvailableSource>& available,
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
        const bool targetIsSinging = model->IsSingingProp();

        for (auto* sibling : roots) {
            if (sibling == nullptr || sibling == model || sibling->IsGroup()) continue;
            if (sibling->GetMapping().empty()) continue;
            if (Lower(Trim(sibling->GetModelType())) != targetType) continue;
            if (sibling->GetDepth() > 1 || model->GetDepth() > 1) {
                if (sibling->GetDepth() != model->GetDepth()) continue;
            }

            const std::string siblingName = sibling->GetCoreModel();
            if (FuzzyBaseTokens(siblingName) != targetBase) continue;
            if (FuzzySideSignature(siblingName) != targetSide) continue;

            // A singing prop's vendor mapping carries face/mouth-movement
            // effect data that's meaningless on a non-singing destination
            // (and vice versa) - e.g. "FlatTree-3" (not a singing prop)
            // must not reuse sibling "FlatTree-1"'s mapping to "Singing
            // Tree Female" just because they share the same fuzzy base
            // tokens/side. Keep looking for a sibling whose vendor mapping
            // matches this destination's own singing-prop status.
            const std::string siblingVendorName = Lower(Trim(sibling->GetMapping()));
            bool vendorIsSinging = false;
            for (const auto& src : available) {
                if (Lower(Trim(src.displayName)) == siblingVendorName) {
                    vendorIsSinging = src.isSingingProp;
                    break;
                }
            }
            if (vendorIsSinging != targetIsSinging) continue;

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

void RunSkipShadow(const std::vector<ImportMappingNode*>& roots,
                   RenderContext& renderContext,
                   const std::string& ruleLabel) {
    for (auto* model : roots) {
        if (model == nullptr) continue;
        if (!model->GetMapping().empty() || model->IsSkipped()) continue;

        Model* layoutModel = renderContext.GetModel(model->GetCoreModel());
        if (layoutModel == nullptr) continue;

        // A real ShadowModelFor attribute is the normal signal, but a model
        // whose StartChannel is itself a reference (e.g. "@MH-1:8", meaning
        // "start wherever model MH starts") is also an overlay/virtual model
        // with no independent effect data of its own, even on the rare model
        // where ShadowModelFor didn't get set/parsed.
        if (layoutModel->IsShadowModel() || StartsWith(layoutModel->GetModelStartChannel(), "@")) {
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
