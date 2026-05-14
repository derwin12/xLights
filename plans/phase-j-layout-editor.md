# Phase J — Layout Editor (iPad)

**Status:** J-0 ✓ 2026-05-07; J-1 common-properties surface ✓
2026-05-08; J-2 substantially complete 2026-05-08 — tap-to-select
(2D + 3D), selection rendering via `ScreenLocation::DrawHandles`,
drag-to-move (2D), per-type handle drag (2D + 3D) via the
descriptor pipeline (`GetHandles` / `BeginDrag` / `DragSession`,
see [`handle-system-refactor.md`](handle-system-refactor.md)),
grid + bounding-box overlays, snap-to-grid, keyboard nudge,
layout undo. Pending: 3D body-drag (camera-aware delta math),
text labels, rubber-band multi-select, per-type properties (J-3).
Promoted from [`future-layout-editing.md`](future-layout-editing.md)
on 2026-05-07 after the iPad app entered App Store review. Phase S
of the gap analysis is the engineering reference; this file is
the iPad-side sub-plan.

**Interaction design** for the touch + Pencil UX layer lives in
[`phase-j-touch-ux.md`](phase-j-touch-ux.md) — toolbar-driven
tool selection, persistent modifier toggles (replacing
Shift/Ctrl), Pencil hover as cursor-equivalent, barrel-tap /
double-tap mappings, and what changes vs stays the same in the
descriptor pipeline.

## Why this matters

Today the iPad app loads a show's pre-existing layout and plays it
back; it cannot *arrange* models. Every iPad user is therefore
dependent on a Mac/PC running desktop xLights to do the layout
work — model placement, sizing, rotation, model groups, layout
groups, locking, the whole sidebar. That's the single largest
remaining workflow gap once App Store submission ships and is the
right next phase to take on.

Source pointers:

- Desktop UI: `src-ui-wx/layout/LayoutPanel.cpp` (10,357 lines),
  `LayoutPanel.h`, `ModelPreview.cpp/.h`.
- Property adapters: `src-ui-wx/modelproperties/` (~40 per-model
  adapters subclassing `ModelPropertyAdapter`).
- Edit math (already wx-free, in `src-core/models/`):
  `ModelScreenLocation.h`, `BoxedScreenLocation.cpp`,
  `TwoPointScreenLocation.cpp`, `ThreePointScreenLocation.cpp`,
  `PolyPointScreenLocation.cpp`, `MultiPointScreenLocation.cpp`,
  `TerrainScreenLocation.cpp`. Drawing math, hit detection, handle
  movement, and locking are all reusable as-is from iPad.
- Align / distribute / flip / resize-to-match math:
  `LayoutPanel.cpp` lines ~5620–6630 (8 align ops + 3 distribute +
  flip H/V + resize-to-match width/height/both). All multi-select
  aware, single undo point per op.
- Undo: `LayoutPanel.cpp` lines 8052–8330. `UndoStep` struct stores
  XML snapshots; portable as data.

## Decisions (2026-05-07)

1. **Full Phase S scope this phase** (J-0 → J-4 below). One ship.
2. **Surfacing:** new full-screen detachable `WindowGroup` opened
   from the **Tools menu → "Edit Layout…"** entry in
   `XLightsCommands.swift:247`. No toolbar button on
   `SequencerView`. Keeps the sequencer toolbar simple and matches
   the precedent set by Import Effects / Check Sequence / Package
   Logs.
3. **Stay model-focused for v1.** PR #6311's controller-source-tree
   on the desktop Layout panel is a model-selection convenience
   only — does not extend the property grid to controller config.
   Worth revisiting after J-4 ships, but not in scope here.
   Standalone Controllers tab work continues to live in
   [`future-controllers-tab.md`](future-controllers-tab.md).
4. **3D editing is in scope.** A 6-DOF translate / rotate / scale
   gizmo on the selected model in 3D view. This is the biggest
   single design question in the phase — see "Risks" below.

## Scope

### J-0 — Bridge surface + read-only layout view ✓ 2026-05-07

Got the screen on the device, validated the detached-window
pattern, established the bridge-surface conventions for layout
state, and shipped a usable read-only inspector. Mutation work
runs in J-1+.

**What landed:**

- Bridge: `XLSequenceDocument.modelsInActiveLayoutGroup`,
  `modelLayoutSummary(_:)`, `layoutDisplayState`. All read-only.
- `iPadRenderContext` parses `Display2DGrid` / `Display2DGridSpacing`
  / `Display2DBoundingBox` from `<settings>` in
  `xlights_rgbeffects.xml` (it already had `Display2DCenter0`).
- `WindowGroup("layout-editor")` in `XLightsApp.swift` with
  `LayoutEditorWindowRoot` and the same token-guarded auto-restore
  protection used by F-1 detached previews.
- Tools menu → "Edit Layout…" entry (`EditLayoutMenuItem` in
  `XLightsCommands.swift`), enabled when a show folder is loaded.
- `LayoutEditorView` — full-screen `NavigationSplitView` with a
  Metal canvas (reuses `PreviewPaneView` with previewName
  `"LayoutEditor"`), layout-group picker in the toolbar, sidebar
  with model list + selected-model summary + display-state
  section. Selection is **sidebar-list driven** — tap a model
  name in the list to inspect its summary.
- `SequencerViewModel.layoutEditorSelectedModel`,
  `layoutEditorOpen`.

**Pulled forward to J-2** (clusters with handle / gizmo / overlay
rendering work):

- Tap-to-select inside the Metal canvas — needs a screen→world
  unproject helper on `XLMetalBridge`.
- In-canvas overlays (model name labels, first-pixel markers, 2D
  grid lines, bounding boxes, selection ring) — needs draw paths
  through `iPadModelPreview` / `XLGridMetalBridge`. Shipping these
  alongside the J-2 handle-rendering work avoids two passes
  through the Metal layer.

### J-1 — Property grid for selected model ✓ 2026-05-08 (common-properties surface)

Common-properties editing landed; per-type properties (Arch count,
Tree branches, Custom-model matrix, DMX channel mapping) are J-3
work. Model rename and copy/paste / reset menus are J-3+ as well.

**What landed:**

- Bridge: `XLSequenceDocument.setLayoutModelProperty(name:key:value:)`,
  `saveLayoutChanges`, `hasUnsavedLayoutChanges`. Per-key dispatch
  to `Model::SetHcenterPos` / `SetVcenterPos` / `SetDcenterPos` /
  `SetWidth` / `SetHeight` / `SetDepth`, `ScreenLocation::SetRotateX/Y/Z`
  / `SetLocked`, `BaseObject::SetLayoutGroup`,
  `Model::SetControllerName`. Each successful edit marks the model
  in `iPadRenderContext._dirtyLayoutModels`; nothing hits disk
  until `saveLayoutChanges` is called.
- Save path: `iPadRenderContext::SaveLayoutChanges` round-trips
  `xlights_rgbeffects.xml`, replacing each dirty model's
  `<model>` node with a fresh `XmlSerializer::SerializeModel`
  output (same path desktop uses for export). Mirrors the existing
  `SaveModelStates` pattern.
- UI: `LayoutEditorPropertiesView` replaces J-0's read-only summary.
  Editable cells for centre X/Y/Z, width/height/depth, rotate
  X/Y/Z, locked toggle, layout-group menu, controller-name field.
  Read-only cells preserved for type, channel range, strings,
  nodes. Save button in toolbar with dirty-state gating; alert on
  write failure.
- Layout-group reassignment refreshes the sidebar model list so
  models that move out of the active group disappear from the
  list immediately.

**Pulled to J-3** (per-type adapter zoo):
- Model rename — needs old-name → new-name bookkeeping in the
  dirty set so `SaveLayoutChanges` can find the on-disk node.
- Long-press → Reset / Copy / Paste menu via
  `PropertyContextMenu`.
- Per-type properties (Arch count, Tree branches, etc.).
- Greying-out of position/size/rotation cells when locked is on
  (today they accept input but the bridge silently rejects via
  `BaseObject::IsLocked` check — works correctly but is bad UX).

### J-2 — Direct manipulation + canvas overlays (in progress)

**First cut ✓ 2026-05-08:**
- `XLMetalBridge.pickModel(atScreenPoint:viewSize:for:)` —
  inverse of `iPadModelPreview::StartDrawing`'s 2D View matrix +
  bounding-box hit-test; iterates active-preview models in
  reverse draw order so the topmost rendered model wins. 2D-only
  (3D ray-cast deferred).
- Single-tap recognizer in `PreviewPaneView`, gated to
  `previewName == "LayoutEditor"`, drives
  `viewModel.layoutEditorSelectedModel`. Tap on empty space
  deselects.
- `XLMetalBridge.setSelectedModel(_:)` plus an `addStep` on the
  transparent program in `drawModelsForDocument` that draws a
  cyan bounding-box outline via `xlVertexAccumulator` /
  `drawLines`. Geometry is in world coords so pan / zoom track
  for free.
