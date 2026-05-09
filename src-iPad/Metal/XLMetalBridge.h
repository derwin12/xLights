#pragma once

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>

@class XLSequenceDocument;

// ObjC bridge for xlStandaloneMetalCanvas + iPadModelPreview.
// Owns the C++ canvas and preview, drives model rendering.

NS_ASSUME_NONNULL_BEGIN

@interface XLMetalBridge : NSObject

- (instancetype)initWithName:( NSString* )name;
- (void)attachLayer:(CAMetalLayer*)layer;
- (void)setDrawableSize:(CGSize)size scale:(CGFloat)scale;

// Draw all models at the given frame time using DisplayEffectOnWindow.
// When setPreviewModel: has set a non-empty name, only that model (plus view
// objects) is drawn — everything else is skipped. Empty/nil preview model =
// full house preview behavior.
- (void)drawModelsForDocument:(XLSequenceDocument*)doc atMS:(int)frameMS pointSize:(float)pointSize;

// Preview-mode model filter. Empty/nil string clears the filter.
- (void)setPreviewModel:(NSString*)modelName;

// Camera controls — route through the active PreviewCamera on iPadModelPreview.
// Zoom: 1.0 default, >1 zooms in. Pan is in world units. Rotate is in degrees.
- (void)setCameraZoom:(float)zoom;
- (float)cameraZoom;
- (void)setCameraPanX:(float)x panY:(float)y;
- (void)offsetCameraPanX:(float)dx panY:(float)dy;
- (float)cameraPanX;
- (float)cameraPanY;
- (void)setCameraAngleX:(float)ax angleY:(float)ay;
- (void)offsetCameraAngleX:(float)dx angleY:(float)dy;
- (float)cameraAngleX;
- (float)cameraAngleY;
- (void)resetCamera;

// 2D/3D mode. True = perspective (3D); false = orthographic (2D).
- (void)setIs3D:(BOOL)is3d;
- (BOOL)is3D;

// Coarse view-object (house mesh / terrain / gridlines / ground images)
// visibility toggle for the House Preview. No effect on Model Preview —
// that pane never draws view objects. Default: YES.
- (void)setShowViewObjects:(BOOL)show;
- (BOOL)showViewObjects;

// Drop the cached background texture. Called when the active layout
// group changes (possibly new background image path). Next draw
// re-loads whatever the render context's active-group path is.
- (void)invalidateBackgroundCache;

// Viewpoints (saved camera positions). List is filtered to the pane's
// current 2D/3D mode. Apply copies a saved PreviewCamera into the
// active camera; saveAs captures the active camera under a new name.
// Persistence for the add/delete mutations requires the document — the
// document triggers the rgbeffects.xml rewrite.
- (NSArray<NSString*>*)viewpointNamesForDocument:(XLSequenceDocument*)doc;
- (BOOL)applyViewpointNamed:(NSString*)name
                forDocument:(XLSequenceDocument*)doc;
- (BOOL)saveCurrentViewAs:(NSString*)name
              forDocument:(XLSequenceDocument*)doc;
- (BOOL)deleteViewpointNamed:(NSString*)name
                 forDocument:(XLSequenceDocument*)doc;
- (void)restoreDefaultViewpointForDocument:(XLSequenceDocument*)doc;

// Fit-to-window camera shortcuts. Fit All frames every model in the
// active layout group (matching the House Preview filter); Fit Model
// frames the single named model (or returns NO if it's missing or
// hidden by the current layout group). Adjusts zoom + pan (2D) or
// distance + pan (3D) without changing rotation angles.
- (BOOL)fitAllModelsForDocument:(XLSequenceDocument*)doc;
- (BOOL)fitModelNamed:(NSString*)name
          forDocument:(XLSequenceDocument*)doc;

// Phase J-2 — Layout Editor selection highlight. Empty / nil
// clears the highlight. The bridge does NOT auto-redraw; SwiftUI
// callers should `setNeedsDisplay` on the MTKView after setting
// the selection so the ring appears on the next frame.
- (void)setSelectedModel:(nullable NSString*)name;

// Phase J-2 — Layout Editor in-canvas overlays. Per-bridge state
// (not persisted to rgbeffects.xml in J-2). Initial values for
// the LayoutEditor pane are seeded from iPadRenderContext's
// `Display2DGrid` / `Display2DBoundingBox` flags on first
// `drawModelsForDocument`. Other panes default both to NO and
// ignore them. 2D-only — neither overlay draws in 3D.
- (void)setShowLayoutGrid:(BOOL)show;
- (BOOL)showLayoutGrid;
- (void)setShowLayoutBoundingBox:(BOOL)show;
- (BOOL)showLayoutBoundingBox;

