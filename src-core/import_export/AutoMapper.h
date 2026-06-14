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

#include <functional>
#include <list>
#include <string>
#include <unordered_set>
#include <vector>

class ImportMappingNode;
class RenderContext;
struct AvailableSource;

namespace AutoMapper {

// Version tag for the QuikMap report summary (DoQuikMap's `summary` /
// QuikMapReport.log). Bump this (v1.00 -> v1.01 -> ...) whenever the report
// format/content changes, so old logs can be told apart from new ones.
constexpr auto QUIKMAP_REPORT_VERSION = "v1.06";

// QuikMap phases, in the order xLightsImportChannelMapDialog::DoQuikMap runs
// them. Numbers are spaced by 5 so new phases can be inserted between
// existing ones without renumbering everything. This is the canonical list -
// other comments referencing "QuikMap Phase N" refer back to this table.
//
//   Phase  0: Skip pass - marks destination roots that are DMX models, or
//             groups that (recursively) contain at least one DMX model, as
//             skipped so no later phase maps them. See RunSkipDMX().
//   Phase  5: Exact name matches (case-insensitive) between vendor models/
//             groups and the user's models/groups. See MatchNorm, Run().
//   Phase 10: Matches between vendor models/groups and aliases defined on
//             the user's models/groups (also tolerant of punctuation
//             differences). See MatchAggressive, Run().
//   Phase 12: Custom-model exact-dimension matches - pairs an unmapped
//             destination Custom model with an unmapped vendor Custom model
//             whose node count, CustomWidth, and CustomHeight are all exactly
//             equal, regardless of name. Runs before the alias/community/
//             fuzzy passes since an exact structural match is a stronger
//             signal than a name guess. See RunCustomExactDimensionMatch().
//   Phase 13: Custom-model submodel-overlap matches - pairs an unmapped
//             destination Custom model with an unmapped vendor Custom model
//             that shares at least 3 submodel names (case-insensitive),
//             regardless of the parent model's name. See
//             RunCustomSubmodelOverlapMatch().
//   Phase 15: Matches against a locally-cached community alias pack
//             (crowdsourced vendor-name -> user-name pairs from
//             mapper.xlights.info). Skipped if no cache is present. See
//             CommunityAliasPack::Matches, Run().
//   Phase 20: Submodel/strand fallback matches by name or alias, matching
//             submodels of unmapped models only against non-group available
//             sources (group roots and group sources are skipped). See
//             RunSubModelFallback().
//   Phase 25: Last-resort fuzzy matches (token overlap, with numeric/side
//             signature and model-family guardrails) for anything still
//             unmapped. See MatchFuzzy, Run().
//   Phase 26: Group-content fuzzy matches - for unmapped destination groups,
//             augments the group's own name tokens with the shared family
//             token of its members when the group is made up entirely of one
//             kind of prop (e.g. "Cane-1".."Cane-4" -> "cane"), and does the
//             same for unmapped vendor ModelGroup sources using
//             AvailableSource::groupMemberNames. Then applies the same
//             token-Jaccard/family/numeric/side rules as Phase 25 to the
//             augmented token sets. This lets e.g. a vendor "Canes" group
//             (members "Cane 1".."Cane 3") match a destination
//             "Group - Candy Canes" (members "Cane-1".."Cane-4") even though
//             the group names alone don't overlap enough. See
//             RunGroupContentFuzzy().
//   Phase 30: Singing-prop matches - pairs unmapped destination roots that
//             are real singing props with unmapped singing-prop vendor
//             models. See RunSingingProp().
//   Phase 32: Singing-prop backfill - for any destination roots that are
//             still-unmapped real singing props (e.g. there were more
//             destination singing props than vendor singing models in
//             Phase 30), reuses vendor singing-prop models round-robin,
//             same vendor model assigned to multiple destinations if
//             needed. See RunSingingPropBackfill().
//   Phase 40: Floodlight matches. A "floodlight" model is a non-group,
//             single-line model with one node per string
//             (IsFloodlightModel). A "flood group" is a ModelGroup whose
//             members are all floodlight models (IsFloodGroup). Unmapped
//             destination flood groups are matched to unmapped vendor flood
//             groups; unmapped individual destination floodlights are then
//             matched 1:1 to unmapped individual vendor floodlights. See
//             RunFloodlight().
//   Phase 41: Floodlight backfill - for any destination roots that are still
//             unmapped flood groups or individual floodlights (e.g. there
//             were more destination flood groups/floodlights than vendor
//             ones in Phase 40), reuses vendor flood-group/floodlight
//             sources round-robin, same vendor source assigned to multiple
//             destinations if needed. See RunFloodlightBackfill().
//   Phase 65: Best-guess matches by shared model "class" for anything still
//             unmapped, excluding singing props (handled by Phase 30). See
//             RunBestGuess().
//   Phase 90: Like-model backfill - for an unmapped destination model that is
//             a numbered sibling of an already-mapped destination model of
//             the same family (e.g. "Cane-4" alongside mapped "Cane-1"),
//             derives the analogous vendor name by substituting the sibling's
//             number for its own (e.g. "Cane 1" -> "Cane 4") and maps to that
//             vendor source if it's still unused. If no such analogous vendor
//             source exists (e.g. the vendor only has 3 canes but the user
//             has 4), and the unmapped model is a Custom model whose node
//             count and CustomWidth/CustomHeight grid shape exactly match an
//             already-mapped same-family sibling's, reuses that sibling's
//             mapped vendor source (duplicate assignment) rather than leaving
//             it for Phase 97's blind same-type pairing. See
//             RunLikeModelBackfill().
//   Phase 95: Group-coverage skip - for each already-mapped destination group
//             whose members all belong to the same model family (e.g. a
//             "group of arches"), marks those member models as skipped so
//             Phase 100 doesn't separately (and redundantly) map them. See
//             RunGroupCoverageSkip().
//   Phase 96: Custom-dimension match - for each destination root still
//             unmapped and not skipped whose model "type" is "Custom" and
//             has a known node count, pairs it with the still-unmapped
//             vendor Custom model/group (model<->model, group<->group) whose
//             node count (and CustomWidth/CustomHeight grid shape) is the
//             closest match, rather than the first-available pairing Phase 97
//             would otherwise make. Candidates are also gated by
//             FuzzyFamiliesCompatible (using FuzzyModelFamilies, which
//             recognizes compound names like "EFlake46"/"ChromaFlake..."/
//             "HFlake1"/"HSpinner1" via substring keywords) so e.g. a vendor
//             "Snowflake 1".."Snowflake N" is only matched against
//             recognizably flake/star/spinner-family destination Custom
//             models, picking the closest by node count among those. Runs
//             after Phase 95 and before Phase 97 so two same-shaped custom
//             props (e.g. two similarly-sized snowflakes) are preferred over
//             an arbitrary custom/custom pairing. See
//             RunCustomDimensionMatch().
//   Phase 97: Model-type catch-all - for each destination root still
//             unmapped and not skipped, pairs it with a still-unmapped
//             vendor model/group of the same kind (model<->model,
//             group<->group) AND the same model "type"
//             (ImportMappingNode::GetModelType / AvailableSource::displayType,
//             e.g. "Arches", "Tree 360", "Matrix"), regardless of name. Runs
//             after Phase 96 and before the unconditional Phase 100
//             catch-all, so like-for-like model types are preferred over a
//             blind pairing. See RunModelTypeCatchAll().
//   Phase 100: Final catch-all - pairs any still-unmapped vendor model/group
//             with any still-unmapped user model/group of the same kind,
//             regardless of name. See RunCatchAll().

// Source-vs-destination matcher. Called per (destination_name, candidate)
// pair. Extra slots are reused by the regex matcher for the regex pattern
// and the substitution model name from .xmaphint files. Aliases are the
// destination model's aliases — checked by aggressive and norm strategies.
using MatcherFn = std::function<bool(const std::string& targetName,
                                     const std::string& candidate,
                                     const std::string& extra1,
                                     const std::string& extra2,
                                     const std::list<std::string>& aliases)>;

// Plain case-insensitive trimmed equality. The default matcher.
bool MatchNorm(const std::string& target, const std::string& candidate,
               const std::string& extra1, const std::string& extra2,
               const std::list<std::string>& aliases);

// Strips common punctuation/whitespace from both sides before comparing,
// also matches against `OldName:<candidate>` aliases. Used as the second
// pass after MatchNorm.
bool MatchAggressive(const std::string& target, const std::string& candidate,
                     const std::string& extra1, const std::string& extra2,
                     const std::list<std::string>& aliases);

// Regex matcher used by .xmaphint files: extra1 is the regex pattern,
// extra2 is the substitution candidate. Returns true if `target` matches
// the pattern AND `candidate` (lowered/trimmed) equals extra2.
bool MatchRegex(const std::string& target, const std::string& candidate,
                const std::string& pattern, const std::string& replacement,
                const std::list<std::string>& aliases);

// Last-resort fuzzy matcher: tokenizes both names (stripping stopwords and
// normalizing common synonyms/plurals), then requires:
//  - model-family compatibility (e.g. a "cane" never matches a "tree"), and
//  - matching numeric signatures if either name contains digits (so "Arch 1"
//    won't match "Arch 2"), and
//  - matching left/right/top/bottom side signatures if either name has one, and
//  - a token Jaccard overlap of at least 0.6.
// `extra1`/`extra2` are unused; present only to satisfy MatcherFn.
bool MatchFuzzy(const std::string& target, const std::string& candidate,
                const std::string& extra1, const std::string& extra2,
                const std::list<std::string>& aliases);

// Group-content fuzzy pass (QuikMap Phase 26), run immediately after the
// Phase 25 MatchFuzzy pass. For each destination root that is still unmapped,
// IsGroup(), and not skipped, looks up the corresponding ModelGroup in the
// layout and - if all of its members share the same non-empty
// FuzzyBaseTokens (i.e. the group is "all one kind of prop", e.g. all
// "Cane-N") - adds that shared family token to the group's own FuzzyTokens.
// Does the same for each unmapped, non-group-source-member, ModelGroup
// AvailableSource using AvailableSource::groupMemberNames. Then applies
// MatchFuzzy's family-compatibility/numeric/side-signature guardrails and
// >= 0.6 token-Jaccard rule to the (possibly augmented) token sets, mapping
// the first candidate that matches.
void RunGroupContentFuzzy(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          RenderContext& renderContext,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel = "");

// Run one auto-map pass over the destination tree.
//
// `roots` lists the top-level destination nodes (the desktop's
// `_dataModel`'s children, the iPad's mapping-tree roots).
// `available` is the source-candidate list (canonical-cased + selection
// state pre-computed by the caller).
// `renderContext` is used to look up aliases on submodels of the user's
// layout.
// `mg` is a one-character filter: "B" both, "M" models only, "G" groups
// only.
// `selectOnly` is set when the caller wants the run scoped to the
// `selectedTargets` / available `selected` flags.
// `selectedTargets` is the set of pointers to selected destination nodes.
// `ruleLabel` is a short human-readable tag (e.g. "Exact", "Alias") recorded
// via ImportMappingNode::SetMappingRule on every node mapped by this pass.
void Run(const std::vector<ImportMappingNode*>& roots,
         const std::vector<AvailableSource>& available,
         RenderContext& renderContext,
         MatcherFn lambda_model, MatcherFn lambda_strand, MatcherFn lambda_node,
         const std::string& extra1, const std::string& extra2,
         const std::string& mg,
         bool selectOnly,
         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
         const std::string& ruleLabel = "");

// Custom-model exact-dimension match pass (QuikMap Phase 12), run after the
// Phase 5 exact-name pass and before Phase 10's alias pass. For each
// destination root that is still unmapped, not skipped, not a group, and
// whose ImportMappingNode::GetModelType() is "Custom" (case-insensitive)
// with GetNodeCount() > 0, finds the first still-unmapped, non-group vendor
// Custom model (AvailableSource::displayType == "Custom") whose nodeCount,
// width, and height are all exactly equal, and maps to it. Unlike
// RunCustomDimensionMatch (Phase 96, a tolerance-based last resort), this
// requires an exact structural match - two props with identical pixel
// counts and grid shape are treated as the same prop regardless of name. As
// with the other catch-all-style phases, any newly-mapped root's still-
// unmapped Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunCustomExactDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                                   const std::vector<AvailableSource>& available,
                                   bool selectOnly,
                                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                   const std::string& ruleLabel = "");

