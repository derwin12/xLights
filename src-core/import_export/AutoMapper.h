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
constexpr auto QUIKMAP_REPORT_VERSION = "v1.13";

// QuikMap phases, in the order xLightsImportChannelMapDialog::DoQuikMap runs
// them. Numbers are spaced by 5 so new phases can be inserted between
// existing ones without renumbering everything. This is the canonical list -
// other comments referencing "QuikMap Phase N" refer back to this table.
//
//   Phase  0: Skip pass - marks destination roots that are DMX models, or
//             groups that (recursively) contain at least one DMX model, as
//             skipped so no later phase maps them. See RunSkipDMX().
//             e.g. dest "DMX-Spot-1" (DMX model) → skipped; never mapped.
//
//   Phase  1: Shadow-model skip pass - marks destination roots that have a
//             non-empty ShadowModelFor attribute as skipped. Shadow models are
//             overlay/virtual models (e.g. MH-DIMMERS shadowing MH-Pan) that
//             carry no independent effect data and should never be auto-mapped.
//             See RunSkipShadow().
//             e.g. dest "MH-DIMMERS" (ShadowModelFor="MH-Pan") → skipped.
//
//   Phase  5: Exact name matches (case-insensitive) between vendor models/
//             groups and the user's models/groups. See MatchNorm, Run().
//             e.g. dest "Cane-1" matches vendor "cane-1" (same name,
//             different case).
//
//   Phase 10: Matches between vendor models/groups and aliases defined on
//             the user's models/groups (also tolerant of punctuation
//             differences). See MatchAggressive, Run().
//             e.g. dest "Holiday Tree" carries alias "Christmas Tree" →
//             matches vendor "Christmas Tree" even though the base names
//             differ.
//
//   Phase 12: Custom-model exact-dimension matches - pairs an unmapped
//             destination Custom model with an unmapped vendor Custom model
//             whose node count, CustomWidth, and CustomHeight are all exactly
//             equal, regardless of name. Runs before the alias/community/
//             fuzzy passes since an exact structural match is a stronger
//             signal than a name guess. See RunCustomExactDimensionMatch().
//             e.g. dest "MyFlake-A" (48 nodes, 8×6 grid) matches vendor
//             "Snowflake 3" (48 nodes, 8×6 grid) — identical structure
//             despite sharing no name tokens.
//
//   Phase 13: Custom-model submodel-overlap matches - pairs an unmapped
//             destination Custom model with an unmapped vendor Custom model
//             that shares at least 3 submodel names (case-insensitive),
//             regardless of the parent model's name. See
//             RunCustomSubmodelOverlapMatch().
//             e.g. dest "BigStar" has submodels "Ring 1", "Ring 2", "Ring 3",
//             "Ring 4" → matches vendor "Star" which also has "Ring 1",
//             "Ring 2", "Ring 3" (3 shared submodel names).
//
//   Phase 15: Matches against a locally-cached community alias pack
//             (crowdsourced vendor-name -> user-name pairs from
//             mapper.xlights.info). Skipped if no cache is present. See
//             CommunityAliasPack::Matches, Run().
//             e.g. community pack contains pair "xmas tree" → "Holiday Pine"
//             → dest "Holiday Pine" matches vendor "xmas tree" with no local
//             alias required.
//
//   Phase 16: "Everything"/"all" group matches - for an unmapped destination
//             ModelGroup root whose name or aliases contain an "everything"
//             or "all" token, matches it against an unmapped vendor
//             ModelGroup whose name also contains one of those tokens. These
//             groups are typically catch-all groups containing every prop, so
//             when either side has more than one such candidate, the
//             candidate with the highest member count is preferred. See
//             RunEverythingGroupMatch().
//             e.g. dest "01 Everything" (208 members) matches vendor "All
//             Props" (240 members) — both contain the "all" token and are the
//             largest such groups on their respective sides.
//
//   Phase 17: Special-keyword group matches - for an unmapped destination
//             ModelGroup root whose name or aliases contain one of a small
//             set of special keywords ("last", "override", "bottom"), matches
//             it against the unmapped vendor ModelGroup whose name also
//             contains one of those keywords that has the most effects; among
//             candidates with effects, the one with the most members wins.
//             See RunSpecialKeywordGroupMatch().
//             e.g. dest "98 All Override (last group)" matches vendor "All
//             with no faces (put on bottom for FADES)" — dest has "last",
//             vendor has "bottom", and that vendor group has effects while
//             other keyword-group candidates do not.
//
//   Phase 18: Horizontal/vertical group matches - for an unmapped destination
//             ModelGroup root whose name or aliases contain a token starting
//             with "horiz" or "vert" (whole-word prefix so "converts" is not
//             a false match), matches it against an unmapped vendor ModelGroup
//             with the same orientation. Both sides' name-token orientation is
//             cross-checked against ImportMappingNode::GetGroupGeometricOrientation
//             / AvailableSource::groupGeomOrientation - the majority vote of
//             the group's own members' world-space bounding-box orientation
//             (members are typically Single Line models, where the X2/Y2
//             endpoint offset's dominant axis gives the orientation; see
//             GeometricOrientationFromBox in the dialog). A destination or
//             candidate whose geometry confidently *disagrees* with its own
//             name-token orientation is rejected outright (the name is
//             assumed stale/mislabeled); among remaining same-orientation
//             candidates, the one with the highest Jaccard token overlap
//             against the destination's name wins, with geometry-confirmed
//             candidates (both sides' members agree with the chosen
//             orientation) breaking ties over geometry-unknown ones. See
//             RunHVGroupMatch().
//             e.g. dest "Horizontal Matrix" (members' bounding boxes
//             confirm wider-than-tall) matches vendor "Horiz Strip" (both
//             horiz, geometry-confirmed) over "Vertical Arch" (vert) —
//             orientation lock-in first, then geometry-confirmed/name-token
//             score breaks ties.
//
//   Phase 20: Submodel/strand fallback matches by name or alias, matching
//             submodels of unmapped models only against non-group available
//             sources (group roots and group sources are skipped). See
//             RunSubModelFallback().
//             e.g. dest model "WreathA" has no top-level vendor match, but
//             its submodel "Ring 1" matches vendor source "Ring 1" directly.
//
//   Phase 25: Last-resort fuzzy matches (token overlap, with numeric/side
//             signature and model-family guardrails) for anything still
//             unmapped. Restricted to same-kind pairings (model<->model,
//             group<->group) via AvailableKindFilter::SameKindOnly - a
//             cross-kind pairing (e.g. dest "Group - Mega Trees" fuzzy-
//             matching vendor model "Mega Tree") is left for Phase 27. See
//             MatchFuzzy, Run().
//             e.g. dest "Candy-Cane-Left-1" fuzzy-matches vendor "Cane 1
//             Left" — shared tokens {cane, 1, left} give Jaccard ≥ 0.6 and
//             numeric/side signatures agree.
//
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
//             e.g. dest "Group - Candy Canes" (all "Cane-N" members →
//             augmented with "cane") fuzzy-matches vendor "Canes" group (all
//             "Cane N" members → also augmented with "cane"), even though
//             "Group - Candy Canes" vs "Canes" alone is below the 0.6
//             Jaccard threshold.
//
//   Phase 27: Cross-kind fuzzy matches - same MatchFuzzy rules as Phase 25,
//             but via AvailableKindFilter::CrossKindOnly, for anything still
//             unmapped after Phases 25/26: a destination model may now match
//             a vendor group, or a destination group may match a vendor
//             model. Run last among the fuzzy passes so a same-kind match is
//             always preferred when one exists. See MatchFuzzy, Run().
//             e.g. dest "Group - Mega Trees" (a group) fuzzy-matches vendor
//             "Mega Tree" (a plain model) only if no group<->group or
//             model<->model candidate was found in Phase 25/26.
//
//   Phase 30: Singing-prop matches - pairs unmapped destination roots that
//             are real singing props with unmapped singing-prop vendor
//             models. See RunSingingProp().
//             e.g. dest "Santa" (Custom with populated NodeRange face
//             mapping) matches vendor "Santa Face" (also a singing prop),
//             claimed before Phase 65's coarser modelClass pairing can grab
//             it.
//
//   Phase 32: Singing-prop backfill - for any destination roots that are
//             still-unmapped real singing props (e.g. there were more
//             destination singing props than vendor singing models in
//             Phase 30), reuses vendor singing-prop models round-robin,
//             same vendor model assigned to multiple destinations if
//             needed. See RunSingingPropBackfill().
//             e.g. dest has "Santa", "Snowman", "Elf" (all singing props);
//             vendor has only "Santa Face" and "Snowman Face" → "Elf" reuses
//             "Santa Face" (first in the pool).
//
//   Phase 40: Floodlight matches. A "floodlight" model is a non-group,
//             single-line model with one node per string
//             (IsFloodlightModel). A "flood group" is a ModelGroup whose
//             members are all floodlight models (IsFloodGroup). Unmapped
//             destination flood groups are matched to unmapped vendor flood
//             groups; unmapped individual destination floodlights are then
//             matched 1:1 to unmapped individual vendor floodlights. See
//             RunFloodlight().
//             e.g. dest "Group - Floods" (4 single-node flood members)
//             matches vendor "Flood Lights" (also a flood group); then dest
//             "Flood-Left" matches vendor "Flood L" 1:1.
//
//   Phase 41: Floodlight backfill - for any destination roots that are still
//             unmapped flood groups or individual floodlights (e.g. there
//             were more destination flood groups/floodlights than vendor
//             ones in Phase 40), reuses vendor flood-group/floodlight
//             sources round-robin, same vendor source assigned to multiple
//             destinations if needed. See RunFloodlightBackfill().
//             e.g. dest has 4 individual floods; vendor has only 2 → flood-3
//             reuses vendor flood-1, flood-4 reuses vendor flood-2.
//
//   Phase 65: Best-guess matches by shared model "class" for anything still
//             unmapped, excluding singing props (handled by Phase 30). See
//             RunBestGuess().
//             e.g. dest "SpiralPumpkin" has modelClass "SpiralTree" →
//             matches vendor "SpiralTree 1" (same modelClass), even though
//             no name tokens overlap.
//
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
//             it for Phase 105's blind same-type pairing. See
//             RunLikeModelBackfill().
//             e.g. dest "Cane-4" is unmapped; sibling "Cane-1" maps to
//             vendor "Cane 1" → candidate "Cane 4" is found and assigned.
//
//   Phase 95: Group-coverage skip - for each already-mapped destination group
//             whose members all belong to the same model family (e.g. a
//             "group of arches"), marks those member models as skipped so
//             Phase 120 doesn't separately (and redundantly) map them. See
//             RunGroupCoverageSkip().
//             e.g. dest "Group - Arches" (all "Arch-N" members) is already
//             mapped → "Arch-1".."Arch-6" are marked skipped so Phase 120
//             doesn't pair each arch individually to an unrelated vendor
//             model.
//
//   Phase 100: Custom-dimension match - for each destination root still
//             unmapped and not skipped whose model "type" is "Custom" and
//             has a known node count, pairs it with the still-unmapped
//             vendor Custom model/group (model<->model, group<->group) whose
//             node count (and CustomWidth/CustomHeight grid shape) is the
//             closest match, rather than the first-available pairing Phase 105
//             would otherwise make. Candidates are also gated by
//             FuzzyFamiliesCompatible (using FuzzyModelFamilies, which
//             recognizes compound names like "EFlake46"/"ChromaFlake..."/
//             "HFlake1"/"HSpinner1" via substring keywords) so e.g. a vendor
//             "Snowflake 1".."Snowflake N" is only matched against
//             recognizably flake/star/spinner-family destination Custom
//             models, picking the closest by node count among those. Runs
//             after Phase 95 and before Phase 105 so two same-shaped custom
//             props (e.g. two similarly-sized snowflakes) are preferred over
//             an arbitrary custom/custom pairing. See
//             RunCustomDimensionMatch().
//             e.g. dest "HFlake1" (46 nodes, Custom) matches vendor
//             "Snowflake 2" (46 nodes, Custom) — same family ("flake"
//             keyword), closest node count among available vendor Customs.
//
//   Phase 105: Model-type catch-all - for each destination root still
//             unmapped and not skipped, pairs it with a still-unmapped
//             vendor model/group of the same kind (model<->model,
//             group<->group) AND the same model "type"
//             (ImportMappingNode::GetModelType / AvailableSource::displayType,
//             e.g. "Arches", "Tree 360", "Matrix"), regardless of name. Runs
//             after Phase 100 and before the unconditional Phase 120
//             catch-all, so like-for-like model types are preferred over a
//             blind pairing. See RunModelTypeCatchAll().
//             e.g. dest "MyArch-3" (type "Arches") matches vendor "Arch C"
//             (type "Arches") purely by model type — no name overlap needed.
//
//   Phase 110: Group-member dimension match - for each destination root that
//             IsGroup(), is already mapped to a vendor ModelGroup, and whose
//             corresponding layout ModelGroup has members, looks up that
//             vendor ModelGroup's AvailableSource::groupMemberNames. For each
//             still-unmapped, non-group destination root that is one of the
//             destination group's members, picks the still-unmapped vendor
//             model that is one of the vendor group's members and has the
//             closest node count (and grid shape, for Custom models), and
//             maps to it. This uses an already-established group<->group
//             mapping as a high-confidence pool for its individual members
//             that didn't fuzzy-match a vendor name directly, so those
//             generic-numbered vendor models aren't left for the unconstrained
//             Phase 120 catch-all. Only considers destination groups whose
//             mapping rule came from a name-based phase — groups matched by
//             EverythingGroup (16), SpecialKeywordGroup (17), or Catchall
//             (105/120) carry no real per-member correspondence and are
//             skipped. Runs after Phase 105 and before Phase 115. See
//             RunGroupMemberDimensionMatch().
//             e.g. dest "Group - Snowflakes" → vendor "Snowflakes" (Phase 25
//             fuzzy); member "EFlake46" (46 nodes) → vendor "Snowflake 2"
//             (46 nodes, closest in the vendor group's member pool).
//
//   Phase 115: Group-member dimension backfill - same group<->group pairings
//             as Phase 110, but for any destination group member still
//             unmapped after Phase 110 (e.g. there are more destination group
//             members than vendor group members), maps it to the closest-by-
//             node-count vendor group member, drawn from the same reusable
//             pool as Phase 110 (so a vendor source Phase 110 already claimed
//             is a valid candidate here too). A vendor source already used
//             by *this backfill pass* is only reused once every other
//             family-compatible pool member has been used at least once -
//             so e.g. with two unmapped destinations and three vendor pool
//             members, both destinations get distinct vendor sources instead
//             of piling onto the single best-scoring one. Runs after Phase
//             110 and before Phase 120. See RunGroupMemberDimensionBackfill().
//             e.g. dest "Group - Stars" → vendor "Stars" (3 members: "Star
//             1".."Star 3", all already used by Phase 110); dest has 5 star
//             members → the 4th and 5th destination stars reuse "Star 1" and
//             "Star 2" respectively (each pool member used once before any
//             is used twice), rather than both piling onto whichever single
//             member scores best. A 6th destination star would then be the
//             first genuine *second* reuse, picked by best score as before.
//
//   Phase 120: Final catch-all - pairs any still-unmapped vendor model/group
//             with any still-unmapped user model/group of the same kind and
//             depth, regardless of name. Exception: a still-unmapped vendor
//             ModelGroup whose name contains a special-sequencer-meaning
//             keyword ("last", "override", "bottom" - see Phase 17/
//             IsSpecialKeywordGroupName) is never used here - if Phase 17
//             didn't already pair it by name/alias, it's left unmapped (along
//             with the destination group under consideration) rather than
//             handed out blindly. Also gated by the same FamiliesCompatible
//             guard as Phase 100/110/115 (permissive when either side has no
//             recognized FuzzyModelFamilies token, so it only blocks genuine
//             cross-family mismatches like "matrix" vs. "star") - without it,
//             an unrelated still-unmapped Custom model could steal a vendor
//             source that was a much better fit for a different still-
//             unmapped destination, purely because of root iteration order.
//             A "Line"-class model (modelClass == "Line" - Single Line, Poly
//             Line, Arches, Candy Canes, Circle, Window Frame; see
//             Model::DetermineClass) and a genuine 2D grid (a Custom/Matrix
//             model with both width and height > 1) are never paired,
//             regardless of family - checked via modelClass rather than a
//             literal type-string match so it covers every "Line"-classified
//             display type, not just "Single Line". Among the remaining
//             candidates, picks the dimensionally-closest one
//             (GroupMemberDimensionScore) rather than just the first one
//             found. See RunCatchAll().
//             e.g. dest "PropX" (type "Tree 360") is still unmapped after
//             all earlier phases; vendor "LeftoverA" (also "Tree 360") is
//             also still unmapped → paired by kind, with no name check, but
//             still requiring family/shape compatibility.
//             e.g. dest "Matrix Seeds" (family "matrix") and dest "3D Star"
//             (family "star") are both still-unmapped Custom models; vendor
//             "Matrix 2" (family "matrix") is still unmapped → only "Matrix
//             Seeds" is family-compatible with it, so "3D Star" can no
//             longer steal it regardless of iteration order.
//             e.g. dest "Driveway - 01L" (a Poly Line, modelClass "Line")
//             must not match vendor "Matrix 2" (a 2D grid) even though
//             neither has a recognized family token - the line/grid shape
//             guard (by modelClass, not the literal "Poly Line" string)
//             blocks it outright.
//
//   Phase 125: Sibling-reuse backfill - for each destination root that is
//             still unmapped, not skipped, and not a group, looks for an
//             already-mapped sibling root (same FuzzyBaseTokens + side
//             signature) and reuses that sibling's vendor mapping verbatim.
//             See RunSiblingReuseBackfill().
//             e.g. dest "Md Star - 02" is unmapped; sibling "Md Star - 01"
//             maps to vendor "Star 1" → "Md Star - 02" reuses "Star 1"
//             (duplicate assignment).

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

