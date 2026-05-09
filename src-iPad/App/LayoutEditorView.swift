import SwiftUI

/// Phase J-0 / J-1 — Layout Editor screen. Opens via Tools → Edit
/// Layout… in its own `WindowGroup("layout-editor")` scene. The
/// user picks a layout group, sees its models in a Metal canvas,
/// and selects a model from the side panel to inspect or edit its
/// common-properties surface (J-1: centre, dimensions, rotation,
/// locked, layout group, controller name).
///
/// J-2 adds tap-to-select on the canvas, drag/resize/rotate
/// handles, and overlay rendering (names, first-pixel, grid,
/// bounding boxes). J-3 adds Add Model + per-type properties.
/// J-4 adds multi-select align/distribute/flip/resize-to-match.
/// See `plans/phase-j-layout-editor.md`.
struct LayoutEditorView: View {
    @Environment(SequencerViewModel.self) var viewModel

    /// Cached at view onAppear / show-folder change so SwiftUI re-
    /// renders when the underlying C++ render context swaps groups.
    /// `viewModel.document` is a stable reference; reading it inside
    /// `body` won't re-fire when its internals change.
    @State private var layoutGroups: [String] = ["Default"]
    @State private var activeLayoutGroup: String = "Default"
    @State private var modelNames: [String] = []
    @State private var displayState: [String: Any] = [:]
    @State private var settings = PreviewSettings(is3DDefault: true,
                                                  showViewObjectsDefault: true)
    @State private var controlsVisible: Bool = false
    /// Bumped by every successful `setLayoutModelProperty` call so
    /// the summary view re-reads from the bridge. Avoids holding
    /// our own copy of the property snapshot in @State (which
    /// could drift from the live Model behind the bridge).
    @State private var summaryToken: Int = 0
    /// Mirror of `document.hasUnsavedLayoutChanges` — Swift can't
    /// observe ObjC method results, so we update this manually
    /// after every set + save.
    @State private var hasUnsavedChanges: Bool = false
    @State private var saveErrorMessage: String? = nil
    /// J-2 — mirrors `document.canUndoLayoutChange`. Updated after
    /// every edit + undo so the toolbar button can grey itself out.
    @State private var canUndo: Bool = false
    /// J-2 — confirm-before-save. The toolbar Save button flips
    /// this on; the alert's primary action calls saveLayoutChanges
    /// for real. Avoids accidental writes to xlights_rgbeffects.xml.
    @State private var showingSaveConfirm: Bool = false
    /// J-2 — Layout-Editor-only overlay toggles. Initial values are
    /// seeded from rgbeffects.xml on first draw inside the bridge;
    /// these mirror the bridge state so the SwiftUI toggle UI
    /// reflects what the user sees on the canvas.
    @State private var showLayoutGrid: Bool = false
    @State private var showLayoutBoundingBox: Bool = false
    @State private var overlaysInitialized: Bool = false