// Custom-model submodel-overlap match pass (QuikMap Phase 13), run
// immediately after RunCustomExactDimensionMatch and before Phase 15. For
// each destination root that is still unmapped, not skipped, not a group,
// and whose ImportMappingNode::GetModelType() is "Custom" (case-insensitive),
// collects its child Strand/SubModel names (ImportMappingNode::GetCoreStrand()
// of its direct children). For each still-unmapped, non-group vendor Custom
// model, collects its submodel names from `<vendorModel>/<submodel>` entries
// in `available`. If the two name sets share at least 3 names
// (case-insensitive), maps the destination root to that vendor model. As
// with the other catch-all-style phases, any newly-mapped root's still-
// unmapped Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunCustomSubmodelOverlapMatch(const std::vector<ImportMappingNode*>& roots,
                                    const std::vector<AvailableSource>& available,
                                    bool selectOnly,
                                    const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                    const std::string& ruleLabel = "");

// Called once after all Run() passes complete. For destination models
// (group roots are skipped - groups have no submodels) that didn't get a
// direct mapping, attempts to map their unmapped submodels in two steps:
//   1. Direct name match: submodel name vs non-slashed, non-group available
//      name.
//   2. Alias match: submodel alias (from layout) vs non-slashed, non-group
//      available name.
// Running this after the aggressive pass ensures correct alias-based slashed
// matches (e.g. "ModelAlias/SubmodelAlias") take priority over coincidental
// plain-name matches from step 1.
void RunSubModelFallback(const std::vector<ImportMappingNode*>& roots,
                         const std::vector<AvailableSource>& available,
                         RenderContext& renderContext,
                         bool selectOnly,
                         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                         const std::string& ruleLabel = "");