- Drag-to-move: `handleOneFingerPan`'s `.began` hit-tests the
  touch and, when it lands on the selected model in 2D, sets a
  `draggingLayoutModel` flag that routes `.changed` deltas
  through `XLMetalBridge.moveModel(_:byDeltaDX:dY:viewSize:for:)`
  instead of the camera-pan path. Drag end posts
  `.layoutEditorModelMoved` so the editor side panel refreshes
  the summary + dirty state. Locked models are silently rejected
  by the bridge.

**Pulled forward from J-0:**
- ✓ 2026-05-08: 2D grid + canvas-bounds bounding-box overlays
  drawn via `xlVertexAccumulator` `addStep` calls on the solid /
  transparent programs in `drawModelsForDocument`. Initial state
  seeds from the rgbeffects.xml `Display2DGrid` /
  `Display2DBoundingBox` flags; live toggles in the LayoutEditor
  controls overlay (`Grid` / `Bounds`). Draws snap to spacing
  multiples in 2D; hidden in 3D mode.
- Model-name / model-info / first-pixel label overlays still
  pending — these need text rendering through
  `CoreGraphicsTextDrawingContext` and a coordinate-projection
  helper, more involved than the simple primitive overlays above.

**Layout undo:**
- ✓ 2026-05-08: `iPadRenderContext` owns a 100-deep undo stack of
  `LayoutUndoEntry` snapshots (modelName + every common-property
  field). Bridge: `pushLayoutUndoSnapshotForModel:`,
  `undoLastLayoutChange`, `canUndoLayoutChange`. The LayoutEditor
  pushes a snapshot before every commit + once at drag-began
  (so a single drag = a single undo entry). Toolbar Undo button
  pops + reapplies via the same setter path. Undo includes a
  refresh of the model list because `layoutGroup` and
  `controllerName` may have changed.
- Redo is not implemented — desktop's `LayoutPanel` doesn't have
  redo either, so iPad parity stays "undo only."

**Snap + keyboard nudge:**
- ✓ 2026-05-08: Snap-to-grid toggle in the LayoutEditor controls
  overlay. Bridge's `moveModel` rounds the post-delta centre to
  the nearest `Display2DGridSpacing` multiple when the toggle is
  on. Off by default. Per-session.
- ✓ 2026-05-08: Keyboard nudge — arrow keys move the selected
  model 1 unit, shift+arrow moves 10. Pushed undo per tap so
  each nudge is independently reversible. Posts
  `.layoutEditorModelMoved` so the canvas repaints (the
  notification convention now uses `object: previewName` +
  `userInfo["model"]: name`, with the bridge coordinator
  listening for repaint).

**Resize / rotate / per-type handles + 3D selection:**
- ✓ 2026-05-08: Refactored from a custom 4-corner system to delegate
  to the existing `ModelScreenLocation::DrawHandles` /
  `CheckIfOverHandles` / `MoveHandle` API (plus the 3D variants).
  Same code path desktop's LayoutPanel uses; zero wx
  dependencies in the call chain. `XLMetalBridge.pickHandle` and
  `dragHandle:toScreenPoint:` are now thin shims that build the
  ray (3D) or pixel coords (2D) and forward.
- This brings along, **for free**:
  - **Per-type handle sets**: Boxed gets 4 corners + rotate; 3D
    Boxed gets 8 corners (top/bottom × 4); ThreePoint gets
    endpoints + shear; PolyPoint gets vertices + curve control
    points; etc.
  - **Rotate handle** — already wired into MoveHandle via
    `ROTATE_HANDLE`.
  - **3D handle drag** — `MoveHandle3D` shipped same time as 2D.
    Camera-aware drag math handled inside the existing code.
  - **Polyline vertex editing** — drag a vertex, drag a
    curve control point, all through PolyPointScreenLocation.
- ✓ 2026-05-08: **3D model picking** via `Model::HitTest3D` ray-cast
  through `VectorMath::ScreenPosToWorldRay`. Returns the closest
  hit by intersection distance. Replaces the previous "return
  nil in 3D" stub.
  - **DisplayModelOnWindow params** for the LayoutEditor pane now
    mirror desktop's `ModelPreview::RenderModels` (line 612):
    `color = xlYELLOW` (selected) / `xlLIGHT_GREY` (others) so
    models render in the layout-edit override colour rather than
    effect output, `allowSelected = true` (so PrepareToDraw
    updates ModelMatrix — required by HitTest3D /
    CheckIfOverHandles3D), `wiring = false`, `highlightFirst`
    bound to a "Pixel 1" toggle in the controls overlay. The
    selected model gets `Selected(true)` per draw so
    `Model::DisplayModelOnWindow`'s built-in DrawHandles path
    fires (Model.cpp:3254) — the bridge no longer draws the
    selection ring or handles itself.
  - **Gotcha #1** for future sessions: omitting `allowSelected=true`
    leaves `ModelMatrix` uninitialised, so HitTest3D ray-casts
    against an identity matrix and misses every model not at
    world origin. Was the bug behind "3D tap doesn't select."
  - **Gotcha #2**: `iPadModelPreview::GetCameraZoomForHandles()`
    must return `1.0` in 2D mode (mirroring desktop's
    `ModelPreview::GetCameraZoomForHandles`), not the active
    camera zoom. The handle-width math multiplies by zoom, and
    the View matrix multiplies by zoom AGAIN at draw time;
    returning the live zoom in both places quadratically scales
    handles, making them invisible at zoom < 1 and huge at
    zoom > 1. Was the bug behind "handles don't appear in 2D."
  - **Gotcha #3**: 3D mode in `BoxedScreenLocation::DrawHandles`
    (5-arg) only draws the centre sphere + `DrawAxisTool` gizmo
    (red/green/blue translate arrows, scale cubes, or rotate
    rings depending on `axis_tool` mode) when
    `active_handle != NO_HANDLE`. Desktop sets it to
    `CENTER_HANDLE` on select (`LayoutPanel.cpp:3152`). Without
    this the user sees only the bounding-box wireframe — no
    actionable handles. The bridge now sets / clears
    `SetActiveHandle` on select / deselect alongside the
    `Selected(true)` flag.
  - **Gotcha #4**: 3D `CheckIfOverHandles3D` returns the handle
    index OR'd with `HANDLE_AXIS` (0x0200000) when an axis arrow
    is hit, with the axis (`X_AXIS` / `Y_AXIS` / `Z_AXIS`) in
    the lower 8 bits. Desktop (`LayoutPanel.cpp:3697`) extracts
    the axis, calls `SetActiveAxis(axis)`, and then passes the
    model's CURRENT `active_handle` (CENTER_HANDLE) to
    MoveHandle3D — not the raw return value. Passing the raw
    `HANDLE_AXIS|axis` value to MoveHandle3D crashes via
    `assert(false)` in `ModelScreenLocation::DragHandle`'s
    default switch branch. Bridge `pickHandle` now does this
    extraction; `endHandleDrag` clears the axis on gesture end.
  - **Gotcha #5**: `BaseObject::MoveHandle3D` and
    `Model::MoveHandle` both return early on `IsFromBase()`
    (models imported from a parent "base" show — desktop's
    Christmas-village pattern of inheriting layout from a
    template). Drag handles still **draw** for from-base models
    (in `FROM_BASE_HANDLES_COLOUR = xlPURPLETRANSLUCENT`) so
    visually they look interactive, but the move is silently
    rejected. iPad's `pickHandle` now short-circuits on
    `m->IsFromBase()` so the drag doesn't engage at all and the
    user gets the camera-orbit fallback. Same protection
    already existed for `loc.IsLocked()`. The 3-arg / 4-arg
    `MSLAXIS` enum: values are `X_AXIS=0`, `Y_AXIS=1`,
    `Z_AXIS=2`, `NO_AXIS=3` — first enumerator gets 0, so
    `axis=0` in logs is X_AXIS, not "no axis."
  - **Gotcha #6**: `SubModel::GetModelScreenLocation()` (see
    `SubModel.h:29`) returns a reference to the **parent's**
    `screenLocation` — submodels do NOT have their own. They're
    effects-buffer carve-outs of a parent (a strand of an arch,
    a face on a DMX moving head), not standalone layout
    entities. Desktop's `LayoutPanel` never exposes them as
    selectable / movable. The iPad LayoutEditor pane filters
    them out at the source: `pickModel`, `drawModelsForDocument`,
    and `XLSequenceDocument.modelsInActiveLayoutGroup` all
    `continue` on `model->GetDisplayAs() == DisplayAsType::SubModel`.
    Without the filter, iterating a submodel of the selected
    model wiped the parent's `active_handle` (since
    `model->GetName()` doesn't match `_selectedModelName` →
    "non-selected" branch fires → clears the SHARED
    screenLocation). Symptom: handles drawn but no drag — every
    pickHandle saw `active_handle = NO_HANDLE` because a
    submodel iteration had cleared it.
- 3D body-drag (move-the-whole-model in 3D) still gated on 2D —
  needs camera-aware delta math we haven't shipped. Drag-on-empty-
  space-in-3D continues to orbit camera. Drag-on-handle in 3D works.

