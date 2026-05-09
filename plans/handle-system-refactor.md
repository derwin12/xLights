# Handle / Gizmo System — Redesign Proposal

**Status:** approved 2026-05-09. Migration is essentially complete.
Every user gesture (hover, click, drag, model creation, polyline
extension, multi-select translate/rotate/scale) across Boxed /
TwoPoint / ThreePoint (supportsAngle + supportsShear, 2D + 3D
shear) / PolyPoint (vertex + curve CPs + 4 boundary corners + 3D
vertex + 3D CP translate + whole-model centre gizmo) / MultiPoint
(via PolyPoint) / Terrain (Boxed centre + TOOL_ELEVATE with tool-
size falloff) routes through the descriptor pipeline
(`GetHandles()` + `handles::HitTest` + `DragSession`). Internal
state — `active_handle`, `selected_handle`, `highlighted_handle`,
`m_over_handle` — is `std::optional<handles::Id>`.

**What's actually left** (see "Remaining work" below):
- **`DrawHandles` rewrite.** Every model's `DrawHandles` override
  still recomputes handle positions; descriptors are consumed in
  most places but not as the single source of truth for drawing.
  Rewriting it to walk `GetHandles()` descriptors directly would
  let us delete `active_axis` / `axis_tool` (as drawing state),
  `saved_*` / `drag_delta` (as drag state — move into the relevant
  `DragSession` subclasses).
- **SpaceMouse `MoveHandle3D(scale, rot, mov)`** — 6-DOF input,
  no DragSession equivalent. Kept by design until the new API
  models 6-DOF gestures.
- **`HandleIdToLegacyHandle` adapter** — only used by three
  int-typed accessors (`GetActiveHandle()`, `GetSelectedHandle()`,
  `GetHandlePosition(int)`) consumed by the right-click menu,
  property panel, and SpaceMouse path. Goes away with the menu /
  panel migration + SpaceMouse migration.

**Audience:** desktop xLights maintainer (refactor touches `src-core/`
and `src-ui-wx/layout/`) plus iPad LayoutEditor implementer (phase J-2/J-3
re-bases on the new API).

## TL;DR

The current handle / 3D-gizmo system has been added to incrementally for
years. It works on desktop because mouse precision papers over a lot of
fragile contracts. iPad's J-2 work hit those contracts hard: roughly one
"why doesn't it move" symptom per work session, each with a different
root cause (touch slop, submodel aliasing, latch lifecycle, axis-bit
packing, fromBase silent-reject, ModelMatrix update gating, …). The
fixes shipped, but the system remains fragile and is the wrong shape for
touch authoring or any future input device.

This doc proposes replacing the handle API with a thin **descriptor-based
data model** and an explicit **drag-session object**, leaving the existing
`DrawHandles` / `MoveHandle3D` paths in place during migration so desktop
behaviour doesn't regress while we move callers over.

## Why now

The J-2 session log on iPad tells the story. Every one of these came from
the current handle API:

1. **Handle width math** — `GetCameraZoomForHandles` returned the live 2D
   camera zoom, which interacts with the View matrix's zoom **twice**, so
   handles either explode or vanish when the user zooms. Desktop hardcodes
   `1.0` in 2D; we matched.
2. **`allowSelected` gating `ModelMatrix`** — without `allow_selected =
   true` to `PrepareToDraw`, `ModelMatrix` stays at identity. `HitTest3D`
   ray-casts against identity → hits nothing not at world origin. Took an
   afternoon to find.
3. **Auto-DrawHandles condition** (`Model.cpp:3254`) requires
   `Selected() && c != nullptr && allowSelected` all simultaneously, even
   though the param means "draw gizmo." Three fields wired up correctly
   to enable one effect.
4. **Latch lifecycle** — `MoveHandle3D(latch=true)` snapshots state;
   `latch=false` computes delta. Calling with `latch=true` on every frame
   silently zeros every drag. Implicit "first call vs subsequent" in a
   bool param.
5. **`HANDLE_AXIS` bit-packing** — `CheckIfOverHandles3D` returns
   `handle | HANDLE_AXIS` with the axis in the lower 8 bits. The caller
   has to know to `(value & 0xff)` for axis, `(value & HANDLE_AXIS)` to
   detect, `(value & HANDLE_SEGMENT)` for segment, `(value & HANDLE_MASK)`
   for the plain index. Forget any of those and you crash via
   `assert(false)` on the default switch branch.
6. **`MSLAXIS` enum values** — `X_AXIS=0`, `NO_AXIS=3`. Counter to every
   reasonable reading; a log line showing `axis=0` looks like "no axis"
   but is actually X.
7. **`active_handle` must be `CENTER_HANDLE` for gizmo to draw** — the
   3D `DrawAxisTool` is gated on `active_handle != NO_HANDLE` inside the
   subclass DrawHandles. The flag is part of the gizmo's *render* contract
   but is also drag-session state.
8. **`SetActiveAxis` must be set BEFORE `MoveHandle3D` runs** — otherwise
   `DragHandle`'s switch hits `default: assert(false)`. The bit-packing
   from #5 makes this easy to get wrong.
9. **Desktop's 3D non-axis drag asserts** — clicking the centre sphere
   and dragging without first picking an axis arrow is a hard crash in
   debug. Not a real workflow on desktop (because mouse-down is
   accompanied by axis-arrow precision); on touch, common.
