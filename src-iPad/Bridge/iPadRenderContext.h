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

// iPadRenderContext — RenderContext for loading, rendering, and displaying
// sequences on iPad.  Includes RenderEngine for effect rendering.

#include "render/RenderContext.h"
#include "render/SequenceData.h"
#include "render/SequenceElements.h"
#include "render/SequenceFile.h"
#include "render/SequenceViewManager.h"
#include "render/RenderEngine.h"
#include "render/RenderCache.h"
#include "render/IRenderProgressSink.h"
#include "render/ViewpointMgr.h"
#include "effects/EffectManager.h"
#include "outputs/OutputManager.h"
#include "models/ModelManager.h"
#include "models/OutputModelManager.h"
#include "models/ViewObjectManager.h"
#include "utils/JobPool.h"
#include "lyrics/PhonemeDictionary.h"
#include "utils/xlImage.h"

#include <list>
#include <map>
#include <memory>
#include <optional>
#include <deque>
#include <set>
#include <string>
#include <vector>

class iPadRenderContext : public RenderContext {
public:
    iPadRenderContext();
    ~iPadRenderContext() override;

    // Show folder management
    bool LoadShowFolder(const std::string& showDir);
    bool LoadShowFolder(const std::string& showDir,
                        const std::list<std::string>& mediaFolders);
    const std::string& GetShowDirectory() const override { return _showDir; }

    // Sequence management
    bool OpenSequence(const std::string& path);
    void CloseSequence();

    // Write the rendered sequence to a v2/zstd/sparse .fseq file matching
    // desktop's `xLightsFrame::WriteFalconPiFile` format. Sparse ranges come
    // from the master view (each ELEMENT_TYPE_MODEL row contributes its
    // channel range, ModelGroups expand to their children). Embeds 'mf' (media
    // filename), 'sp' (source), and FE/FC (FPP Effects/Commands) variable
    // headers when applicable. Returns false on I/O failure or if no sequence
    // is loaded; returns true even if `_sequenceData` is empty (writes a
    // header-only file) — callers should ensure a render has completed first.
    bool WriteFseq(const std::string& path);

    // Try to short-circuit a render by loading frame data from `path`. Returns
    // true only if the file opens, the fseq's mtime is >= the xsq's mtime, and
    // the channel/frame/step shape matches the currently-loaded sequence.
    // On success `_sequenceData` is populated and the caller can skip
    // `RenderAll`. On any mismatch returns false and leaves `_sequenceData`
    // untouched so a normal render can proceed.
    bool TryLoadFseq(const std::string& fseqPath, const std::string& xsqPath);

    // RenderContext implementation
    const std::string& GetFseqDirectory() const override { return _showDir; }
    const std::list<std::string>& GetMediaFolders() const override { return _mediaFolders; }
    bool IsInShowFolder(const std::string& file) const override;
    bool IsInShowOrMediaFolder(const std::string& file) const override;
    // Copy `file` into `<showDir>/<subdirectory>`, returning the final
    // absolute path. Appends `_N` on name collision unless `reuse` and
    // the existing file's contents already match. Empty string on
    // failure (no show folder configured, copy error).
    std::string MoveToShowFolder(const std::string& file,
                                  const std::string& subdirectory,
                                  bool reuse) override;

    // Same as MoveToShowFolder but the destination root is one of the
    // configured media folders (`mediaFolderPath` must appear in
    // `_mediaFolders` or we refuse). Used by the iPad fileImporter
    // "destination: Media Folder X" branch.
    std::string CopyToMediaFolder(const std::string& file,
                                   const std::string& mediaFolderPath,
                                   const std::string& subdirectory);
    std::string MakeRelativePath(const std::string& file) const override;

    SequenceElements& GetSequenceElements() override { return _sequenceElements; }
    SequenceViewManager& GetSequenceViewManager() { return _viewsManager; }
    bool IsSequenceLoaded() const override { return _sequenceFile && _sequenceFile->IsOpen(); }
    AudioManager* GetCurrentMediaManager() const override;
    const std::string& GetHeaderInfo(HEADER_INFO_TYPES type) const override;

    // B43: alt audio track switching for the *waveform display*. Does
    // NOT change playback — playback still plays the main sequence
    // track via GetCurrentMediaManager. -1 = main, 0..N-1 = alt index.
    // GetWaveformMedia falls back to the main track when the requested
    // alt index is out of range or its AudioManager hasn't loaded.
    int GetWaveformTrackIndex() const { return _waveformTrackIndex; }
    void SetWaveformTrackIndex(int idx);
    AudioManager* GetWaveformMedia() const;
    int GetAltTrackCount() const;
    std::string GetAltTrackDisplayName(int idx) const;