// Constrains the bare-model match in Run() by whether the destination root
// and the candidate source are the same "kind" (model vs. ModelGroup).
// `Any` is the historical behavior (no constraint - used by Phase 5/10/15,
// where the matchers are precise enough that an accidental model<->group
// pairing is effectively impossible). `SameKindOnly` is used by Phase 25 so
// the broad fuzzy matcher prefers model<->model and group<->group pairings.
// `CrossKindOnly` is used by Phase 27 as the fallback over whatever Phase 25
// left unmapped, allowing a destination model to match a vendor group (or
// vice versa).
enum class AvailableKindFilter { Any, SameKindOnly, CrossKindOnly };

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
// `kindFilter` further constrains the bare-model match - see
// AvailableKindFilter.
// `allowSharedSource` lets a bare-model vendor source be claimed by more
// than one destination root in this (and later) passes. Off by default - a
// vendor source is normally claimed by at most one destination, so e.g.
// Phase 25/27's fuzzy fallback won't steal a source an earlier phase already
// used. Phase 10 (Alias) passes true: a user can deliberately put the same
// alias on multiple props (e.g. two different "Mega Tree" stand-ins that
// should both receive the same vendor effects), so alias matches must be
// allowed to fan out to more than one destination.
void Run(const std::vector<ImportMappingNode*>& roots,
         const std::vector<AvailableSource>& available,
         RenderContext& renderContext,
         MatcherFn lambda_model, MatcherFn lambda_strand, MatcherFn lambda_node,
         const std::string& extra1, const std::string& extra2,
         const std::string& mg,
         bool selectOnly,
         const std::unordered_set<const ImportMappingNode*>& selectedTargets,
         const std::string& ruleLabel = "",
         AvailableKindFilter kindFilter = AvailableKindFilter::Any,
         bool allowSharedSource = false);