    var body: some View {
        @Bindable var vm = viewModel
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(min: 240, ideal: 320, max: 420)
        } detail: {
            canvas
        }
        .navigationTitle("Edit Layout")
        .toolbar {
            ToolbarItem(placement: .primaryAction) {
                Menu {
                    ForEach(layoutGroups, id: \.self) { name in
                        Button {
                            activeLayoutGroup = name
                        } label: {
                            HStack {
                                Text(name)
                                if name == activeLayoutGroup {
                                    Spacer()
                                    Image(systemName: "checkmark")
                                }
                            }
                        }
                    }
                } label: {
                    HStack(spacing: 4) {
                        Image(systemName: "square.stack.3d.up")
                        Text(activeLayoutGroup).lineLimit(1)
                    }
                }
                .disabled(layoutGroups.count <= 1)
            }
            ToolbarItem(placement: .primaryAction) {
                Button {
                    performUndo()
                } label: {
                    Label("Undo", systemImage: "arrow.uturn.backward")
                }
                .disabled(!canUndo)
            }
            ToolbarItem(placement: .primaryAction) {
                Button {
                    showingSaveConfirm = true
                } label: {
                    Label("Save", systemImage: "square.and.arrow.down")
                }
                .disabled(!hasUnsavedChanges)
            }
        }
        .onAppear { refresh() }
        .onChange(of: viewModel.isShowFolderLoaded) { _, _ in refresh() }
        .onChange(of: activeLayoutGroup) { _, newValue in
            viewModel.document.setActiveLayoutGroup(newValue)
            refreshModelList()
        }
        .focusable()
        // J-2 — keyboard nudge for the selected model. Arrow keys
        // move 1 unit, shift+arrow moves 10. Pushed undo per nudge
        // so each tap is independently reversible.
        .onKeyPress(.upArrow,    phases: .down) { keyPress in nudge(0,  +1, keyPress) }
        .onKeyPress(.downArrow,  phases: .down) { keyPress in nudge(0,  -1, keyPress) }
        .onKeyPress(.leftArrow,  phases: .down) { keyPress in nudge(-1,  0, keyPress) }
        .onKeyPress(.rightArrow, phases: .down) { keyPress in nudge(+1,  0, keyPress) }
        .onReceive(NotificationCenter.default.publisher(for: .layoutEditorModelMoved)) { note in
            // Drag-to-move on the canvas (or a keyboard nudge / undo)
            // mutates the bridge directly; refresh the summary +
            // dirty-state so the side panel reflects the new centre
            // and the Save button enables. Convention: object =
            // previewName ("LayoutEditor"), userInfo["model"] = name.
            let movedName = note.userInfo?["model"] as? String
            guard movedName == viewModel.layoutEditorSelectedModel else { return }
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
        }
        .alert("Save failed",
               isPresented: Binding(get: { saveErrorMessage != nil },
                                    set: { if !$0 { saveErrorMessage = nil } })) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(saveErrorMessage ?? "")
        }
        // Confirm-before-save. xlights_rgbeffects.xml is the show's
        // master layout file; an unintended save during testing is
        // expensive to recover from. Discard-changes uses the undo
        // stack to roll back every staged in-memory mutation.
        .confirmationDialog("Save layout changes?",
                            isPresented: $showingSaveConfirm,
                            titleVisibility: .visible) {
            Button("Save to xlights_rgbeffects.xml") {
                saveLayoutChanges()
            }
            Button("Discard Changes", role: .destructive) {
                discardChanges()
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("Saving overwrites the show's xlights_rgbeffects.xml. Discarding rolls back every in-memory edit through the undo stack.")
        }
    }

    // MARK: - Sidebar

    @ViewBuilder
    private var sidebar: some View {
        List(selection: bindingSelection) {
            Section("Models in \(activeLayoutGroup) (\(modelNames.count))") {
                ForEach(modelNames, id: \.self) { name in
                    Text(name)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .tag(name)
                }
            }

            if let selectedName = viewModel.layoutEditorSelectedModel,
               let summary = viewModel.document.modelLayoutSummary(selectedName) {
                Section("Selected Model") {
                    LayoutEditorPropertiesView(
                        modelName: selectedName,
                        summary: summary,
                        layoutGroups: layoutGroups,
                        token: summaryToken,
                        commit: { key, value in
                            commitProperty(modelName: selectedName,
                                           key: key, value: value)
                        }
                    )
                }
            }

            Section("Display") {
                let bg = (displayState["backgroundImage"] as? String) ?? ""
                LabeledContent("Background", value: bg.isEmpty
                               ? "—"
                               : (bg as NSString).lastPathComponent)
                LabeledContent("Canvas",
                               value: "\(displayState["previewWidth"] as? Int ?? 0) × \(displayState["previewHeight"] as? Int ?? 0)")
                LabeledContent("2D centre = 0",
                               value: (displayState["display2DCenter0"] as? Bool ?? false) ? "Yes" : "No")
                LabeledContent("Default mode",
                               value: (displayState["layoutMode3D"] as? Bool ?? true) ? "3D" : "2D")
                LabeledContent("Grid",
                               value: gridLabel)
                LabeledContent("Bounding box",
                               value: (displayState["display2DBoundingBox"] as? Bool ?? false) ? "On" : "Off")
            }
        }
        .listStyle(.sidebar)
    }

    private var gridLabel: String {
        let on = (displayState["display2DGrid"] as? Bool) ?? false
        let spacing = (displayState["display2DGridSpacing"] as? Int)
            ?? Int((displayState["display2DGridSpacing"] as? NSNumber)?.intValue ?? 100)
        return on ? "On (\(spacing))" : "Off"
    }

    /// Sidebar List uses Set<String?> via @Bindable, but we want
    /// single-select semantics. Wrap it manually.
    private var bindingSelection: Binding<String?> {
        Binding(
            get: { viewModel.layoutEditorSelectedModel },
            set: { viewModel.layoutEditorSelectedModel = $0 }
        )
    }

    // MARK: - Canvas

    @ViewBuilder
    private var canvas: some View {
        ZStack(alignment: .topTrailing) {
            // Reuse the PreviewPaneView used by House Preview. New
            // previewName so its NotificationCenter routing
            // (zoom/reset/fit) is independent of the embedded house
            // pane that may also be visible.
            //
            // previewModelName is nil so the canvas draws every
            // model in the active layout group, not single-model
            // mode. selectedModelName drives Fit Selected.
            PreviewPaneView(previewName: "LayoutEditor",
                            previewModelName: nil,
                            controlsVisible: $controlsVisible,
                            settings: settings,
                            status: PreviewStatus())
                .background(Color.black)

            VStack(alignment: .trailing, spacing: 4) {
                Button {
                    controlsVisible.toggle()
                } label: {
                    Image(systemName: controlsVisible
                          ? "slider.horizontal.3"
                          : "slider.horizontal.below.rectangle")
                }
                .buttonStyle(.bordered)
                .controlSize(.small)

                if controlsVisible {
                    LayoutEditorCanvasControls(previewName: "LayoutEditor",
                                               settings: settings,
                                               selectedModelName:
                                                viewModel.layoutEditorSelectedModel)
                }
            }
            .padding(8)
        }
    }

    // MARK: - Refresh

    private func refresh() {
        layoutGroups = viewModel.document.layoutGroups()
        activeLayoutGroup = viewModel.document.activeLayoutGroup()
        if viewModel.isShowFolderLoaded {
            settings.is3D = viewModel.document.layoutMode3D()
        }
        displayState = viewModel.document.layoutDisplayState()
        // Seed overlay toggles from the show's saved rgbeffects.xml
        // values on first refresh after the editor opens. Subsequent
        // refreshes don't clobber whatever the user has toggled
        // since.
        if !overlaysInitialized {
            settings.showLayoutGrid =
                (displayState["display2DGrid"] as? Bool) ?? false
            settings.showLayoutBoundingBox =
                (displayState["display2DBoundingBox"] as? Bool) ?? false
            overlaysInitialized = true
        }
        refreshModelList()
    }

    private func refreshModelList() {
        modelNames = viewModel.document.modelsInActiveLayoutGroup()
        // If the previously-selected model isn't in the new list,
        // clear selection so the side panel doesn't show stale data.
        if let sel = viewModel.layoutEditorSelectedModel,
           !modelNames.contains(sel) {
            viewModel.layoutEditorSelectedModel = nil
        }
    }

    private func commitProperty(modelName: String, key: String, value: Any) {
        // Capture undo BEFORE the edit so the snapshot reflects the
        // pre-edit state. Pushed unconditionally — even no-op edits
        // leave a stale entry, which we accept as cheap (the user
        // can just hit Undo twice).
        viewModel.document.pushLayoutUndoSnapshot(forModel: modelName)
        let changed = viewModel.document.setLayoutModelProperty(modelName,
                                                                key: key,
                                                                value: value)
        if changed {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            // layoutGroup edits change which models are filtered into
            // the active group; controllerName edits affect the
            // displayed channel range / strings via Reinitialize. Both
            // mean the sidebar list / summary need to repaint.
            if key == "layoutGroup" || key == "controllerName" {
                refreshModelList()
            }
        }
    }

    /// Arrow-key nudge: +1 unit per tap, +10 with shift. dx/dy are
    /// signed direction (-1, 0, +1); the magnitude is decided here.
    /// Returns `.handled` only when there's a selected model so the
    /// arrow key still scrolls the sidebar list when no model is
    /// active. Each tap creates one undo entry.
    private func nudge(_ dxSign: Float, _ dySign: Float, _ keyPress: KeyPress) -> KeyPress.Result {
        guard let name = viewModel.layoutEditorSelectedModel else { return .ignored }
        guard let summary = viewModel.document.modelLayoutSummary(name) else { return .ignored }
        let big = keyPress.modifiers.contains(.shift)
        let step: Float = big ? 10.0 : 1.0
        let dx = dxSign * step
        let dy = dySign * step

        viewModel.document.pushLayoutUndoSnapshot(forModel: name)

        if dx != 0 {
            let cur = (summary["centerX"] as? NSNumber)?.floatValue ?? 0
            _ = viewModel.document.setLayoutModelProperty(name,
                                                          key: "centerX",
                                                          value: NSNumber(value: cur + dx))
        }
        if dy != 0 {
            let cur = (summary["centerY"] as? NSNumber)?.floatValue ?? 0
            _ = viewModel.document.setLayoutModelProperty(name,
                                                          key: "centerY",
                                                          value: NSNumber(value: cur + dy))
        }

        summaryToken &+= 1
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        canUndo = viewModel.document.canUndoLayoutChange()
        // Repaint the canvas — the bridge state changed but updateUIView
        // doesn't notice without a setNeedsDisplay nudge.
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                        object: "LayoutEditor",
                                        userInfo: ["model": name])
        return .handled
    }

    private func performUndo() {
        guard viewModel.document.undoLastLayoutChange() else { return }
        summaryToken &+= 1
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        canUndo = viewModel.document.canUndoLayoutChange()
        // The undo may have moved a model out of the active group
        // (or back into it) — refresh the list so the sidebar
        // reflects post-undo state.
        refreshModelList()
        // Force a canvas repaint by posting our model-moved
        // notification; the bridge will see the bumped model state
        // on the next draw.
        if let sel = viewModel.layoutEditorSelectedModel {
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                            object: "LayoutEditor",
                                            userInfo: ["model": sel])
        }
    }

    private func saveLayoutChanges() {
        let ok = viewModel.document.saveLayoutChanges()
        if ok {
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        } else {
            saveErrorMessage = "Couldn't write xlights_rgbeffects.xml. Check the Tools → Package Logs output for details."
        }
    }

    /// J-2 — roll back every staged in-memory layout edit by
    /// repeatedly popping the undo stack. Walks until empty (or
    /// until 200 iterations as a safety bound). The dirty set
    /// stays populated — `MarkLayoutModelDirty` was called by each
    /// undo restore — but it's now redundant since the in-memory
    /// state matches what was on disk before the edit session.
    /// Caller refresh repaints the canvas + sidebar summary.
    private func discardChanges() {
        var iterations = 0
        while viewModel.document.canUndoLayoutChange() && iterations < 200 {
            _ = viewModel.document.undoLastLayoutChange()
            iterations += 1
        }
        // Undo's restore path re-marked every reverted model
        // dirty even though the in-memory state now matches what's
        // on disk. Drop that bookkeeping so the Save button
        // disables cleanly.
        viewModel.document.clearDirtyLayoutChanges()
        summaryToken &+= 1
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        canUndo = viewModel.document.canUndoLayoutChange()
        refreshModelList()
        if let sel = viewModel.layoutEditorSelectedModel {
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                            object: "LayoutEditor",
                                            userInfo: ["model": sel])
        }
    }
}