10. **`IsFromBase` silent-reject** — `BaseObject::MoveHandle3D` returns
    early on from-base models. The handles still draw, the gizmo still
    highlights on hover, but drag does nothing. No signal to the caller.
11. **`SubModel::GetModelScreenLocation` aliases to parent** — submodels
    do not have their own screen location. Iterating models in the layout
    editor and naively clearing per-model state on the non-selected ones
    therefore wipes the parent's state every time a submodel of the
    selected model is iterated. Dead-easy to write the bug; days to
    diagnose because the symptom (`active_handle` resets every frame for
    arches but not for matrices) looks like memory corruption.
12. **Drawing/hit-test coupling** — `CheckIfOverHandles*` reads
    `mHandlePosition[]` and `handle_aabb_min/max[]` arrays that are only
    populated during `DrawHandles`. Hit-test before any draw → hits empty
    AABBs at world origin. Reordering the render path can silently break
    selection.
13. **Per-subclass handle index space differs** — `Boxed` has 5 (or 8 in
    3D); `TwoPoint` has 3 (CENTER/START/END); `ThreePoint` adds SHEAR_HANDLE;
    `PolyPoint` has dynamic N + curve control points. The same handle id
    means different things on different models. Generic UI code can't
    treat them uniformly.
14. **`axis_tool` cycle requires tap-on-active-handle** — the convention
    "tap centre to cycle TRANSLATE→SCALE→ROTATE" is conditional on
    `m_over_handle == GetActiveHandle()`. Easy to break (we did, on iPad,
    by treating the tap as a drag intent).