// Custom-model exact-dimension match pass (QuikMap Phase 12), run after the
// Phase 5 exact-name pass and before Phase 10's alias pass. For each
// destination root that is still unmapped, not skipped, not a group, and
// whose ImportMappingNode::GetModelType() is "Custom" (case-insensitive)
// with GetNodeCount() > 0, finds the first still-unmapped, non-group vendor
// Custom model (AvailableSource::displayType == "Custom") whose nodeCount,
// width, and height are all exactly equal, and maps to it. Unlike
// RunCustomDimensionMatch (Phase 100, a tolerance-based last resort), this
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

// Special-keyword group match pass (QuikMap Phase 17), run after the
// "everything"/"all" group match pass (Phase 16) and before the submodel
// fallback pass (Phase 20). For each destination root that IsGroup(), is not
// skipped, is still unmapped, and whose GetModelName() or any GetAliases()
// entry contains (case-insensitive) one of a small set of special keywords
// ("last", "override", "bottom"), finds the first still-unmapped vendor
// ModelGroup source whose name also
// contains one of those keywords - not necessarily the same one - and maps
// to it. These keywords identify groups with special sequencer semantics
// (e.g. a catch-all "Last"/"Override" group, or a "Bottom" layer group)
// that a plain name/alias/fuzzy match is unlikely to pair correctly. As with
// the other catch-all-style phases, any newly-mapped root's still-unmapped
// Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunSpecialKeywordGroupMatch(const std::vector<ImportMappingNode*>& roots,
                                  const std::vector<AvailableSource>& available,
                                  bool selectOnly,
                                  const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                  const std::string& ruleLabel = "");

