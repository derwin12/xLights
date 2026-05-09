# Future — iPad layout editing

Parking lot for iPad features that only make sense once we bring the
desktop Layout panel (model placement / world layout / wiring) across.
All explicitly out of scope today: the iPad MVP loads a show's
pre-existing layout and plays it back, but does not let the user
*arrange* models.

**Phase S promoted to active work 2026-05-07** —
[phase-j-layout-editor.md](phase-j-layout-editor.md) is the iPad-side
sub-plan. The items below are kept for context; Phase S-pro items
remain future work.

The 2026-04-23 gap analysis (§2.2) split this work into two phases:

- **Phase S (minimum viable)** — LA-1 toolbar (Add Model buttons),
  LA-2 model tree, LA-3 basic property grid, LA-4 2D canvas
  (single-select + drag), LA-7 sticky-button → place flow, LA-9
  minimal model-group create/delete, LA-10 Layout Views, LA-11
  locking, LA-16 layout undo/redo, LA-18 background/grid overlays,
  VO-1 ImageObject controls, VO-2 GridlinesObject editing.
  Estimated 3 months.
- **Phase S-pro** — LA-5 3D canvas (gizmo, gestures, no SpaceMouse),
  LA-6 right-click menus, LA-8 alignment, LA-12 bulk-edit (15+ ops),
  LA-13 ImportPreviewsModelsDialog, LA-15 DXF/STL/VRML export, VO-3
  MeshObject, VO-4 TerrainObject (heightmap painting), VO-5
  RulerObject, VO-6 ViewObjectPanel. Estimated 3+ months.

Custom model authoring, Face/State editing, DMX deep authoring, and
the Wiring view live in [`future-custom-models.md`](future-custom-models.md).

---

## L-1. Model name / info / first-pixel overlays

Desktop exposes `SetShowModelNames` / `SetShowModelInfo` /
`_showFirstPixel` toggles that draw per-model text labels (display
name, start/end channel summary) and a coloured marker at node 0 of
each model in the preview. Text rendering uses
`fontInfo`/`xlFontInfo` with a zoom-driven font size
(`ModelPreview.cpp:813`). These are diagnostic aids used while
laying out models — identifying which pixel is which, spotting
channel-assignment mistakes, confirming orientation — so they belong
with the Layout editor rather than playback-mode preview.

When layout editing lands on iPad:

- Three independent toggles in the preview controls overlay (names,
  info, first pixel).
- Reuse the desktop text block — already wx-free. Text rendering on
  iPad goes through `CoreGraphicsTextDrawingContext`
  (`src-iPad/Bridge/`).
- Scale font size sensibly for touch; desktop's zoom-driven sizing
  should port directly.

## L-2. 2D grid + bounding-box overlays

Desktop exposes `SetDisplay2DGrid` / `SetDisplay2DBoundingBox`
(ModelPreview.h:147–151). Settings live in `xlights_rgbeffects.xml`
(`Display2DGrid`, `Display2DGridSpacing`, `Display2DBoundingBox`).
These are measurement / alignment aids for positioning models —
useful while laying out, distracting during playback. *(Note:
`Display2DCenter0` is part of the 2D view matrix — iPad already
honours it; it is not a layout-editor overlay.)*

When layout editing lands:

- Grid overlay at configurable spacing (default 100 units), origin
  either at centre or top-left.
- Bounding-box overlay showing the union of all visible models.
- Both draw in `ModelPreview::RenderModels` via the grid-line
  accumulator; iPad can reuse the same accumulator path.
- Hook the toggles into `PreviewControlsOverlay`; persistence either
  as per-user iPad state or round-tripped through `rgbeffects` once
  we write the file.

## L-3. Model selection + manipulation in the preview

The desktop Layout panel lets the user select models in the preview
and manipulate them directly:

- Click-to-select / rubber-band-select.
- Drag-to-move with snapping.
- Resize handles (BoxedScreenLocation's 4 corner + midpoint handles).
- Polyline vertex editing for PolyLine-style models.
- Rotation handle.
- Keyboard nudging (arrow keys → pixel / sub-pixel movement).

All of this lives in `LayoutPanel.cpp` + each model's
`ScreenLocation::DrawHandles` / `MouseDown` / `MouseMove` /
`MouseUp`. Touch-equivalent gestures and Apple Pencil support need
design work when this phase starts.

## L-4. Property grid for selected model

Desktop shows a full property grid in the Layout panel sidebar when a
model is selected — dimensions, rotation, start channel, strings,
nodes-per-string, pixel style, custom model matrix, DMX channels,
State/Face definitions, etc. Some of this overlaps with the effect
inspector we already have in Phase C, but model-level properties are
a distinct editing surface.

## L-5. Align / distribute / flip / resize-to-match

Toolbar actions from the desktop Layout panel that operate on
multi-selected models:

- Align left / right / top / bottom / centre (horizontal & vertical).
- Distribute horizontally / vertically.
- Flip horizontal / vertical.
- Resize to match width / height / both.
- Rotate 90° / 180° / 270° / mirror.

Each is a short C++ routine in `LayoutPanel`; the hard part on iPad
is multi-select UX and an action surface (toolbar, context menu,
gesture).

## L-6. Bulk edit / export helpers

Additional Layout-editor amenities to port when the editor lands:

- **Bulk edit** — select N models, change one property on all of
  them.
- **CAD / DXF export** — dumps the current layout to a CAD file.
- **Wiring view** — shows controller-to-model wiring overlaid on the
  preview.
- **Import previews from other shows** — `ImportPreviewsModelsDialog`
  on desktop.

## L-7. xLights Views management

The Display Elements dialog's "Views" tab is partly about playback
scoping (which timing tracks / models a sequence edits) and partly
about layout scoping (which models belong to a preview). Phase F-6
covers the playback side. The layout-scoping side — creating /
renaming / reassigning per-view model visibility — belongs here if
it turns out it can't cleanly live in Phase F.