// Singing-prop pass (QuikMap Phase 30), run after fuzzy matching and before
// the best-guess pass. For each destination root that is still unmapped, is
// not a group, and is a real singing prop (ImportMappingNode::IsSingingProp -
// a Custom model with at least one populated faceInfo NodeRange), claims the
// next not-yet-used non-group available source that is also a singing prop
// (AvailableSource::isSingingProp). This avoids RunBestGuess's coarser
// modelClass=="SingingFace" matching pairing up singing props with unrelated
// "Custom singing face" models that have no actual face nodes defined. As
// with RunBestGuess/RunCatchAll, any newly-mapped root's still-unmapped
// Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunSingingProp(const std::vector<ImportMappingNode*>& roots,
                     const std::vector<AvailableSource>& available,
                     bool selectOnly,
                     const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                     const std::string& ruleLabel = "");

// Singing-prop backfill pass (QuikMap Phase 32), run immediately after
// RunSingingProp. For each destination root that is still unmapped, is not a
// group, and is a real singing prop (ImportMappingNode::IsSingingProp),
// assigns the next vendor singing-prop source (AvailableSource::isSingingProp,
// non-group, no slash) in round-robin order - reusing sources already claimed
// by Phase 30 or earlier Phase 32 assignments if necessary, since there may be
// more destination singing props than distinct vendor singing models. As with
// RunSingingProp, any newly-mapped root's still-unmapped Strand/Node children
// are then filled from `<reusedVendorModel>/...` sources of the corresponding
// depth (also allowing reuse).
void RunSingingPropBackfill(const std::vector<ImportMappingNode*>& roots,
                            const std::vector<AvailableSource>& available,
                            bool selectOnly,
                            const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                            const std::string& ruleLabel = "");