// Horizontal/vertical group match pass (QuikMap Phase 18), run after the
// special-keyword group pass (Phase 17) and before the submodel fallback pass
// (Phase 20). For each destination root that IsGroup(), is not skipped, is
// still unmapped, and whose GetModelName() or any GetAliases() entry contains
// a whole-word token that starts with "horiz" or "vert" (prefix match so
// "converts" is not a false positive), detects the orientation and matches it
// against still-unmapped vendor ModelGroup sources with the same orientation.
// Among same-orientation candidates the one with the highest Jaccard token
// overlap against the destination name is preferred.
void RunHVGroupMatch(const std::vector<ImportMappingNode*>& roots,
                     const std::vector<AvailableSource>& available,
                     bool selectOnly,
                     const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                     const std::string& ruleLabel = "");

// "Everything"/"all" group match pass (QuikMap Phase 16), run after the
// community alias pack pass (Phase 15) and before the special-keyword group
// pass (Phase 17). For each destination root that IsGroup(), is not skipped,
// is still unmapped, and whose GetModelName() or any GetAliases() entry
// contains an "everything" or "all" token (whole-word, e.g. "Group - All
// Props" or "Everything" but not "Waterfall"), and for each still-unmapped
// vendor ModelGroup source whose displayName contains one of those tokens,
// maps the destination candidate to the vendor candidate. If more than one
// candidate is found on either side, the one with the highest model/member
// count is used (destination counts come from the corresponding layout
// ModelGroup::ModelNames via renderContext; vendor counts come from
// AvailableSource::groupMemberNames). As with the other catch-all-style
// phases, any newly-mapped root's still-unmapped Strand/Node children are
// then filled from not-yet-used `<mappedVendorModel>/...` sources of the
// corresponding depth.
void RunEverythingGroupMatch(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              RenderContext& renderContext,
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
// still unmapped) as skipped via ImportMappingNode::SetSkipped, so Phase 120
// doesn't separately map e.g. "Arches-1-Left"/"Arches-6-R" when the
// containing "Arches" group has already been mapped to a vendor "Arches"
// group.
void RunGroupCoverageSkip(const std::vector<ImportMappingNode*>& roots,
                          RenderContext& renderContext,
                          const std::string& ruleLabel = "");

// Last-resort catch-all pass (QuikMap Phase 120). Ignores naming entirely:
// for each destination root still unmapped, claims the next not-yet-used
// available source of the matching kind (ModelGroup destinations only draw
// from ModelGroup sources, everything else only draws from non-ModelGroup
// sources). Exception: for ModelGroup destinations, a candidate vendor
// ModelGroup whose name matches IsSpecialKeywordGroupName (the "last"/
// "override"/"bottom" keywords used by Phase 17) is skipped - if it wasn't
// claimed by Phase 17's name/alias-based match, it's left unmapped rather
// than handed to an unrelated destination group here, and so is the
// destination group under consideration. For a root that ends up mapped
// (here or in an earlier phase),
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

// Sibling-reuse backfill pass (QuikMap Phase 125), run after RunCatchAll. For
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

// Custom-dimension match pass (QuikMap Phase 100), run after
// RunGroupCoverageSkip and before RunModelTypeCatchAll. For each destination
// root that is still unmapped, not skipped, and whose
// ImportMappingNode::GetModelType() is "Custom" (case-insensitive) with
// GetNodeCount() > 0, scores every still-unmapped vendor Custom model/group
// of the same kind (model<->model, group<->group, AvailableSource::displayType
// == "Custom") by similarity of AvailableSource::nodeCount and
// width/height aspect ratio, and maps to the closest match if it is within a
// reasonable tolerance. Destination roots with no candidate within tolerance
// are left unmapped for Phase 105/120. As with RunModelTypeCatchAll, any
// newly-mapped root's still-unmapped Strand/Node children are then filled
// from not-yet-used `<mappedVendorModel>/...` sources of the corresponding
// depth.
void RunCustomDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                              const std::vector<AvailableSource>& available,
                              bool selectOnly,
                              const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                              const std::string& ruleLabel = "");