**Direct manipulation (still pending):**
- **Resize handles** drawn as overlay quads sized for touch
  (~32 pt). Map touch through
  `BoxedScreenLocation::MoveHandle` / sibling subclasses. Each
  ScreenLocation subclass already implements its own handle set —
  iPad just needs to render them and translate touch → handle
  index. No core math changes.
- **Rotate handle** — same pattern.
- **PolyLine vertex editing** — tap-to-add-vertex, drag vertex,
  long-press to remove. PolyPointScreenLocation already has the
  segment-control-point math.
- **3D gizmo** — see "Risks" for the design open question.
  Working assumption: a translate/rotate/scale gizmo with
  per-axis handles, pinch-on-handle = uniform scale, two-finger
  twist = rotate-about-axis. Detailed design happens in J-2's
  first week before we commit code.
- **Pinch ambiguity**: pinch on empty space = camera zoom (J-0
  already); pinch on selected model = uniform scale (new).
  Decision tree based on hit-test at gesture-start.
- **Rubber-band multi-select** — two-finger drag in empty space.
  Mirrors the existing two-finger marquee idiom in the effects
  grid.
- **Keyboard nudge** — arrow keys (with Bluetooth keyboard) for
  pixel / sub-pixel movement. Reuses `XLightsCommands` keyboard
  routing.
- **Locking** respected: handles don't render on locked models;
  drag is no-op.
- **Undo / redo** — port `UndoStep` struct verbatim from
  `LayoutPanel.cpp:8235`. Hook to `viewModel.undoManager` (already
  on `SequencerViewModel`). Per-op (drag = single undo entry,
  recorded on touch-down state).

### J-3 — Per-type properties + model creation (~4–6 wk)

The long tail. Per-model property pages and the Add Model toolbar.

**Per-type properties:**

- Port adapter logic for the high-traffic models first: Matrix,
  Tree, Arch, Star, Custom, PolyLine, Single Line, Icicles,
  Window Frame, Wreath, Candy Cane, Cube, Sphere, Image. ~14 of
  the ~40 desktop adapters cover ~95% of shipping model usage.
- Remaining adapters land opportunistically; missing per-type
  page = read-only "Edit on desktop" placeholder.
- DMX deep authoring (channel mapping, fixture definitions) stays
  in [`future-custom-models.md`](future-custom-models.md) — this
  phase only covers DMX position/dimension editing.
- Face / State editing also stays in `future-custom-models.md`.

**Model creation:**

- Add Model toolbar at the top of the editor screen with sticky-
  toggle buttons (Arch, Tree, Star, Matrix, Cube, etc.).
- Tap-button-then-tap-canvas to drop. Drag during the drop sets
  initial bounding box.
- Polyline gets a multi-tap mode (each tap adds a vertex; double-
  tap or button-deselect ends).
- Reuses desktop's `CreateNewModel(type)` factory and
  `Model::InitializeLocation()` flow — both already wx-free.

**Model groups:**

- Create / delete / rename model group.
- Drag-to-add-to-group + remove. UI shape TBD; likely a sheet
  with two columns (in-group / available) similar to
  `DisplayElementsSheet`.

### J-17 — Objects tab finish ✓ 2026-05-14

Closes the remaining Objects-tab deferred items in one pass.

**View-object rename:**

- `renameViewObject:to:` — sanitizes via `Model::SafeModelName`,
  refuses collisions / 2D Background. Calls
  `ViewObjectManager::Rename` (returns false on the desktop
  because its cross-reference iteration is commented out — we
  verify by lookup instead).
- `iPadRenderContext::_renamedViewObjects` map + extended
  HasDirty / Clear / SaveLayoutChanges entry guard.
- `SaveLayoutChanges` view-object patch branch: when a dirty
  VO has a pending rename, locates the on-disk element by the
  OLD name, updates the `name` attribute, then patches the
  rest.

**View-object duplicate (shallow):**

- `duplicateViewObject:` — round-trips through
  `XmlSerializingVisitor` → `ViewObjectManager::CreateObject`
  so per-type attrs (Mesh ObjFile, Image bitmap, Terrain
  heightmap PointData) come along automatically. Unique name
  via `GenerateObjectName`, position offset (+50, +50, 0),
  lock cleared. 2D Background refused.

**Unified undo for VOs + heightmap:**

- `LayoutUndoEntry` now discriminated by `UndoTarget::
  {Model, ViewObject, ViewObjectHeightmap}`. VO entries
  snapshot world position + scale (via
  `BoxedScreenLocation::GetScaleX/Y/Z` for boxed VOs;
  `GetScaleMatrix()` fallback for others) + rotation + locked
  + layoutGroup. Heightmap entries snapshot just the
  PointData string.
- New bridge methods:
  `pushLayoutUndoSnapshotForViewObject:` and
  `pushTerrainHeightmapUndoSnapshot:`.
- `UndoLastLayoutChange` dispatches by entry kind. One Undo
  button reverts whatever the user did last regardless of
  kind.
- Push sites added: `commitObjectProperty`, every VO drag /
  pinch / twist gesture begin (handle drag, body drag in
  2D + 3D, pinch, twist), and the terrain heightmap tap
  before each brush.

**SwiftUI:**

- VO property pane "Name" row gets pencil + duplicate icons
  alongside the name. Pencil → opens
  `RenameGroupSheet` (now generic — accepts `kindLabel:
  "Object"` to title the sheet correctly). Duplicate → fires
  the bridge, auto-selects the new copy so the user can drag
  it into place.
- `RenameGroupSheet` generalized: new `kindLabel` parameter
  (defaults to "Group") swaps title + footer text. Same
  sanitization preview + collision check.
- `ViewObjectCrudModifiers` gained the rename sheet sheet
  hosting + sanitize callback + existing-names lookup
  (model + group + object names combined for collision).

### J-16 — Group name sanitization ✓ 2026-05-14

Follow-up to the rename work: neither the create nor the rename
sheet validated against the desktop's character restrictions. The
canonical sanitizer is `Model::SafeModelName` which strips
`, ~ ! ; < > " ' & : | @ / \ \t \r \n` plus surrounding
whitespace. (`,` and `/` are wire-format delimiters — `,`
separates members in a group's `models` attribute, `/` separates
parent from submodel in fully-qualified names.)

**Bridge** (`XLSequenceDocument`):

- New `sanitizedModelName:` wraps `Model::SafeModelName` so the
  SwiftUI sheets can preview the sanitized form before submit.
- `createModelGroup:` and `renameModelGroup:to:` now run the
  same sanitizer internally as defence-in-depth — passing a
  name with illegal characters doesn't fail, it just takes
  effect with the bad characters silently stripped (matches
  desktop convention).

**SwiftUI:**

- `NewGroupSheet` and `RenameGroupSheet` both:
  - Live-preview the sanitized name with an info banner
    ("Will save as 'X'") when input differs from sanitized.
  - Caption listing the disallowed characters so the user knows
    what got stripped without trial-and-error.
  - Disable Create/Rename when sanitized result is empty or
    collides with another model/group.
  - Submit the sanitized name (not the raw text).

### J-16 — Group rename ✓ 2026-05-14

Closes the last J-7 / J-9 group-CRUD deferral.

**Bridge** (`XLSequenceDocument`):

- `renameModelGroup:to:` — refuses collisions with existing
  model/group names, empty/same-as-old. Calls
  `ModelManager::Rename` which updates the in-memory
  references in every group containing this one (member list
  vectors get the name fix). After rename, walks all groups
  and marks any that now directly reference the new name as
  dirty so the save patcher rewrites their `models` attribute.

**Render context** (`iPadRenderContext`):

- New `_renamedGroups: map<newName → oldOnDiskName>` slot. The
  `MarkGroupRenamed` helper handles the edge cases:
  rename-after-rename collapses to the original on-disk name;
  renaming back to the original drops the pending rename;
  rename of an in-memory-only created group just retitles the
  pending creation.
- `MarkGroupDeleted` cleans up any pending rename for the
  deleted name so the delete pass finds the right element on
  disk.
- `HasDirtyLayoutModels` + `ClearDirtyLayoutModels` /
  `SaveLayoutChanges` entry guard now include
  `_renamedGroups`.
- `SaveLayoutChanges` group-patch branch: when a dirty group
  has a renamed-from entry, finds the element by the OLD name
  and updates its `name` attribute before patching the rest.

**SwiftUI:**

- New `RenameGroupSheet` — text field pre-filled with the
  current name, live collision check against models AND groups
  (excluding the current name so submitting unchanged closes
  cleanly), Cancel / Rename buttons.
- Group property pane's "Name" row gains a small pencil icon.
  Tap → opens the rename sheet.
- `handleRenameGroup` re-points the sidebar selection to the
  new name on success so the property pane reopens on the
  renamed group.

**Dropped from the deferred list:**

- ~~Cross-pane drag from Models list onto Group member list~~
  — out of scope; the AddMember sheet's tree picker covers
  the use case.