    Model* GetModel(const std::string& name) const override;
    EffectManager& GetEffectManager() override { return _effectManager; }
    OutputModelManager* GetOutputModelManager() override { return &_outputModelManager; }

    bool AbortRender(int maxTimeMs = 60000) override;
    void RenderEffectForModel(const std::string& model, int startms, int endms, bool clear) override;
    TimingElement* AddTimingElement(const std::string& name,
                                    const std::string& subType = "") override;
    void SuspendAutoSave(bool) override {}
    bool IsLowDefinitionRender() const override { return true; }

    // Rendering
    void RenderAll();
    void SetModelColors(int frameMS);
    SequenceData& GetSequenceData() { return _sequenceData; }
    bool IsRenderDone();

    // True when the most recent render had at least one job aborted
    // (via SignalAbort — typically from HandleMemoryWarning or an
    // explicit AbortRender). Counter resets at every Render() start
    // (`_abortedRenderJobs = 0` in RenderEngine::Render).
    bool WasRenderAborted() const;

    // Coarse fraction (0..1) of the in-flight Render() call's frame work
    // that has completed. Walks every active RenderProgressInfo's per-row
    // job and sums each `IRenderJobStatus::GetCurrentFrame()` against the
    // total frame range. Returns 1.0 when no render is active so a UI
    // can use this directly without racing IsRenderDone().
    float GetRenderProgressFraction() const;

    // === Preset model / preview rendering =================================
    // Mirrors xLightsFrame's standalone preset-render scaffolding: a 64×64
    // RGB `MatrixModel` owned by its own `ModelManager`, plus a dedicated
    // `SequenceElements` / `SequenceData` pair. Used to render an effect
    // in isolation for media-picker thumbnails (shader previews, future
    // preset GIFs, etc.) without touching the user's real sequence.
    void EnsurePresetModel();
    Model* GetPresetModel() { EnsurePresetModel(); return _presetModel; }
    SequenceElements& GetPresetSequenceElements() { return _presetSequenceElements; }
    SequenceData& GetPresetSequenceData() { return _presetSequenceData; }

    // Render an effect sitting in `seqElements` on `matrixModel` for
    // `numFrames` frames at `frameTimeMs`. Returns the per-frame RGBA
    // rasterisation as `xlImage`s (same layout `ShaderPreviewGenerator`
    // stores via `MediaCacheEntry::SetPreviewFrames`). Synchronously
    // blocks the calling thread until the render completes (iPad path:
    // call from a utility queue, not the main thread). Ports
    // `xLightsFrame::RenderEffectToFrames` from
    // `src-ui-wx/app-shell/TabConvert.cpp:856`.
    std::vector<std::shared_ptr<xlImage>> RenderEffectToFrames(
        Model* matrixModel,
        SequenceData& seqData,
        SequenceElements& seqElements,
        size_t numFrames,
        int frameTimeMs);

    // Fill a `ShaderMediaCacheEntry`'s preview-frame strip by rendering
    // the shader on the preset matrix model at default parameter
    // values. No-op if the entry already has a preview cached. Ports
    // `GenerateShaderPreview` from
    // `src-ui-wx/media/ShaderPreviewGenerator.cpp`. Synchronous; call
    // from a utility-priority thread.
    void GenerateShaderPreview(class ShaderMediaCacheEntry* entry);

    // Memory-pressure response. Called from Swift when the system signals
    // memory warning / critical. Aborts any in-flight render and purges the
    // render cache so we don't hold onto frame buffers we no longer need.
    void HandleMemoryWarning();
    void HandleMemoryCritical();

    // Accessors
    OutputManager& GetOutputManager() { return _outputManager; }
    ModelManager& GetModelManager() { return *_modelManager; }
    ViewObjectManager& GetAllObjects() { return *_viewObjectManager; }
    // J-7 — null-safe checks. `GetModelManager()` / `GetAllObjects()`
    // dereference the unique_ptr without guarding, so callers that
    // can run before `LoadShowFolder` must check via these first.
    // `GetModelsForActivePreview()` does this internally; methods
    // that call `GetModels()` direct do not.
    bool HasModelManager() const { return _modelManager != nullptr; }
    bool HasViewObjectManager() const { return _viewObjectManager != nullptr; }
    SequenceFile* GetSequenceFile() { return _sequenceFile.get(); }
    // B49: expose the render engine so the export-model bridge can
    // call `RenderEngine::ExportModelData` without creating a
    // second engine. Engine may be null before `EnsureRenderEngine`
    // has been called.
    RenderEngine* GetRenderEngine() { EnsureRenderEngine(); return _renderEngine.get(); }