// Floodlight pass (QuikMap Phase 40), run after the singing-prop passes and
// before the best-guess pass. A "floodlight" is a non-group, single-line
// model with one node per string (ImportMappingNode::IsFloodlight /
// AvailableSource::isFloodlight); a "flood group" is a ModelGroup whose
// members are all floodlights (ImportMappingNode::IsFloodGroup /
// AvailableSource::isFloodGroup).
//   1. For each still-unmapped destination root that is a flood group,
//      claims the next not-yet-used available ModelGroup source that is also
//      a flood group.
//   2. For each still-unmapped destination root that is an individual
//      floodlight (not a group), claims the next not-yet-used non-group
//      available source that is also an individual floodlight - so if the
//      destination has no flood group (or the vendor doesn't), individual
//      floods are still paired up 1:1.
void RunFloodlight(const std::vector<ImportMappingNode*>& roots,
                   const std::vector<AvailableSource>& available,
                   bool selectOnly,
                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                   const std::string& ruleLabel = "");

// Floodlight backfill pass (QuikMap Phase 41), run immediately after
// RunFloodlight. For each destination root that is still unmapped and is a
// flood group (ImportMappingNode::IsFloodGroup), assigns the next available
// ModelGroup flood-group source (AvailableSource::isFloodGroup) in
// round-robin order - reusing sources already claimed by Phase 40 or earlier
// Phase 41 assignments if necessary. Then, for each destination root that is
// still unmapped and is an individual floodlight
// (ImportMappingNode::IsFloodlight), assigns the next non-group available
// floodlight source (AvailableSource::isFloodlight) in round-robin order,
// also reusing sources as needed.
void RunFloodlightBackfill(const std::vector<ImportMappingNode*>& roots,
                           const std::vector<AvailableSource>& available,
                           bool selectOnly,
                           const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                           const std::string& ruleLabel = "");