/// Editable properties panel for the selected model. Common-
/// properties surface only (J-1) — per-type properties (number of
/// arches, tree branches, custom-model matrix, DMX channel mapping)
/// land in J-3.
///
/// Each editable cell pushes its value through `commit(key, value)`
/// the moment it loses focus / commits, which routes through
/// `setLayoutModelProperty` and bumps the parent's `summaryToken`
/// so this view re-binds against the fresh summary. The on-disk
/// rgbeffects.xml is *not* updated until the parent calls
/// `saveLayoutChanges` (Save button in toolbar).
private struct LayoutEditorPropertiesView: View {
    let modelName: String
    let summary: [String: Any]
    let layoutGroups: [String]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("Name") { Text(modelName).truncationMode(.middle) }
            row("Type") { Text(summary["displayAs"] as? String ?? "—") }
            row("Layout group") { layoutGroupPicker }
            row("Locked") {
                Toggle("", isOn: lockedBinding)
                    .labelsHidden()
                    .controlSize(.mini)
            }

            Divider().padding(.vertical, 2)

            row("Centre X") { numberField(key: "centerX") }
            row("Centre Y") { numberField(key: "centerY") }
            row("Centre Z") { numberField(key: "centerZ") }

            Divider().padding(.vertical, 2)

            row("Width")  { numberField(key: "width",  min: 0) }
            row("Height") { numberField(key: "height", min: 0) }
            row("Depth")  { numberField(key: "depth",  min: 0) }