    // B85 — lazy-loaded phoneme dictionary. First call loads
    // `standard_dictionary` / `extended_dictionary` /
    // `user_dictionary` / `phoneme_mapping` from the bundled
    // `dictionaries/` resource folder (+ the show dir for
    // user overrides). Thread-safe is NOT required — callers are
    // on the main thread.
    PhonemeDictionary& GetPhonemeDictionary();
    // Virtual preview canvas size from <settings><previewWidth/Height>
    // in xlights_rgbeffects.xml, defaulted to desktop's 1280×720 when
    // absent. Consumed by iPadModelPreview in House Preview mode so the
    // 2D ortho projection maps world coords onto pixel coords the same
    // way desktop does.
    int GetPreviewWidth() const { return _previewWidth; }
    int GetPreviewHeight() const { return _previewHeight; }

    // <settings><Display2DCenter0 value="1"/>. When set, desktop places
    // world X=0 at the horizontal centre of the preview (shows with
    // models laid out around a centered origin, e.g. -600..+600 rather
    // than 0..1200). Ignoring this flag was the cause of the House
    // Preview rendering blank for center-origin shows.
    bool GetDisplay2DCenter0() const { return _display2DCenter0; }

    // Layout-editor display toggles (Phase J-0). Read from <settings>
    // in xlights_rgbeffects.xml; default off / 100-unit grid spacing.
    // Read-only on iPad in J-0 — setters land alongside layout-editor
    // mutation in J-1+.
    bool GetDisplay2DGrid() const { return _display2DGrid; }
    long GetDisplay2DGridSpacing() const { return _display2DGridSpacing; }
    bool GetDisplay2DBoundingBox() const { return _display2DBoundingBox; }

    // <settings><LayoutMode3D value="1"/>. Desktop's last-used 3D vs 2D
    // preference for the House Preview, read at show-folder load.
    // iPad uses this as the initial value for House Preview's is3D
    // toggle (the user can still flip it per-session via the overlay
    // picker; we don't write back since layout editing is desktop-only).
    bool GetLayoutMode3D() const { return _layoutMode3D; }

    // House Preview background image + brightness/alpha/scale — values
    // come from `<settings>` in xlights_rgbeffects.xml and are read-only
    // on iPad (editing lives in the desktop Layout panel, out of iPad
    // scope). Path is FixFile-resolved against the show directory; empty
    // string means "no background".
    //
    // These are the DEFAULT-group values. Use GetActiveBackground*()
    // below to pick between these and the named-group overrides when a
    // non-Default layout group is active.
    const std::string& GetBackgroundImage() const { return _backgroundImage; }
    int GetBackgroundBrightness() const { return _backgroundBrightness; }
    int GetBackgroundAlpha() const { return _backgroundAlpha; }
    bool GetScaleBackgroundImage() const { return _scaleBackgroundImage; }

    // Named layout groups from <layoutGroups> in rgbeffects.xml. Each
    // carries its own background settings and scopes the House Preview
    // to a filtered set of models (those whose `layout_group` matches
    // the group name, plus any marked "All Previews"). The "Default"
    // group is implicit (driven by the top-level <settings> values
    // above) — it is NOT listed in GetNamedLayoutGroups().
    struct NamedLayoutGroup {
        std::string name;
        std::string backgroundImage;
        int backgroundBrightness = 100;
        int backgroundAlpha = 100;
        bool scaleBackgroundImage = false;
    };
    const std::vector<NamedLayoutGroup>& GetNamedLayoutGroups() const { return _namedLayoutGroups; }

    // Active House-Preview layout group. "Default" means the implicit
    // default preview (models with layout_group == "Default" or
    // "All Previews"); other values must match a named group from
    // GetNamedLayoutGroups(). Unknown names fall back to "Default".
    const std::string& GetActiveLayoutGroup() const { return _activeLayoutGroup; }
    void SetActiveLayoutGroup(const std::string& name);