// Phase J-2 — snap-to-grid for drag-to-move. When YES, the post-
// delta centre of the moved model snaps to the nearest grid
// spacing multiple (read from the iPadRenderContext's
// `Display2DGridSpacing`, with 2D-center0 origin honoured).
// Default: NO. Per-session — not persisted to rgbeffects.xml.
- (void)setSnapToGrid:(BOOL)snap;
- (BOOL)snapToGrid;

// Phase J-2 — first-pixel highlight (`highlightFirst` arg to
// `DisplayModelOnWindow`). When YES, the first node of every
// model is rendered in cyan instead of its native colour, so the
// user can spot wiring origin while laying out.
// Default: NO. LayoutEditor pane only — other panes ignore.
- (void)setShowFirstPixel:(BOOL)show;
- (BOOL)showFirstPixel;

// Phase J-2 — return the topmost model whose world bounding box
// contains `point` (in view-point coordinates relative to the
// MTKView's bounds), or nil if no model is hit. `viewSize` is the
// MTKView's bounds in points (not pixels) — the bridge multiplies
// by its stored scale factor internally.
//
// Today: 2D-only. In 3D mode the method returns nil; full 3D
// ray-cast hit testing lands alongside the gizmo work.
//
// Iterates the active layout group's models in reverse draw order
// (last drawn = on top), so a small model rendered on top of a
// larger backdrop wins the pick.
- (nullable NSString*)pickModelAtScreenPoint:(CGPoint)point
                                    viewSize:(CGSize)viewSize
                                 forDocument:(XLSequenceDocument*)doc;

// Phase J-2 — translate a screen-space drag delta (points) into a
// world-space move on the named model's centre. Returns YES if the
// model existed, was unlocked, and accepted the move (and was
// marked dirty); NO otherwise. 2D-only (3D drag follows the gizmo
// design). The caller is responsible for scoping this to the model
// it intends to drag — typically the LayoutEditor's currently-
// selected model.
- (BOOL)moveModel:(NSString*)name
       byDeltaDX:(CGFloat)dx
              dY:(CGFloat)dy
        viewSize:(CGSize)viewSize
     forDocument:(XLSequenceDocument*)doc;

// Phase J-2 — resize handles. The bridge draws 4 corner handles
// around the selected model when the LayoutEditor selection ring
// is active. Hit-test returns 0..3 (corner index, see below) for a
// handle hit, or -1 for no handle. The drag method takes the
// touch's screen point and resizes the model so the dragged corner
// follows the touch and the OPPOSITE corner stays fixed.
//
// Corner indices, with ortho Y-up:
//   0 = top-left    (-x, +y)
//   1 = top-right   (+x, +y)
//   2 = bottom-right (+x, -y)
//   3 = bottom-left (-x, -y)
//
// Both methods return immediately for unselected / locked / empty
// preview / 3D mode. 3D resize handles ship with the gizmo work.
- (NSInteger)pickHandleAtScreenPoint:(CGPoint)point
                            viewSize:(CGSize)viewSize
                         forDocument:(XLSequenceDocument*)doc;

- (BOOL)dragHandle:(NSInteger)handleIndex
   toScreenPoint:(CGPoint)point
        viewSize:(CGSize)viewSize
     forDocument:(XLSequenceDocument*)doc;

// Phase J-2 — call when the gesture that started a handle drag
// ends, so per-drag state on the screen location (active_axis,
// latching) is cleared and the next gesture starts clean.
- (void)endHandleDragForDocument:(XLSequenceDocument*)doc;

// Phase J-2 — single-tap dispatch for the LayoutEditor pane in
// 3D. When a tap lands on the active centre handle of the
// currently-selected model, cycles `axis_tool` between
// translate / scale / rotate (mirrors desktop's
// LayoutPanel.cpp:3726). Returns YES if the tool was advanced
// (caller should repaint); NO if the tap should fall through
// to model-selection. 2D / no-selection returns NO immediately.
- (BOOL)handleCenterHandleTapAtScreenPoint:(CGPoint)point
                                  viewSize:(CGSize)viewSize
                               forDocument:(XLSequenceDocument*)doc;

// Diagnostic surface for the SwiftUI preview pane. `errorReason`
// returns the most recent silent-fail reason (no Metal layer, 0×0
// drawable, render context missing, StartDrawing failed, no models
// to draw, etc.) or nil when everything's fine. `hasRenderedSuccessfully`
// flips true after the first frame that completes through
// `EndDrawing`. Together they let the SwiftUI view distinguish
// "still warming up" (no error, hasRendered=false) from "actually
// broken" (errorReason set, hasRendered=false for ≥ a couple of
// seconds).
//
// Each unique error is also logged via spdlog at WARN level on first
// occurrence, so iPad → Tools → Package Logs captures the failure
// for tester reports.
- (nullable NSString*)errorReason;
- (BOOL)hasRenderedSuccessfully;

@end

NS_ASSUME_NONNULL_END