- ~~Member-count cap warnings~~ — controllers don't see
  groups; the cap concept only applies to models.

**Still deferred (with re-framed semantics):**

- Group duplicate — when prioritized, will be a **shallow
  copy**: new group named `<name>-1` with the same
  `ModelNames` vector + copy of group settings. Members are
  not cloned. (The earlier "semantics question" framing was
  me overthinking — shallow is what users want.)

### J-15 — VO 3D drag + pinch + twist ✓ 2026-05-14

Closes the J-13 deferred gestures: 3D body-drag, pinch-to-scale,
and two-finger twist-to-rotate all now work on the selected
view object.

**Bridge** (`XLMetalBridge`):

- New begin methods: `beginBodyDrag3DForViewObject:atScreenPoint:viewSize:forDocument:`,
  `beginPinchScaleForViewObject:forDocument:`,
  `beginTwistRotateForViewObject:forDocument:`. Each grabs the
  VO's `ObjectScreenLocation`, latches the same saved-state
  fields the model path uses, and flips a target-is-VO flag.
- The existing `dragBody3DToScreenPoint:`,
  `applyPinchScaleFactor:`, `applyTwistRotationRadians:`,
  `endBodyDrag3D`, `endPinchScale`, `endTwistRotate` now branch
  on the flag: read the screen location from
  `ViewObjectManager` instead of `ModelManager`, and call
  `MarkLayoutViewObjectDirty` at the dirty-mark step.

**SwiftUI** (`PreviewPaneView`):

- `handlePinch` `.began`: if no model is the target and a VO is
  selected, tries `beginPinchScale(forViewObject:…)`. Falls
  through to camera zoom on miss / non-applicable target.
- `handleRotate` `.began`: same pattern for twist-to-rotate
  using `beginTwistRotate(forViewObject:…)`.
- `handleOneFingerPan` `.began`: VO branch upgrades 3D path
  to call `beginBodyDrag3D(forViewObject:…)` instead of
  bailing out (2D path unchanged — uses `moveViewObject:`).
- `.changed` for `draggingLayoutViewObject`: 3D dispatches
  through `dragBody3D(toScreenPoint:)`; 2D continues to use
  `moveViewObject:byDeltaDX:dY:`.
- `.ended`: VO 3D drag calls `endBodyDrag3D()` (state cleanup
  on the bridge).

### J-14 — VO endpoint handle drag ✓ 2026-05-14

Drag-to-move on a view object body works (J-13), but the handles
that draw at the endpoints of a selected Ruler / any boxed VO
corner weren't tappable — clicking them fell through to body
drag.

**Bridge** (`XLMetalBridge`):

- New `pickViewObjectHandleAtScreenPoint:viewSize:forDocument:`.
  Mirrors `pickHandleAtScreenPoint:` for models but routes
  through the SELECTED view object's
  `ObjectScreenLocation`. Calls `loc.GetHandles(...)` →
  `handles::HitTest` → `loc.CreateDragSession(...)`. On a hit,
  stashes the VO name in a new `_dragSessionViewObjectName`
  state slot. Refuses the 2D Background pseudo-object.
- `endHandleDragForDocument:` extended: when
  `_dragSessionViewObjectName` is non-empty at commit time,
  marks the VO dirty + reloads it (so the canvas re-reads the
  new endpoint positions) instead of the model dirty path.
  Cleared on session end.

**SwiftUI** (`PreviewPaneView`):

- Pan `.began` for a selected VO tries
  `pickViewObjectHandle:` first. On a hit, sets
  `draggingLayoutHandle` (reused from the model path) and
  pipes touch updates through the existing
  `bridge.dragHandle:` — which routes via `_dragSession`
  regardless of target type. Falls through to body drag
  (`moveViewObject:`) on a miss.

The result: tapping a ruler's start or end endpoint and
dragging now moves just that endpoint, with the other end
staying put — matching desktop behaviour.

**Deferred:** 3D body drag for VOs (still no plane-anchor
session); pinch-to-scale / twist-to-rotate on view objects.

### J-14 — VO field gaps from J-12 + J-13 testing ✓ 2026-05-14

User report: missing Mesh-only toggle, missing Scale X/Y/Z on
Mesh+Image, Ruler showing center+rotate instead of two-point
endpoints.

**Bridge** (`XLSequenceDocument.viewObjectLayoutSummary:` +
`setLayoutViewObjectProperty:`):

- Mesh: `meshOnly` (BOOL), `scaleX/Y/Z` (read via
  `BoxedScreenLocation::GetScaleX/Y/Z`, written via the
  matching setters).
- Image: `scaleX/Y/Z` (same `BoxedScreenLocation` plumbing).
- Ruler (and any future two-point view object): `twoPoint = YES`
  discriminator plus `p1X/Y/Z` (absolute world coords of point 1
  = `WorldPos`) and `p2X/Y/Z` (absolute world coords of point 2 =
  `WorldPos + (X2,Y2,Z2)`). Setting `p1*` keeps the absolute
  point 2 unchanged by re-basing `X2/Y2/Z2`; setting `p2*` just
  updates the offset.

**Save patcher** (`iPadRenderContext::SaveLayoutChanges`):

- Mesh: writes `MeshOnly`.
- Ruler / any two-point VO: writes `X2/Y2/Z2`. WorldPos (X/Y/Z =
  point 1) is already written by the common path.

**SwiftUI** (`LayoutEditorObjectPropertiesView`):

- Mesh per-type rows: added Mesh-only toggle + Scale X/Y/Z
  (3-decimal precision via new `scaleField` helper).
- Image per-type rows: added Scale X/Y/Z.
- Size/Location section reorganizes for two-point VOs: shows
  Point 1 X/Y/Z and Point 2 X/Y/Z instead of Centre + Rotate.
  Locked stays at the bottom.
- Dimensions section hidden for two-point VOs (the line's extent
  comes from its endpoints, not from bounding-box dimensions).

### J-13 — VO drag-to-move + basic terrain heightmap editor ✓ 2026-05-14

**Drag-to-move for view objects (2D)**

Closes the J-6 deferred "drag-to-move on canvas" gap for Mesh /
Image / Gridlines / Terrain. 2D only for now — 3D body-drag
re-uses the existing plane-anchor math, but that work is deferred.

- New `pickViewObjectAtScreenPoint:viewSize:forDocument:` —
  mirrors `pickModelAtScreenPoint`, searches
  `ViewObjectManager`. 2D box-test + 3D `HitTest3D` ray cast.
  Returns the topmost (last-drawn) hit.
- New `moveViewObject:byDeltaDX:dY:viewSize:forDocument:` —
  delta-based 2D move with identical math to `moveModel:` but
  writes through `vo->SetHcenterPos / SetVcenterPos` and marks
  the VO dirty. Honours locked, isFromBase, snap-to-grid.
- `PreviewPaneView.handleSingleTap`: when no model is under the
  touch, try the VO hit-test next. Matching VO selection flips
  to the Objects tab via the J-11 mutex.
- `PreviewPaneView` one-finger pan: state vars
  `draggingLayoutViewObject` + `layoutDragViewObjectName`
  parallel the model drag slots. `.began` checks for a selected
  VO under the touch and starts a VO drag rather than falling
  through to camera-pan; `.changed` dispatches to
  `moveViewObject:`; `.ended` posts the standard
  `layoutEditorModelMoved` notification so the property pane
  re-reads the new transform.

**Basic terrain heightmap editor**

- New `editTerrainHeight:atScreenPoint:viewSize:delta:brushRadiusPoints:forDocument:`
  on `XLMetalBridge`. Unprojects the touch (2D or 3D ray-onto-
  plane), maps to the terrain's `(u, v)` grid using its
  spacing/width/depth, reads the current `PointData` heights,
  applies `delta` at the nearest grid point. When
  `brushRadiusPoints > 0`, applies a cosine falloff to
  neighbouring points within the radius for a smoother
  deformation. Writes the new heights back to
  `TerrainScreenLocation::SetDataFromString`, increments change
  count, marks the VO dirty.
- View-model state on `SequencerViewModel`: `terrainEditTarget`
  (terrain name when edit mode is on, nil otherwise),
  `terrainEditDelta` (per-tap magnitude), `terrainEditRaise`
  (Raise / Lower direction toggle), `terrainEditBrushPoints`
  (brush radius in screen points; 0 = single-point edit).
- Property pane: `LayoutEditorObjectPropertiesView.terrainEditControls`
  block renders inside the per-type Terrain section. Edit
  Heightmap toggle, Raise/Lower segmented control, Step
  magnitude slider (0.1–10), Brush radius slider (0–80pt).
- Canvas tap handler short-circuits when
  `terrainEditTarget != nil`: signs the delta from the Raise
  toggle, calls `editTerrainHeight`, repaints. Miss falls
  through to normal pick.
- Edit mode auto-clears when: the selected object changes away
  from the terrain, the user clears selection, or the editor
  window closes.

**Deferred:**

- 3D body-drag for view objects (existing plane-anchor pipeline
  is model-specific; same lift as a parallel
  `beginViewObjectBodyDrag3D` set of methods).