// Model-type catch-all pass (QuikMap Phase 105), run after
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
// those are left for Phase 100's dimension-based match or Phase 120. As with
// RunCatchAll, any newly-mapped root's still-
// unmapped Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunModelTypeCatchAll(const std::vector<ImportMappingNode*>& roots,
                          const std::vector<AvailableSource>& available,
                          bool selectOnly,
                          const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                          const std::string& ruleLabel = "");

// Group-member dimension match pass (QuikMap Phase 110), run after
// RunModelTypeCatchAll and before RunCatchAll. For each destination root that
// IsGroup(), is already mapped to a vendor ModelGroup, and whose
// corresponding layout ModelGroup (renderContext.GetModel) has at least one
// member, builds the destination group's member-name set
// (ModelGroup::ModelNames) and looks up the mapped vendor ModelGroup's
// AvailableSource::groupMemberNames for its member-name set. If the vendor
// group's member count is more than 3x the destination group's (plus a
// small slack), the pairing is skipped entirely - a vendor group that much
// larger has no real per-member correspondence (e.g. a name/fuzzy match
// landed on a huge "All"-style container group), and dimension-only scoring
// across such a pool tends to pick unrelated prop types just because node
// counts happen to be close. Otherwise, for each still-unmapped, non-group,
// non-skipped destination root whose GetCoreModel() is one of the
// destination group's members, scores every still-unmapped, non-group vendor
// model that is one of the vendor group's members by node-count (and, for
// Custom models, grid-shape) closeness, and maps to the closest one. As with
// the other catch-all-style phases, any newly-mapped root's still-unmapped
// Strand/Node children are then filled from not-yet-used
// `<mappedVendorModel>/...` sources of the corresponding depth.
void RunGroupMemberDimensionMatch(const std::vector<ImportMappingNode*>& roots,
                                   const std::vector<AvailableSource>& available,
                                   RenderContext& renderContext,
                                   bool selectOnly,
                                   const std::unordered_set<const ImportMappingNode*>& selectedTargets,
                                   const std::string& ruleLabel = "");