// Best-guess pass (QuikMap Phase 65), run after fuzzy matching and before the
// catch-all. For each destination root that is still unmapped and has a
// non-empty model "class" (e.g. "SingingFace", "SpiralTree" - see
// Model::DetermineClass), claims the next not-yet-used available source
// (non-group, matching `model<->model`/`group<->group`) whose modelClass is
// the same. This lets e.g. an unrecognized vendor singing pumpkin map onto
// the user's singing tombstone purely because both are singing faces, even
// though fuzzy name matching found nothing in common. As with RunCatchAll,
// any newly-mapped root's still-unmapped Strand/Node children are then
// filled from not-yet-used `<mappedVendorModel>/...` sources of the
// corresponding depth.
void RunBestGuess(const std::vector<ImportMappingNode*>& roots,
                  const std::vector<AvailableSource>& available,
                  bool selectOnly,
                  const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                  const std::string& ruleLabel = "");

// Like-model backfill pass (QuikMap Phase 90), run after RunBestGuess and
// before RunGroupCoverageSkip/RunCatchAll. For each destination root that is
// still unmapped, not a group, and not skipped, looks for another destination
// root (a "sibling") that:
//  - belongs to the same model family (same fuzzy base tokens and side
//    signature, e.g. both "Cane" models, both "-Left"), and
//  - has a single numeric signature different from this root's, and
//  - is already mapped to a vendor source whose name contains that number.
// If found, substitutes the sibling's number for this root's number in the
// sibling's mapped vendor name (e.g. mapped "Cane 1" -> candidate "Cane 4"
// for unmapped "Cane-4"), and maps this root to that vendor source if it
// exists and is not already used by another mapping.
void RunLikeModelBackfill(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel = "");

// Group-coverage skip pass (QuikMap Phase 95), run after
// RunLikeModelBackfill and before RunCatchAll. For each destination root that
// IsGroup() and is already mapped (to a vendor group, by an earlier phase),
// looks up the corresponding ModelGroup in the layout and checks whether all
// of its member models share the same fuzzy "base" tokens (i.e. the group is
// e.g. "all Arches"). If so, marks each member model's destination root (if
// still unmapped) as skipped via ImportMappingNode::SetSkipped, so Phase 100
// doesn't separately map e.g. "Arches-1-Left"/"Arches-6-R" when the
// containing "Arches" group has already been mapped to a vendor "Arches"
// group.
void RunGroupCoverageSkip(const std::vector<ImportMappingNode*>& roots,
                          RenderContext& renderContext,
                          const std::string& ruleLabel = "");