- Heightmap undo (each tap is a separate dirty event; would need
  a snapshot stack analogous to `_layoutUndoStack` for VOs).
- Smoothing / level-out / flatten tools (current editor only
  raises or lowers; no brush profiles).
- Mesh-only mode toggle for Mesh objects.

### J-12 — Objects tab: per-type editing, file pickers, create/delete ✓ 2026-05-14

Brought the Objects tab up to feature parity with the Models +
Groups tabs.

**Bridge** (`XLSequenceDocument`):

- `viewObjectLayoutSummary:` now returns `typeKind` discriminator
  + per-type fields:
  - Mesh: `objFile`, `brightness`
  - Image: `imageFile`, `brightness`, `transparency`
  - Gridlines: `gridSpacing`, `gridWidth`, `gridHeight`,
    `gridColor`, `hasAxis`, `pointToFront`
  - Terrain: `imageFile`, `brightness`, `transparency`,
    `gridSpacing`, `gridWidth`, `gridDepth`, `hideGrid`,
    `hideImage`, `gridColor`
  - Ruler: `units` (0..5 enum index), `length`, `unitOptions`
- `setLayoutViewObjectProperty:` accepts every key above plus
  `active`. Dynamic-cast guards mean a key sent to the wrong
  type silently no-ops.
- `availableViewObjectTypes` — types the iPad accepts for
  creation. Ruler is filtered out when one already exists
  (it's a show-singleton).
- `createViewObjectWithType:` — calls
  `ViewObjectManager::CreateAndAddObject`, defaults layout
  group to active, records the auto-generated name in
  `_createdViewObjects` for the save patcher.
- `deleteViewObject:` — `ViewObjectManager::Delete` + record
  in `_deletedViewObjects`. Refuses `2D Background` since it's
  a pseudo-object.

**Render context** (`iPadRenderContext`):

- New `_createdViewObjects` / `_deletedViewObjects` sets +
  `MarkViewObjectCreated` / `MarkViewObjectDeleted` helpers
  (parallel to the J-7 group plumbing).
- `SaveLayoutChanges`:
  - Pass: drop deleted `<view_object>` elements.
  - Pass: append fresh elements for created objects (just the
    `name` + `DisplayAs` attrs; the existing dirty-patcher
    fills in the rest).
  - Existing dirty-patcher: now also writes per-type attrs
    (`ObjFile`, `Brightness`, `Image`, `Transparency`,
    `GridLineSpacing`/`GridWidth`/`GridHeight`/`GridColor`/
    `GridAxis`/`PointToFront`, terrain's `Terrian*` legacy
    spellings, ruler `Units`/`Length`).
- `HasDirtyLayoutModels` / `ClearDirtyLayoutModels` cover the
  new sets.

**SwiftUI:**

- `LayoutEditorObjectPropertiesView` rewritten with
  `DisclosureGroup`s in the same order as the Models tab:
  Header → per-type (expanded) → Appearance → Dimensions →
  Size/Location. Per-type body switches on the `typeKind`
  discriminator so each VO type shows the right control set.
- Tag / Grid color via the same `ColorPicker` + hex-bridge
  helpers used by the Models tab.
- File pickers for Mesh `.obj`, Image bitmap, and Terrain
  image. Generic `onPickFile(key, types)` callback in the
  property view → `handleObjectFilePick` in
  LayoutEditorView writes through `commitObjectProperty`.
- Objects roster: `+` button in the section header opens an
  `AddViewObjectSheet` with the available types; swipe-left
  on a row → delete confirmation. "2D Background" is filtered
  out of both affordances (pseudo-object).
- `ViewObjectCrudModifiers` factored out as a `ViewModifier`
  (sheet + delete alert + file picker) to keep
  `LayoutEditorView.body` under the type-checker's complexity
  budget.

**Deferred:** view-object undo (parallel snapshot stack for
ScreenLocation + per-type fields), drag-to-move on the canvas
(existing drag pipeline assumes ModelManager), terrain
heightmap editing (deep canvas tool).

### J-11 — Sidebar selection mutual exclusion ✓ 2026-05-14

User report: with a group selected on the Groups tab, tapping a
model on the canvas left BOTH selections active (group's cyan
member tint + model's yellow primary highlight). Confusing.

New rule: **at most one of {model, group, object} is selected at
any time**, and the sidebar tab follows the active selection.

**Behaviour:**

- Pick a model (sidebar Models list OR canvas tap) → clears any
  group/object selection AND flips the sidebar to the Models tab
  so the property pane matches what's selected on the canvas.
- Pick a group → clears any model / object selection. Tab switch
  is implicit (sidebar tap was already on Groups).
- Pick an object (or "2D Background" pseudo-object) → clears any
  model / group selection.
- Switch tabs via the segmented picker → clears whatever
  selections were live in the tabs you're leaving. Eliminates
  "I'm on the Groups tab but a model is still tinted on the
  canvas from earlier".

**Implementation:** new `SidebarSelectionMutex` `ViewModifier`
hosts the four `.onChange` handlers (model / group / object /
sidebarTab). Factored out because `LayoutEditorView.body` was
already at the type-checker's complexity ceiling — adding the
four chained `.onChange` modifiers inline pushed it over.

### J-10 — Group polish round 2 ✓ 2026-05-14

Three follow-ups from J-9 testing:

**1. Members list height.** The J-9 SwiftUI `List` inside the
property pane's outer `ScrollView` collapsed to a ~2-row internal
scroll area, making drag-to-reorder nearly impossible. Replaced
with a manual `VStack` of rows + per-row `.draggable` /
`.dropDestination` so the outer pane scrolls the whole content
and every member is visible. Drop-target highlight shows where
the drag will land. A trailing drop on the container (outside any
row) appends to the end.

**2. Layout Style options.** Bumped from the J-9 4-entry set to
all 15 desktop options (`ModelGroupPanel.wxs`). The 4 special
entries keep their compact lowercase wire form
(`grid`/`minimalGrid`/`horizontal`/`vertical`); the rest
round-trip their display label verbatim, matching desktop's
`OnChoiceModelLayoutTypeSelect` fall-through.

**3. Group canvas tint.** Cyan didn't differentiate against the
default grey at pixel sizes 1–2 (the common case). Switched to
yellow for both primary selection and group members — the active
gizmo / handles still distinguish "what's actively editable" from
"what's a member of the selected group".

Submodel-only groups: when a member is `Parent/Sub` (resolved to
a `SubModel*` via `ModelManager`), the new render path collects
those into `selectedGroupSubmodelsByParent`. After the main model
render loop, each such submodel is drawn as a yellow overlay on
top of its parent — `DisplayModelOnWindow` on a `SubModel`
iterates only that submodel's `Nodes`, so only those node ranges
light up. Mirrors desktop's "group of submodels → highlight just
the submodel regions" behaviour.

**Deferred:** drag from outside the property pane (e.g. drop a
model from the Models tab list onto the member list). Touch-only
gesture work; the in-list reorder is sufficient for the common
authoring flow.

### J-9 — Group property polish ✓ 2026-05-14

Closed every J-7 group-CRUD deferral that affects daily authoring.
Settings get proper pickers, members get a tree-picker for
submodels and drag-to-reorder, plus a tag color.

**Bridge** (`XLSequenceDocument`):

- `modelGroupLayoutSummary:` now also returns:
  - `layoutStyleOptions`: NSArray<NSDictionary{value,label}>.
    Four hard-coded styles matching desktop's
    ModelGroupPanel.wxs (`grid`/`minimalGrid`/`horizontal`/
    `vertical`, with their friendly labels).
  - `defaultCameraOptions`: NSArray<NSString>. Always starts
    with `"2D"`, then every 3D camera the show has saved via
    `ViewpointMgr`.
  - `tagColor`: `#RRGGBB` (round-trips via `Model::SetTagColourAsString`
    which ModelGroup inherits).
- `setLayoutModelGroupProperty:`:
  - `tagColor` (NSString) — round-trips through
    `SetTagColourAsString`.
  - `members` (NSArray<NSString>) — replaces the entire member
    list. Used by drag-to-reorder; future bulk-move can reuse
    the same path.
- New `submodelsForModel:` returns the full-name list
  (`Parent/Sub`) for one parent — lazy lookup so the
  AddMember tree only loads what the user expands.

**SwiftUI:**

- `LayoutEditorGroupPropertiesView`:
  - Default Camera → Menu populated from
    `defaultCameraOptions`. Tap to select; check mark on the
    current value.
  - Layout Style → Menu with the 4 friendly labels. If the on-
    disk value isn't in the list (legacy XML), shows "Custom: …"
    label + "Currently: X" footer entry so the user can see what's
    there without losing it.
  - Grid Size → int field (precision 0), narrower frame.
  - Tag Color → `ColorPicker` row with hex preview (same bridge
    helper used by the Models tab).
  - Members → wrapped in a SwiftUI `List` with `.onMove` for
    drag-to-reorder + `.onDelete` for swipe-to-remove. An Edit
    toggle on the Members header switches between view-mode
    (red minus button per row) and edit-mode (drag handles).
- `AddMemberSheet`:
  - Switched from a flat `editMode: .active` selection list to
    a custom tree. Top-level rows for models / groups; chevron
    button toggles a row to surface its submodels (lazy-fetched
    via the new `submodelsForModel:` bridge).
  - Tap-to-pick at either level — user can add `Arch1`,
    `Arch1/Inner`, or both. Picked rows show a filled checkmark;
    rows already in the group dim out with an "(already a
    member)" hint so the tree stays informative.
  - Search filter matches against parent name OR any submodel
    name, so typing `"Inner"` surfaces the parent row whose
    submodel contains it (parallels the vendor search filter).

**Deferred:**

- Group rename. Same plan as J-7 — needs an old-name → new-name
  map threaded into the save patcher.
- Drag from Models tab into a Group's member list (would skip
  the Add Member sheet entirely for casual edits). Touch-only
  gesture work; not blocking.

