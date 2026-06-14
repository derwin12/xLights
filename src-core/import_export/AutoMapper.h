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

// Skip pass (QuikMap Phase 0), run before any other phase. Marks each
// destination root that is a DMX model, or a ModelGroup that (recursively)
// contains at least one DMX model, as skipped (ImportMappingNode::SetSkipped)
// so no later phase considers it for mapping. Already-mapped roots are left
// alone.
void RunSkipDMX(const std::vector<ImportMappingNode*>& roots,
                RenderContext& renderContext,
                const std::string& ruleLabel = "");

} // namespace AutoMapper