None of these are isolated bugs — they're predictable symptoms of an
API that lets a lot of state leak between draw, hit-test, drag, and
selection. The shape of the J-2 plan ("pull forward overlay rendering,
add per-type handles, add 3D gizmo") makes it worse: every new feature
means tracking another handful of these contracts in another front-end.

## Goals for the redesign

In rough priority order:

1. **Make hit-testing input-device-aware**. Touch needs ~40-pt slop on
   handles and possibly bigger draw targets. Mouse can stay precise.
   Today the same world-coord AABB serves both, badly.
2. **Decouple drawing from hit-testing**. Hit-testing should not depend
   on `DrawHandles` having run first.
3. **Move per-edit-session state OFF `ModelScreenLocation`**. Frontend
   owns the drag session (which handle, which axis, which tool); model
   exposes pure transformations.
4. **Replace the handle-id bit-packing with a struct**. Compile-time
   safety for axis / segment / curve-control-point classification.
5. **Generalize handle iteration**. Frontend can ask "what handles does
   this model have?" without hardcoding per-subclass logic.
6. **Drag-session lifecycle is explicit**. `BeginDrag → UpdateDrag →
   EndDrag` rather than `MoveHandle3D(..., latch=true)` then
   `MoveHandle3D(..., latch=false)` repeating.
7. **Surface mutation rejections instead of swallowing them**.
   `IsFromBase` / `IsLocked` should be reported via the descriptor stream
   so the UI can offer a meaningful response (greyed handle, tooltip),
   not a silent no-op drag.
8. **Don't break desktop**. Refactor incrementally; keep the existing
   `MoveHandle3D` available until callers migrate.

## Proposed design

### Layer 1 — Handle descriptors (pure data)

```cpp
namespace handles {

enum class Role : uint8_t {
    Body,         // tap to select the model (the model's bounding shape)
    Move,         // drag to translate (e.g. centre)
    ResizeCorner, // 8 of these on 3D Boxed; 4 on 2D Boxed
    ResizeEdge,   // mid-edge handles where applicable
    Rotate,       // rotation handle (BoxedScreenLocation's offset square)
    Endpoint,     // TwoPoint START / END
    Vertex,       // PolyPoint vertex
    CurveControl, // PolyPoint Bézier control point
    Shear,        // ThreePoint shear / arch height
    AxisArrow,    // 3D translate gizmo arrows
    AxisCube,     // 3D scale gizmo cubes
    AxisRing,     // 3D rotate gizmo rings
    CentreCycle,  // 3D centre sphere — tap to cycle tool
};

enum class Axis : uint8_t { X, Y, Z };

// Stable identifier for one handle on one model. The frontend stores
// these between events; never has to bit-unpack anything.
struct Id {
    Role role;
    int  index;       // Vertex/Endpoint: 0..N-1; ResizeCorner: 0..7; AxisArrow: axis
    Axis axis;        // Only meaningful for axis* roles; ignored otherwise
    int  segment;     // Only meaningful for CurveControl; -1 otherwise
    auto operator<=>(const Id&) const = default;
};

struct Descriptor {
    Id          id;
    glm::vec3   worldPos;        // World-space centre of the handle
    glm::vec3   localPos;        // Model-space (for callers that want to draw in local coords)
    glm::vec3   axisDir;         // For axis* roles: world-space direction; otherwise zero
    float       suggestedRadius; // World-space radius the model would draw at scale=1, zoom=1
    bool        editable;        // false if model is locked / fromBase / submodel
    xlColor     suggestedColor;  // Unselected colour the desktop draws today
    bool        isActive;        // For axis* / CentreCycle: true if this is the current tool target
    bool        isHovered;       // Frontend-driven; descriptor is mutable for this one field
};

class DescriptorStream {
public:
    // Snapshot the model's handle layout at this moment. Cheap.
    // Drawing and hit-testing both read this; neither writes it.
    virtual std::vector<Descriptor> GetHandles(int viewMode) const = 0;
    // 0 = layout-2D, 1 = layout-3D. (Future: 2 = sequencer-preview etc.)
};

} // namespace handles
```

`Model::GetHandleStream()` returns a `DescriptorStream*`. Each
`ScreenLocation` subclass implements `GetHandles(viewMode)` to produce
its handle list. No `mHandlePosition[]` member, no `handle_aabb_min/max`,
no bit-packed indices.

### Layer 2 — Drag session (explicit lifecycle)

```cpp
namespace handles {

// Frontend creates one of these when the user grabs a handle.
// Lives until mouse-up / touch-end. Owns no model state directly —
// stores enough to apply or revert mutations on the model below.
class DragSession {
public:
    virtual ~DragSession() = default;

    // Apply an incremental update. `worldPoint` is where the user's
    // pointer currently is in world coords; the session translates that
    // into the right per-Role mutation (resize, rotate, vertex move, ...).
    // Returns the mutation result so the frontend knows whether to
    // mark the model dirty / rebuild caches.
    enum class Result { Unchanged, Updated, NeedsInit };
    virtual Result Update(const glm::vec3& worldPoint, ModifierFlags) = 0;

    // Revert the model to the state captured at Begin. For undo on
    // gesture cancel, or for snap-while-dragging.
    virtual void Revert() = 0;

    // Commit the cumulative mutation. Returns the descriptor list
    // to mark dirty for save (model name + which fields changed).
    virtual std::vector<DirtyField> Commit() = 0;
};

// Factory method on the model. Frontend calls this on tap-down on a
// handle. Returns nullptr if the handle isn't actually editable
// (locked, fromBase, etc.) — frontend can show a "can't edit" cue
// instead of silently no-op'ing.
std::unique_ptr<DragSession> Model::BeginDrag(handles::Id, const glm::vec3& startWorldPoint);

} // namespace handles
```

Properties of this design:

- **No latch flag.** State is captured in `BeginDrag` (the `Update`
  signature has no boolean to mis-pass).
- **No persistent `active_*` on the screen location.** `DragSession`
  owns the saved-position / saved-intersect / accumulated-rotation
  fields it needs. Multiple gestures can't fight over them; submodel
  aliasing is irrelevant because there's no shared field to clobber.
- **No swallowed rejections.** `BeginDrag` returns nullptr if the model
  isn't editable. Frontend shows whatever signal it wants.
- **Frontend owns "current tool"** (translate / scale / rotate). The
  model doesn't know what `axis_tool` is. The descriptor stream returns
  whichever gizmo handles are appropriate for the frontend's currently-
  selected tool.

### Layer 3 — Frontend hit-testing

```cpp
// Pure utility, lives in the front-end (desktop or iPad), not src-core.
struct HitTestOptions {
    float bodyTolerancePixels;   // 0 for mouse, 8-12 for touch
    float handleTolerancePixels; // 4 for mouse, 24-40 for touch
    bool  preferAxisHandles;     // axis arrows beat body-pick when within tolerance
};

std::optional<handles::Id> HitTest(
    const std::vector<handles::Descriptor>& handles,
    const Camera& camera,
    glm::vec2 screenPoint,
    HitTestOptions opts);
```

The desktop calls this with mouse-tight tolerances; iPad calls it with
touch-friendly tolerances. The model layer never sees the difference.

### What this replaces

| Old API                                         | Replaced by |
|---|---|
| `DrawHandles(program, zoom, scale, fromBase)`   | Frontend renders from `Descriptor` stream however it wants |
| `DrawHandles(program, zoom, scale, drawBounding, fromBase)` | Same |
| `DrawAxisTool(...)`                              | Frontend renders gizmo from `AxisArrow` / `AxisCube` / `AxisRing` descriptors |
| `CheckIfOverHandles(preview, handle, x, y)`      | Frontend `HitTest()` |
| `CheckIfOverHandles3D(...)`                      | Frontend `HitTest()` |
| `CheckIfOverAxisHandles3D(...)`                  | Frontend `HitTest()` filtered to axis handles |
| `MoveHandle(preview, handle, shift, x, y)`       | `DragSession::Update(worldPoint, modifiers)` |
| `MoveHandle3D(..., latch, ...)`                  | `BeginDrag` + `Update` + `Commit` |
| `MoveHandle3D(scale, handle, rot, mov)` (programmatic) | `Model::ApplyOffset(...)` / `Model::ApplyRotation(...)` direct mutator API |
| `DragHandle(preview, x, y, latch)`               | Internal helper of `DragSession` |
| `SetActiveHandle / GetActiveHandle`              | DragSession owns it; frontend tracks it for selection-display purposes |
| `SetActiveAxis / GetActiveAxis`                  | Same — encoded in the `Id` of the active descriptor |
| `axis_tool` / `AdvanceAxisTool / SetAxisTool`    | Frontend state. DescriptorStream takes the current tool as a parameter to filter which axis handles to emit |
| Bit-packed `HANDLE_AXIS`, `HANDLE_SEGMENT`, `HANDLE_CP0/1` | `Id` struct fields |
| `MSLAXIS` enum (with `NO_AXIS = 3` gotcha)      | `std::optional<handles::Axis>` |
| `mHandlePosition[]`, `handle_aabb_min/max[]`, `active_handle_pos`, `saved_position`, `saved_intersect`, `saved_point`, `saved_angle`, `saved_size`, `saved_scale`, `saved_rotate`, `drag_delta` | All become DragSession-private, none on the screen location |

### What stays unchanged

- The math inside the screen-location subclasses (e.g.,
  `BoxedScreenLocation::MoveHandle3D` lines 689-820 doing the actual
  matrix updates) — that's correct, just moves into the DragSession
  implementations.
- `IsLocked` / `IsFromBase` flags — same semantics, just consulted by
  `BeginDrag` to return nullptr.
- Existing color / handle-size constants — frontend draws with them.
- Per-screen-location `PrepareToDraw` logic — needed for rendering the
  model itself; just stops being entangled with handle rendering.

## Migration strategy

The risk is touching every model and every layout-edit pathway in one
commit. Avoid that with a phased introduction:

### Phase R-1 — Add the new API alongside the old ✓ 2026-05-09

What landed:

- `src-core/models/handles/Handles.h` — `Role` (Body / Move / ResizeCorner /
  ResizeEdge / Rotate / Endpoint / Vertex / CurveControl / Shear /
  AxisArrow / AxisCube / AxisRing / CentreCycle), `Axis`, `Tool`,
  `ViewMode`, `Modifier`, `Id` struct, `Descriptor` struct.
- `src-core/models/handles/DragSession.h` — `WorldRay`, `UpdateResult`,
  `DirtyField`, `CommitResult`, abstract `DragSession` class.
- `src-core/models/handles/HitTest.h/.cpp` — `HitTestOptions`,
  `ScreenProjection`, `HitTest()` returning closest descriptor's
  `Id` within tolerance. Naive O(N) — fine for typical ~5-50
  handles per model.
- `Model::GetHandles(ViewMode, Tool)` + `Model::BeginDrag(Id, ray)`
  virtuals (default no-op) hung off `Model`.
  `ModelWithScreenLocation<T>` overrides both to delegate to its
  screen location, with the `IsFromBase()` short-circuit baked in.
- `ModelScreenLocation::GetHandles` + `CreateDragSession` virtuals
  (default no-op).
- `BoxedScreenLocation::GetHandles` returns `CentreCycle` + 3 axis
  arrows when in 3D + Translate tool. (2D / Scale / Rotate
  unimplemented for R-1; stay on legacy.)
- `BoxedScreenLocation::CreateDragSession` returns a private-impl
  `BoxedTranslateSession` that captures `savedWorldPos` +
  `savedIntersect` (constraint plane derived from the handle's
  axis), applies axis-locked translate in `Update()`, supports
  `Revert()` + `Commit()` returning `DirtyField::Position`.
- Drops the `DescriptorStream` indirection from the original
  proposal — `Model::GetHandles` returns `vector<Descriptor>`
  directly. One less abstraction.

What R-1 explicitly does NOT cover (pushed to R-2 as iPad needs each):
- 2D Boxed: corner resize, rotate handle, body drag-to-move
- 3D Boxed: scale (axis cubes), rotate (axis rings)
- TwoPoint, ThreePoint, PolyPoint, MultiPoint, Terrain — none of
  the new API; these stay 100% legacy.
- Centre-cycle tool switching (frontend-driven; not a drag)

Build verification: desktop Debug ✓, iPad lib Debug ✓, include
policy clean. Old API path unchanged — no callers use the new API
yet, so behaviour is byte-identical to pre-R-1.

### Phase R-2 — iPad uses the new API only (~1 wk)

- Rewire `XLMetalBridge::pickHandle*` / `dragHandle*` to call
  `Descriptor`-based hit-testing + `DragSession`.
- Delete iPad's ad-hoc `_handleDragNeedsLatch`, ad-hoc submodel filter,
  ad-hoc `active_handle` re-set in the draw loop, ad-hoc fromBase
  guard. All become consequences of `BeginDrag()` returning nullptr.
- Validate visually + via the existing iPad LayoutEditor flows (drag
  arches, drag matrices, drag polylines).
- Old desktop code path unchanged.

### Phase R-3 — Migrate desktop LayoutPanel to the new API ✓ 2026-05-09

What landed (incremental cut, mirrors R-2's iPad pattern):

- `LayoutPanel` gains `m_dragSession` + `m_dragSessionId` state
  alongside the legacy fields.
- `XLIGHTS_LEGACY_HANDLES` macro defined in
  `src-core/models/handles/Handles.h` — placeholder for R-5 strip; default
  ON. Runtime fallback (BeginDrag returning nullptr) is what
  actually keeps the legacy path safe today.
- `ProcessLeftMouseClick3D` (mouse-down): inserted descriptor-based
  hit-test before the legacy `CheckIfOverHandles3D` call. On
  successful `Model::GetHandles → HitTest → BeginDrag`, the new
  session is stored, undo is captured, status is set, and the
  legacy hit-test is skipped. Anything else (CentreCycle, locked,
  fromBase, role not yet ported, non-Boxed model) falls through
  unchanged.
- `OnPreviewMouseMove3D`: when `m_dragSession` is set, route the
  move through `DragSession::Update(WorldRay)` and short-circuit
  the legacy `MoveHandle3D` + multi-select group-drag block.
- `OnPreviewLeftUp`: when `m_dragSession` is set, `Commit()` and
  schedule `WORK_RGBEFFECTS_CHANGE | WORK_MODELS_CHANGE_REQUIRING_RERENDER
  | WORK_RELOAD_PROPERTYGRID | WORK_REDRAW_LAYOUTPREVIEW`.
- Build verification: desktop Debug ✓, iPad lib ✓, include policy
  ✓.

What R-3 explicitly does NOT cover (carry into R-4):

- Multi-select group translate / rotate / scale (legacy `MoveHandle3D`
  + `last_centerpos/last_worldrotate/last_worldscale` delta logic).
- 2D `ProcessLeftMouseClick` / `OnPreviewMouseMove2D` paths
  (lines ~5200-5310 in LayoutPanel.cpp).
- New-model creation (`m_creating_bound_rect` + the `_newModel`
  branch at ~3770-3801 still calls `MoveHandle3D(latch=true)`).
- Hover cursor feedback (legacy `CheckIfOverHandles3D` at ~5143
  still drives `SetCursor`); new API has no hover signal yet.
- Desktop gizmo highlight during drag — relies on
  `active_axis`/`active_handle`, neither of which the new path
  sets. Visual regression for desktop until `Descriptor.isActive`
  rendering lands.

### Phase R-3.5 — desktop polish before R-4 (deferred)

Would close gaps from "What R-3 does NOT cover" once `R-4`'s
extra screen-location subclasses are ready. Splitting it out
because none of these block iPad / desktop functional parity for
single-model 3D translate.

### Phase R-4 — Implement remaining screen-location types (~1 wk)

- `TwoPointScreenLocation`, `ThreePointScreenLocation`,
  `PolyPointScreenLocation`, `MultiPointScreenLocation`,
  `TerrainScreenLocation` each get their own `GetHandles` /
  `BeginDrag` impls.
- Most of the math is verbatim ports.

### Phase R-5 — Decommission compatibility layer ✓ 2026-05-09 (medium scope)

What landed:

- `XLIGHTS_LEGACY_HANDLES` macro removed. The runtime fallback
  (`BeginDrag` returning nullptr, `handledByNewApi=false` in
  LayoutPanel) is the only remaining gate, and it's the correct
  one — no compile-time switch was ever needed.
- Plan + memory updated to reflect the kept-by-design legacy
  surface (see below).

### Phase R-6 — Selection-only descriptors + placement gestures ✓

What landed:

- `Descriptor::selectionOnly` flag — picker treats the hit as a
  selection event (set active_handle, advance tool, etc.) rather
  than opening a drag. Lets sphere-style sub-handle picks
  (CENTER / START / END / Vertex) flow through the descriptor
  pipeline instead of the legacy hit-test.
- `ModelScreenLocation::BeginCreate(modelName, clickRay, mode)`
  — placement gesture for newly-created models. Boxed wraps
  `BoxedScaleSession` with shift+ctrl forced; TwoPoint reuses
  endpoint translate; PolyPoint uses a fresh
  `PolyPointCreationSession`.
- `BeginExtend(vertexIndex)` for polyline extension — each
  click adds a vertex via `AddHandle()` then opens a session on
  the new vertex.
- Multi-select translate / rotate / scale routes through
  `m_dragSessionId.role`.

### Phase R-7 — Legacy hit-test + state strip ✓

What landed (across several sessions; the big one was 2026-05-10/11):

- **Deleted functions:** `MoveHandle(preview, ...)`,
  `MoveHandle3D(preview, ...)`, `CheckIfOverHandles` (2D),
  `CheckIfOverHandles3D`, `CheckIfOverAxisHandles3D`. Plus the
  `creating_model` flag and the LayoutPanel convergence
  check / bridge fallback / `m_over_handle` legacy click branch.
- **Hover migrated** to `Model::GetHandles` + `handles::HitTest`
  in both 2D and 3D paths (desktop + iPad bridge). Cursor
  selection (resize-corner directional cursors, segment cursor,
  hand cursor) routes off the descriptor `Role`.
- **Segment selection** routes through `Role::Segment`
  descriptors with line-segment hit-test (chord sub-segments for
  curves, endpoint deadzone so vertex picks still win).
- **`HitTest3D` `selected_segment` side-effect removed** —
  the previous "selecting a model accidentally updates which
  segment shows as selected" bug.
- **`handle_aabb_min[]`/`handle_aabb_max[]` arrays deleted** —
  126 lines of pure dead state across 5 ScreenLocation subclasses.
- **`mHandlePosition[]` array + `active_handle_pos` member
  deleted.** External readers (`GetActiveHandlePosition()`,
  `GetHcenterPos/Vcenter/Dcenter`, `GetHandlePosition(int)`)
  compute on demand. `DrawHandles` consumes `GetHandles()`
  descriptor positions for drawing (with Z-extruded wireframe
  corners computed locally — they're never user-clickable, so
  don't belong in the descriptor list).

### Phase R-7.5 — State-representation switch to `std::optional<Id>` ✓

What landed (2026-05-11):

- **`m_over_handle`** in LayoutPanel — was bit-packed int, now
  `std::optional<handles::Id>`. Polyline-create vertex counter
  split out into its own `m_polyline_create_handle` int (different
  semantics, different lifetime).
- **`highlighted_handle`** on ModelScreenLocation — `optional<Id>`.
  `MouseOverHandle(int)` becomes
  `MouseOverHandle(std::optional<handles::Id>)`. DrawHandles'
  `highlighted_handle == HANDLE_AXIS + n` checks become
  `IsAxisHandle(highlighted_handle, Axis::X|Y|Z)`; legacy `==`
  bit-equality checks become `IsRole` / `IsHandle` calls.
- **`selected_handle`** on PolyPointScreenLocation — `optional<Id>`.
  Vertex-index highlight comparisons use `IsHandle(...,
  Role::Vertex, i)` instead of `i == (selected_handle - 1)`.
  CP highlight uses the 4-arg `IsHandle(..., Role::CurveControl,
  cpIndex, segment)` overload (and as a side-effect fixes a
  pre-existing typo where the cp1 sphere was checking CP0).
- **`active_handle`** on ModelScreenLocation — `optional<Id>`.
  Each subclass's `SetActiveHandle(int)` override converts the
  legacy int via a tiny per-subclass helper (`TwoPointLegacyToId`
  / `ThreePointLegacyToId` / `TerrainLegacyToId`; PolyPoint reuses
  `SetSelectedHandle` for the conversion). New
  `SetActiveHandle(std::optional<handles::Id>)` overload skips the
  round-trip for descriptor-pipeline callers.

**Remaining `HandleIdToLegacyHandle` use** is exactly three
adapter spots: `GetActiveHandle()` / `GetSelectedHandle()` returning
int for the right-click menu + property panel, and `GetHandlePosition(int)`
for SpaceMouse. The hot paths (hover, click, drag, iPad pick) speak
`handles::Id` natively.

### Phase R-8 — `DrawHandles` rewrite + `saved_*` migration ✓ 2026-05-11

What landed:

- **`DragHandle` signature** changed from `(preview, x, y, bool latch)`
  reading `saved_position` and writing `saved_intersect` +
  `drag_delta` to `(preview, x, y, glm::vec3& outIntersect,
  glm::vec3 planePoint = origin)`. All call sites updated. Dead
  members deleted: `saved_intersect`, `saved_position`,
  `drag_delta`. `saved_size` / `saved_scale` / `saved_rotate` kept
  + documented as SpaceMouse-only baseline state (dies with R-9).
- **Axis-indicator line consolidated** into
  `ModelScreenLocation::DrawActiveAxisIndicator(pos, program)` —
  replaces five identical `switch(active_axis)` blocks across
  Boxed/Terrain/TwoPoint/PolyPoint DrawHandles. TwoPoint keeps an
  inline `Shear` planar-cross special case; PolyPoint keeps an
  inline `TOOL_XY_TRANS` planar-cross special case.
- **`DrawAxisTool` signature** changed from `glm::vec3&` to
  `const glm::vec3&` — never wrote through the reference, const
  lets callers pass immutable anchors.
- **Boxed end-to-end pass.** 2D DrawHandles now walks
  `GetHandles(TwoD, Translate)` directly with no role filter (the
  filter was a no-op). 3D drops a tautological GetHandles loop and
  uses `(GetHcenterPos, GetVcenterPos, GetDcenterPos)` for the
  gizmo anchor directly. The 12-edge wireframe stays inline — it's
  decorative-only, never a hit target.

Bug fixes also rolled into R-8 during testing:

- **Outer `drawLines` covered DrawAxisTool's triangle data** in
  Terrain / TwoPoint / PolyPoint (XY_TRANS branch). The legacy
  inline indicator block reset `startVert`/`startLines` after
  DrawAxisTool; the helper replacement dropped the reset, so the
  outer step reinterpreted axis-tool triangle vertices as line
  pairs and painted garbage over the gizmo. Fix: default the line
  range to `[endTriangles, endTriangles)` and only widen it in
  the inline-shear/XY_TRANS branch.
- **2D Boxed rotate handle didn't update `rotate_quat`.**
  `BoxedScreenLocation::SetRotation(int r)` was a Z-only shadow
  that only wrote `rotatez`, leaving `TranslatePoint` (which the
  handle/wireframe positions go through) reading a stale quat.
  Now delegates to `ModelScreenLocation::SetRotation(vec3)` so the
  canonical rebuild path fires.
- **All `SetRotate*` setters now funnel through `SetRotation(vec3)`.**
  Property-panel rotation fields would otherwise hit the same
  footgun. `SetRotate(x,y,z)`, `SetRotateX`, `SetRotateY`,
  `SetRotateZ` rewritten to delegate.
- **2D bounding-box corners now rotate with the model.**
  `BOUNDING_RECT_OFFSET` was being added in *world* space after
  `TranslatePoint`, so the offset only pointed outward at 0°. At
  180° it pointed inward (corners collapsed toward model centre);
  at 90°/270° it was perpendicular. Fold the offset into local
  space (divided by scale to cancel the scale step) so it rotates
  through `TranslatePoint`. Mirrors the 3D wireframe pattern.

State-storage fixes (also needed for axis-gizmo drags to draw):

- **`SetActiveHandle(optional<Id>)` base default stores the Id
  directly** instead of round-tripping through
  `HandleIdToLegacyHandle` → `SetActiveHandle(int)`. The int
  route was lossy: Boxed (no int override) saw any
  `HANDLE_AXIS+x` and reset `active_handle` to nullopt, killing
  the gizmo. PolyPoint gets an Id override so its
  `selected_handle` stays in lockstep.
- **Axis-gizmo roles filtered out of `SetActiveHandle`.**
  `AxisArrow` / `AxisCube` / `AxisRing` are modifiers on the
  active body handle, not handles in their own right. Storing
  them as `active_handle` breaks `IsRole(active_handle,
  Endpoint)` (etc.) checks that subclass `GetHandles` use to
  decide whether to emit axis descriptors — next frame, no
  arrows are emitted, `GetHandlePosition(HANDLE_AXIS+x)` returns
  `(0,0,0)`, and `DrawAxisTool` draws the gizmo at world origin.
- **LayoutPanel sync on axis-arrow drag start.**
  `active_axis = static_cast<MSLAXIS>(hit->id.axis)` so
  `DrawActiveAxisIndicator`'s long red/green/blue line draws
  during the drag. `MouseOverHandle(hit->id)` so
  `highlighted_handle` lights up the dragged axis yellow in
  `DrawAxisTool`. Body-handle drags (Vertex / Endpoint /
  CurveControl / etc.) still go through the original
  `SetActiveHandle(optional<Id>)` path.

What's deferred to R-8b (low priority):

- **`active_axis` / `axis_tool` as members** — still consumed by
  `DrawAxisTool` directly. The plan called for descriptor-driven
  dispatch in `DrawAxisTool` but that's a shared helper across
  all subclasses; refactoring it gates on first wanting it. See
  `Boxed::DrawHandles` proof-of-concept comment.
- **Per-subclass `DrawHandles` walks** — Boxed walks
  `GetHandles()` for 2D. 3D still has body-specific draw code
  (the wireframe is appropriate as a per-subclass concern).
  TwoPoint/ThreePoint/PolyPoint/Terrain partially consume
  descriptors but still build positions inline in places.
  Tightening these is fine work for future polish.

### Phase R-9 — SpaceMouse 6-DOF migration (deferred)

User has SpaceMouse hardware off-site; testing requires it. Plan:

- Add `BeginSpaceMouseSession(modelName, handle::Id)` returning
  a `DragSession` variant that takes 6-DOF updates instead of
  ray-based ones.
- Migrate the single `LayoutPanel::OnPreviewMotion3D` caller of
  `MoveHandle3D(scale, int handle, rot, mov)` to the new session.
- Delete `BaseObject::MoveHandle3D(scale, ...)`,
  `ModelScreenLocation::MoveHandle3D(scale, ...)`, and the
  `GetHandlePosition(int)` adapter that the SpaceMouse path is
  the last consumer of.

### Phase R-10 — Right-click menu + property panel use `Id` ✓ 2026-05-11 (migration phase)

What landed:

- `GetSelectedHandleId()` accessor added on `ModelScreenLocation`
  (default `nullopt`), `PolyPointScreenLocation` (returns
  `selected_handle`), and `Model` (delegates).
- `ScreenLocationPropertyHelper::OnPropertyGridChange` —
  "REAL Segment N" handler reads the parsed segment index
  directly instead of round-tripping through
  `SetSelectedHandle(N-1) → GetSelectedHandle()`. The
  `SetSelectedHandle` call survives for visual feedback but the
  int-returning getter is no longer consulted.
- `LayoutPanel::AddSingleModelOptionsToBaseMenu` —
  "Delete Point" gating uses
  `GetSelectedHandleId() && role == Vertex` instead of a
  3-clause int range check (`> 0 && < 0x4000 && <= NumHandles`).
- `LayoutPanel::OnPreviewModelPopup` (3D + 2D both) —
  "Delete Point" handlers read `GetSelectedHandleId()` and
  construct the legacy 1-based int (`index + 1`) at the call to
  `DeleteHandle`, the only remaining int-taking sink in this
  path.
- `LayoutPanel::OnPreviewLeftDown` polyline-create — vertex
  advance reads `GetActiveHandleId()` + checks role, then
  constructs the next `Vertex` Id explicitly. No more
  `int handle = GetActiveHandle(); handle++; SetActiveHandle(handle)`
  bit-counting.
- `TwoPoint::DrawHandles` (3D) and `Terrain::DrawHandles` +
  `Terrain::CreateDragSession` — internal callers swap
  `GetHandlePosition(GetActiveHandle())` for
  `GetActiveHandlePosition()` so the int handle is hidden inside
  the helper.
- **Deleted** `GetSelectedHandle()` (the int returner) —
  `ModelScreenLocation` virtual, `PolyPoint` override, and
  `Model` wrapper. All callers migrated. The Id-returning
  `GetSelectedHandleId()` is the only public selected-handle
  read now.

**Remaining for R-10b (blocked on R-9 SpaceMouse work):**

- `LayoutPanel::OnPreviewMotion3D` (the 6-DOF handler) still
  reads `GetActiveHandle()` as int to feed `MoveHandle3D`.
  Until R-9 replaces `MoveHandle3D` with a SpaceMouse-flavoured
  `DragSession`, this is the last external `GetActiveHandle()`
  caller.
- After R-9 lands and `GetActiveHandle()` /
  `GetHandlePosition(int)` lose their last callers, we can
  delete `HandleIdToLegacyHandle`, every `*_HANDLE` legacy
  constant (`CENTER_HANDLE`, `L_TOP_HANDLE`, `HANDLE_CP0/1`,
  `HANDLE_AXIS`, `HANDLE_SEGMENT`, `HANDLE_MASK`, etc.), and
  the int-taking `SetActiveHandle(int)` virtual + every subclass
  override. Significant internal cleanup — many descriptor-side
  `id.index = CENTER_HANDLE` constructions need replacement, and
  `PolyPoint::SetSelectedHandle(int)` still uses
  `HANDLE_CP0/CP1` bit-packing for curve-control handles.

**Deliberately kept legacy surface** (post-R-10):

| Legacy entry point | Why kept |
|---|---|
| `ModelScreenLocation::DrawHandles` per-subclass overrides | Drawing is still per-subclass; Boxed walks GetHandles for 2D, others still build in-place. Tightening is polish, not blocking. |
| `MoveHandle3D(scale, glm::vec3& rot, glm::vec3& mov)` | SpaceMouse 6-DOF. Targeted by **R-9**. |
| `active_axis`, `axis_tool` on ModelScreenLocation | Consumed by `DrawAxisTool` directly. Refactoring gates on shared-helper descriptor-driven dispatch (R-8b polish). |
| `saved_size`, `saved_scale`, `saved_rotate` on ModelScreenLocation | SpaceMouse baseline state. Die with R-9. |
| `HandleIdToLegacyHandle` adapter + every `*_HANDLE` constant | Right-click menu + property panel read int views. Targeted by **R-10**. |
| `SetActiveHandle(int)` virtual + subclass overrides | Legacy int callers (model-init paths). Sunset with R-10 along with `*_HANDLE` constants. |

Each phase ends with both desktop and iPad working on a full release
build.

## Open questions

- **Per-frame descriptor cost.** `GetHandles(viewMode)` runs every
  frame for the selected model (and zero models for non-selected, since
  hit-testing isn't needed there). Should be cheap — Boxed produces 5
  handles, PolyPoint maybe 50 in the worst case. If profile shows it's
  not, the descriptor stream can cache and invalidate via the existing
  `IncrementChangeCount` signal.
- **3D gizmo styling parity.** Desktop's gizmo is opinionated (red /
  green / blue arrows, orange centre, yellow on hover, purple from-base,
  red locked). The proposal keeps this by having descriptors carry
  `suggestedColor`, but eventually iPad might want a different visual
  language. The descriptor → render mapping is frontend-local, so no
  problem; just want to flag it.
- **Submodel descriptor exposure.** Should `SubModel::GetHandleStream()`
  return the parent's handles, or an empty stream? Empty is cleaner
  (the parent is the one with the layout — submodels don't move
  independently). That's what desktop's LayoutPanel already does
  implicitly.
- **Model-creation path.** *Resolved (R-6b).* `InitializeLocation`
  still runs to place the model at the click point (and set up
  `active_axis` / scale defaults), then
  `ModelScreenLocation::BeginCreate(modelName, clickRay, mode)`
  returns a placement `DragSession`. Boxed wraps `BoxedScaleSession`
  with shift+ctrl forced (matching legacy `creating_model || ...`);
  TwoPoint reuses `TwoPointTranslateSession` / `TwoPointEndpointSession`
  on END_HANDLE; PolyPoint uses a fresh `PolyPointCreationSession`
  that mirrors the legacy `TOOL_XY_TRANS` branch. PolyLine extension
  uses `BeginExtend(vertexIndex)` — each click adds a vertex via
  `AddHandle()` then opens a session on the new vertex.
- **Hover state.** `MouseOverHandle` lives on the screen location
  today, sets `highlighted_handle`, drives the `xlYELLOWTRANSLUCENT`
  highlight. Desktop's mouse-move sees it; iPad doesn't have hover.
  The new API should let the frontend set hover state per descriptor
  without touching the model.
- **Programmatic mutation API.** Bulk operations (align, distribute,
  scripting) currently use `MoveHandle3D(scale, handle, rot, mov)`.
  Replace with explicit `Model::ApplyOffset(vec3)` /
  `Model::ApplyRotation(...)` rather than going through `BeginDrag`.

## Risks

- **Schedule slip on R-3.** Migrating desktop's `LayoutPanel.cpp`
  (10K lines) without missing a corner case is the riskiest step.
  Mitigation: keep the old API behind a `XLIGHTS_LEGACY_HANDLES`
  compile-time flag during R-3 so we can A/B test side by side.
- **PolyPoint complexity.** PolyPoint's curve / control-point /
  segment handling is the most intricate. Likely needs its own
  detailed mini-design before R-4.
- **Gamepad / "managed agents" path.** The
  `MoveHandle3D(float scale, int handle, glm::vec3& rot, glm::vec3& mov)`
  overload is reportedly used by external automation. Need to verify
  who calls it before deciding what to replace it with.
- **Discoverability of the cycle.** If we keep desktop's "tap centre
  to cycle tool", we need `Role::CentreCycle` descriptors emitted only
  when in 3D mode; easy to forget. Alternative: separate explicit tool
  picker UI on desktop (a real radio control), drop the centre-cycle
  shortcut. Unclear if desktop users would tolerate that.

## Decisions (2026-05-09)

1. **Commit to the redesign:** yes, full R-1 → R-5 migration.
2. **Phase boundary:** R-1 + R-2 ship as the first cuttable unit
   (iPad cuts over, desktop unchanged). R-3+ comes after. Rationale:
   gets the new API in front of TestFlight testers earlier, which
   surfaces more issues before the desktop work commits.
3. **`XLIGHTS_LEGACY_HANDLES` compile-time flag during R-3:** yes.
   Safer rollback if the desktop regression triage gets long;
   removed in R-5 along with the rest of the legacy surface.

This is now a phase plan. The J-2 gotcha list in
`phase-j-layout-editor.md` graduates from "workarounds shipped" to
"fixed in the redesign" once R-2 lands.