### J-8 — 2D Background as a synthetic Objects-tab entry ✓ 2026-05-14

Moved the per-layout-group background settings (image / brightness
/ alpha / scale) out of the read-only "Display" roll-up and into
the Objects tab as a synthetic "2D Background" pseudo-object. It's
not in `ViewObjectManager` — it's an attribute of the active
layout group — so the bridge special-cases it.

**Bridge** (`iPadRenderContext`):

- `SetActiveBackgroundImage:` / `Brightness:` / `Alpha:` /
  `ScaleBackgroundImage:` write through to the active group's
  storage (top-level `_backgroundImage…` for Default, or the
  matching `_namedLayoutGroups[i]` for a named group). Each
  records the group name in `_dirtyBackgroundGroups`.
- `HasDirtyLayoutModels` / `ClearDirtyLayoutModels` now also
  include the background dirty set.
- `SaveLayoutChanges`: new pass writes `backgroundImage`,
  `backgroundBrightness`, `backgroundAlpha`, `scaleImage`
  attributes onto the matching XML target (`<settings>` for
  Default, `<layoutGroups><layoutGroup name="...">` for named).

**Bridge** (`XLSequenceDocument`):

- `viewObjectsInActiveLayoutGroup` always returns
  `"2D Background"` at index 0 (even before a show is loaded).
- `viewObjectLayoutSummary:@"2D Background"` returns a
  schema-flagged dictionary (`isBackground: YES`,
  `backgroundImage`, `backgroundBrightness`, `backgroundAlpha`,
  `scaleBackgroundImage`).
- `setLayoutViewObjectProperty:` routes 2D Background keys to
  the new `iPadRenderContext` setters instead of
  `ViewObjectManager`.

**SwiftUI:**

- New `LayoutEditorBackgroundPropertiesView` — image path with
  folder-picker + clear button, Brightness + Alpha int fields,
  Scale-to-fit toggle, plus a one-liner explaining that this
  is a per-group attribute rather than a moveable object.
- `propertyPaneBody` dispatches to it when the selection's
  `isBackground` is true.
- Background image uses `.fileImporter` with image UTTypes
  (png/jpeg/tiff/bmp/gif). Same security-scoped-bookmark dance
  as the existing model importer.
- "Background" row removed from the Models tab's `displaySection`
  — it's now editable on the Objects tab instead of read-only on
  every tab.

**Deferred:**

- "Show 2D Background only when relevant" — currently visible
  even in 3D-default shows. Desktop hides backgrounds when the
  layout is 3D-only.

### J-8 — Desktop-order collapsible property sections ✓ 2026-05-14

Reorganized the Models tab property pane to match desktop's
wxPropertyGrid ordering, and made the sections collapsible
(SwiftUI `DisclosureGroup`).

**New section order:**

1. Header (always visible): Name, Type
2. **Model Properties** — per-type descriptors. Expanded by
   default. Section title = the model's `displayAs` so each
   model type shows its own category name (matches desktop).
3. **Controller Connection** — Preview (layoutGroup), Controller,
   Channel range (read-only).
4. **String Properties** — String Type (NODE_TYPE picker),
   Strings, Nodes.
5. **Appearance** — Active, Pixel Size, Pixel Style, Transparency,
   Black Transparency, Tag Color, Description.
6. **Dimensions** — Width, Height, Depth.
7. **Size/Location** — Centre X/Y/Z, Rotate X/Y/Z, Locked.