    // Active-group view of the background settings. Picks the correct
    // source (Default vs. a named group) so callers don't need to
    // switch on GetActiveLayoutGroup() themselves.
    const std::string& GetActiveBackgroundImage() const;
    int GetActiveBackgroundBrightness() const;
    int GetActiveBackgroundAlpha() const;
    bool GetActiveScaleBackgroundImage() const;

    // J-8 (2D Background pseudo-object) — write through to the
    // correct storage (default <settings> or named group) and
    // record the group name in `_dirtyBackgroundGroups`. The save
    // patcher rewrites the matching XML attributes in place. Each
    // setter returns YES iff the value actually changed (matching
    // the layout-property setter convention).
    bool SetActiveBackgroundImage(const std::string& path);
    bool SetActiveBackgroundBrightness(int brightness);
    bool SetActiveBackgroundAlpha(int alpha);
    bool SetActiveScaleBackgroundImage(bool scale);

    // Expanded list of models to render for the active layout group.
    // Filters by layout_group and expands ModelGroup children exactly
    // like desktop UpdateModelsList (TabSequence.cpp:1209), minus
    // duplicate suppression. Call once per frame.
    std::vector<Model*> GetModelsForActivePreview() const;

    // Whether the active group draws view objects. Only "Default"
    // draws them (matching desktop — view objects are hard-coded to
    // layout_group "Default").
    bool ActivePreviewShowsViewObjects() const { return _activeLayoutGroup == "Default"; }

    // Saved camera views (viewpoints). Loaded from the `<Viewpoints>`
    // node in xlights_rgbeffects.xml; each camera is flagged 2D or 3D
    // and named. UI filters by the preview's current mode before
    // showing them.
    ViewpointMgr& GetViewpointMgr() { return _viewpointMgr; }
    const ViewpointMgr& GetViewpointMgr() const { return _viewpointMgr; }
    PreviewCamera* GetNamedCamera3D(const std::string& name) override {
        return _viewpointMgr.GetNamedCamera3D(name);
    }

    // Rewrite just the `<Viewpoints>` subtree of the on-disk
    // xlights_rgbeffects.xml so saved-as / delete survive app restart.
    // Preserves every other node (models, layoutGroups, settings, …)
    // by re-reading the file, replacing the Viewpoints child, and
    // writing back. Returns false if the file couldn't be read /
    // serialized. Heavy-ish (one disk round-trip per save), but
    // viewpoint edits are a rare user action.
    bool SaveViewpoints();

    // Mark a model as having dirty in-memory <stateInfo> so the next
    // SaveModelStates() call rewrites its on-disk entry. DMX state
    // saves are the current caller; future model-edit flows can
    // tag the same path.
    void MarkModelStateDirty(const std::string& modelName) {
        if (!modelName.empty()) _dirtyStateModels.insert(modelName);
    }

    // For each model in `_dirtyStateModels`, locate its `<model>` node
    // in xlights_rgbeffects.xml, drop the existing `<stateInfo>` children,
    // and rewrite them from the live `Model::GetStateInfo()` map via
    // `Model::WriteStateInfo`. One disk round-trip per call. Clears
    // the dirty set on success. Returns false on read/write failure;
    // missing model nodes are skipped with a warning but don't fail
    // the save.
    bool SaveModelStates();