            Divider().padding(.vertical, 2)

            row("Rotate X") { numberField(key: "rotateX") }
            row("Rotate Y") { numberField(key: "rotateY") }
            row("Rotate Z") { numberField(key: "rotateZ") }

            Divider().padding(.vertical, 2)

            row("Controller") { controllerField }
            row("Channel range") {
                Text("\(uintVal("startChannel"))..\(uintVal("endChannel"))")
                    .foregroundStyle(.secondary)
            }
            row("Strings") {
                Text("\(intVal("stringCount"))").foregroundStyle(.secondary)
            }
            row("Nodes") {
                Text("\(uintVal("nodeCount"))").foregroundStyle(.secondary)
            }
        }
        .font(.caption.monospacedDigit())
    }

    // MARK: - Cells

    @ViewBuilder
    private func row(_ label: String, @ViewBuilder _ content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 90, alignment: .leading)
            Spacer(minLength: 8)
            content()
                .lineLimit(1)
        }
    }

    private func numberField(key: String, min: Double? = nil) -> some View {
        LayoutEditorDoubleField(
            // Reset edits when token bumps (after a commit elsewhere)
            // so the field re-reads the bridge value.
            id: "\(modelName).\(key).\(token)",
            initial: doubleVal(key),
            min: min,
            commit: { newValue in commit(key, NSNumber(value: newValue)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    private var layoutGroupPicker: some View {
        Menu {
            ForEach(layoutGroups, id: \.self) { name in
                Button {
                    commit("layoutGroup", name as NSString)
                } label: {
                    HStack {
                        Text(name)
                        if name == (summary["layoutGroup"] as? String) {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Text(summary["layoutGroup"] as? String ?? "Default")
                .truncationMode(.middle)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    private var lockedBinding: Binding<Bool> {
        Binding(
            get: { summary["locked"] as? Bool ?? false },
            set: { commit("locked", NSNumber(value: $0)) }
        )
    }

    private var controllerField: some View {
        LayoutEditorStringField(
            id: "\(modelName).controllerName.\(token)",
            initial: summary["controllerName"] as? String ?? "",
            commit: { commit("controllerName", $0 as NSString) }
        )
        .frame(maxWidth: 140, alignment: .trailing)
    }

    // MARK: - Lookups

    private func doubleVal(_ key: String) -> Double {
        (summary[key] as? NSNumber)?.doubleValue ?? 0.0
    }

    private func intVal(_ key: String) -> Int {
        (summary[key] as? NSNumber)?.intValue ?? 0
    }

    private func uintVal(_ key: String) -> UInt64 {
        (summary[key] as? NSNumber)?.uint64Value ?? 0
    }
}

/// Inline double-typed text field. Re-creates state from the
/// `initial` value whenever `id` changes (so a token bump elsewhere
/// repaints us) but otherwise lets the user keep typing without
/// fighting an outside writer.
private struct LayoutEditorDoubleField: View {
    let id: String
    let initial: Double
    let min: Double?
    let commit: (Double) -> Void

    @State private var draft: String = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $draft)
            .multilineTextAlignment(.trailing)
            .keyboardType(.numbersAndPunctuation)
            .textFieldStyle(.roundedBorder)
            .focused($focused)
            .id(id)
            .onAppear { draft = format(initial) }
            .onChange(of: id) { _, _ in
                if !focused { draft = format(initial) }
            }
            .onChange(of: focused) { _, nowFocused in
                if !nowFocused { commitDraft() }
            }
            .onSubmit { commitDraft() }
    }

    private func commitDraft() {
        let trimmed = draft.trimmingCharacters(in: .whitespaces)
        guard let parsed = Double(trimmed) else {
            draft = format(initial)
            return
        }
        var v = parsed
        if let lo = min, v < lo { v = lo }
        commit(v)
    }

    private func format(_ v: Double) -> String {
        // 2 decimals matches desktop's wxPropertyGrid display
        // precision for layout properties.
        String(format: "%.2f", v)
    }
}

/// Inline string field for controller name, etc. Mirrors
/// `LayoutEditorDoubleField` but with no parse / clamp.
private struct LayoutEditorStringField: View {
    let id: String
    let initial: String
    let commit: (String) -> Void

    @State private var draft: String = ""
    @FocusState private var focused: Bool

    var body: some View {
        TextField("", text: $draft)
            .multilineTextAlignment(.trailing)
            .textFieldStyle(.roundedBorder)
            .focused($focused)
            .id(id)
            .onAppear { draft = initial }
            .onChange(of: id) { _, _ in
                if !focused { draft = initial }
            }
            .onChange(of: focused) { _, nowFocused in
                if !nowFocused { commit(draft) }
            }
            .onSubmit { commit(draft) }
    }
}

/// Canvas-side toggles: 2D/3D, view-objects, fit-all/fit-selected,
/// camera reset. Subset of `PreviewControlsOverlay` — the Layout
/// Editor doesn't need viewpoints, save-image, or detach (no
/// embedded counterpart to detach from).
private struct LayoutEditorCanvasControls: View {
    let previewName: String
    @Bindable var settings: PreviewSettings
    let selectedModelName: String?

    var body: some View {
        VStack(alignment: .trailing, spacing: 4) {
            HStack(spacing: 4) {
                Button { post(.zoomOut) }   label: { Image(systemName: "minus.magnifyingglass") }
                Button { post(.zoomReset) } label: { Text("1×").font(.caption.monospacedDigit()) }
                Button { post(.zoomIn) }    label: { Image(systemName: "plus.magnifyingglass") }
                Button { post(.reset) }     label: { Image(systemName: "arrow.counterclockwise") }
            }
            .buttonStyle(.bordered)
            .controlSize(.small)

            HStack(spacing: 4) {
                Button {
                    NotificationCenter.default.post(name: .previewFitAll,
                                                    object: previewName)
                } label: {
                    Image(systemName: "arrow.up.left.and.arrow.down.right.rectangle")
                }
                Button {
                    guard let sel = selectedModelName, !sel.isEmpty else { return }
                    NotificationCenter.default.post(name: .previewFitModel,
                                                    object: previewName,
                                                    userInfo: ["name": sel])
                } label: {
                    Image(systemName: "viewfinder")
                }
                .disabled(selectedModelName?.isEmpty ?? true)
            }
            .buttonStyle(.bordered)
            .controlSize(.small)

            Picker("", selection: $settings.is3D) {
                Text("2D").tag(false)
                Text("3D").tag(true)
            }
            .pickerStyle(.segmented)
            .frame(width: 96)

            Toggle(isOn: $settings.showViewObjects) {
                Text("View Objs").font(.caption2)
            }
            .toggleStyle(.button)
            .controlSize(.small)

            // J-2 — overlay toggles. Hidden in 3D mode (overlays
            // currently 2D-only). Tinted "on" state matches the
            // sequencer's lightbulb output toggle for consistency.
            if !settings.is3D {
                Toggle(isOn: $settings.showLayoutGrid) {
                    Text("Grid").font(.caption2)
                }
                .toggleStyle(.button)
                .controlSize(.small)

                Toggle(isOn: $settings.showLayoutBoundingBox) {
                    Text("Bounds").font(.caption2)
                }
                .toggleStyle(.button)
                .controlSize(.small)

                Toggle(isOn: $settings.snapToGrid) {
                    Text("Snap").font(.caption2)
                }
                .toggleStyle(.button)
                .controlSize(.small)
            }

            // First-pixel highlight applies in 2D and 3D — keep
            // outside the 2D-only block above.
            Toggle(isOn: $settings.showFirstPixel) {
                Text("Pixel 1").font(.caption2)
            }
            .toggleStyle(.button)
            .controlSize(.small)
        }
    }

    private enum Action {
        case zoomIn, zoomOut, zoomReset, reset
    }

    private func post(_ action: Action) {
        let name: Notification.Name
        switch action {
        case .zoomIn:    name = .previewZoomIn
        case .zoomOut:   name = .previewZoomOut
        case .zoomReset: name = .previewZoomReset
        case .reset:     name = .previewResetCamera
        }
        NotificationCenter.default.post(name: name, object: previewName)
    }
}

/// Scene root for the standalone Layout Editor window. Mirrors the
/// `DetachedHousePreviewRoot` pattern: a token check guards against
/// iPadOS auto-restoring this scene on launch (we want the user to
/// reopen it explicitly via the Tools menu, not have it pop up
/// behind the main sequencer window).
struct LayoutEditorWindowRoot: View {
    @Environment(SequencerViewModel.self) var viewModel
    @Environment(\.dismissWindow) private var dismissWindow
    @State private var suppressed: Bool = false

    var body: some View {
        Group {
            if suppressed {
                Color.black
            } else {
                LayoutEditorView()
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .frame(minWidth: 480, minHeight: 360)
        .navigationTitle("Edit Layout")
        .onAppear {
            if viewModel.pendingDetachTokens.remove("layout-editor") != nil {
                viewModel.layoutEditorOpen = true
            } else {
                suppressed = true
                DispatchQueue.main.async {
                    dismissWindow(id: "layout-editor")
                }
            }
        }
        .onDisappear {
            if !suppressed { viewModel.layoutEditorOpen = false }
        }
    }
}