**Bridge additions** to `modelLayoutSummary:`: `active`,
`pixelSize`, `pixelStyle` (+`pixelStyleOptions`), `transparency`,
`blackTransparency`, `tagColor`, `stringType`
(+`stringTypeOptions` — desktop's NODE_TYPE_VALUES verbatim),
`description`. Corresponding cases added to
`setLayoutModelProperty:key:value:`.

**SwiftUI changes:**

- `LayoutEditorPropertiesView` rewritten as a stack of
  `DisclosureGroup`s. Per-type descriptor rendering moved
  inline (the standalone `LayoutEditorTypePropertiesView` is
  retired for the Models tab — kept around for future reuse).
- `section(_:title:)` generic helper materializes the content
  before handing it to `DisclosureGroup`, sidestepping
  "non-escaping captured by escaping closure" on the content
  builder.
- Tag Color uses SwiftUI's `ColorPicker` with a `#RRGGBB`
  bridge via new `Color.toHexString()` / `Color(hexString:)`
  extension helpers.
- Per-section expanded state is `@State Bool`. Default:
  Model Properties expanded, everything else collapsed.

### J-7 — Bridge null-guard fix ✓ 2026-05-14

User reported `EXC_BAD_ACCESS` at address 0x28 in
`modelGroupsInActiveLayoutGroup`. Stack: libc++ `__tree::begin()`
→ `map::map(const map&)` (copy ctor) → `ModelManager::GetModels()`
→ the new bridge method.

**Root cause:** offset 0x28 (= 40) is exactly the offset of
`std::map<std::string, Model*> models;` inside `ModelManager`
(after vtable + 2 pointers + bool/padding + 2 ints). So the copy
ctor's source map address is `(ModelManager*)0 + 40` →
`_modelManager` is `nullptr` and `*_modelManager` was UB.
`iPadRenderContext::GetModelManager()` returns `*_modelManager`
without guarding — `_modelManager` is only created inside
`LoadShowFolder`. The Layout Editor can be reached before a
show is loaded.

**Fix:**

- New `iPadRenderContext::HasModelManager()` /
  `HasViewObjectManager()` accessors return true iff the
  unique_ptr is set.
- Every J-5/J-6/J-7 bridge method that calls
  `GetModelManager()` / `GetAllObjects()` direct (not via
  `GetModelsForActivePreview()` which guards internally) now
  early-returns when these are nil. Affected methods:
  - `modelGroupsInActiveLayoutGroup`,
    `modelGroupLayoutSummary:`,
    `setLayoutModelGroupProperty:key:value:`
  - `viewObjectsInActiveLayoutGroup`,
    `viewObjectLayoutSummary:`,
    `setLayoutViewObjectProperty:key:value:`
  - `perTypePropertiesForModel:`,
    `setPerTypeProperty:onModel:value:`
  - `addModel:toGroup:`, `removeModel:fromGroup:`,
    `createModelGroup:members:`, `deleteModelGroup:`
  - `flipModels:axis:forDocument:`,
    `duplicateModels:forDocument:` (on XLMetalBridge).

### J-7 — Multi-select: Flip / Duplicate / Group-from-selection ✓ 2026-05-14

Three high-frequency operations that closed remaining J-4 (multi-
select) gaps and made the single-model + multi-select action bars
feature-complete for the common authoring workflows.

**Bridge surface** (`XLMetalBridge`):

- `flipModels:axis:forDocument:` — `horizontal` rotates 180°
  about Y_AXIS, `vertical` about X_AXIS. Mirrors desktop's
  `BaseObject::FlipHorizontal` / `FlipVertical`. Each model
  flips about its own origin (not the selection's centroid);
  this matches desktop and avoids a separate "selection
  bounding box" computation.
- `duplicateModels:forDocument:` — round-trips each source
  through `XmlSerializer::SerializeModel`, gets a unique name
  via `ModelManager::GenerateModelName`, clears controller
  mapping, unlocks, offsets by (+50, +50, 0), and re-deserializes
  via `ModelManager::CreateModel`. Returns the new names in
  source order. ModelGroup duplication is intentionally skipped
  (member-reference semantics are ambiguous — share with the
  source, or deep-copy?).

**SwiftUI:**

- `MultiSelectActionBar`: gained a Flip ▾ menu (H / V) before
  Match Size; Duplicate + Group buttons after, separated from
  Clear by a divider.
- `InlineModelActionBar` (single-model): gained a Duplicate
  button next to Delete. Same `performDuplicate(of: [selected])`
  helper so the single + multi paths share one code path.
- `NewGroupSheet` reused for Group-from-selection: a new
  `pendingGroupFromSelection: [String]?` state slot tells
  `handleCreateGroup` whether to pass `nil` (empty group) or
  the captured selection (group-from-selection). On success
  the sidebar flips to the Groups tab so the user immediately
  sees the new group + its members.
- `performDuplicate` shifts the multi-selection to the new
  duplicates so the user can drag / nudge them right away.

**Deferred:**

- ModelGroup duplicate (semantics question).
- View-object duplicate (would need a parallel
  `duplicateViewObjects:` since they live in `ViewObjectManager`).
- "Flip around selection centroid" — common desktop affordance
  but needs centroid math + a UI toggle distinguishing per-
  model vs. group flip.

### J-7 — Model group CRUD ✓ 2026-05-14

The Groups tab is no longer settings-only. Users can create new
groups, delete groups, and edit membership (add / remove) from
the sidebar without bouncing back to desktop.

**Bridge surface** (`XLSequenceDocument.h`):

- `addModel:toGroup:` / `removeModel:fromGroup:` — member ops.
  `add` is a no-op for already-direct members; both fire
  `MarkLayoutModelDirty(group)` so the patcher rewrites the
  `<modelGroup>` element on save.
- `createModelGroup:members:` — creates the group with curated
  defaults (layout="minimalGrid", gridSize=400,
  DefaultCamera="2D", LayoutGroup=active). Refuses to collide
  with an existing model/group name.
- `deleteModelGroup:` — removes from `ModelManager` AND records
  the name in `_deletedGroups` so the save patcher drops the
  `<modelGroup>` element.

**Render context** (`iPadRenderContext`):

- New `_createdGroups` + `_deletedGroups` sets. `MarkGroupCreated`
  / `MarkGroupDeleted` helpers handle the create-then-delete-
  then-create-with-same-name edge cases.
- `HasDirtyLayoutModels` now also reports created/deleted state
  (the "unsaved changes" indicator stays accurate).
- `SaveLayoutChanges` extended:
  - Pass 0a: remove `<modelGroup>` elements for every deleted
    name.
  - Pass 0b: append fresh `<modelGroup>` elements for created
    names (full attribute set including `models`,
    `centreDefined`, `selected`).
  - Existing group-patch path: now also writes the `models`
    comma-delimited member list so add/remove edits persist.

**SwiftUI:**

- `NewGroupSheet` — name field + collision check (existing
  models or groups in `ModelManager`). Defaults focus to the
  text field. Created group is auto-selected so the user can
  start adding members.
- `AddMemberSheet` — multi-select picker, lists every model in
  the active layout group that isn't yet a direct member.
  Filterable. `editMode = .active` so SwiftUI's
  selection-with-checkmarks UI lights up.
- Groups list: swipe-left on a row → confirm-then-delete. Plus
  button in the section header opens NewGroupSheet. Empty-state
  surface includes a "Create a Group" CTA.
- Group property pane: each member row gets a red minus button
  (one-tap remove); "Members" row gets a plus button that opens
  AddMemberSheet. Read-only roll-up replaced.
- `GroupCrudModifiers` factored out as a `ViewModifier` so the
  three new presentations (two sheets + alert) don't blow
  LayoutEditorView's body past the type-checker's complexity
  budget.

**Deferred:**

- Group rename. Would need a per-name "renamed from → to" map
  threaded through save so the patcher can find the old
  `<modelGroup>` element. Manageable but not blocking.
- Drag-to-reorder members in the property pane (member order
  affects the buffer style — but the bridge doesn't currently
  expose a "move member to position N" op).
- Member-count cap warnings (some controllers have soft limits).

### J-6 — Sidebar → canvas selection sync ✓ 2026-05-14

Closes the deferred-from-J-5 promise that "tapping a group selects
it on the canvas". Picking in the Groups tab now tints every
member of the selected group cyan in the preview, so the user
sees at a glance what the group contains. Picking in the Objects
tab puts handles on the view object so it can be inspected /
edited directly via the canvas in addition to the property pane.

**View model** (`SequencerViewModel`):

- New `layoutEditorSelectedGroup: String?` and
  `layoutEditorSelectedObject: String?`. Independent of
  `layoutEditorSelectedModel`, so flipping tabs preserves every
  tab's selection.
- `LayoutEditorView` switched from local `@State` to view-model
  bindings (`groupListBinding` / `objectListBinding`).

**Canvas bridge** (`XLMetalBridge`):

- `setSelectedGroup:` / `setSelectedViewObject:` setters.
- Model render loop resolves the selected group's flat members
  once per frame via `ModelGroup::GetFlatModels(true, false)` and
  tints any member that isn't already the primary / extras
  selection with `xlCYAN`. Primary stays yellow; extras stay
  yellow.
- View-object render loop calls `Selected(true)` and
  `Draw(..., allowSelected=true)` on the matching object so its
  ScreenLocation handles appear.

**PreviewPaneView** diffs the new selections against
`Coordinator.lastPushedGroup` / `lastPushedObject` and only
repaints on Δ.

**Deferred:**

- Drag-to-move on a selected group (would move every member —
  needs a separate gesture handler).
- Drag-to-move / resize on a selected view object via canvas
  handles (the handles draw, but the existing model-drag
  pipeline assumes the target is in `ModelManager`).
- "Group bounding box" overlay — groups don't have a natural
  geometric extent so cyan member tint is the chosen affordance
  rather than a bounding box.

### J-6 — View object editing + save ✓ 2026-05-14

Removes the "use desktop to edit view objects" footnote from the
J-5 Objects tab. Mesh / Image / Gridlines / Terrain / Ruler now
support the same common-property surface as models (centre X/Y/Z,
width/height/depth, rotate X/Y/Z, locked, layoutGroup).

**Bridge surface** (`XLSequenceDocument.h`):

- `setLayoutViewObjectProperty:key:value:` — single setter
  covering the eleven keys above. Mirrors
  `setLayoutModelProperty:` for symmetry.

**Save infrastructure** (`iPadRenderContext`):

- New `_dirtyLayoutViewObjects` set parallels
  `_dirtyLayoutModels`.
- `MarkLayoutViewObjectDirty(name)` helper added.
- `SaveLayoutChanges` extended: after patching `<model>` /
  `<modelGroup>` elements, walks `<view_objects>` for each dirty
  view object and patches `WorldPosX/Y/Z`, `ScaleX/Y/Z`,
  `RotateX/Y/Z`, `LayoutGroup`, `Locked` attributes in place.
  Per-type attributes (Mesh `ObjFile`, Image bitmap path,
  Terrain heightmap path) round-trip untouched.
- `ClearDirtyLayoutChanges` drains both sets so the Discard
  Changes path leaves no stale dirty marks.

**SwiftUI:** `LayoutEditorObjectPropertiesView` upgraded from
read-only labels to the same editable rows the Models tab uses
(`LayoutEditorDoubleField` + Toggle + layoutGroup Menu). The
"View object editing on iPad is read-only" footer removed.
`commitObjectProperty(...)` mirrors `commitProperty` minus undo
bookkeeping — view objects don't ride the model undo stack yet.

**Deferred:**

- View-object undo (parallel `LayoutUndoEntry` variant for
  ScreenLocation snapshots).
- Per-type editing (Mesh `.obj` path picker, Image bitmap
  picker, Terrain heightmap, Ruler unit). These need their own
  file pickers + dependency-resolution paths.

### J-6 — Per-type model properties ✓ 2026-05-14

Closes the biggest user-visible gap right after the sidebar
refactor: picking a Tree without seeing "# Branches" / "Bottom-
Top Ratio" or a Matrix without seeing "# Strings" / "Nodes per
String" was jarring. The new Models tab property pane shows a
"Type Properties" section below the common transform fields.

**Bridge surface** (`XLSequenceDocument.h`):

- `perTypePropertiesForModel:` — returns
  `NSArray<NSDictionary>` of descriptors. Each entry has `key`,
  `label`, `kind` (`int`/`double`/`bool`/`enum`/`string`),
  `value`, plus optional `min`/`max`/`step`/`precision`/`options`/
  `enabled`/`help`. Iterating descriptors avoids 18 hand-written
  SwiftUI views.
- `setPerTypeProperty:onModel:value:` — single setter that
  dispatches on the descriptor key. Calls `Reinitialize()` so
  geometry / node count updates land immediately, then marks the
  model dirty.

**Types covered:** Matrix, Tree (extends Matrix), Sphere (extends
Matrix), Star, Arches, Icicles, Circle, Wreath, Single Line,
Candy Canes (partial — Sticks/Reverse/AlternateNodes editable;
height + nodes/cane setter not exposed by header), Spinner,
Window Frame, Cube, Channel Block, Custom (read-only matrix dims).

**SwiftUI renderer:** `LayoutEditorTypePropertiesView` is the
single generic component — it reads the descriptor list and
renders a numeric / toggle / menu / text control per `kind`.
Same `commit(key, value)` callback shape as
`LayoutEditorPropertiesView`. Wired into the Models tab via a
new `commitPerTypeProperty(...)` that pushes undo, sets, bumps
the summary token, repaints the canvas, and refreshes the model
list (string-count edits can shift channel ranges).

**Deferred to a later pass:**

- DMX models (per-fixture channel mapping, moving-head config).
- Image / Label / SubModel.
- PolyLine / MultiPoint vertex editor (the existing tap-and-
  manipulate canvas affordance already covers most of this).
- Custom-model matrix authoring (needs a 2D grid editor).
- CandyCane `nodesPerCane` + `height` setters (header exposes
  getters but not corresponding `Set…` methods).

### J-5 — Sidebar tabs (Models / Groups / Objects) ✓ 2026-05-14

Replaces the single scrolling sidebar where selecting a model
required scrolling past the entire model list to reach its
properties. The sidebar is now a vertical split with a draggable
divider:

- **Top half:** segmented Picker → Models / Groups / Objects.
  Each tab has its own search field and its own selection
  (switching tabs preserves the others'). Controllers tab
  intentionally skipped — iPad has no controller editor yet.
- **Bottom half:** scrollable property pane bound to whichever
  tab is active. Models reuse the existing
  `LayoutEditorPropertiesView`; Groups get
  `LayoutEditorGroupPropertiesView` (layout group, default
  camera, layout style, grid size, 2D centre, locked + read-only
  members roll-up); Objects get `LayoutEditorObjectPropertiesView`
  (transform/locked, all read-only in J-5).
- Canvas selection still follows the Models tab only — picking a
  group/object shows its properties but does not select it on the
  canvas (no canvas-side group/object selection model exists yet
  on iPad).

**Bridge additions** (`XLSequenceDocument.h`):

- `modelGroupsInActiveLayoutGroup` — names of ModelGroups whose
  layout_group matches the active group (or "All Previews").
- `modelGroupLayoutSummary:` — dictionary mirroring
  `modelLayoutSummary` plus group-specific fields (members,
  defaultCamera, layout, gridSize, centerX/Y, centerDefined).
- `setLayoutModelGroupProperty:key:value:` — supports
  layoutGroup, locked, defaultCamera, layout, gridSize,
  centerX/Y. Uses the same dirty/undo plumbing as model edits.
- `viewObjectsInActiveLayoutGroup` / `viewObjectLayoutSummary:`
  — read-only view-object surface (no setter in J-5).

`iPadRenderContext::SaveLayoutChanges` now patches the
`<modelGroup>` element's attributes in place when a dirty name
resolves to a ModelGroup (groups don't round-trip cleanly through
`XmlSerializer::SerializeModel` because their on-disk form is a
flat attribute list under `<modelGroups>`, not `<models>`).

**Deferred to a later pass:**

- ViewObject editing + save infrastructure (`<view_objects>`
  patcher in `SaveLayoutChanges`).
- Group/object canvas selection (highlight the group's bounding
  box when picked from the sidebar).
- Default-camera picker (needs a `<perspectives>` enumeration
  bridge); layout-style picker (needs the enum of valid styles
  exposed).
- Controllers tab (whole controller-setup surface).

### J-4 — Multi-select operations (~1–2 wk)

Direct ports of the desktop math; the work is the contextual
toolbar + selection handling.

- **Align (8 ops):** top, bottom, ground, left, right, h-center,
  v-center, d-center.
- **Distribute (3):** horizontal, vertical, depth.
- **Flip (2):** horizontal, vertical.
- **Resize to match:** width, height, both.
- **Bulk edit:** select N models → set one property → applies to
  all.
- All available from a contextual toolbar that surfaces only when
  ≥2 models are selected. Single undo entry per op.

## Out of scope (S-pro / future)

These ride in [`future-layout-editing.md`](future-layout-editing.md)
or sibling `future-*.md` files; do not creep them into J-0..J-4:

- Custom-model authoring (matrix editor, draw model). Lives in
  [`future-custom-models.md`](future-custom-models.md).
- Face / State definition editors.
- DMX channel / fixture authoring.
- Wiring view / Visualise port-mapping.
- DXF / STL / VRML export.
- ImportPreviewsModelsDialog.
- ViewObject heavy editing — MeshObject, TerrainObject heightmap
  painting, RulerObject. (J-0..J-4 covers ImageObject and
  GridlinesObject only.)
- ControllerModelDialog (port-mapping diagram), PixelTestDialog —
  in [`future-controllers-tab.md`](future-controllers-tab.md).
- The PR #6311 controller-source-tree — revisit once J-4 ships.

## Bridge surface — full list

For J-0..J-4, `XLSequenceDocument.h` gains:

```objc
// Layout state
- (NSArray<NSString*>*)modelsInActiveLayoutGroup;
- (NSDictionary*)modelLayoutSummary:(NSString*)name;
- (NSDictionary*)layoutBackgroundState;
- (BOOL)setLayoutBackgroundProperty:(NSString*)key value:(id)value;

// Per-model property edits (generic; routes to Model::SetProperty)
- (id)getModelProperty:(NSString*)name key:(NSString*)key;
- (BOOL)setModelProperty:(NSString*)name key:(NSString*)key value:(id)value;

// Model lifecycle
- (NSString*)createModelOfType:(NSString*)type
                       atPoint:(CGPoint)worldXY
                          size:(CGSize)worldWH;
- (BOOL)deleteModel:(NSString*)name;
- (BOOL)renameModel:(NSString*)oldName to:(NSString*)newName;
- (BOOL)setModelLocked:(NSString*)name locked:(BOOL)locked;

// Direct manipulation (called from gesture handlers; same shape
// as desktop's CheckIfOverHandles / MoveHandle)
- (NSInteger)hitTestHandleForModel:(NSString*)name
                          atPoint:(CGPoint)worldXY
                            zoom:(CGFloat)zoom;
- (BOOL)moveHandle:(NSInteger)handleIndex
          forModel:(NSString*)name
           toPoint:(CGPoint)worldXY;

// Multi-select ops (J-4)
- (BOOL)alignModels:(NSArray<NSString*>*)names mode:(NSInteger)mode;
- (BOOL)distributeModels:(NSArray<NSString*>*)names axis:(NSInteger)axis;
- (BOOL)flipModels:(NSArray<NSString*>*)names axis:(NSInteger)axis;
- (BOOL)resizeModels:(NSArray<NSString*>*)names
              matchWidth:(BOOL)w
             matchHeight:(BOOL)h;

// Layout undo
- (NSData*)snapshotLayoutState;
- (BOOL)restoreLayoutState:(NSData*)snapshot;

// Model-group editing
- (BOOL)createModelGroup:(NSString*)name;
- (BOOL)deleteModelGroup:(NSString*)name;
- (BOOL)addModel:(NSString*)model toGroup:(NSString*)group;
- (BOOL)removeModel:(NSString*)model fromGroup:(NSString*)group;
```

The exact signatures will firm up during J-0 implementation; the
above is the surface to plan against, not a contract.

## Risks

- **3D gizmo design.** Touch 3D editing is the genuinely novel
  part — the desktop relies on mouse precision the iPad doesn't
  have. Plan to spend the first week of J-2 on a focused design
  pass (paper / Figma) before committing code. Fallback if it
  blocks: ship 2D editing in J-2, treat 3D gizmo as a post-J-4
  follow-up. Don't let it gate the rest of the phase.
- **Per-type adapter coverage.** 40 adapters is a lot; J-3 covers
  ~14 high-traffic ones. The remaining tail risks user reports of
  "I can't edit X on iPad." Mitigation: read-only fallback that
  links to "edit on desktop" for missing types.
- **Undo coherence with sequencer undo.** `SequencerViewModel`
  already owns an `UndoManager` for effect edits. Layout edits
  can share the same undo manager (and therefore the same Cmd-Z
  surface) or live in their own. Decide during J-2 — leaning
  toward separate, since layout edits change `xlights_rgbeffects.xml`
  while effect edits change the sequence document.
- **Memory and per-frame cost of overlays.** Drawing N model labels
  + N first-pixel markers + grid lines per frame is fine on
  desktop; on iPad we'll want to batch through `XLGridMetalBridge`
  primitives and likely cache labels.
- **Coordinate conversions.** Desktop's `ViewportMgr` does
  world↔screen mapping; iPad will need the same helper exposed.
  Already partly there — `XLMetalBridge.fitAllModelsForDocument`
  knows how to compute camera bounds — but explicit
  `unproject(touchPoint) → worldXY` and the inverse are the next
  step.
- **`xlights_rgbeffects.xml` write contention.** Layout edits
  modify the same file the desktop writes. iCloud sync between a
  Mac running xLights and the iPad open in the editor could
  collide. v1 ships with last-writer-wins; if it bites, revisit
  with a real merge strategy.

## Open questions

- Should layout undo share `SequencerViewModel.undoManager` or get
  its own? (J-2 decision.)
- Single layout-group property page or one shared with the model
  property page? (J-1 decision.)
- 3D gizmo gesture set — translate axes, rotate rings, scale handles
  — exact mapping. (J-2 design week.)
- Add Model toolbar — top bar, side rail, or popover? Affects how
  much horizontal canvas space we keep. (J-3 decision.)