// Group-member dimension backfill pass (QuikMap Phase 115), run immediately
// after RunGroupMemberDimensionMatch and before RunCatchAll. Uses the same
// name-based-mapped destination-group <-> vendor-ModelGroup pairings as
// RunGroupMemberDimensionMatch (including the same large-vendor-group skip),
// but for any destination group member that is
// still unmapped (e.g. the destination group has more members than the
// vendor group), reuses the closest-by-node-count vendor group member - even
// one already claimed by Phase 110 or an earlier root in this phase - so the
// same vendor source may end up assigned to multiple destinations.
void RunGroupMemberDimensionBackfill(const std::vector<ImportMappingNode*>& roots,
                                      const std::vector<AvailableSource>& available,
                                      RenderContext& renderContext,
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

// Shadow-model skip pass (QuikMap Phase 1), run immediately after RunSkipDMX.
// Marks each destination root whose layout Model has a non-empty ShadowModelFor
// attribute (i.e. IsShadowModel() == true) as skipped. Shadow models are
// overlay/virtual models that carry no independent effect data.
void RunSkipShadow(const std::vector<ImportMappingNode*>& roots,
                   RenderContext& renderContext,
                   const std::string& ruleLabel = "");

// Parses [T:Xxx] / [T:Xxx_Yyy] type-hint tags out of a model's Description
// field and returns the corresponding alias-like strings - a "[T:Matrix]"
// hint is treated exactly as if the model had an alias of "Matrix" for both
// alias-based (Phase 10) and family-based (Fuzzy/GroupContentFuzzy/
// GroupMemberDimension) matching. Unrecognized tags are ignored.
std::vector<std::string> ParseTypeHintAliases(const std::string& description);

} // namespace AutoMapper