    // Phase J-1 — layout-property edits (transforms, dimensions,
    // rotation, locked, name, layoutGroup, controllerName). Each
    // edit calls MarkLayoutModelDirty; SaveLayoutChanges() rewrites
    // the on-disk `<model>` for every dirty entry by serializing the
    // in-memory Model with `XmlSerializer::SerializeModel()` and
    // replacing the matching node in xlights_rgbeffects.xml.
    void MarkLayoutModelDirty(const std::string& modelName) {
        if (!modelName.empty()) _dirtyLayoutModels.insert(modelName);
    }
    // J-6 (view object editing) — view objects live in their own
    // XML section (`<view_objects>`); SaveLayoutChanges() walks
    // this set separately so a dirty model + dirty object can both
    // land in a single save.
    void MarkLayoutViewObjectDirty(const std::string& objectName) {
        if (!objectName.empty()) _dirtyLayoutViewObjects.insert(objectName);
    }
    // J-12 (view object CRUD) — structural lifecycle. Mirrors
    // the J-7 group create/delete plumbing.
    void MarkViewObjectCreated(const std::string& objectName) {
        if (objectName.empty()) return;
        _createdViewObjects.insert(objectName);
        _deletedViewObjects.erase(objectName);
    }
    void MarkViewObjectDeleted(const std::string& objectName) {
        if (objectName.empty()) return;
        if (_createdViewObjects.erase(objectName) > 0) {
            _dirtyLayoutViewObjects.erase(objectName);
            return;
        }
        _deletedViewObjects.insert(objectName);
        _dirtyLayoutViewObjects.erase(objectName);
    }
    // J-7 (group CRUD) — structural group lifecycle. A newly-
    // created group needs a fresh `<modelGroup>` element appended;
    // a deleted group needs its element removed. Plain edits go
    // through the normal dirty set.
    void MarkGroupCreated(const std::string& groupName) {
        if (groupName.empty()) return;
        _createdGroups.insert(groupName);
        // If the user deletes then re-creates with the same name,
        // cancel the pending delete.
        _deletedGroups.erase(groupName);
    }
    void MarkGroupDeleted(const std::string& groupName) {
        if (groupName.empty()) return;
        // If the group was created in-memory and never saved, the
        // delete cancels out — nothing on disk to remove.
        if (_createdGroups.erase(groupName) > 0) {
            _dirtyLayoutModels.erase(groupName);
            return;
        }
        _deletedGroups.insert(groupName);
        _dirtyLayoutModels.erase(groupName);
    }
    bool HasDirtyLayoutModels() const {
        return !_dirtyLayoutModels.empty() ||
               !_dirtyLayoutViewObjects.empty() ||
               !_createdGroups.empty() ||
               !_deletedGroups.empty() ||
               !_dirtyBackgroundGroups.empty() ||
               !_createdViewObjects.empty() ||
               !_deletedViewObjects.empty();
    }
    bool SaveLayoutChanges();
    // Clear the dirty set without writing to disk — used after a
    // Discard Changes that has rolled back every in-memory edit
    // through the undo stack. The undo restores re-marked every
    // model dirty; without this clear, hasUnsavedLayoutChanges()
    // reports true and the Save button stays enabled.
    void ClearDirtyLayoutModels() {
        _dirtyLayoutModels.clear();
        _dirtyLayoutViewObjects.clear();
        _createdGroups.clear();
        _deletedGroups.clear();
        _dirtyBackgroundGroups.clear();
        _createdViewObjects.clear();
        _deletedViewObjects.clear();
    }

    // Phase J-2 — layout undo. Snapshot the common-properties
    // surface (centre, dimensions, rotation, locked, layoutGroup,
    // controllerName) of `modelName` onto an in-memory undo stack.
    // Caller is expected to push BEFORE making the edit. UndoLast
    // pops the most recent snapshot and reapplies its values
    // through the regular setters, which marks the model dirty
    // again so the change persists on next save. Stack is capped
    // at 100 entries (oldest dropped on overflow).
    struct LayoutUndoEntry {
        std::string modelName;
        float hcenter, vcenter, dcenter;
        float width, height, depth;
        float rotateX, rotateY, rotateZ;
        bool locked;
        std::string layoutGroup;
        std::string controllerName;
    };
    void PushLayoutUndoSnapshotForModel(const std::string& modelName);
    bool UndoLastLayoutChange();
    bool CanUndoLayoutChange() const { return !_layoutUndoStack.empty(); }
    size_t LayoutUndoDepth() const { return _layoutUndoStack.size(); }

    // Model pixel data for a given frame — returns (x, y, r, g, b) tuples
    struct PixelData {
        float x, y;
        uint8_t r, g, b;
    };
    std::vector<PixelData> GetModelPixels(const std::string& modelName, int frameMS);
    std::vector<PixelData> GetAllModelPixels(int frameMS);

    // Per-state effect bracket colours, sourced from the show folder's
    // <colors> palette in xlights_rgbeffects.xml. Falls back to desktop
    // defaults (xLights_color[] in ColorManager.h) when a key is absent —
    // so a fresh show with no palette customisations gets the same look
    // as desktop xLights, and a customised palette round-trips between
    // the two clients.
    enum class EffectBracketState {
        Default = 0,
        Selected,
        Locked,
        Disabled,
    };
    struct PaletteColor {
        uint8_t r = 0, g = 0, b = 0;
    };
    PaletteColor GetEffectBracketColor(EffectBracketState state) const;

private:
    std::string _showDir;
    std::list<std::string> _mediaFolders;

