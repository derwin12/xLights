# QuikMap Mapping Phases

QuikMap is the automatic ("non-AI") channel mapper used when importing
effects from one sequence/show into another. It runs in a fixed order of
**phases**. Each phase tries one specific mapping strategy, and only looks
at models that are *still unmapped* when it runs - so an early phase always
gets first pick, and later phases only clean up what's left over.

Phase numbers are spaced out (5, 10, 25, 93, 110...) so new phases can be
slotted in between existing ones without renumbering everything. The number
you see in a QuikMap report (e.g. `[Phase 93: MatrixBackfill]`) tells you
exactly which strategy produced that mapping - handy when a mapping looks
wrong and you want to know why QuikMap made that choice.

This file is a plain-language summary. For the exact technical rules, see
the comment block at the top of `src-core/import_export/AutoMapper.h`.

---

## Phase 0 - Skip DMX models

**What it does:** Marks DMX models (and groups containing one) as
"skipped" so nothing ever tries to map them later. DMX fixtures don't carry
RGB pixel effects the way regular models do.

**Example:** `DMX-Spot-1` is a DMX moving head → skipped, never mapped.

## Phase 1 - Skip shadow models

**What it does:** Skips "shadow" models - overlay/virtual models that
exist only to mirror another model's channels and have no effects of their
own. Detected either by the model's `ShadowModelFor` setting, or by a
start channel that begins with `@` (meaning "start wherever that other
model starts").

**Example:** `MH-DIMMERS` is a shadow of `MH-Pan` → skipped.

## Phase 2 - Skip LED Panel Matrix protocol

**What it does:** Marks models/groups wired through an "LED Panel Matrix"
controller protocol as skipped. These report Matrix-like dimensions/node
counts (so they'd otherwise look like a normal pixel matrix), but they
aren't addressable the same way - mapping them produces a corrupt/garbled
import. This skip also applies to the vendor side: a vendor model with this
protocol is excluded from any matrix-like candidate pool (e.g. Phase 93),
so it can't get round-robined onto every real destination matrix.

**Example:** `Shark` (a face model with `ControllerConnection
Protocol="LED Panel Matrix"`) → skipped, never mapped.

## Phase 5 - Exact name match

**What it does:** The simplest, most confident match - if a vendor model
and one of your models have the exact same name (ignoring upper/lower
case), map them. Compares the whole name as-is (no word-splitting), so
vendor/brand names in a name (see Phase 25's note below) still have to
match exactly here.

**Example:** Your `Cane-1` matches the vendor's `cane-1`.

## Phase 10 - Alias match

**What it does:** Matches a vendor model against any alias you've set on
one of your own models (also tolerant of punctuation differences).

**Example:** Your `Holiday Tree` has an alias `Christmas Tree` → matches
vendor `Christmas Tree`.

## Phase 12 - Custom model exact-dimension match

**What it does:** For Custom-shaped models, pairs one of yours with a
vendor one if they have the exact same node count and grid size -
regardless of name.

**Example:** Your `MyFlake-A` (48 nodes, 8x6 grid) matches vendor
`Snowflake 3` (48 nodes, 8x6 grid) even though the names don't match at all.

## Phase 13 - Custom model submodel-overlap match

**What it does:** For Custom models, matches by shared submodel names (3
or more in common) rather than the parent model's own name.

**Example:** Your `BigStar` has submodels "Ring 1".."Ring 4"; vendor `Star`
has "Ring 1".."Ring 3" → 3 shared submodel names is enough to match.

## Phase 15 - Community alias pack

**What it does:** Matches against a shared, crowdsourced list of known
vendor-name → your-name pairings (downloaded from the community). Skipped
if you don't have that list cached locally.

**Example:** The community list says vendor `xmas tree` = your
`Holiday Pine` → matched, even though you never set up that alias yourself.

## Phase 16 - "Everything" group match

**What it does:** Matches your "everything"/"all props" group to the
vendor's equivalent catch-all group.

**Example:** Your `01 Everything` (208 members) matches vendor `All Props`
(240 members) - both are clearly "contains everything" groups.

## Phase 17 - Special-keyword group match

**What it does:** Matches groups with special sequencer meaning in their
name, like "last", "override", or "bottom" (used for effect layering
order).

**Example:** Your `98 All Override (last group)` matches vendor `All with
no faces (put on bottom for FADES)`.

## Phase 18 - Horizontal/vertical group match

**What it does:** Matches groups whose name (and the actual physical
layout of their members) says "horizontal" or "vertical".

**Example:** Your `Horizontal Matrix` group matches vendor `Horiz Strip` -
both the names and the members' real-world layout agree they're horizontal.

## Phase 20 - Submodel/strand fallback

**What it does:** If a model itself didn't match anything, try matching
its individual submodels/strands by name.

**Example:** Your `WreathA` model has no vendor match, but its submodel
`Ring 1` matches a vendor source named `Ring 1` directly.

## Phase 25 - Fuzzy match (same kind)

**What it does:** "Best guess" name matching - splits names into words,
ignores small differences, and requires enough shared words (and matching
numbers/sides) to be confident. Only matches model-to-model or
group-to-group, never mixes the two. Filler words ("group", "and", "on",
"of", "with", etc.) and known vendor/brand names (GE, EFL, Boscoyo, Chroma,
Twinkly, Impression, Daycor, Living Light Shows/LLS, GRP, SS, Showstopper,
PPD, PixelTrim, PixNode, xTreme) are stripped before comparing, so they
don't drag down or inflate the word-overlap score.

**Example:** Your `Candy-Cane-Left-1` fuzzy-matches vendor `Cane 1 Left` -
the words "cane", "1", "left" line up well enough.

**Group-vs-group tie-breaker:** This phase (and Phases 5/10/27, which share
the same matching code) scans vendor candidates highest-effect-count-first,
so a vendor model/group with substantial real effect data wins any tie over
an equally-name-matching but barely-used one. For group-to-group matches
specifically, if *more than one* vendor group passes the name check equally
well, QuikMap looks at what each group's members actually structurally are
(matrix/tree/star/snowflake, verified the same way Phase 107 does - real
dimensions and `IsAStarModel`/`IsASnowflakeModel`, not just member names) and
prefers the vendor group whose member composition best overlaps your
group's, rather than just taking the first name match found.

**Example:** Your group `Trees Stars` (members are a mix of actual tree and
star models) matches two vendor groups equally well by name: `Flat Trees
And Stars` and `Stars On Trees`. If `Stars On Trees`'s own members are
confirmed to be a tree+star mix while `Flat Trees And Stars`'s members are
actually all stars, QuikMap prefers `Stars On Trees`.

## Phase 26 - Fuzzy match using group contents

**What it does:** Same fuzzy logic as Phase 25, but if a group is made up
entirely of one kind of prop (e.g. all canes), that prop type is folded
into the group's own name for matching purposes.

**Example:** Your group `Group - Candy Canes` (members `Cane-1`..`Cane-4`)
matches vendor group `Canes` (members `Cane 1`..`Cane 3`) - the group names
alone weren't similar enough, but "all members are canes" tips it over.

## Phase 27 - Fuzzy match (mixed kind)

**What it does:** Same as Phase 25, but now allows a model to match a
group, or a group to match a model - only used as a last resort if Phase
25/26 found nothing.

**Example:** Your group `Group - Mega Trees` fuzzy-matches vendor model
`Mega Tree` (a single model, not a group).

## Phase 28 - Family-anchored fuzzy match

**What it does:** A relaxed version of fuzzy matching for props with a
strong "family" keyword (matrix, star, cane, etc.) - doesn't require as
much overall word overlap, since the family word alone is a strong signal.
Guarded so it won't cross singing/non-singing props or mega-tree/small-tree
or star/tree mismatches.

**Example:** Your `Matrix-mini-left` matches vendor `Matrix 2` - the bare
name overlap is weak, but both share the "matrix" family and similar node
counts.

## Phase 30 - Singing prop match

**What it does:** Matches real singing-face props (ones with actual
mouth/eye animation data) to vendor singing props.

**Example:** Your `Santa` (has real mouth/eye node ranges) matches vendor
`Santa Face` (also a singing prop).

## Phase 32 - Singing prop backfill

**What it does:** If you have more singing props than the vendor does,
reuses a vendor singing prop for more than one of yours rather than
leaving extras unmapped.

**Example:** You have `Santa`, `Snowman`, `Elf` (all singing); vendor only
has `Santa Face` and `Snowman Face` → `Elf` reuses `Santa Face`.

## Phase 40 - Floodlight match

**What it does:** Matches floodlight groups and individual floodlights
(single-node-per-string models, often used as wash lights).

**Example:** Your group `Group - Floods` matches vendor `Flood Lights`;
then your `Flood-Left` matches vendor `Flood L` individually.

## Phase 41 - Floodlight backfill

**What it does:** Reuses vendor floodlights for any of yours left over
after Phase 40.

**Example:** You have 4 floods, vendor has 2 → your 3rd and 4th floods
reuse the vendor's 1st and 2nd.

## Phase 65 - Best-guess by model class

**What it does:** For anything still unmapped, makes a best guess based on
the model's general "class" (a simplified category like SpiralTree),
ignoring names entirely. Singing props are excluded (handled earlier).

**Example:** Your `SpiralPumpkin` is classified as a spiral tree → matches
vendor `SpiralTree 1`, even though no words in the names overlap.

## Phase 90 - Like-model backfill

**What it does:** If you have a numbered sibling already mapped (e.g.
`Cane-1`), and another sibling with the same naming pattern (`Cane-4`) is
still unmapped, works out the matching vendor name by substituting the
number.

**Example:** Your `Cane-1` is mapped to vendor `Cane 1`; your `Cane-4` is
unmapped → QuikMap tries vendor `Cane 4` and maps it if available.

## Phase 93 - Matrix backfill

**What it does:** Matches anything that structurally looks like a flat
pixel grid/matrix (by actual shape, not name) to the closest-sized vendor
matrix. Spinning trees, cubes, and spheres are explicitly excluded even
though they can look grid-shaped on paper.

**Example:** Your `Pixel Grid` (32x16 Custom model) matches the
closest-sized vendor matrix by dimensions, no name overlap needed.

## Phase 95 - Group-coverage skip

**What it does:** If a group is already mapped and all its members are one
kind of prop, marks those individual members as skipped - so they aren't
separately (and redundantly) re-mapped one by one later.

**Example:** Your `Group - Arches` (all `Arch-N` members) is mapped →
`Arch-1`..`Arch-6` are marked skipped.

## Phase 100 - Custom-dimension match

**What it does:** For Custom models with a known node count, finds the
closest-matching vendor Custom model by node count/grid shape, still
respecting family and singing-prop status.

**Example:** Your `HFlake1` (46 nodes) matches vendor `Snowflake 2` (46
nodes) - same "flake" family, closest node count available.

## Phase 107 - Homogeneous-family group backfill

**What it does:** Looks for one of your groups where *every single
member* is the same recognized type (all matrix, all star, or all
snowflake). If so, treats that as confirmation that type is present in
your show, then maps any other leftover model of that same type to the
closest vendor match.

**Example:** Your group `Group 12` has members `Star A`..`Star D`, all
recognized as stars → confirms "star" is present, so your separate, unmapped
`Star E` (outside that group) gets matched to the best remaining vendor
star.

## Phase 110 - Group-member dimension match

**What it does:** If one of your groups is already mapped to a vendor
group, uses that vendor group's member list as a trusted pool to map your
group's individual unmapped members by closest size.

**Example:** Your group `Group - Snowflakes` is mapped to vendor
`Snowflakes`; your member `EFlake46` (46 nodes) matches the vendor group's
member `Snowflake 2` (46 nodes).

## Phase 115 - Group-member dimension backfill

**What it does:** Same idea as Phase 110, but for when you have *more*
group members than the vendor group does - reuses vendor members instead
of leaving extras unmapped.

**Example:** Vendor group `Stars` only has 3 members but your group has 5
stars → the 4th and 5th reuse vendor members 1 and 2.

## Phase 120 - Final catch-all

**What it does:** The last-resort pass - pairs anything still unmapped
purely by general shape/kind compatibility, ignoring names. Still blocks
obviously wrong pairings (a line-shaped prop can't become a grid; a singing
prop can't become a non-singing one).

**Example:** Your leftover `PropX` (a "Tree 360" type) is still unmapped;
vendor `LeftoverA` (also "Tree 360") is still unmapped → paired by shape,
no name match needed.

## Phase 125 - Sibling-reuse backfill

**What it does:** For anything still unmapped, checks if a same-pattern
sibling of yours already has a mapping, and reuses it.

**Example:** Your `Md Star - 02` is unmapped; its sibling `Md Star - 01`
is mapped to vendor `Star 1` → `Md Star - 02` reuses `Star 1`.

---

## Quick reference table

| Phase | Name | One-liner |
|---|---|---|
| 0 | Skip DMX | Never map DMX fixtures |
| 1 | Skip shadow models | Never map overlay/shadow models |
| 2 | Skip LED Panel Matrix protocol | Never map LED-panel-wired models, either side |
| 5 | Exact name | Same name, different case |
| 10 | Alias | Matches one of your saved aliases |
| 12 | Custom exact dimension | Same node count + grid size |
| 13 | Custom submodel overlap | 3+ shared submodel names |
| 15 | Community alias pack | Crowdsourced name pairings |
| 16 | Everything group | Matches your "all props" group |
| 17 | Special-keyword group | Matches "last"/"override"/"bottom" groups |
| 18 | Horizontal/vertical group | Matches by orientation |
| 20 | Submodel fallback | Matches submodels/strands by name |
| 25 | Fuzzy (same kind) | Best-guess word matching |
| 26 | Fuzzy (group contents) | Fuzzy + "all one kind of prop" boost |
| 27 | Fuzzy (mixed kind) | Fuzzy, model<->group allowed |
| 28 | Family-anchored fuzzy | Relaxed match on a strong family keyword |
| 30 | Singing prop | Matches real singing-face props |
| 32 | Singing prop backfill | Reuses singing props if you have more |
| 40 | Floodlight | Matches flood groups/individuals |
| 41 | Floodlight backfill | Reuses floods if you have more |
| 65 | Best-guess by class | Matches by simplified model category |
| 90 | Like-model backfill | Numbered-sibling substitution |
| 93 | Matrix backfill | Matches by real grid/matrix shape |
| 95 | Group-coverage skip | Skips members of an already-mapped group |
| 100 | Custom dimension | Closest Custom model by node count |
| 107 | Homogeneous-family group backfill | "All-one-type" group confirms a family is present |
| 110 | Group-member dimension | Maps group members via the group's vendor pool |
| 115 | Group-member dimension backfill | Same, reusing pool if you have more members |
| 120 | Final catch-all | Last resort, shape-only pairing |
| 125 | Sibling-reuse backfill | Reuses a same-pattern sibling's mapping |