// Last-resort catch-all pass (QuikMap Phase 100). Ignores naming entirely:
// for each destination root still unmapped, claims the next not-yet-used
// available source of the matching kind (ModelGroup destinations only draw
// from ModelGroup sources, everything else only draws from non-ModelGroup
// sources). For a root that ends up mapped (here or in an earlier phase),
// any still-unmapped Strand/Node children are then matched against
// not-yet-used `<mappedVendorModel>/...` sources of the corresponding depth
// (2-part paths -> Strand children, 3-part paths -> Node children), so
// type-to-type pairing (model<->model, group<->group, strand<->strand,
// node<->node) is preserved even though names are ignored.
void RunCatchAll(const std::vector<ImportMappingNode*>& roots,
                 const std::vector<AvailableSource>& available,
                 bool selectOnly,
                 const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                 const std::string& ruleLabel = "");

// Sibling-reuse backfill pass (QuikMap Phase 101), run after RunCatchAll. For
// each destination root that is still unmapped, not skipped, and not a group,
// looks for an already-mapped sibling root (same FuzzyBaseTokens and
// FuzzySideSignature, e.g. "Md Star - 01"/"Md Star - 02" share base "md star"
// and no side) whose ImportMappingNode::GetModelType() matches
// (case-insensitive), and reuses that sibling's vendor mapping verbatim
// (multiple destination roots may end up pointing at the same vendor
// source). This catches numbered families where the vendor side has fewer
// like-typed models than the destination (e.g. two "Md Star" props but only
// one vendor "Star" model) - rather than leaving the extra ones unmapped,
// they reuse their sibling's mapping. Does not touch Strand/Node children.
void RunSiblingReuseBackfill(const std::vector<ImportMappingNode*>& roots,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel = "");

// Custom-dimension match pass (QuikMap Phase 96), run after
// RunGroupCoverageSkip and before RunModelTypeCatchAll. For each destination
// root that is still unmapped, not skipped, and whose
// ImportMappingNode::GetModelType() is "Custom" (case-insensitive) with
// GetNodeCount() > 0, scores every still-unmapped vendor Custom model/group
// of the same kind (model<->model, group<->group, AvailableSource::displayType
// == "Custom") by similarity of AvailableSource::nodeCount and
// width/height aspect ratio, and maps to the closest match if it is within a
// reasonable tolerance. Destination roots with no candidate within tolerance
// are left unmapped for Phase 97/100. As with RunModelTypeCatchAll, any
// newly-mapped root's still-unmapped Strand/Node children are then filled
// from not-yet-used `<mappedVendorModel>/...` sources of the corresponding
// depth.
void RunCustomDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel = "");

// Model-type catch-all pass (QuikMap Phase 97), run after
// RunGroupCoverageSkip and before RunCatchAll. For each destination root
// that is still unmapped and not skipped, claims the next not-yet-used
// available source of the matching kind (ModelGroup destinations only draw
// from ModelGroup sources, everything else only draws from non-ModelGroup
// sources) whose AvailableSource::displayType matches the destination's
// ImportMappingNode::GetModelType() (case-insensitive). Names are ignored,
// but the model "type" (e.g. "Arches", "Tree 360", "Matrix") must agree -
// this is a tighter, like-for-like alternative to RunCatchAll's
// unconditional pairing. Destination roots whose GetModelType() is "Custom"
// are skipped entirely - "Custom" covers too wide a range of shapes (e.g. a
// 3D Cube and a flat Snowflake) for a type-only match to be meaningful, so
// those are left for Phase 96's dimension-based match or Phase 100. As with
// RunCatchAll, any newly-mapped root's still-
// unmapped Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunModelTypeCatchAll(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel = "");

// Skip pass (QuikMap Phase 0), run before any other phase. Marks each
// destination root that is a DMX model, or a ModelGroup that (recursively)
// contains at least one DMX model, as skipped (ImportMappingNode::SetSkipped)
// so no later phase considers it for mapping. Already-mapped roots are left
// alone.
void RunSkipDMX(const std::vector<ImportMappingNode*>& roots,
                RenderContext& renderContext,
                const std::string& ruleLabel = "");

} // namespace AutoMapper