    OutputManager _outputManager;
    OutputModelManager _outputModelManager;
    std::unique_ptr<ModelManager> _modelManager;
    std::unique_ptr<ViewObjectManager> _viewObjectManager;
    EffectManager _effectManager;
    SequenceElements _sequenceElements;
    SequenceViewManager _viewsManager;
    std::unique_ptr<SequenceFile> _sequenceFile;
    std::optional<pugi::xml_document> _sequenceDoc;

    // Rendering
    SequenceData _sequenceData;
    std::unique_ptr<JobPool> _jobPool;
    RenderCache _renderCache;
    std::unique_ptr<RenderEngine> _renderEngine;
    unsigned int _modelsChangeCount = 0;

    // Virtual preview canvas size — desktop defaults.
    int _previewWidth = 1280;
    int _previewHeight = 720;
    bool _display2DCenter0 = false;
    bool _display2DGrid = false;
    long _display2DGridSpacing = 100;
    bool _display2DBoundingBox = false;
    bool _layoutMode3D = true;

    std::string _backgroundImage;
    int _backgroundBrightness = 100;
    int _backgroundAlpha = 100;
    bool _scaleBackgroundImage = false;

    std::vector<NamedLayoutGroup> _namedLayoutGroups;
    std::string _activeLayoutGroup = "Default";

    // B85 phoneme dictionary, lazy-loaded.
    std::unique_ptr<PhonemeDictionary> _phonemeDict;

    ViewpointMgr _viewpointMgr;

    // Models whose in-memory <stateInfo> map has diverged from the
    // on-disk xlights_rgbeffects.xml. SaveModelStates() reads + drains
    // this set.
    std::set<std::string> _dirtyStateModels;

    // J-1 — models whose layout-relevant in-memory state (transforms,
    // dimensions, rotation, locked, name, layoutGroup, controllerName)
    // has diverged from the on-disk file. SaveLayoutChanges() reads +
    // drains this set.
    std::set<std::string> _dirtyLayoutModels;
    // J-6 — view objects with pending edits in the
    // `<view_objects>` section. SaveLayoutChanges() patches each
    // matching `<view_object>` element in place; the on-disk form
    // is a flat attribute list so we don't need full serialization.
    std::set<std::string> _dirtyLayoutViewObjects;
    // J-7 — model groups that exist in-memory but not yet on
    // disk. SaveLayoutChanges() appends fresh `<modelGroup>`
    // elements for these. Cleared on save.
    std::set<std::string> _createdGroups;
    // J-7 — model groups that should be removed from disk on
    // next save (already gone from in-memory ModelManager).
    std::set<std::string> _deletedGroups;
    // J-12 — view objects created in-memory that need full
    // serialization on next save (append to <view_objects>).
    std::set<std::string> _createdViewObjects;
    // J-12 — view objects to drop from disk on next save.
    std::set<std::string> _deletedViewObjects;
    // J-8 (2D Background pseudo-object) — set of layout-group
    // names whose background settings have unsaved edits.
    // "Default" means the top-level `<settings>` element;
    // anything else maps into `<layoutGroups>`.
    std::set<std::string> _dirtyBackgroundGroups;

    // J-2 — undo stack for layout edits. Bounded to 100 entries.
    std::deque<LayoutUndoEntry> _layoutUndoStack;
    static constexpr size_t kLayoutUndoMaxDepth = 100;

    // Cache of the show folder's <colors> palette so per-frame bracket
    // queries don't re-scan XML. Populated on every LoadShowFolder.
    // Empty entries fall through to ColorManager defaults at lookup
    // time (see GetEffectBracketColor).
    std::map<std::string, PaletteColor> _palette;

    // B43: -1 = main sequence audio, 0..N-1 = alt track index.
    int _waveformTrackIndex = -1;

    // Preset model scaffolding — lazily built on first preview render.
    Model* _presetModel = nullptr;
    std::unique_ptr<ModelManager> _presetModelManager;
    SequenceElements _presetSequenceElements{ this };
    SequenceData _presetSequenceData;

    // Ensures the render engine + its pool are ready before using them
    // from a preview render path (before `RenderAll` has been called).
    void EnsureRenderEngine();

    // Re-allocates `_sequenceData` only when the sequence's shape
    // (numChannels / numFrames / frameTime) has actually changed.
    // Normally a no-op — OpenSequence pre-allocates once and
    // subsequent RenderAll passes reuse. Triggers a fresh init
    // after duration / frame-rate / channel-count mutations.
    void EnsureSequenceDataSized();
};
