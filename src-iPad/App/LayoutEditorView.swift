import SwiftUI
import UniformTypeIdentifiers

/// J-5 (sidebar tabs) — Layout Editor left-pane roster picker.
/// Controllers tab is intentionally omitted in J-5; iPad doesn't
/// yet have a controller editor and surfacing the tab with no UI
/// behind it would be a regression in clarity.
enum LayoutSidebarTab: String, CaseIterable, Identifiable {
    case models, groups, objects
    var id: String { rawValue }
    var label: String {
        switch self {
        case .models: return "Models"
        case .groups: return "Groups"
        case .objects: return "Objects"
        }
    }
    var systemImage: String {
        switch self {
        case .models: return "cube"
        case .groups: return "square.stack.3d.up"
        case .objects: return "scribble.variable"
        }
    }
}

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
    /// Phase J-2 (touch UX) — long-press contextual menu target.
    /// Set by the `.layoutEditorContextMenu` notification; cleared
    /// when the user picks an item or dismisses. The
    /// `.confirmationDialog` shows the appropriate item set.
    @State private var contextMenuTarget: LayoutContextMenuTarget? = nil
    /// Phase J-3 (touch UX) — cached list of model-type strings
    /// the Add-Model picker shows. Read from the bridge at
    /// refresh time so changes to the curated list don't require
    /// a rebuild.
    @State private var availableModelTypes: [String] = []
    /// J-3 (touch UX) — drives the Add-Model sheet. Using a sheet
    /// instead of an inline Menu avoids whatever launch-time issue
    /// the SwiftUI Menu in the canvas overlay triggers.
    @State private var addModelSheetVisible: Bool = false
    /// J-3 (touch UX) — pending delete confirmation. Set when the
    /// user taps the trash icon in the inline action bar; cleared
    /// after the alert resolves either way.
    @State private var pendingDeleteModelName: String? = nil
    /// J-4 (import) — drives the .xmodel file picker.
    @State private var importerVisible: Bool = false
    /// J-4 (import) — set when a fresh import fails (file unreadable,
    /// XML parse failure, unknown model type). Surfaced as an alert.
    @State private var importErrorMessage: String? = nil
    /// J-4 (download) — drives the vendor catalog browser sheet.
    @State private var downloadBrowserVisible: Bool = false

    /// J-5 (sidebar tabs) — which roster the top half of the sidebar
    /// shows. Each tab keeps its own selection so flipping tabs to
    /// inspect a group or object doesn't lose the model selection
    /// driving the canvas.
    @State private var sidebarTab: LayoutSidebarTab = .models
    /// J-5 — fraction of the sidebar's height occupied by the top
    /// roster (list). The bottom (property pane) takes the rest.
    /// Persists per-session via @State; clamped to [0.2, 0.8] by the
    /// divider's drag handler so neither pane vanishes.
    @State private var sidebarTopFraction: CGFloat = 0.45
    /// J-5 — Groups roster. The selection itself lives on the
    /// view model (`layoutEditorSelectedGroup`) so PreviewPaneView
    /// can sync it to the canvas tint.
    @State private var groupNames: [String] = []
    /// J-5 — ViewObjects roster. Selection on the view model so
    /// the canvas picks up handles.
    @State private var objectNames: [String] = []
    /// J-5 — search text per tab. Reset on editor close.
    @State private var modelFilter: String = ""
    @State private var groupFilter: String = ""
    @State private var objectFilter: String = ""

    /// J-7 (group CRUD) — sheet visibility flags + targets.
    @State private var newGroupSheetVisible: Bool = false
    @State private var addMemberSheetVisible: Bool = false
    @State private var pendingDeleteGroupName: String? = nil

    /// J-7 (multi-select) — when true, the next NewGroupSheet
    /// "Create" passes the current selection as the initial member
    /// list instead of creating an empty group. Cleared after use.
    @State private var pendingGroupFromSelection: [String]? = nil

    /// J-8 — file picker for the 2D Background image.
    @State private var backgroundImagePickerVisible: Bool = false

    /// J-12 — generic view-object file picker (Mesh `.obj`,
    /// Image bitmap, Terrain image). `pendingObjectFilePickKey`
    /// tracks which property key to write on completion;
    /// `objectFilePickerTypes` controls the allowed UTTypes.
    @State private var objectFilePickerVisible: Bool = false
    @State private var objectFilePickerTypes: [UTType] = []
    @State private var pendingObjectFilePickKey: String? = nil

    /// J-12 — Add View Object sheet visibility.
    @State private var addViewObjectSheetVisible: Bool = false
    /// J-12 — pending delete confirmation for a view object.
    @State private var pendingDeleteObjectName: String? = nil

    /// J-4 (import) — UTTypes the .fileImporter accepts. Declared
    /// as a static so the SwiftUI type-checker can resolve the
    /// `[UTType]` literal in reasonable time (the `?? .data` chain
    /// inline tripped its budget).
    private static let importableModelTypes: [UTType] = {
        ["xmodel", "gdtf", "lff", "lpf"].compactMap {
            UTType(filenameExtension: $0)
        }
    }()

    /// J-12 — Add-View-Object sheet → bridge create. Auto-
    /// selects the new object so the property pane opens on
    /// the right summary.
    private func handleCreateViewObject(_ type: String) {
        addViewObjectSheetVisible = false
        guard let name = viewModel.document.createViewObject(withType: type) else { return }
        viewModel.layoutEditorSelectedObject = name
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        refreshModelList()
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                         object: "LayoutEditor",
                                         userInfo: ["model": name])
    }

    /// J-12 — Delete-confirmation alert → bridge delete.
    private func handleDeleteViewObject(_ name: String) {
        guard viewModel.document.deleteViewObject(name) else { return }
        if viewModel.layoutEditorSelectedObject == name {
            viewModel.layoutEditorSelectedObject = nil
        }
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        refreshModelList()
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                         object: "LayoutEditor",
                                         userInfo: ["model": name])
    }

    /// J-12 — Per-type file picker completion. Routes the picked
    /// path back through commitObjectProperty under whatever key
    /// the property pane stashed in `pendingObjectFilePickKey`.
    private func handleObjectFilePick(_ result: Result<[URL], Error>) {
        let key = pendingObjectFilePickKey ?? ""
        pendingObjectFilePickKey = nil
        guard !key.isEmpty,
              let name = viewModel.layoutEditorSelectedObject else { return }
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            let granted = url.startAccessingSecurityScopedResource()
            let path = url.path
            _ = XLSequenceDocument.obtainAccess(toPath: path, enforceWritable: false)
            if granted { url.stopAccessingSecurityScopedResource() }
            commitObjectProperty(objectName: name, key: key, value: path as NSString)
        case .failure(let err):
            saveErrorMessage = err.localizedDescription
        }
    }

    /// J-8 — `.fileImporter` completion for the 2D Background
    /// image. Same security-scoped access dance as the model
    /// importer: start access, persist the bookmark via
    /// ObtainAccessToURL, stop access, then push the path
    /// through the regular object-property commit so the
    /// summaryToken bumps and the canvas repaints.
    private func handleBackgroundImagePick(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            let granted = url.startAccessingSecurityScopedResource()
            let path = url.path
            _ = XLSequenceDocument.obtainAccess(toPath: path, enforceWritable: false)
            if granted { url.stopAccessingSecurityScopedResource() }
            commitObjectProperty(objectName: "2D Background",
                                 key: "backgroundImage",
                                 value: path as NSString)
        case .failure(let err):
            saveErrorMessage = err.localizedDescription
        }
    }

    /// J-4 (import) — `.fileImporter` completion. Stashes the
    /// picked path for the next canvas-tap to consume.
    private func handleImportPick(_ result: Result<[URL], Error>) {
        switch result {
        case .success(let urls):
            guard let url = urls.first else { return }
            let granted = url.startAccessingSecurityScopedResource()
            let path = url.path
            _ = XLSequenceDocument.obtainAccess(toPath: path, enforceWritable: false)
            if granted { url.stopAccessingSecurityScopedResource() }
            viewModel.layoutPendingImportPath = path
        case .failure(let err):
            importErrorMessage = err.localizedDescription
        }
    }

    var body: some View {
        @Bindable var vm = viewModel
        NavigationSplitView {
            sidebar
                .navigationSplitViewColumnWidth(min: 240, ideal: 320, max: 420)
        } detail: {
            canvas
        }
        .navigationTitle("Edit Layout")
        // NavigationSplitView in this scene hides its column chrome,
        // so `.toolbar`'s `.primaryAction` slots never render.
        // Layout-group switcher / Undo / Save / Add Model / Select
        // all live in the canvas overlay (top-left + top-right) so
        // they're actually reachable.
        .onAppear { refresh() }
        .onDisappear {
            // Drop any half-started Add-Model so the editor opens
            // fresh next time rather than re-entering creation mode
            // on a stale type.
            viewModel.layoutPendingNewModelType = nil
            viewModel.layoutPolylineInProgress = nil
            viewModel.layoutPendingImportPath = nil
            // J-4 (multi-select) — exit edit mode and collapse to
            // the primary so the next open starts in a known
            // single-select state.
            viewModel.layoutEditMode = false
            if let primary = viewModel.layoutEditorSelectedModel {
                viewModel.layoutEditorSelection = [primary]
            } else {
                viewModel.layoutEditorSelection.removeAll()
            }
            // J-6 — clear group / object picks so a re-open starts
            // clean (matches model-selection lifecycle).
            viewModel.layoutEditorSelectedGroup = nil
            viewModel.layoutEditorSelectedObject = nil
            // J-13 — exit any active terrain edit session so the
            // next open doesn't paint heightmap data on the first
            // tap.
            viewModel.terrainEditTarget = nil
        }
        .onChange(of: viewModel.isShowFolderLoaded) { _, _ in refresh() }
        .onChange(of: activeLayoutGroup) { _, newValue in
            viewModel.document.setActiveLayoutGroup(newValue)
            refreshModelList()
        }
        .onChange(of: viewModel.layoutEditorSelectedModel) { _, newSelection in
            // Seed the toolbar tool from the newly-selected
            // model's axis_tool so switching models doesn't leak
            // the previous selection's mode.
            if let name = newSelection, !name.isEmpty {
                let current = viewModel.document.axisTool(forModel: name)
                if current != "none" {
                    settings.axisTool = current
                }
            }
        }
        .modifier(SidebarSelectionMutex(sidebarTab: $sidebarTab))
        .focusable()
        // J-2 — keyboard nudge for the selected model. Arrow keys
        // move 1 unit, shift+arrow moves 10. Pushed undo per nudge
        // so each tap is independently reversible.
        .onKeyPress(.upArrow,    phases: .down) { keyPress in nudge(0,  +1, keyPress) }
        .onKeyPress(.downArrow,  phases: .down) { keyPress in nudge(0,  -1, keyPress) }
        .onKeyPress(.leftArrow,  phases: .down) { keyPress in nudge(-1,  0, keyPress) }
        .onKeyPress(.rightArrow, phases: .down) { keyPress in nudge(+1,  0, keyPress) }
        // J-3 (touch UX) — Esc/Return end mid-polyline creation
        // (mirrors desktop's polyline create commit hot-keys).
        // Also drops fresh-model placement mode if armed.
        .onKeyPress(.escape, phases: .down) { _ in endCreationModes() }
        .onKeyPress(.return, phases: .down) { _ in endCreationModes() }
        .onReceive(NotificationCenter.default.publisher(for: .layoutEditorModelMoved)) { note in
            // Drag-to-move on the canvas (or a keyboard nudge / undo)
            // mutates the bridge directly; refresh the summary +
            // dirty-state so the side panel reflects the new centre
            // and the Save button enables. Convention: object =
            // previewName ("LayoutEditor"), userInfo["model"] = name.
            let movedName = note.userInfo?["model"] as? String
            // Add Model creates a model the side panel hasn't seen
            // yet — refresh the model list so the new name appears.
            if let name = movedName, !modelNames.contains(name) {
                refreshModelList()
            }
            guard movedName == viewModel.layoutEditorSelectedModel else { return }
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
        }
        .onReceive(NotificationCenter.default.publisher(for: .layoutEditorContextMenu)) { note in
            // Long-press on a handle. The bridge has already done the
            // hit-test and packaged the result. Translate to our
            // typed target and let the confirmationDialog open.
            guard let info = note.userInfo,
                  let type = info["type"] as? String,
                  let modelName = info["modelName"] as? String else { return }
            switch type {
            case "vertex":
                if let idx = (info["vertexIndex"] as? NSNumber)?.intValue {
                    contextMenuTarget = .vertex(modelName: modelName, vertexIndex: idx)
                }
            case "segment":
                if let idx = (info["segmentIndex"] as? NSNumber)?.intValue {
                    let curve = (info["hasCurve"] as? NSNumber)?.boolValue ?? false
                    contextMenuTarget = .segment(modelName: modelName,
                                                  segmentIndex: idx,
                                                  hasCurve: curve)
                }
            case "curve_control":
                if let idx = (info["segmentIndex"] as? NSNumber)?.intValue {
                    contextMenuTarget = .curveControl(modelName: modelName,
                                                       segmentIndex: idx)
                }
            default:
                break
            }
        }
        // J-3 (touch UX) — Pencil Pro squeeze maps to layout undo.
        // Posted by PreviewPaneView's UIPencilInteraction; same
        // entry point as the toolbar's Undo button so undo state /
        // dirty markers / canvas repaint stay consistent.
        .onReceive(NotificationCenter.default.publisher(for: .layoutEditorPencilUndo)) { _ in
            if canUndo { performUndo() }
        }
        .confirmationDialog(contextMenuTarget?.title ?? "",
                            isPresented: Binding(
                                get: { contextMenuTarget != nil },
                                set: { if !$0 { contextMenuTarget = nil } }),
                            titleVisibility: .visible) {
            contextMenuButtons
        }
        .sheet(isPresented: $addModelSheetVisible) {
            AddModelSheet(types: availableModelTypes,
                           labelFor: modelTypeLabel) { type in
                viewModel.layoutPendingNewModelType = type
                addModelSheetVisible = false
            }
        }
        // J-4 (download) — vendor catalog browser. On download
        // we reuse the same `layoutPendingImportPath` path Import
        // uses, so the user gets the familiar "tap canvas to
        // place" banner regardless of where the file came from.
        .sheet(isPresented: $downloadBrowserVisible) {
            VendorBrowserSheet(onDownloaded: { path in
                viewModel.layoutPendingImportPath = path
                downloadBrowserVisible = false
            })
        }
        .modifier(GroupCrudModifiers(
            newGroupSheetVisible: $newGroupSheetVisible,
            addMemberSheetVisible: $addMemberSheetVisible,
            pendingDeleteGroupName: $pendingDeleteGroupName,
            groupNames: groupNames,
            modelNames: modelNames,
            onCreateGroup: handleCreateGroup,
            onAddMembers: handleAddMembers,
            onDeleteGroup: handleDeleteGroup,
            selectedGroupName: viewModel.layoutEditorSelectedGroup,
            currentMembers: selectedGroupMembers,
            submodelsFor: { parent in
                viewModel.document.submodels(forModel: parent)
            }
        ))
        // J-4 (import) — .xmodel file picker. iPadOS's
        // `.fileImporter` is the SwiftUI-native wrapping of
        // UIDocumentPickerViewController; the resulting URL has
        // security-scoped access already, so we call
        // `ObtainAccessToURL` to persist the bookmark and stash
        // the path for the next canvas-tap placement.
        .fileImporter(isPresented: $importerVisible,
                      allowedContentTypes: Self.importableModelTypes,
                      allowsMultipleSelection: false,
                      onCompletion: handleImportPick)
        // J-8 — 2D Background image picker. Accepts common image
        // formats; the bridge calls `ObtainAccessToURL` so the
        // sandbox bookmark persists across launches.
        .fileImporter(isPresented: $backgroundImagePickerVisible,
                      allowedContentTypes: [.png, .jpeg, .tiff, .bmp, .gif, .image],
                      allowsMultipleSelection: false,
                      onCompletion: handleBackgroundImagePick)
        .modifier(ViewObjectCrudModifiers(
            objectFilePickerVisible: $objectFilePickerVisible,
            objectFilePickerTypes: objectFilePickerTypes,
            addViewObjectSheetVisible: $addViewObjectSheetVisible,
            pendingDeleteObjectName: $pendingDeleteObjectName,
            availableTypes: viewModel.document.availableViewObjectTypes(),
            onCreateObject: handleCreateViewObject,
            onDeleteObject: handleDeleteViewObject,
            onFilePicked: handleObjectFilePick
        ))
        .alert("Import failed",
               isPresented: Binding(get: { importErrorMessage != nil },
                                    set: { if !$0 { importErrorMessage = nil } })) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(importErrorMessage ?? "")
        }
        .alert("Save failed",
               isPresented: Binding(get: { saveErrorMessage != nil },
                                    set: { if !$0 { saveErrorMessage = nil } })) {
            Button("OK", role: .cancel) { }
        } message: {
            Text(saveErrorMessage ?? "")
        }
        // J-3 (touch UX) — delete-model confirmation. Triggered by
        // the trash icon in the inline action bar. The delete is
        // an in-memory mutation through the bridge; user must still
        // hit Save to persist (matches every other layout edit).
        .alert("Delete \(pendingDeleteModelName ?? "")?",
               isPresented: Binding(get: { pendingDeleteModelName != nil },
                                    set: { if !$0 { pendingDeleteModelName = nil } })) {
            Button("Delete", role: .destructive) {
                if let name = pendingDeleteModelName {
                    deleteModel(name: name)
                }
            }
            Button("Cancel", role: .cancel) { }
        } message: {
            Text("Removes this model from the current layout. Save the layout to make the change permanent; Undo or Discard will roll it back.")
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

    // MARK: - Sidebar (J-5: vertical split, tabbed roster + property pane)

    @ViewBuilder
    private var sidebar: some View {
        GeometryReader { geo in
            let total = geo.size.height
            let topHeight = max(120, min(total - 160, total * sidebarTopFraction))
            VStack(spacing: 0) {
                rosterPane
                    .frame(height: topHeight)
                sidebarDivider(totalHeight: total)
                propertyPane
                    .frame(maxHeight: .infinity)
            }
        }
    }

    /// Top half: tab picker + search + filtered list. The list's
    /// selection is routed back to the right `@State` via
    /// `rosterSelectionBinding` so each tab keeps its own.
    @ViewBuilder
    private var rosterPane: some View {
        VStack(spacing: 0) {
            Picker("", selection: $sidebarTab) {
                ForEach(LayoutSidebarTab.allCases) { tab in
                    Label(tab.label, systemImage: tab.systemImage).tag(tab)
                }
            }
            .pickerStyle(.segmented)
            .padding(.horizontal, 8)
            .padding(.top, 6)
            .padding(.bottom, 4)

            // Per-tab search field. Trimming + case-insensitive
            // contains is enough for the largest realistic show
            // (~500 models); switch to substring index if it ever
            // turns into a bottleneck.
            HStack(spacing: 6) {
                Image(systemName: "magnifyingglass")
                    .foregroundStyle(.secondary)
                    .font(.caption)
                TextField("Filter", text: currentFilterBinding)
                    .textFieldStyle(.plain)
                    .font(.caption)
                    .autocorrectionDisabled()
                    .textInputAutocapitalization(.never)
                if !currentFilterBinding.wrappedValue.isEmpty {
                    Button {
                        currentFilterBinding.wrappedValue = ""
                    } label: {
                        Image(systemName: "xmark.circle.fill")
                            .foregroundStyle(.secondary)
                    }
                    .buttonStyle(.plain)
                }
            }
            .padding(.horizontal, 10)
            .padding(.vertical, 4)
            .background(.quaternary, in: RoundedRectangle(cornerRadius: 6, style: .continuous))
            .padding(.horizontal, 8)
            .padding(.bottom, 4)

            rosterList
        }
    }

    @ViewBuilder
    private var rosterList: some View {
        switch sidebarTab {
        case .models:
            List(selection: modelListBinding) {
                Section("\(activeLayoutGroup) (\(filteredModelNames.count)\(modelNames.count != filteredModelNames.count ? " of \(modelNames.count)" : ""))") {
                    ForEach(filteredModelNames, id: \.self) { name in
                        Text(name)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .tag(name)
                    }
                }
            }
            .listStyle(.sidebar)
            .overlay {
                if modelNames.isEmpty {
                    ContentUnavailableView("No models",
                                            systemImage: "cube",
                                            description: Text("This layout group has no models. Tap + on the canvas to add one."))
                } else if filteredModelNames.isEmpty {
                    ContentUnavailableView.search(text: modelFilter)
                }
            }
        case .groups:
            List(selection: groupListBinding) {
                Section {
                    ForEach(filteredGroupNames, id: \.self) { name in
                        Text(name)
                            .lineLimit(1)
                            .truncationMode(.middle)
                            .tag(name)
                            .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                Button(role: .destructive) {
                                    pendingDeleteGroupName = name
                                } label: {
                                    Label("Delete", systemImage: "trash")
                                }
                            }
                    }
                } header: {
                    HStack(spacing: 6) {
                        Text("Groups (\(filteredGroupNames.count)\(groupNames.count != filteredGroupNames.count ? " of \(groupNames.count)" : ""))")
                        Spacer()
                        Button {
                            newGroupSheetVisible = true
                        } label: {
                            Image(systemName: "plus.circle.fill")
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("New group")
                    }
                }
            }
            .listStyle(.sidebar)
            .overlay {
                if groupNames.isEmpty {
                    ContentUnavailableView {
                        Label("No groups", systemImage: "square.stack.3d.up")
                    } description: {
                        Text("Model groups assigned to \(activeLayoutGroup) or All Previews will appear here.")
                    } actions: {
                        Button("Create a Group") {
                            newGroupSheetVisible = true
                        }
                        .buttonStyle(.borderedProminent)
                    }
                } else if filteredGroupNames.isEmpty {
                    ContentUnavailableView.search(text: groupFilter)
                }
            }
        case .objects:
            List(selection: objectListBinding) {
                Section {
                    ForEach(filteredObjectNames, id: \.self) { name in
                        // 2D Background is a synthetic pseudo-object;
                        // skip the delete swipe so the user can't try
                        // to remove it.
                        if name == "2D Background" {
                            Text(name)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .tag(name)
                        } else {
                            Text(name)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .tag(name)
                                .swipeActions(edge: .trailing, allowsFullSwipe: false) {
                                    Button(role: .destructive) {
                                        pendingDeleteObjectName = name
                                    } label: {
                                        Label("Delete", systemImage: "trash")
                                    }
                                }
                        }
                    }
                } header: {
                    HStack(spacing: 6) {
                        Text("Objects (\(filteredObjectNames.count)\(objectNames.count != filteredObjectNames.count ? " of \(objectNames.count)" : ""))")
                        Spacer()
                        Button {
                            addViewObjectSheetVisible = true
                        } label: {
                            Image(systemName: "plus.circle.fill")
                        }
                        .buttonStyle(.plain)
                        .accessibilityLabel("New view object")
                    }
                }
            }
            .listStyle(.sidebar)
            .overlay {
                // Note: 2D Background pseudo-object always returns
                // a non-empty array, so the empty-state path here
                // would only fire if the list were truly empty —
                // which shouldn't happen unless the show isn't
                // loaded.
                if filteredObjectNames.isEmpty && objectNames.isEmpty {
                    ContentUnavailableView {
                        Label("No view objects", systemImage: "scribble.variable")
                    } description: {
                        Text("Meshes, images, gridlines, terrain, and rulers in this preview will appear here.")
                    } actions: {
                        Button("Add a View Object") {
                            addViewObjectSheetVisible = true
                        }
                        .buttonStyle(.borderedProminent)
                    }
                } else if filteredObjectNames.isEmpty {
                    ContentUnavailableView.search(text: objectFilter)
                }
            }
        }
    }

    /// Bottom half: scrollable property editor bound to the
    /// current tab's selection. Empty-state hint when nothing is
    /// picked. Each tab's selection is independent — switching tabs
    /// shows the *other* tab's selection's properties.
    @ViewBuilder
    private var propertyPane: some View {
        VStack(alignment: .leading, spacing: 0) {
            propertyPaneHeader
            Divider()
            ScrollView {
                propertyPaneBody
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 10)
                    .padding(.vertical, 8)
            }
        }
        .background(Color(uiColor: .secondarySystemBackground))
    }

    @ViewBuilder
    private var propertyPaneHeader: some View {
        HStack(spacing: 6) {
            Image(systemName: sidebarTab.systemImage)
                .foregroundStyle(.secondary)
                .font(.caption)
            Text(propertyPaneHeaderText)
                .font(.caption.weight(.semibold))
                .lineLimit(1)
                .truncationMode(.middle)
            Spacer()
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
    }

    private var propertyPaneHeaderText: String {
        switch sidebarTab {
        case .models:
            return viewModel.layoutEditorSelectedModel.map { "Model: \($0)" } ?? "No model selected"
        case .groups:
            return viewModel.layoutEditorSelectedGroup.map { "Group: \($0)" } ?? "No group selected"
        case .objects:
            return viewModel.layoutEditorSelectedObject.map { "Object: \($0)" } ?? "No object selected"
        }
    }

    @ViewBuilder
    private var propertyPaneBody: some View {
        switch sidebarTab {
        case .models:
            if let name = viewModel.layoutEditorSelectedModel,
               let summary = viewModel.document.modelLayoutSummary(name) {
                // ObjC bridges NSArray<NSDictionary*> as
                // `[[AnyHashable: Any]]` — coerce keys to String so
                // the descriptor view's `[[String: Any]]` matches.
                let rawDescriptors = viewModel.document.perTypeProperties(forModel: name)
                let descriptors: [[String: Any]] = rawDescriptors.compactMap { entry in
                    var out: [String: Any] = [:]
                    for (k, v) in entry {
                        if let key = k as? String { out[key] = v }
                    }
                    return out.isEmpty ? nil : out
                }
                LayoutEditorPropertiesView(
                    modelName: name,
                    summary: summary,
                    typeDescriptors: descriptors,
                    layoutGroups: layoutGroups,
                    token: summaryToken,
                    commit: { key, value in
                        commitProperty(modelName: name, key: key, value: value)
                    },
                    typeCommit: { key, value in
                        commitPerTypeProperty(modelName: name, key: key, value: value)
                    }
                )
                Divider().padding(.vertical, 8)
                displaySection
            } else {
                emptyPropertyHint(
                    title: "Pick a model",
                    body: "Tap a model in the list above to edit its position, size, rotation, layout group, and controller mapping."
                )
                Divider().padding(.vertical, 8)
                displaySection
            }
        case .groups:
            if let name = viewModel.layoutEditorSelectedGroup,
               let summary = viewModel.document.modelGroupLayoutSummary(name) {
                LayoutEditorGroupPropertiesView(
                    groupName: name,
                    summary: summary,
                    layoutGroups: layoutGroups,
                    token: summaryToken,
                    commit: { key, value in
                        commitGroupProperty(groupName: name, key: key, value: value)
                    },
                    onRemoveMember: { memberName in
                        if viewModel.document.removeModel(memberName, fromGroup: name) {
                            summaryToken &+= 1
                            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
                        }
                    },
                    onAddMember: { addMemberSheetVisible = true },
                    onReorderMembers: { newOrder in
                        commitGroupProperty(groupName: name,
                                            key: "members",
                                            value: newOrder as NSArray)
                    }
                )
            } else {
                emptyPropertyHint(
                    title: "Pick a group",
                    body: "Model groups bundle related models together. Tap a group above to inspect its members and edit its layout settings."
                )
            }
        case .objects:
            if let name = viewModel.layoutEditorSelectedObject,
               let summary = viewModel.document.viewObjectLayoutSummary(name) {
                if (summary["isBackground"] as? Bool) ?? false {
                    LayoutEditorBackgroundPropertiesView(
                        summary: summary,
                        token: summaryToken,
                        commit: { key, value in
                            commitObjectProperty(objectName: name, key: key, value: value)
                        },
                        onPickImage: { backgroundImagePickerVisible = true }
                    )
                } else {
                    LayoutEditorObjectPropertiesView(
                        objectName: name,
                        summary: summary,
                        layoutGroups: layoutGroups,
                        token: summaryToken,
                        commit: { key, value in
                            commitObjectProperty(objectName: name, key: key, value: value)
                        },
                        onPickFile: { key, types in
                            pendingObjectFilePickKey = key
                            objectFilePickerTypes = types
                            objectFilePickerVisible = true
                        }
                    )
                }
            } else {
                emptyPropertyHint(
                    title: "Pick a view object",
                    body: "View objects (meshes, images, gridlines, terrain, rulers) are decorative elements that don't take channels. Tap one to inspect its position and size."
                )
            }
        }
    }

    @ViewBuilder
    private func emptyPropertyHint(title: String, body: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            Text(body)
                .font(.caption)
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Display info — moved out of the roster list into the property
    /// pane's footer so it's always visible without scrolling.
    @ViewBuilder
    private var displaySection: some View {
        // J-8 — Background row moved to the Objects tab's
        // synthetic "2D Background" entry; rest of the display
        // properties (canvas size, default mode, grid, bbox) stay
        // here as a read-only roll-up.
        VStack(alignment: .leading, spacing: 4) {
            Text("Display")
                .font(.caption.weight(.semibold))
                .foregroundStyle(.secondary)
            displayRow("Canvas",
                       value: "\(displayState["previewWidth"] as? Int ?? 0) × \(displayState["previewHeight"] as? Int ?? 0)")
            displayRow("2D centre = 0",
                       value: (displayState["display2DCenter0"] as? Bool ?? false) ? "Yes" : "No")
            displayRow("Default mode",
                       value: (displayState["layoutMode3D"] as? Bool ?? true) ? "3D" : "2D")
            displayRow("Grid", value: gridLabel)
            displayRow("Bounding box",
                       value: (displayState["display2DBoundingBox"] as? Bool ?? false) ? "On" : "Off")
        }
        .font(.caption.monospacedDigit())
    }

    @ViewBuilder
    private func displayRow(_ label: String, value: String) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label).foregroundStyle(.secondary)
            Spacer(minLength: 8)
            Text(value).lineLimit(1).truncationMode(.middle)
        }
    }

    /// Draggable divider between the roster pane and the property
    /// pane. Vertical drag adjusts `sidebarTopFraction`. Clamps so
    /// neither pane shrinks below a usable threshold.
    @ViewBuilder
    private func sidebarDivider(totalHeight: CGFloat) -> some View {
        ZStack {
            Rectangle()
                .fill(Color(uiColor: .separator))
                .frame(height: 0.5)
            // Wider hit-target so the drag feels touchable even with
            // a hair-thin line. The capsule grip is purely cosmetic.
            Capsule()
                .fill(.secondary.opacity(0.4))
                .frame(width: 36, height: 4)
        }
        .frame(height: 16)
        .frame(maxWidth: .infinity)
        .contentShape(Rectangle())
        .gesture(
            DragGesture(minimumDistance: 0)
                .onChanged { value in
                    let proposed = sidebarTopFraction + value.translation.height / totalHeight
                    sidebarTopFraction = max(0.2, min(0.8, proposed))
                }
        )
    }

    // MARK: - Roster filtering

    private var filteredModelNames: [String] {
        let q = modelFilter.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return modelNames }
        return modelNames.filter { $0.localizedCaseInsensitiveContains(q) }
    }

    private var filteredGroupNames: [String] {
        let q = groupFilter.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return groupNames }
        return groupNames.filter { $0.localizedCaseInsensitiveContains(q) }
    }

    private var filteredObjectNames: [String] {
        let q = objectFilter.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return objectNames }
        return objectNames.filter { $0.localizedCaseInsensitiveContains(q) }
    }

    private var currentFilterBinding: Binding<String> {
        switch sidebarTab {
        case .models:  return $modelFilter
        case .groups:  return $groupFilter
        case .objects: return $objectFilter
        }
    }

    /// Models list uses the same SequencerViewModel selection slot
    /// the canvas reads from — so picking a model in the sidebar
    /// drives the canvas handles, action bar, and keyboard nudge.
    private var modelListBinding: Binding<String?> {
        Binding(
            get: { viewModel.layoutEditorSelectedModel },
            set: { viewModel.layoutSelectSingle($0) }
        )
    }

    /// J-6 — Group list binding writes to the view model so
    /// PreviewPaneView can sync the cyan-member tint to the canvas.
    private var groupListBinding: Binding<String?> {
        Binding(
            get: { viewModel.layoutEditorSelectedGroup },
            set: { viewModel.layoutEditorSelectedGroup = $0 }
        )
    }
    private var objectListBinding: Binding<String?> {
        Binding(
            get: { viewModel.layoutEditorSelectedObject },
            set: { viewModel.layoutEditorSelectedObject = $0 }
        )
    }

    private var gridLabel: String {
        let on = (displayState["display2DGrid"] as? Bool) ?? false
        let spacing = (displayState["display2DGridSpacing"] as? Int)
            ?? Int((displayState["display2DGridSpacing"] as? NSNumber)?.intValue ?? 100)
        return on ? "On (\(spacing))" : "Off"
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

            // J-4 — top-left overlay: document-state actions.
            // NavigationSplitView's primary-action toolbar items
            // never paint in this scene (the column chrome is
            // hidden), so the layout-group switcher / Undo / Save
            // would otherwise be unreachable. Same story for the
            // top-right "+", which has always been canvas-overlay.
            VStack {
                HStack(spacing: 6) {
                    if layoutGroups.count > 1 {
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
                                Text(activeLayoutGroup)
                                    .lineLimit(1)
                                    .frame(maxWidth: 140)
                                Image(systemName: "chevron.down")
                                    .font(.caption2)
                            }
                            .font(.caption)
                        }
                        .menuStyle(.borderlessButton)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 4)
                        .background(.regularMaterial, in: Capsule())
                    }

                    Button {
                        performUndo()
                    } label: {
                        Image(systemName: "arrow.uturn.backward")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .disabled(!canUndo)

                    Button {
                        showingSaveConfirm = true
                    } label: {
                        ZStack(alignment: .topTrailing) {
                            Image(systemName: "square.and.arrow.down")
                            if hasUnsavedChanges {
                                // Unsaved-changes dot.
                                Circle()
                                    .fill(.orange)
                                    .frame(width: 8, height: 8)
                                    .offset(x: 3, y: -3)
                            }
                        }
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .disabled(!hasUnsavedChanges)

                    Spacer()
                }
                .padding(8)
                Spacer()
            }

            VStack(alignment: .trailing, spacing: 4) {
                // J-3 (touch UX) — Add Model.
                Button {
                    addModelSheetVisible = true
                } label: {
                    Image(systemName: "plus.circle.fill")
                        .font(.title3)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!viewModel.isShowFolderLoaded || availableModelTypes.isEmpty)

                // J-4 (import) — Import .xmodel. Opens the iPadOS
                // file picker (iCloud Drive, Files, AirDrop receive
                // folder, etc.). Picked file flips the editor into
                // placement mode; the next canvas tap drops the
                // imported model at the touch point.
                Button {
                    importerVisible = true
                } label: {
                    Image(systemName: "square.and.arrow.down.on.square")
                        .font(.title3)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!viewModel.isShowFolderLoaded)

                // J-4 (download) — Download from vendor catalog.
                // Sheet fetches xlights.org's vendor list +
                // inventories through the shared core catalog
                // code, then lets the user pick a wiring. On
                // download, the local xmodel path flips us into
                // the same placement flow as Import.
                Button {
                    downloadBrowserVisible = true
                } label: {
                    Image(systemName: "icloud.and.arrow.down")
                        .font(.title3)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(!viewModel.isShowFolderLoaded)

                // J-4 (multi-select) — Photos-style Select toggle.
                Button {
                    let entering = !viewModel.layoutEditMode
                    viewModel.layoutEditMode = entering
                    if !entering, let primary = viewModel.layoutEditorSelectedModel {
                        viewModel.layoutEditorSelection = [primary]
                    } else if entering, viewModel.layoutEditorSelection.isEmpty,
                              let seed = viewModel.layoutEditorSelectedModel {
                        viewModel.layoutEditorSelection = [seed]
                    }
                } label: {
                    Image(systemName: viewModel.layoutEditMode
                                       ? "checkmark.circle.fill"
                                       : "checkmark.circle")
                        .font(.title3)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .tint(viewModel.layoutEditMode ? .accentColor : .secondary)
                .disabled(!viewModel.isShowFolderLoaded)

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
            .frame(maxWidth: .infinity, alignment: .trailing)
            .padding(8)

            // J-3 (touch UX) — creation-mode banner. Visible while
            // `layoutPendingNewModelType` is set (first-vertex tap)
            // OR `layoutPolylineInProgress` is set (mid-polyline
            // appending). The polyline branch swaps Cancel for Done
            // so the user can stop adding vertices.
            if let pendingType = viewModel.layoutPendingNewModelType {
                VStack {
                    HStack(spacing: 12) {
                        Image(systemName: "plus.circle.fill")
                            .foregroundStyle(.green)
                        Text("Tap canvas to place \(Text(modelTypeLabel(pendingType)).fontWeight(.semibold))")
                            .foregroundStyle(.white)
                        Button("Cancel") {
                            viewModel.layoutPendingNewModelType = nil
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.regularMaterial, in: Capsule())
                    .shadow(radius: 3, y: 2)
                    .padding(.top, 12)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
                .allowsHitTesting(true)
            } else if let polyName = viewModel.layoutPolylineInProgress {
                VStack {
                    HStack(spacing: 12) {
                        Image(systemName: "scribble.variable")
                            .foregroundStyle(.green)
                        Text("Tap to add vertex to \(Text(polyName).fontWeight(.semibold))")
                            .foregroundStyle(.white)
                        Button("Done") {
                            viewModel.layoutPolylineInProgress = nil
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.regularMaterial, in: Capsule())
                    .shadow(radius: 3, y: 2)
                    .padding(.top, 12)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
                .allowsHitTesting(true)
            } else if let importPath = viewModel.layoutPendingImportPath {
                // J-4 (import) — pending-import banner. The file
                // was picked; the next canvas tap drops the model
                // at the touch point.
                let fileName = (importPath as NSString).lastPathComponent
                VStack {
                    HStack(spacing: 12) {
                        Image(systemName: "square.and.arrow.down.on.square.fill")
                            .foregroundStyle(.green)
                        Text("Tap canvas to place \(Text(fileName).fontWeight(.semibold))")
                            .foregroundStyle(.white)
                        Button("Cancel") {
                            viewModel.layoutPendingImportPath = nil
                        }
                        .buttonStyle(.borderedProminent)
                        .controlSize(.small)
                    }
                    .padding(.horizontal, 14)
                    .padding(.vertical, 8)
                    .background(.regularMaterial, in: Capsule())
                    .shadow(radius: 3, y: 2)
                    .padding(.top, 12)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
                .allowsHitTesting(true)
            }

            // J-2 UX — model-name labels overlay. Off by default;
            // user enables via the canvas controls. Renders one
            // small Text per visible model at its projected centre;
            // refreshes each animation frame.
            if settings.showModelLabels {
                ModelLabelsOverlay()
                    .allowsHitTesting(false)
            }

            // Inline action bar floating above the selected model
            // — see plans/phase-j-touch-ux.md. Anchored to the
            // model's top-centre in screen coords; re-queries
            // every animation frame so it tracks pan / zoom /
            // orbit / drag without observer wiring. Hidden in
            // multi-select mode (the multi-select bar takes over
            // since the inline bar's per-model actions don't make
            // sense across a set).
            if viewModel.layoutEditorSelection.count < 2,
               let selected = viewModel.layoutEditorSelectedModel,
               !selected.isEmpty {
                InlineModelActionBar(modelName: selected,
                                      summaryToken: summaryToken,
                                      onPropertyChange: {
                                          summaryToken &+= 1
                                          hasUnsavedChanges =
                                              viewModel.document.hasUnsavedLayoutChanges()
                                      },
                                      onRequestDelete: {
                                          pendingDeleteModelName = selected
                                      },
                                      onRequestDuplicate: {
                                          performDuplicate(of: [selected])
                                      })
                    .allowsHitTesting(true)
            }

            // J-4 (multi-select) — operations bar. Visible
            // whenever 2+ models are selected. Hosts Align ▾,
            // Distribute ▾, Match Size ▾, and a Clear action.
            // Top-centered like the creation banner so it doesn't
            // fight the bottom tool toolbar.
            if viewModel.layoutEditorSelection.count >= 2 {
                VStack {
                    MultiSelectActionBar(
                        selection: viewModel.layoutEditorSelection,
                        leader: viewModel.layoutEditorSelectedModel,
                        onAlign: { edge in performAlign(by: edge) },
                        onDistribute: { axis in performDistribute(axis: axis) },
                        onMatchSize: { dim in performMatchSize(dimension: dim) },
                        onFlip: { axis in performFlip(axis: axis) },
                        onDuplicate: { performDuplicate(of: Array(viewModel.layoutEditorSelection)) },
                        onGroup: { newGroupFromSelectionPrompt() },
                        onClear: {
                            viewModel.layoutEditorSelection.removeAll()
                            viewModel.layoutEditorSelectedModel = nil
                        })
                        .padding(.top, 12)
                    Spacer()
                }
                .frame(maxWidth: .infinity)
                .allowsHitTesting(true)
            }

            // Bottom tool toolbar — see plans/phase-j-touch-ux.md.
            // Replaces desktop's CentreCycle (tap centre sphere to
            // advance axis tool) with a persistent picker. Visible
            // only when a model is selected.
            if let selected = viewModel.layoutEditorSelectedModel,
               !selected.isEmpty {
                VStack {
                    Spacer()
                    LayoutEditorToolToolbar(settings: settings,
                                             selectedModelName: selected,
                                             onToolChange: { tool in
                                                 syncToolToBridge(tool: tool, modelName: selected)
                                             })
                    .padding(.bottom, 12)
                }
            }
        }
    }

    /// Phase J-2 (touch UX) — buttons shown in the long-press
    /// `.confirmationDialog`. The menu's content depends on what
    /// the user long-pressed on (vertex / segment / curve control).
    @ViewBuilder
    private var contextMenuButtons: some View {
        switch contextMenuTarget {
        case .vertex(let name, let idx):
            Button("Delete Point", role: .destructive) {
                _ = viewModel.document.deleteVertex(at: idx, forModel: name)
                postLayoutMutation(modelName: name)
            }
            Button("Cancel", role: .cancel) { }
        case .segment(let name, let idx, let hasCurve):
            Button("Add Point") {
                _ = viewModel.document.insertVertex(inSegment: idx, forModel: name)
                postLayoutMutation(modelName: name)
            }
            if hasCurve {
                Button("Remove Curve", role: .destructive) {
                    _ = viewModel.document.setCurve(false, onSegment: idx, forModel: name)
                    postLayoutMutation(modelName: name)
                }
            } else {
                Button("Define Curve") {
                    _ = viewModel.document.setCurve(true, onSegment: idx, forModel: name)
                    postLayoutMutation(modelName: name)
                }
            }
            Button("Cancel", role: .cancel) { }
        case .curveControl(let name, let idx):
            Button("Remove Curve", role: .destructive) {
                _ = viewModel.document.setCurve(false, onSegment: idx, forModel: name)
                postLayoutMutation(modelName: name)
            }
            Button("Cancel", role: .cancel) { }
        case nil:
            EmptyView()
        }
    }

    private func postLayoutMutation(modelName: String) {
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                         object: "LayoutEditor",
                                         userInfo: ["model": modelName])
    }

    /// Push toolbar tool selection through to the document. The
    /// settings update happens in the Toolbar's binding; this
    /// follow-up call writes to the screen location's `axis_tool`
    /// so `GetHandles` emits the matching descriptor set on the
    /// next draw. Repaint is triggered by the
    /// `.layoutEditorModelMoved` notification (covers refresh of
    /// the canvas overlay so the new gizmo appears immediately).
    private func syncToolToBridge(tool: String, modelName: String) {
        _ = viewModel.document.setAxisTool(tool, forModel: modelName)
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                         object: "LayoutEditor",
                                         userInfo: ["model": modelName])
    }

    // MARK: - Refresh

    private func refresh() {
        layoutGroups = viewModel.document.layoutGroups()
        activeLayoutGroup = viewModel.document.activeLayoutGroup()
        if viewModel.isShowFolderLoaded {
            settings.is3D = viewModel.document.layoutMode3D()
        }
        displayState = viewModel.document.layoutDisplayState()
        availableModelTypes = viewModel.document.availableModelTypesForCreation()
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

    /// Friendlier labels for the Add-Model picker. Falls back to
    /// the raw type string when no mapping is defined — keeps the
    /// menu functional even as the curated set grows.
    private func modelTypeLabel(_ type: String) -> String {
        switch type {
        case "Poly Line":   return "Poly Line"
        case "Single Line": return "Single Line"
        case "Window Frame":return "Window Frame"
        case "Candy Canes": return "Candy Canes"
        case "Channel Block": return "Channel Block"
        default:            return type
        }
    }

    private func refreshModelList() {
        modelNames = viewModel.document.modelsInActiveLayoutGroup()
        groupNames = viewModel.document.modelGroupsInActiveLayoutGroup()
        objectNames = viewModel.document.viewObjectsInActiveLayoutGroup()
        // If the previously-selected model isn't in the new list,
        // clear selection so the side panel doesn't show stale data.
        if let sel = viewModel.layoutEditorSelectedModel,
           !modelNames.contains(sel) {
            viewModel.layoutSelectSingle(nil)
        }
        if let g = viewModel.layoutEditorSelectedGroup, !groupNames.contains(g) {
            viewModel.layoutEditorSelectedGroup = nil
        }
        if let o = viewModel.layoutEditorSelectedObject, !objectNames.contains(o) {
            viewModel.layoutEditorSelectedObject = nil
        }
    }

    /// J-6 (objects) — commit a view-object property edit. View
    /// objects don't participate in the model undo stack yet (the
    /// stack snapshots Models specifically); on undo the user can
    /// still Discard Changes for a full rollback through the dirty
    /// set. Bumps the summary token + repaints the canvas like the
    /// model path.
    private func commitObjectProperty(objectName: String, key: String, value: Any) {
        let changed = viewModel.document.setLayoutViewObjectProperty(objectName,
                                                                     key: key,
                                                                     value: value)
        if changed {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            // canUndo is intentionally NOT bumped — view-object
            // edits aren't on the undo stack today.
            if key == "layoutGroup" {
                refreshModelList()
            }
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                            object: "LayoutEditor",
                                            userInfo: ["model": objectName])
        }
    }

    /// J-7 (group CRUD) — current group's member set, used by the
    /// add-member sheet to filter the candidate list. Returns an
    /// empty set when no group is selected.
    private var selectedGroupMembers: Set<String> {
        guard let name = viewModel.layoutEditorSelectedGroup,
              let summary = viewModel.document.modelGroupLayoutSummary(name) else {
            return []
        }
        return Set(summary["models"] as? [String] ?? [])
    }

    /// J-7 — new-group sheet "Create" callback. Bridge does the
    /// validation (name collision); on success we select the new
    /// group so the user can immediately add members.
    /// `pendingGroupFromSelection`, when non-nil, supplies the
    /// initial member list (multi-select "Group" action populates
    /// it before opening the sheet).
    private func handleCreateGroup(_ name: String) {
        let members = pendingGroupFromSelection
        pendingGroupFromSelection = nil
        if viewModel.document.createModelGroup(name, members: members) {
            viewModel.layoutEditorSelectedGroup = name
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            refreshModelList()
            // If the user grouped from a model selection, flip to
            // the Groups tab so the new group + its members appear.
            if members != nil {
                sidebarTab = .groups
                viewModel.layoutEditorSelection.removeAll()
                viewModel.layoutEditorSelectedModel = nil
            }
        }
        newGroupSheetVisible = false
    }

    /// J-7 — add-member sheet "Add" callback. Each member fires
    /// its own bridge call (each one marks the group dirty
    /// independently, but the UX presents it as a single batch).
    private func handleAddMembers(_ picked: [String]) {
        guard let groupName = viewModel.layoutEditorSelectedGroup else {
            addMemberSheetVisible = false
            return
        }
        for name in picked {
            _ = viewModel.document.addModel(name, toGroup: groupName)
        }
        if !picked.isEmpty {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        }
        addMemberSheetVisible = false
    }

    /// J-7 — delete-group confirmation "Delete" callback.
    private func handleDeleteGroup(_ name: String) {
        if viewModel.document.deleteModelGroup(name) {
            if viewModel.layoutEditorSelectedGroup == name {
                viewModel.layoutEditorSelectedGroup = nil
            }
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            refreshModelList()
        }
    }

    /// J-5 (groups) — commit a group property edit. Mirrors
    /// `commitProperty` for models: pushes an undo snapshot first
    /// (so the user can revert), invokes the bridge setter, and
    /// updates the dirty / undo flags on success. Group transforms
    /// flow through the same `_dirtyLayoutModels` set; save handler
    /// rewrites the `<modelGroup>` element in place.
    private func commitGroupProperty(groupName: String, key: String, value: Any) {
        viewModel.document.pushLayoutUndoSnapshot(forModel: groupName)
        let changed = viewModel.document.setLayoutModelGroupProperty(groupName,
                                                                     key: key,
                                                                     value: value)
        if changed {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            if key == "layoutGroup" {
                refreshModelList()
            }
        }
    }

    /// J-6 (per-type properties) — commit a Tree/Arch/Matrix/etc.
    /// type-specific edit. Pushes undo, invokes the bridge, then
    /// bumps the summary token so both panes (common + per-type)
    /// re-read from the bridge. Mirrors `commitProperty` for common
    /// props; structural changes (string count etc.) refresh the
    /// model list too because channel ranges can shift.
    private func commitPerTypeProperty(modelName: String, key: String, value: Any) {
        viewModel.document.pushLayoutUndoSnapshot(forModel: modelName)
        let changed = viewModel.document.setPerTypeProperty(key,
                                                            onModel: modelName,
                                                            value: value)
        if changed {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            refreshModelList()
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                            object: "LayoutEditor",
                                            userInfo: ["model": modelName])
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

    /// J-3 (touch UX) — exit fresh-model placement and / or mid-
    /// polyline create. Returns `.handled` if either mode was
    /// active so the keypress is consumed (otherwise Esc/Return
    /// would fall through to focused controls / sidebar).
    private func endCreationModes() -> KeyPress.Result {
        var consumed = false
        if viewModel.layoutPendingNewModelType != nil {
            viewModel.layoutPendingNewModelType = nil
            consumed = true
        }
        if viewModel.layoutPolylineInProgress != nil {
            viewModel.layoutPolylineInProgress = nil
            consumed = true
        }
        if viewModel.layoutPendingImportPath != nil {
            viewModel.layoutPendingImportPath = nil
            consumed = true
        }
        return consumed ? .handled : .ignored
    }

    /// J-3 (touch UX) — delete the named model from the bridge,
    /// clear selection so the action bar/toolbar go away, then
    /// refresh the sidebar list and dirty/undo state. Repaint the
    /// canvas via the standard layout-mutation notification so the
    /// model disappears immediately. Persisting requires Save —
    /// matches every other layout edit.
    private func deleteModel(name: String) {
        guard viewModel.document.deleteModel(name) else { return }
        if viewModel.layoutEditorSelectedModel == name {
            viewModel.layoutSelectSingle(nil)
        }
        summaryToken &+= 1
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        canUndo = viewModel.document.canUndoLayoutChange()
        refreshModelList()
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                        object: "LayoutEditor",
                                        userInfo: ["model": name])
    }

    // MARK: - J-4 multi-select operations

    /// Align all selected models' named edge / centre to the
    /// primary (leader). Pushes an undo snapshot per moved model
    /// so the user can revert individual moves; bumps the summary
    /// token + dirty state and repaints the canvas.
    private func performAlign(by edge: String) {
        guard let leader = viewModel.layoutEditorSelectedModel,
              viewModel.layoutEditorSelection.count >= 2 else { return }
        let names = Array(viewModel.layoutEditorSelection)
        for n in names where n != leader {
            viewModel.document.pushLayoutUndoSnapshot(forModel: n)
        }
        guard let bridge = XLightsBridgeBox.bridgeForLayoutEditor() else { return }
        let moved = bridge.alignModels(names,
                                        toLeader: leader,
                                        by: edge,
                                        for: viewModel.document)
        if moved {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                             object: "LayoutEditor",
                                             userInfo: ["model": leader])
        }
    }

    /// Distribute centres along the named axis. Snapshots every
    /// candidate before mutating so single-step undo works.
    private func performDistribute(axis: String) {
        guard viewModel.layoutEditorSelection.count >= 3 else { return }
        let names = Array(viewModel.layoutEditorSelection)
        for n in names {
            viewModel.document.pushLayoutUndoSnapshot(forModel: n)
        }
        guard let bridge = XLightsBridgeBox.bridgeForLayoutEditor() else { return }
        let moved = bridge.distributeModels(names,
                                              axis: axis,
                                              for: viewModel.document)
        if moved {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                             object: "LayoutEditor",
                                             userInfo: [:])
        }
    }

    /// Resize the selection (except leader) so the named dimension
    /// matches the leader. `dim` ∈ {"width","height","depth","all"}.
    private func performMatchSize(dimension dim: String) {
        guard let leader = viewModel.layoutEditorSelectedModel,
              viewModel.layoutEditorSelection.count >= 2 else { return }
        let names = Array(viewModel.layoutEditorSelection)
        for n in names where n != leader {
            viewModel.document.pushLayoutUndoSnapshot(forModel: n)
        }
        guard let bridge = XLightsBridgeBox.bridgeForLayoutEditor() else { return }
        let resized = bridge.matchSize(ofModels: names,
                                         toLeader: leader,
                                         dimension: dim,
                                         for: viewModel.document)
        if resized {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                             object: "LayoutEditor",
                                             userInfo: ["model": leader])
        }
    }

    /// J-7 — Flip the multi-selection 180° about the given axis.
    /// Uses the bridge implementation (matches desktop flip math).
    /// Each model flips in place; pushing undo per model so each
    /// flip is independently revertible.
    private func performFlip(axis: String) {
        guard !viewModel.layoutEditorSelection.isEmpty else { return }
        let names = Array(viewModel.layoutEditorSelection)
        for n in names {
            viewModel.document.pushLayoutUndoSnapshot(forModel: n)
        }
        guard let bridge = XLightsBridgeBox.bridgeForLayoutEditor() else { return }
        let flipped = bridge.flipModels(names, axis: axis, for: viewModel.document)
        if flipped {
            summaryToken &+= 1
            hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
            canUndo = viewModel.document.canUndoLayoutChange()
            NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                             object: "LayoutEditor",
                                             userInfo: [:])
        }
    }

    /// J-7 — Duplicate each model in `names`. The bridge clones
    /// them with a small (+50, +50) offset so they don't overlap
    /// the originals; the new selection becomes the duplicates so
    /// the user can immediately drag them to position.
    private func performDuplicate(of names: [String]) {
        guard !names.isEmpty else { return }
        guard let bridge = XLightsBridgeBox.bridgeForLayoutEditor() else { return }
        let dups = bridge.duplicateModels(names, for: viewModel.document)
        guard !dups.isEmpty else { return }
        summaryToken &+= 1
        hasUnsavedChanges = viewModel.document.hasUnsavedLayoutChanges()
        refreshModelList()
        // Shift selection to the new duplicates so the user can
        // drag / nudge them right away.
        viewModel.layoutEditorSelection = Set(dups)
        viewModel.layoutEditorSelectedModel = dups.first
        NotificationCenter.default.post(name: .layoutEditorModelMoved,
                                         object: "LayoutEditor",
                                         userInfo: [:])
    }

    /// J-7 — Group-from-selection. Reuses NewGroupSheet for the
    /// name prompt; the create handler reads
    /// `pendingGroupFromSelection` to decide whether to pass
    /// members through to the bridge.
    private func newGroupFromSelectionPrompt() {
        guard !viewModel.layoutEditorSelection.isEmpty else { return }
        pendingGroupFromSelection = Array(viewModel.layoutEditorSelection)
        newGroupSheetVisible = true
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

/// J-8 — Editable properties panel for the selected model.
///
/// Ordered to match desktop's wxPropertyGrid:
/// 1. Header (Name / Type / always visible)
/// 2. Model Properties (per-type descriptors — expanded by default)
/// 3. Controller Connection
/// 4. String Properties
/// 5. Appearance
/// 6. Dimensions
/// 7. Size/Location
///
/// Each section after #1 is a `DisclosureGroup` so the user can
/// collapse what they don't need. Default-expanded: per-type only —
/// matching desktop's "the bits that vary per model first" intent.
private struct LayoutEditorPropertiesView: View {
    let modelName: String
    let summary: [String: Any]
    let typeDescriptors: [[String: Any]]
    let layoutGroups: [String]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void
    let typeCommit: (_ key: String, _ value: Any) -> Void

    @State private var expandedTypeProps: Bool = true
    @State private var expandedController: Bool = false
    @State private var expandedStringProps: Bool = false
    @State private var expandedAppearance: Bool = false
    @State private var expandedDimensions: Bool = false
    @State private var expandedSizeLocation: Bool = false

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            // 1. Header — non-collapsible identity block.
            row("Name") { Text(modelName).truncationMode(.middle) }
            row("Type") { Text(summary["displayAs"] as? String ?? "—") }

            // 2. Model Properties (per-type). Title carries the
            // model's `displayAs` for parity with desktop's
            // category-header convention.
            if !typeDescriptors.isEmpty {
                section($expandedTypeProps,
                        title: typeSectionTitle) {
                    ForEach(Array(typeDescriptors.enumerated()), id: \.offset) { _, d in
                        typeDescriptorRow(d)
                    }
                }
            }

            // 3. Controller Connection.
            section($expandedController, title: "Controller Connection") {
                row("Preview") { layoutGroupPicker }
                row("Controller") { controllerField }
                row("Channel range") {
                    Text("\(uintVal("startChannel"))..\(uintVal("endChannel"))")
                        .foregroundStyle(.secondary)
                }
            }

            // 4. String Properties.
            section($expandedStringProps, title: "String Properties") {
                row("String Type") { stringTypePicker }
                row("Strings") {
                    Text("\(intVal("stringCount"))").foregroundStyle(.secondary)
                }
                row("Nodes") {
                    Text("\(uintVal("nodeCount"))").foregroundStyle(.secondary)
                }
            }

            // 5. Appearance.
            section($expandedAppearance, title: "Appearance") {
                row("Active") {
                    Toggle("", isOn: boolBinding(key: "active"))
                        .labelsHidden()
                        .controlSize(.mini)
                }
                row("Pixel Size") { intField(key: "pixelSize", min: 1, max: 100) }
                row("Pixel Style") { pixelStylePicker }
                row("Transparency") { intField(key: "transparency", min: 0, max: 100) }
                row("Black Transparency") {
                    intField(key: "blackTransparency", min: 0, max: 100)
                }
                row("Tag Color") { tagColorPicker }
                row("Description") { descriptionField }
            }

            // 6. Dimensions.
            section($expandedDimensions, title: "Dimensions") {
                row("Width")  { numberField(key: "width",  min: 0) }
                row("Height") { numberField(key: "height", min: 0) }
                row("Depth")  { numberField(key: "depth",  min: 0) }
            }

            // 7. Size/Location.
            section($expandedSizeLocation, title: "Size/Location") {
                row("Centre X") { numberField(key: "centerX") }
                row("Centre Y") { numberField(key: "centerY") }
                row("Centre Z") { numberField(key: "centerZ") }
                row("Rotate X") { numberField(key: "rotateX") }
                row("Rotate Y") { numberField(key: "rotateY") }
                row("Rotate Z") { numberField(key: "rotateZ") }
                row("Locked") {
                    Toggle("", isOn: lockedBinding)
                        .labelsHidden()
                        .controlSize(.mini)
                }
            }
        }
        .font(.caption.monospacedDigit())
    }

    // MARK: - Section header

    private var typeSectionTitle: String {
        // Desktop uses the type string verbatim as the category
        // label. Falls back to "Model Properties" if missing.
        let t = summary["displayAs"] as? String ?? ""
        return t.isEmpty ? "Model Properties" : t
    }

    @ViewBuilder
    private func section<Content: View>(_ expanded: Binding<Bool>,
                                         title: String,
                                         @ViewBuilder _ content: () -> Content) -> some View {
        // DisclosureGroup's content closure is escaping, so we
        // materialize `content` once here and let DisclosureGroup
        // capture the resulting View. Otherwise SwiftUI complains
        // that the non-escaping `content` is captured by an
        // escaping closure.
        let body = VStack(alignment: .leading, spacing: 4) { content() }
            .padding(.vertical, 4)
        DisclosureGroup(isExpanded: expanded) {
            body
        } label: {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.primary)
        }
        .accentColor(.secondary)
    }

    // MARK: - Rows

    @ViewBuilder
    private func row(_ label: String, @ViewBuilder _ content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 110, alignment: .leading)
            Spacer(minLength: 8)
            content()
                .lineLimit(1)
        }
    }

    // MARK: - Editors

    private func numberField(key: String, min: Double? = nil) -> some View {
        LayoutEditorDoubleField(
            id: "\(modelName).\(key).\(token)",
            initial: doubleVal(key),
            min: min,
            commit: { newValue in commit(key, NSNumber(value: newValue)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    private func intField(key: String, min: Double?, max: Double?) -> some View {
        LayoutEditorDoubleField(
            id: "\(modelName).\(key).\(token)",
            initial: Double(intVal(key)),
            min: min,
            max: max,
            precision: 0,
            commit: { newValue in commit(key, NSNumber(value: Int(newValue))) }
        )
        .frame(maxWidth: 90, alignment: .trailing)
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

    private var stringTypePicker: some View {
        let options = (summary["stringTypeOptions"] as? [String]) ?? []
        let current = summary["stringType"] as? String ?? ""
        return Menu {
            ForEach(options, id: \.self) { name in
                Button {
                    commit("stringType", name as NSString)
                } label: {
                    HStack {
                        Text(name)
                        if name == current {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Text(current.isEmpty ? "—" : current)
                .truncationMode(.middle)
                .frame(maxWidth: 160, alignment: .trailing)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    private var pixelStylePicker: some View {
        let options = (summary["pixelStyleOptions"] as? [String]) ?? []
        let idx = intVal("pixelStyle")
        let current = (idx >= 0 && idx < options.count) ? options[idx] : "—"
        return Menu {
            ForEach(Array(options.enumerated()), id: \.offset) { i, name in
                Button {
                    commit("pixelStyle", NSNumber(value: i))
                } label: {
                    HStack {
                        Text(name)
                        if i == idx {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Text(current)
                .truncationMode(.middle)
                .frame(maxWidth: 130, alignment: .trailing)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    /// Tag color is stored on Model as a string (typically
    /// `#RRGGBB`). SwiftUI's ColorPicker gives us a Color; we
    /// convert to hex on commit. Showing the swatch + label
    /// matches desktop's "block + text" presentation.
    private var tagColorPicker: some View {
        let hex = summary["tagColor"] as? String ?? ""
        let parsed = Color(hexString: hex) ?? .black
        return HStack(spacing: 6) {
            ColorPicker("", selection: Binding(
                get: { parsed },
                set: { newColor in
                    let s = newColor.toHexString()
                    commit("tagColor", s as NSString)
                }
            ), supportsOpacity: false)
                .labelsHidden()
            Text(hex.isEmpty ? "—" : hex)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 80, alignment: .leading)
        }
    }

    private var descriptionField: some View {
        LayoutEditorStringField(
            id: "\(modelName).description.\(token)",
            initial: summary["description"] as? String ?? "",
            commit: { commit("description", $0 as NSString) }
        )
        .frame(maxWidth: 160, alignment: .trailing)
    }

    private var lockedBinding: Binding<Bool> {
        Binding(
            get: { summary["locked"] as? Bool ?? false },
            set: { commit("locked", NSNumber(value: $0)) }
        )
    }

    private func boolBinding(key: String) -> Binding<Bool> {
        Binding(
            get: { summary[key] as? Bool ?? false },
            set: { commit(key, NSNumber(value: $0)) }
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

    // MARK: - Per-type row dispatcher

    @ViewBuilder
    private func typeDescriptorRow(_ d: [String: Any]) -> some View {
        let key = d["key"] as? String ?? ""
        let label = d["label"] as? String ?? key
        let kind = d["kind"] as? String ?? ""
        let enabled = (d["enabled"] as? Bool) ?? true
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 110, alignment: .leading)
            Spacer(minLength: 8)
            typeDescriptorControl(kind: kind, key: key, d: d)
                .disabled(!enabled)
                .opacity(enabled ? 1.0 : 0.5)
                .lineLimit(1)
        }
    }

    @ViewBuilder
    private func typeDescriptorControl(kind: String, key: String, d: [String: Any]) -> some View {
        switch kind {
        case "int":
            let v = (d["value"] as? NSNumber)?.doubleValue ?? 0
            let minV = (d["min"] as? NSNumber)?.doubleValue
            let maxV = (d["max"] as? NSNumber)?.doubleValue
            LayoutEditorDoubleField(
                id: "\(modelName).\(key).\(token)",
                initial: v, min: minV, max: maxV, precision: 0,
                commit: { newValue in typeCommit(key, NSNumber(value: Int(newValue))) }
            )
            .frame(maxWidth: 110, alignment: .trailing)
        case "double":
            let v = (d["value"] as? NSNumber)?.doubleValue ?? 0
            let minV = (d["min"] as? NSNumber)?.doubleValue
            let maxV = (d["max"] as? NSNumber)?.doubleValue
            let precision = (d["precision"] as? NSNumber)?.intValue ?? 2
            LayoutEditorDoubleField(
                id: "\(modelName).\(key).\(token)",
                initial: v, min: minV, max: maxV, precision: precision,
                commit: { newValue in typeCommit(key, NSNumber(value: newValue)) }
            )
            .frame(maxWidth: 110, alignment: .trailing)
        case "bool":
            let v = (d["value"] as? Bool) ?? false
            Toggle("", isOn: Binding(
                get: { v },
                set: { typeCommit(key, NSNumber(value: $0)) }
            ))
            .labelsHidden()
            .controlSize(.mini)
        case "enum":
            let idx = (d["value"] as? NSNumber)?.intValue ?? 0
            let opts = (d["options"] as? [String]) ?? []
            Menu {
                ForEach(Array(opts.enumerated()), id: \.offset) { i, label in
                    Button {
                        typeCommit(key, NSNumber(value: i))
                    } label: {
                        HStack {
                            Text(label)
                            if i == idx {
                                Spacer()
                                Image(systemName: "checkmark")
                            }
                        }
                    }
                }
            } label: {
                Text(idx >= 0 && idx < opts.count ? opts[idx] : "—")
                    .truncationMode(.middle)
                    .frame(maxWidth: 160, alignment: .trailing)
            }
            .menuStyle(.button)
            .controlSize(.mini)
        case "string":
            let v = d["value"] as? String ?? ""
            LayoutEditorStringField(
                id: "\(modelName).\(key).\(token)",
                initial: v,
                commit: { typeCommit(key, $0 as NSString) }
            )
            .frame(maxWidth: 160, alignment: .trailing)
        default:
            Text("?").foregroundStyle(.tertiary)
        }
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

/// J-12 — Add-View-Object sheet + delete-confirmation alert +
/// per-type file picker. Factored out so the modifier chain on
/// LayoutEditorView's body stays inside the Swift type-checker's
/// complexity budget.
private struct ViewObjectCrudModifiers: ViewModifier {
    @Binding var objectFilePickerVisible: Bool
    let objectFilePickerTypes: [UTType]
    @Binding var addViewObjectSheetVisible: Bool
    @Binding var pendingDeleteObjectName: String?
    let availableTypes: [String]
    let onCreateObject: (String) -> Void
    let onDeleteObject: (String) -> Void
    let onFilePicked: (Result<[URL], Error>) -> Void

    func body(content: Content) -> some View {
        content
            .fileImporter(isPresented: $objectFilePickerVisible,
                          allowedContentTypes: objectFilePickerTypes.isEmpty
                                                  ? [.data]
                                                  : objectFilePickerTypes,
                          allowsMultipleSelection: false,
                          onCompletion: onFilePicked)
            .sheet(isPresented: $addViewObjectSheetVisible) {
                AddViewObjectSheet(
                    types: availableTypes,
                    onSelect: { type in
                        onCreateObject(type)
                    },
                    onCancel: { addViewObjectSheetVisible = false }
                )
            }
            .alert(deleteTitle,
                   isPresented: Binding(
                        get: { pendingDeleteObjectName != nil },
                        set: { if !$0 { pendingDeleteObjectName = nil } })) {
                Button("Delete", role: .destructive) {
                    if let name = pendingDeleteObjectName { onDeleteObject(name) }
                }
                Button("Cancel", role: .cancel) { }
            } message: {
                Text("Removes this view object from the show. Save the layout to make the change permanent.")
            }
    }

    private var deleteTitle: String {
        "Delete \(pendingDeleteObjectName ?? "")?"
    }
}

/// J-12 — view-object type picker shown by the Objects tab's
/// "+" button.
private struct AddViewObjectSheet: View {
    let types: [String]
    let onSelect: (String) -> Void
    let onCancel: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(types, id: \.self) { type in
                Button {
                    onSelect(type)
                } label: {
                    HStack {
                        Image(systemName: iconFor(type))
                            .foregroundStyle(.secondary)
                        Text(type)
                            .foregroundStyle(.primary)
                        Spacer()
                    }
                }
            }
            .navigationTitle("Add View Object")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { onCancel() }
                }
            }
        }
        .presentationDetents([.medium])
    }

    private func iconFor(_ type: String) -> String {
        switch type {
        case "Image":     return "photo"
        case "Mesh":      return "cube.transparent"
        case "Gridlines": return "grid"
        case "Terrain":   return "mountain.2"
        case "Ruler":     return "ruler"
        default:          return "square.on.square"
        }
    }
}

/// J-11 — enforce exactly-one-active-selection across the three
/// sidebar tabs (Models / Groups / Objects). Setting any one
/// clears the other two; explicit tab switch drops the previous
/// tab's selection; a canvas tap that lands on a model flips
/// the sidebar to the Models tab so the property pane shows the
/// right surface.
///
/// Factored out as a ViewModifier so LayoutEditorView's body
/// chain stays under the Swift type-checker's complexity budget.
private struct SidebarSelectionMutex: ViewModifier {
    @Environment(SequencerViewModel.self) var viewModel
    @Binding var sidebarTab: LayoutSidebarTab

    func body(content: Content) -> some View {
        content
            .onChange(of: viewModel.layoutEditorSelectedModel) { _, newSel in
                guard let name = newSel, !name.isEmpty else { return }
                if viewModel.layoutEditorSelectedGroup != nil {
                    viewModel.layoutEditorSelectedGroup = nil
                }
                if viewModel.layoutEditorSelectedObject != nil {
                    viewModel.layoutEditorSelectedObject = nil
                }
                if sidebarTab != .models {
                    sidebarTab = .models
                }
            }
            .onChange(of: viewModel.layoutEditorSelectedGroup) { _, newSel in
                guard newSel != nil else { return }
                if viewModel.layoutEditorSelectedModel != nil ||
                   !viewModel.layoutEditorSelection.isEmpty {
                    viewModel.layoutSelectSingle(nil)
                }
                if viewModel.layoutEditorSelectedObject != nil {
                    viewModel.layoutEditorSelectedObject = nil
                }
            }
            .onChange(of: viewModel.layoutEditorSelectedObject) { oldSel, newSel in
                guard newSel != nil else {
                    // Selection cleared — drop any terrain edit
                    // session so the next tap doesn't paint.
                    if viewModel.terrainEditTarget != nil {
                        viewModel.terrainEditTarget = nil
                    }
                    return
                }
                if viewModel.layoutEditorSelectedModel != nil ||
                   !viewModel.layoutEditorSelection.isEmpty {
                    viewModel.layoutSelectSingle(nil)
                }
                if viewModel.layoutEditorSelectedGroup != nil {
                    viewModel.layoutEditorSelectedGroup = nil
                }
                // J-13 — if the user picks a different VO than the
                // terrain currently being edited, exit edit mode.
                if let editing = viewModel.terrainEditTarget,
                   newSel != editing {
                    viewModel.terrainEditTarget = nil
                }
            }
            .onChange(of: sidebarTab) { _, newTab in
                // Explicit tab switch drops the leaving tab's
                // selection so the canvas tint reflects where the
                // user is now looking.
                switch newTab {
                case .models:
                    viewModel.layoutEditorSelectedGroup = nil
                    viewModel.layoutEditorSelectedObject = nil
                case .groups:
                    if viewModel.layoutEditorSelectedModel != nil ||
                       !viewModel.layoutEditorSelection.isEmpty {
                        viewModel.layoutSelectSingle(nil)
                    }
                    viewModel.layoutEditorSelectedObject = nil
                case .objects:
                    if viewModel.layoutEditorSelectedModel != nil ||
                       !viewModel.layoutEditorSelection.isEmpty {
                        viewModel.layoutSelectSingle(nil)
                    }
                    viewModel.layoutEditorSelectedGroup = nil
                }
            }
    }
}

/// J-7 (group CRUD) — extracted modifier hosting the new-group
/// sheet, add-member sheet, and delete-confirmation alert.
/// LayoutEditorView's body was over the type-checker's complexity
/// budget once these landed inline; factoring keeps the call site
/// to a single `.modifier(...)`.
private struct GroupCrudModifiers: ViewModifier {
    @Binding var newGroupSheetVisible: Bool
    @Binding var addMemberSheetVisible: Bool
    @Binding var pendingDeleteGroupName: String?
    let groupNames: [String]
    let modelNames: [String]
    let onCreateGroup: (String) -> Void
    let onAddMembers: ([String]) -> Void
    let onDeleteGroup: (String) -> Void
    let selectedGroupName: String?
    let currentMembers: Set<String>
    /// J-9 — bridge lookup so the AddMemberSheet's tree can lazily
    /// fetch submodels for a parent. Captured at the call site so
    /// the sheet itself doesn't need a view-model handle.
    let submodelsFor: (String) -> [String]

    func body(content: Content) -> some View {
        content
            .sheet(isPresented: $newGroupSheetVisible) {
                NewGroupSheet(
                    existingNames: Set(groupNames + modelNames),
                    onCreate: onCreateGroup,
                    onCancel: { newGroupSheetVisible = false }
                )
            }
            .sheet(isPresented: $addMemberSheetVisible) {
                if let groupName = selectedGroupName {
                    // Candidates = every visible model whose top-
                    // level name isn't already a direct group
                    // member. Submodels live inside the tree and
                    // are filtered separately (an "already a
                    // member" hint is shown but they still appear).
                    let candidates = modelNames.filter { !currentMembers.contains($0) }
                    AddMemberSheet(
                        groupName: groupName,
                        candidates: candidates,
                        existingMembers: currentMembers,
                        submodelsFor: submodelsFor,
                        onAdd: onAddMembers,
                        onCancel: { addMemberSheetVisible = false }
                    )
                }
            }
            .alert(deleteAlertTitle,
                   isPresented: Binding(
                        get: { pendingDeleteGroupName != nil },
                        set: { if !$0 { pendingDeleteGroupName = nil } })) {
                Button("Delete", role: .destructive) {
                    if let name = pendingDeleteGroupName { onDeleteGroup(name) }
                }
                Button("Cancel", role: .cancel) { }
            } message: {
                Text("Removes this group from the active layout. Save the layout to make the change permanent.")
            }
    }

    private var deleteAlertTitle: String {
        "Delete \(pendingDeleteGroupName ?? "")?"
    }
}

/// J-6 (per-type properties) — renders the descriptor list
/// returned by `perTypePropertiesForModel:`. Generic over kind so
/// new model types can be wired by adding a bridge case without
/// touching SwiftUI.
private struct LayoutEditorTypePropertiesView: View {
    let modelName: String
    let descriptors: [[String: Any]]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void

    var body: some View {
        if descriptors.isEmpty {
            EmptyView()
        } else {
            VStack(alignment: .leading, spacing: 6) {
                Text("Type Properties")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.secondary)
                ForEach(Array(descriptors.enumerated()), id: \.offset) { _, d in
                    descriptorRow(d)
                }
            }
            .font(.caption.monospacedDigit())
        }
    }

    @ViewBuilder
    private func descriptorRow(_ d: [String: Any]) -> some View {
        let key = d["key"] as? String ?? ""
        let label = d["label"] as? String ?? key
        let kind = d["kind"] as? String ?? ""
        let enabled = (d["enabled"] as? Bool) ?? true

        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 110, alignment: .leading)
            Spacer(minLength: 8)
            controlFor(kind: kind, key: key, d: d)
                .disabled(!enabled)
                .opacity(enabled ? 1.0 : 0.5)
                .lineLimit(1)
        }
    }

    @ViewBuilder
    private func controlFor(kind: String, key: String, d: [String: Any]) -> some View {
        switch kind {
        case "int":
            let v = (d["value"] as? NSNumber)?.doubleValue ?? 0
            let minV = (d["min"] as? NSNumber)?.doubleValue
            let maxV = (d["max"] as? NSNumber)?.doubleValue
            LayoutEditorDoubleField(
                id: "\(modelName).\(key).\(token)",
                initial: v,
                min: minV,
                max: maxV,
                precision: 0,
                commit: { newValue in commit(key, NSNumber(value: Int(newValue))) }
            )
            .frame(maxWidth: 110, alignment: .trailing)
        case "double":
            let v = (d["value"] as? NSNumber)?.doubleValue ?? 0
            let minV = (d["min"] as? NSNumber)?.doubleValue
            let maxV = (d["max"] as? NSNumber)?.doubleValue
            let precision = (d["precision"] as? NSNumber)?.intValue ?? 2
            LayoutEditorDoubleField(
                id: "\(modelName).\(key).\(token)",
                initial: v,
                min: minV,
                max: maxV,
                precision: precision,
                commit: { newValue in commit(key, NSNumber(value: newValue)) }
            )
            .frame(maxWidth: 110, alignment: .trailing)
        case "bool":
            let v = (d["value"] as? Bool) ?? false
            Toggle("", isOn: Binding(
                get: { v },
                set: { commit(key, NSNumber(value: $0)) }
            ))
            .labelsHidden()
            .controlSize(.mini)
        case "enum":
            let idx = (d["value"] as? NSNumber)?.intValue ?? 0
            let opts = (d["options"] as? [String]) ?? []
            Menu {
                ForEach(Array(opts.enumerated()), id: \.offset) { i, label in
                    Button {
                        commit(key, NSNumber(value: i))
                    } label: {
                        HStack {
                            Text(label)
                            if i == idx {
                                Spacer()
                                Image(systemName: "checkmark")
                            }
                        }
                    }
                }
            } label: {
                Text(idx >= 0 && idx < opts.count ? opts[idx] : "—")
                    .truncationMode(.middle)
                    .frame(maxWidth: 160, alignment: .trailing)
            }
            .menuStyle(.button)
            .controlSize(.mini)
        case "string":
            let v = d["value"] as? String ?? ""
            LayoutEditorStringField(
                id: "\(modelName).\(key).\(token)",
                initial: v,
                commit: { commit(key, $0 as NSString) }
            )
            .frame(maxWidth: 160, alignment: .trailing)
        default:
            Text("?").foregroundStyle(.tertiary)
        }
    }
}

/// J-5 → J-7 — property editor for a ModelGroup. Settings
/// surface (layout group / camera / layout style / grid size /
/// centre) plus an editable member list (swipe to delete, "+ Add
/// Member" to open the picker sheet).
private struct LayoutEditorGroupPropertiesView: View {
    let groupName: String
    let summary: [String: Any]
    let layoutGroups: [String]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void
    let onRemoveMember: (_ memberName: String) -> Void
    let onAddMember: () -> Void
    let onReorderMembers: (_ newOrder: [String]) -> Void

    /// J-10 — drag-target tracking for the manual VStack member
    /// list. Storing the dragged name + hovered index here lets us
    /// commit reorders without an internal-scroll List (which was
    /// shrinking the visible window to 2 rows inside the outer
    /// property pane's ScrollView).
    @State private var draggingMember: String? = nil
    @State private var dropTargetIndex: Int? = nil

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("Name") { Text(groupName).truncationMode(.middle) }
            row("Type") { Text(summary["displayAs"] as? String ?? "ModelGroup") }
            row("Layout group") { layoutGroupPicker }
            row("Locked") {
                Toggle("", isOn: lockedBinding)
                    .labelsHidden()
                    .controlSize(.mini)
            }

            Divider().padding(.vertical, 2)

            row("Default camera") { defaultCameraPicker }
            row("Layout style")   { layoutStylePicker }
            row("Grid size")      { gridSizeField }
            row("Tag color")      { tagColorPicker }

            Divider().padding(.vertical, 2)

            row("2D centre") {
                Text((summary["centerDefined"] as? Bool ?? false) ? "Custom" : "Auto")
                    .foregroundStyle(.secondary)
            }
            row("Centre X") { numberField(key: "centerX") }
            row("Centre Y") { numberField(key: "centerY") }

            Divider().padding(.vertical, 2)

            membersHeader

            if let members = summary["models"] as? [String], !members.isEmpty {
                // J-10 — manual VStack of rows + per-row drag.
                // Avoids embedding a SwiftUI List inside the
                // property pane's outer ScrollView (the inner List
                // collapses to ~2 rows). The outer ScrollView is
                // already in charge of vertical scrolling, so this
                // grows-to-fit naturally.
                VStack(alignment: .leading, spacing: 0) {
                    ForEach(Array(members.enumerated()), id: \.element) { idx, m in
                        memberRow(name: m,
                                  index: idx,
                                  members: members,
                                  isDropTarget: dropTargetIndex == idx)
                    }
                }
                .padding(.leading, 8)
                .padding(.top, 2)
                .dropDestination(for: String.self) { items, _ in
                    // Drop landing OUTSIDE any specific row → move
                    // to end. Per-row dropDestination already
                    // committed any "drop on row" case before we
                    // get here.
                    guard let dragged = items.first ?? draggingMember,
                          let from = members.firstIndex(of: dragged) else {
                        draggingMember = nil
                        dropTargetIndex = nil
                        return false
                    }
                    var reordered = members
                    reordered.remove(at: from)
                    reordered.append(dragged)
                    onReorderMembers(reordered)
                    draggingMember = nil
                    dropTargetIndex = nil
                    return true
                }
            }
        }
        .font(.caption.monospacedDigit())
    }

    @ViewBuilder
    private var membersHeader: some View {
        HStack(alignment: .firstTextBaseline) {
            Text("Members")
                .foregroundStyle(.secondary)
                .frame(minWidth: 100, alignment: .leading)
            Spacer(minLength: 8)
            Text("\(summary["modelCount"] as? Int ?? 0)")
                .foregroundStyle(.secondary)
            Button {
                onAddMember()
            } label: {
                Image(systemName: "plus.circle.fill")
                    .font(.caption)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Add member")
        }
    }

    /// J-10 — single member row inside the manual VStack list.
    /// Drag handle on the left, delete on the right. Long-press
    /// or grab-the-handle to drag; drop-on-target inserts before
    /// that row. SwiftUI's `.draggable` + per-row
    /// `.dropDestination` handle the data transport.
    @ViewBuilder
    private func memberRow(name: String,
                            index: Int,
                            members: [String],
                            isDropTarget: Bool) -> some View {
        HStack(spacing: 6) {
            Image(systemName: "line.3.horizontal")
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .frame(width: 12)
            memberIcon(for: name)
                .font(.caption2)
                .foregroundStyle(.tertiary)
            Text(name)
                .lineLimit(1)
                .truncationMode(.middle)
                .font(.caption2)
                .foregroundStyle(.secondary)
            Spacer()
            Button {
                onRemoveMember(name)
            } label: {
                Image(systemName: "minus.circle")
                    .font(.caption2)
                    .foregroundStyle(.red)
            }
            .buttonStyle(.plain)
            .accessibilityLabel("Remove \(name)")
        }
        .padding(.vertical, 3)
        .padding(.horizontal, 4)
        .background(rowBackground(name: name, isDropTarget: isDropTarget))
        .draggable(name) {
            // Drag preview — small, opaque pill that mirrors the
            // row visually so the user sees what they're moving.
            HStack(spacing: 4) {
                memberIcon(for: name)
                Text(name).font(.caption2)
            }
            .padding(.horizontal, 6)
            .padding(.vertical, 2)
            .background(.thinMaterial, in: Capsule())
        }
        .dropDestination(for: String.self) { items, _ in
            guard let dragged = items.first,
                  let from = members.firstIndex(of: dragged) else {
                draggingMember = nil
                dropTargetIndex = nil
                return false
            }
            var to = index
            // Inserting after the source position needs the index
            // shifted back by one because we remove first.
            if from < to { to -= 0 }
            if from == to {
                draggingMember = nil
                dropTargetIndex = nil
                return false
            }
            var reordered = members
            reordered.remove(at: from)
            // Clamp in case `to` ended up past the new end.
            let insertAt = min(to, reordered.count)
            reordered.insert(dragged, at: insertAt)
            onReorderMembers(reordered)
            draggingMember = nil
            dropTargetIndex = nil
            return true
        } isTargeted: { targeted in
            if targeted {
                dropTargetIndex = index
            } else if dropTargetIndex == index {
                dropTargetIndex = nil
            }
        }
    }

    @ViewBuilder
    private func rowBackground(name: String, isDropTarget: Bool) -> some View {
        if isDropTarget {
            // Light tint to show where the dragged row will land.
            RoundedRectangle(cornerRadius: 4)
                .fill(Color.accentColor.opacity(0.18))
        } else {
            Color.clear
        }
    }

    @ViewBuilder
    private func memberIcon(for name: String) -> some View {
        // Members can be top-level models, ModelGroups, or
        // submodels (Parent/Sub form). Use the cube icon for
        // simple models and a layered icon for everything else
        // so the eye can scan the list.
        if name.contains("/") {
            Image(systemName: "square.on.square")
        } else {
            Image(systemName: "cube")
        }
    }

    // MARK: - Cells

    @ViewBuilder
    private func row(_ label: String, @ViewBuilder _ content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 100, alignment: .leading)
            Spacer(minLength: 8)
            content()
                .lineLimit(1)
        }
    }

    private func numberField(key: String) -> some View {
        LayoutEditorDoubleField(
            id: "\(groupName).\(key).\(token)",
            initial: doubleVal(key),
            min: nil,
            commit: { v in commit(key, NSNumber(value: v)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    private var lockedBinding: Binding<Bool> {
        Binding(
            get: { summary["locked"] as? Bool ?? false },
            set: { commit("locked", NSNumber(value: $0)) }
        )
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

    /// J-9 — Default Camera picker. Source list comes from the
    /// bridge so any user-defined 3D camera (saved viewpoint) is
    /// included alongside the always-present "2D".
    private var defaultCameraPicker: some View {
        let current = summary["defaultCamera"] as? String ?? "2D"
        let opts = (summary["defaultCameraOptions"] as? [String]) ?? ["2D"]
        return Menu {
            ForEach(opts, id: \.self) { name in
                Button {
                    commit("defaultCamera", name as NSString)
                } label: {
                    HStack {
                        Text(name)
                        if name == current {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Text(current)
                .truncationMode(.middle)
                .frame(maxWidth: 140, alignment: .trailing)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    /// J-9 — Layout Style picker. Options come from the bridge as
    /// {value, label} pairs (xml wire form vs user-facing). If the
    /// stored value isn't in the list (legacy XML), we still show
    /// it but mark it as "Custom: …".
    private var layoutStylePicker: some View {
        let current = summary["layout"] as? String ?? "minimalGrid"
        let rawOpts = (summary["layoutStyleOptions"] as? [[String: String]]) ?? []
        let opts: [(value: String, label: String)] = rawOpts.compactMap {
            guard let v = $0["value"], let l = $0["label"] else { return nil }
            return (v, l)
        }
        let knownLabel = opts.first(where: { $0.value == current })?.label
        return Menu {
            ForEach(opts, id: \.value) { entry in
                Button {
                    commit("layout", entry.value as NSString)
                } label: {
                    HStack {
                        Text(entry.label)
                        if entry.value == current {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
            if knownLabel == nil && !current.isEmpty {
                Divider()
                Text("Currently: \(current)")
            }
        } label: {
            Text(knownLabel ?? (current.isEmpty ? "—" : "Custom: \(current)"))
                .truncationMode(.middle)
                .frame(maxWidth: 160, alignment: .trailing)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    private var gridSizeField: some View {
        LayoutEditorDoubleField(
            id: "\(groupName).gridSize.\(token)",
            initial: Double(summary["gridSize"] as? Int ?? 400),
            min: 1,
            max: nil,
            precision: 0,
            commit: { v in commit("gridSize", NSNumber(value: Int(v))) }
        )
        .frame(maxWidth: 90, alignment: .trailing)
    }

    /// J-9 — Tag color picker. Same `#RRGGBB` bridge as the Models
    /// tab. Empty string → black fallback.
    private var tagColorPicker: some View {
        let hex = summary["tagColor"] as? String ?? ""
        let parsed = Color(hexString: hex) ?? .black
        return HStack(spacing: 6) {
            ColorPicker("", selection: Binding(
                get: { parsed },
                set: { newColor in
                    commit("tagColor", newColor.toHexString() as NSString)
                }
            ), supportsOpacity: false)
                .labelsHidden()
            Text(hex.isEmpty ? "—" : hex)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 80, alignment: .leading)
        }
    }

    private func doubleVal(_ key: String) -> Double {
        (summary[key] as? NSNumber)?.doubleValue ?? 0.0
    }
}

/// J-8 — Editor for the synthetic "2D Background" pseudo-object.
/// The active layout group owns its own background settings; this
/// view edits whichever group the user has picked in the layout-
/// group switcher (top-of-canvas overlay). No transform surface —
/// the desktop's background isn't an object you move around, it's
/// an attribute of the preview.
private struct LayoutEditorBackgroundPropertiesView: View {
    let summary: [String: Any]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void
    let onPickImage: () -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("Name") {
                Text(summary["name"] as? String ?? "2D Background")
                    .truncationMode(.middle)
            }
            row("Type") {
                Text(summary["displayAs"] as? String ?? "2D Background")
                    .foregroundStyle(.secondary)
            }
            row("Layout group") {
                Text(summary["layoutGroup"] as? String ?? "Default")
                    .foregroundStyle(.secondary)
            }
            Divider().padding(.vertical, 2)
            row("Image") {
                HStack(spacing: 6) {
                    Text(imageLabel)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .frame(maxWidth: 110, alignment: .trailing)
                    Button {
                        onPickImage()
                    } label: {
                        Image(systemName: "folder")
                            .font(.caption)
                    }
                    .buttonStyle(.plain)
                    if !imagePath.isEmpty {
                        Button(role: .destructive) {
                            commit("backgroundImage", "" as NSString)
                        } label: {
                            Image(systemName: "xmark.circle")
                                .font(.caption)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            Divider().padding(.vertical, 2)
            row("Brightness") {
                intField(key: "backgroundBrightness", min: 0, max: 100)
            }
            row("Alpha") {
                intField(key: "backgroundAlpha", min: 0, max: 100)
            }
            row("Scale to fit") {
                Toggle("", isOn: Binding(
                    get: { summary["scaleBackgroundImage"] as? Bool ?? false },
                    set: { commit("scaleBackgroundImage", NSNumber(value: $0)) }
                ))
                    .labelsHidden()
                    .controlSize(.mini)
            }

            Divider().padding(.vertical, 4)
            Text("The 2D background is a per-layout-group attribute, not a real view object. It draws behind your models in the 2D preview at the configured brightness / alpha.")
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
        }
        .font(.caption.monospacedDigit())
    }

    private var imagePath: String {
        summary["backgroundImage"] as? String ?? ""
    }

    private var imageLabel: String {
        if imagePath.isEmpty { return "—" }
        return (imagePath as NSString).lastPathComponent
    }

    @ViewBuilder
    private func row(_ label: String, @ViewBuilder _ content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 100, alignment: .leading)
            Spacer(minLength: 8)
            content()
                .lineLimit(1)
        }
    }

    private func intField(key: String, min: Double, max: Double) -> some View {
        LayoutEditorDoubleField(
            id: "background.\(key).\(token)",
            initial: Double((summary[key] as? NSNumber)?.intValue ?? 0),
            min: min,
            max: max,
            precision: 0,
            commit: { newValue in commit(key, NSNumber(value: Int(newValue))) }
        )
        .frame(maxWidth: 90, alignment: .trailing)
    }
}

/// J-12 — Editable properties for a ViewObject. Mirrors the
/// Models tab's collapsible-section layout: Header (always
/// visible) → per-type → Appearance → Dimensions → Size/Location.
/// Per-type section opens by default since that's the unique
/// surface for this object.
private struct LayoutEditorObjectPropertiesView: View {
    @Environment(SequencerViewModel.self) var viewModel
    let objectName: String
    let summary: [String: Any]
    let layoutGroups: [String]
    let token: Int
    let commit: (_ key: String, _ value: Any) -> Void
    let onPickFile: (_ key: String, _ accepting: [UTType]) -> Void

    @State private var expandedTypeProps: Bool = true
    @State private var expandedAppearance: Bool = false
    @State private var expandedDimensions: Bool = false
    @State private var expandedSizeLocation: Bool = false

    var typeKind: String { summary["typeKind"] as? String ?? "other" }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            row("Name") { Text(objectName).truncationMode(.middle) }
            row("Type") { Text(summary["displayAs"] as? String ?? "—") }

            if typeKind != "other" {
                section($expandedTypeProps, title: typeSectionTitle) {
                    perTypeBody
                }
            }

            section($expandedAppearance, title: "Appearance") {
                row("Active") {
                    Toggle("", isOn: boolBinding("active"))
                        .labelsHidden()
                        .controlSize(.mini)
                }
                row("Layout group") { layoutGroupPicker }
            }

            if !((summary["twoPoint"] as? Bool) ?? false) {
                // Two-point screen locations (e.g. Ruler) don't
                // have a meaningful bounding-box width/height/depth
                // — the line's extent comes from its endpoints.
                section($expandedDimensions, title: "Dimensions") {
                    row("Width")  { numberField(key: "width",  min: 0) }
                    row("Height") { numberField(key: "height", min: 0) }
                    row("Depth")  { numberField(key: "depth",  min: 0) }
                }
            }

            section($expandedSizeLocation, title: "Size/Location") {
                if (summary["twoPoint"] as? Bool) ?? false {
                    // J-14 — two-point screen location (Ruler).
                    // Point 1 is the world origin; point 2 is the
                    // absolute coord of the second endpoint.
                    row("Point 1 X") { numberField(key: "p1X") }
                    row("Point 1 Y") { numberField(key: "p1Y") }
                    row("Point 1 Z") { numberField(key: "p1Z") }
                    row("Point 2 X") { numberField(key: "p2X") }
                    row("Point 2 Y") { numberField(key: "p2Y") }
                    row("Point 2 Z") { numberField(key: "p2Z") }
                } else {
                    row("Centre X") { numberField(key: "centerX") }
                    row("Centre Y") { numberField(key: "centerY") }
                    row("Centre Z") { numberField(key: "centerZ") }
                    row("Rotate X") { numberField(key: "rotateX") }
                    row("Rotate Y") { numberField(key: "rotateY") }
                    row("Rotate Z") { numberField(key: "rotateZ") }
                }
                row("Locked") {
                    Toggle("", isOn: boolBinding("locked"))
                        .labelsHidden()
                        .controlSize(.mini)
                }
            }
        }
        .font(.caption.monospacedDigit())
    }

    private var typeSectionTitle: String {
        let t = summary["displayAs"] as? String ?? ""
        return t.isEmpty ? "Object Properties" : t
    }

    // MARK: - Per-type editor bodies

    @ViewBuilder
    private var perTypeBody: some View {
        switch typeKind {
        case "mesh":
            row("Object file") { fileRow(key: "objFile", types: meshTypes) }
            row("Mesh only") {
                Toggle("", isOn: boolBinding("meshOnly"))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            row("Brightness") { intField(key: "brightness", min: 0, max: 100) }
            row("Scale X") { scaleField(key: "scaleX") }
            row("Scale Y") { scaleField(key: "scaleY") }
            row("Scale Z") { scaleField(key: "scaleZ") }
        case "image":
            row("Image") { fileRow(key: "imageFile", types: imageTypes) }
            row("Brightness")   { intField(key: "brightness",   min: 0, max: 100) }
            row("Transparency") { intField(key: "transparency", min: 0, max: 100) }
            row("Scale X") { scaleField(key: "scaleX") }
            row("Scale Y") { scaleField(key: "scaleY") }
            row("Scale Z") { scaleField(key: "scaleZ") }
        case "gridlines":
            row("Spacing")  { intField(key: "gridSpacing", min: 1, max: 10000) }
            row("Width")    { intField(key: "gridWidth",   min: 1, max: 100000) }
            row("Height")   { intField(key: "gridHeight",  min: 1, max: 100000) }
            row("Color")    { gridColorPicker }
            row("Axis")     {
                Toggle("", isOn: boolBinding("hasAxis"))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            row("Point to front") {
                Toggle("", isOn: boolBinding("pointToFront"))
                    .labelsHidden()
                    .controlSize(.mini)
            }
        case "terrain":
            row("Image")     { fileRow(key: "imageFile", types: imageTypes) }
            row("Spacing")   { intField(key: "gridSpacing", min: 1, max: 10000) }
            row("Width")     { intField(key: "gridWidth",   min: 1, max: 100000) }
            row("Depth")     { intField(key: "gridDepth",   min: 1, max: 100000) }
            row("Grid color") { gridColorPicker }
            row("Brightness")   { intField(key: "brightness",   min: 0, max: 100) }
            row("Transparency") { intField(key: "transparency", min: 0, max: 100) }
            row("Hide grid")  {
                Toggle("", isOn: boolBinding("hideGrid"))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            row("Hide image") {
                Toggle("", isOn: boolBinding("hideImage"))
                    .labelsHidden()
                    .controlSize(.mini)
            }
            Divider().padding(.vertical, 2)
            terrainEditControls
        case "ruler":
            row("Units") { unitsPicker }
            row("Length") { doubleField(key: "length", min: 0.0001) }
            Divider().padding(.vertical, 2)
            Text("The Ruler defines real-world scale for the layout — set its length to match a known dimension in your show.")
                .font(.caption2)
                .foregroundStyle(.tertiary)
                .fixedSize(horizontal: false, vertical: true)
        default:
            EmptyView()
        }
    }

    // MARK: - Terrain edit controls (J-13)

    @ViewBuilder
    private var terrainEditControls: some View {
        let editing = viewModel.terrainEditTarget == objectName
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("Edit Heightmap")
                    .foregroundStyle(.secondary)
                    .frame(minWidth: 110, alignment: .leading)
                Spacer()
                Toggle("", isOn: Binding(
                    get: { editing },
                    set: { isOn in
                        viewModel.terrainEditTarget = isOn ? objectName : nil
                    }
                ))
                .labelsHidden()
                .controlSize(.mini)
            }
            if editing {
                row("Direction") {
                    Picker("", selection: Binding(
                        get: { viewModel.terrainEditRaise },
                        set: { viewModel.terrainEditRaise = $0 }
                    )) {
                        Text("Raise").tag(true)
                        Text("Lower").tag(false)
                    }
                    .pickerStyle(.segmented)
                    .frame(maxWidth: 160, alignment: .trailing)
                }
                row("Step") {
                    Slider(value: Binding(
                        get: { Double(viewModel.terrainEditDelta) },
                        set: { viewModel.terrainEditDelta = Float($0) }
                    ), in: 0.1...10.0, step: 0.1)
                    .frame(maxWidth: 140)
                    Text(String(format: "%.1f", viewModel.terrainEditDelta))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 32, alignment: .trailing)
                }
                row("Brush") {
                    Slider(value: Binding(
                        get: { Double(viewModel.terrainEditBrushPoints) },
                        set: { viewModel.terrainEditBrushPoints = CGFloat($0) }
                    ), in: 0...80, step: 1)
                    .frame(maxWidth: 140)
                    Text(viewModel.terrainEditBrushPoints == 0
                          ? "point"
                          : "\(Int(viewModel.terrainEditBrushPoints))pt")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 36, alignment: .trailing)
                }
                Text("Tap the terrain on the canvas to \(viewModel.terrainEditRaise ? "raise" : "lower") the nearest grid point. The brush radius applies a cosine falloff to neighbours; 0 edits a single point.")
                    .font(.caption2)
                    .foregroundStyle(.tertiary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    // MARK: - Section helper

    @ViewBuilder
    private func section<Content: View>(_ expanded: Binding<Bool>,
                                         title: String,
                                         @ViewBuilder _ content: () -> Content) -> some View {
        let body = VStack(alignment: .leading, spacing: 4) { content() }
            .padding(.vertical, 4)
        DisclosureGroup(isExpanded: expanded) {
            body
        } label: {
            Text(title)
                .font(.caption.weight(.semibold))
                .foregroundStyle(.primary)
        }
        .accentColor(.secondary)
    }

    // MARK: - Cells

    @ViewBuilder
    private func row(_ label: String, @ViewBuilder _ content: () -> some View) -> some View {
        HStack(alignment: .firstTextBaseline) {
            Text(label)
                .foregroundStyle(.secondary)
                .frame(minWidth: 110, alignment: .leading)
            Spacer(minLength: 8)
            content()
                .lineLimit(1)
        }
    }

    private func numberField(key: String, min: Double? = nil) -> some View {
        LayoutEditorDoubleField(
            id: "\(objectName).\(key).\(token)",
            initial: doubleVal(key),
            min: min,
            commit: { newValue in commit(key, NSNumber(value: newValue)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    private func doubleField(key: String, min: Double?) -> some View {
        LayoutEditorDoubleField(
            id: "\(objectName).\(key).\(token)",
            initial: doubleVal(key),
            min: min,
            commit: { newValue in commit(key, NSNumber(value: newValue)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    /// J-14 — scale field with higher precision than the default
    /// numberField (scales are typically 0.5, 1.25, etc., not
    /// whole numbers). Allows negative values (flips).
    private func scaleField(key: String) -> some View {
        LayoutEditorDoubleField(
            id: "\(objectName).\(key).\(token)",
            initial: doubleVal(key),
            min: nil,
            max: nil,
            precision: 3,
            commit: { newValue in commit(key, NSNumber(value: newValue)) }
        )
        .frame(maxWidth: 110, alignment: .trailing)
    }

    private func intField(key: String, min: Double, max: Double) -> some View {
        LayoutEditorDoubleField(
            id: "\(objectName).\(key).\(token)",
            initial: Double(intVal(key)),
            min: min,
            max: max,
            precision: 0,
            commit: { newValue in commit(key, NSNumber(value: Int(newValue))) }
        )
        .frame(maxWidth: 90, alignment: .trailing)
    }

    @ViewBuilder
    private func fileRow(key: String, types: [UTType]) -> some View {
        let path = summary[key] as? String ?? ""
        let label = path.isEmpty ? "—" : (path as NSString).lastPathComponent
        HStack(spacing: 6) {
            Text(label)
                .foregroundStyle(.secondary)
                .lineLimit(1)
                .truncationMode(.middle)
                .frame(maxWidth: 110, alignment: .trailing)
            Button {
                onPickFile(key, types)
            } label: {
                Image(systemName: "folder")
                    .font(.caption)
            }
            .buttonStyle(.plain)
            if !path.isEmpty {
                Button(role: .destructive) {
                    commit(key, "" as NSString)
                } label: {
                    Image(systemName: "xmark.circle")
                        .font(.caption)
                }
                .buttonStyle(.plain)
            }
        }
    }

    private var gridColorPicker: some View {
        let hex = summary["gridColor"] as? String ?? ""
        let parsed = Color(hexString: hex) ?? .green
        return HStack(spacing: 6) {
            ColorPicker("", selection: Binding(
                get: { parsed },
                set: { newColor in
                    commit("gridColor", newColor.toHexString() as NSString)
                }
            ), supportsOpacity: false)
                .labelsHidden()
            Text(hex.isEmpty ? "—" : hex)
                .font(.caption2)
                .foregroundStyle(.secondary)
                .frame(maxWidth: 80, alignment: .leading)
        }
    }

    private var unitsPicker: some View {
        // Index → label mapping from RulerObject.h:
        //   0=m, 1=cm, 2=mm, 3=yd, 4=ft, 5=in
        let opts = (summary["unitOptions"] as? [String]) ?? ["m", "cm", "mm", "yd", "ft", "in"]
        let idx = intVal("units")
        return Menu {
            ForEach(Array(opts.enumerated()), id: \.offset) { i, label in
                Button {
                    commit("units", NSNumber(value: i))
                } label: {
                    HStack {
                        Text(label)
                        if i == idx {
                            Spacer()
                            Image(systemName: "checkmark")
                        }
                    }
                }
            }
        } label: {
            Text(idx >= 0 && idx < opts.count ? opts[idx] : "—")
                .truncationMode(.middle)
                .frame(maxWidth: 80, alignment: .trailing)
        }
        .menuStyle(.button)
        .controlSize(.mini)
    }

    private func boolBinding(_ key: String) -> Binding<Bool> {
        Binding(
            get: { summary[key] as? Bool ?? false },
            set: { commit(key, NSNumber(value: $0)) }
        )
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

    private func doubleVal(_ key: String) -> Double {
        (summary[key] as? NSNumber)?.doubleValue ?? 0.0
    }
    private func intVal(_ key: String) -> Int {
        (summary[key] as? NSNumber)?.intValue ?? 0
    }

    // J-12 — UTType lists for the file pickers. `.image` covers
    // most user-friendly bitmap formats; mesh is intentionally
    // narrow (xLights only loads `.obj`).
    private var imageTypes: [UTType] {
        [.png, .jpeg, .tiff, .bmp, .gif, .image]
    }
    private var meshTypes: [UTType] {
        // No native UTType for OBJ — use a filename-extension type.
        [UTType(filenameExtension: "obj") ?? .data]
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
    /// J-6 — optional clamps + precision; backwards compatible
    /// with call sites that only set `min`.
    var max: Double? = nil
    var precision: Int = 2
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
        if let hi = max, v > hi { v = hi }
        commit(v)
    }

    private func format(_ v: Double) -> String {
        // 2 decimals matches desktop's wxPropertyGrid default;
        // overridden via `precision` for int fields (0) or higher-
        // precision floats.
        String(format: "%.\(Swift.max(0, precision))f", v)
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
// Phase J-2 (touch UX) — floating action bar anchored above the
// selected model. Queries the bridge each animation frame so it
// tracks pan / zoom / orbit / drag. Hides when the model is off-
// screen.
//
// First-cut actions: Lock toggle + a "deselect" affordance. Add
// Duplicate / Delete once the bridge supports them.
// Phase J-3 (touch UX) — model-type picker. A sheet (rather than
// an inline Menu) so the list of 18 types is comfortable to
// browse on touch and presents a familiar iOS modal model.
// Cancel via swipe-down or tap outside; selection dismisses
// automatically.
private struct AddModelSheet: View {
    let types: [String]
    let labelFor: (String) -> String
    let onSelect: (String) -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        NavigationStack {
            List(types, id: \.self) { type in
                Button {
                    onSelect(type)
                } label: {
                    HStack {
                        Image(systemName: "plus.rectangle.on.rectangle")
                            .foregroundStyle(.secondary)
                        Text(labelFor(type))
                            .foregroundStyle(.primary)
                    }
                }
            }
            .navigationTitle("Add Model")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { dismiss() }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}

/// J-7 (group CRUD) — name-only sheet for creating a fresh
/// ModelGroup in the active layout group. Member list is populated
/// from the property pane after creation (one-step-at-a-time UX is
/// less error-prone than a sheet with two distinct sections).
private struct NewGroupSheet: View {
    let existingNames: Set<String>
    let onCreate: (String) -> Void
    let onCancel: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name: String = ""
    @FocusState private var nameFocused: Bool

    private var trimmedName: String {
        name.trimmingCharacters(in: .whitespacesAndNewlines)
    }
    private var collision: Bool {
        !trimmedName.isEmpty && existingNames.contains(trimmedName)
    }
    private var canCreate: Bool {
        !trimmedName.isEmpty && !collision
    }

    var body: some View {
        NavigationStack {
            Form {
                Section("Name") {
                    TextField("Group name", text: $name)
                        .focused($nameFocused)
                        .autocorrectionDisabled()
                        .textInputAutocapitalization(.words)
                }
                if collision {
                    Section {
                        Label("\"\(trimmedName)\" is already in use",
                               systemImage: "exclamationmark.triangle.fill")
                            .foregroundStyle(.orange)
                    }
                }
                Section {
                    Text("The new group lands in the active layout group with empty members. Add models from the property pane after it's created.")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }
            .navigationTitle("New Group")
            .navigationBarTitleDisplayMode(.inline)
            .toolbar {
                ToolbarItem(placement: .cancellationAction) {
                    Button("Cancel") { onCancel() }
                }
                ToolbarItem(placement: .confirmationAction) {
                    Button("Create") {
                        onCreate(trimmedName)
                    }
                    .disabled(!canCreate)
                }
            }
            .onAppear { nameFocused = true }
        }
        .presentationDetents([.medium])
    }
}

/// J-7 → J-9 (group CRUD) — multi-select member picker with
/// submodel tree. Top-level rows are models / groups; tapping the
/// chevron expands a row to surface that model's submodels
/// (full "Parent/Sub" names). Tap-to-toggle works at any level
/// so the user can pick "Arch1" or specifically "Arch1/Inner".
/// Existing members of the target group are pre-filtered out at
/// the call site.
private struct AddMemberSheet: View {
    let groupName: String
    /// Top-level model / group names (no submodels yet).
    let candidates: [String]
    /// Names of submodels already-in-the-group. The sheet
    /// dims / hides these inside the tree so the user can't
    /// re-add them.
    let existingMembers: Set<String>
    /// Async submodel lookup: returns the "Parent/Sub" full
    /// names for one parent. Called lazily when the user
    /// expands a row.
    let submodelsFor: (String) -> [String]
    let onAdd: ([String]) -> Void
    let onCancel: () -> Void

    @State private var picked: Set<String> = []
    @State private var expanded: Set<String> = []
    @State private var filter: String = ""

    /// Filtered top-level candidates. Match is OR across model
    /// name and any submodel name (so searching "Inner" surfaces
    /// the parent even if its submodel matches, like the vendor
    /// search does).
    private var filteredCandidates: [String] {
        let q = filter.trimmingCharacters(in: .whitespaces)
        guard !q.isEmpty else { return candidates }
        return candidates.filter { name in
            if name.localizedCaseInsensitiveContains(q) { return true }
            return submodelsFor(name).contains(where: {
                $0.localizedCaseInsensitiveContains(q)
            })
        }
    }

    var body: some View {
        NavigationStack {
            if candidates.isEmpty {
                ContentUnavailableView {
                    Label("Nothing left to add", systemImage: "checkmark.circle")
                } description: {
                    Text("This group already contains every available model.")
                }
                .navigationTitle("Add to \(groupName)")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Done") { onCancel() }
                    }
                }
            } else {
                List {
                    ForEach(filteredCandidates, id: \.self) { name in
                        AddMemberRow(
                            name: name,
                            picked: $picked,
                            expanded: $expanded,
                            existingMembers: existingMembers,
                            submodelsFor: submodelsFor
                        )
                    }
                }
                .listStyle(.plain)
                .searchable(text: $filter,
                            placement: .navigationBarDrawer(displayMode: .always),
                            prompt: "Filter models or submodels")
                .navigationTitle("Add to \(groupName)")
                .navigationBarTitleDisplayMode(.inline)
                .toolbar {
                    ToolbarItem(placement: .cancellationAction) {
                        Button("Cancel") { onCancel() }
                    }
                    ToolbarItem(placement: .confirmationAction) {
                        Button("Add\(picked.isEmpty ? "" : " \(picked.count)")") {
                            onAdd(Array(picked))
                        }
                        .disabled(picked.isEmpty)
                    }
                }
            }
        }
        .presentationDetents([.medium, .large])
    }
}

/// J-9 — single row in AddMemberSheet. Handles the chevron-toggle
/// for expanding submodels and the tap-to-pick gesture for the
/// row itself (model or submodel).
private struct AddMemberRow: View {
    let name: String
    @Binding var picked: Set<String>
    @Binding var expanded: Set<String>
    let existingMembers: Set<String>
    let submodelsFor: (String) -> [String]

    private var submodels: [String] { submodelsFor(name) }
    private var hasSubmodels: Bool { !submodels.isEmpty }
    private var isExpanded: Bool { expanded.contains(name) }
    private var isPicked: Bool { picked.contains(name) }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            row(name: name,
                indent: 0,
                showChevron: hasSubmodels,
                isExpanded: isExpanded,
                isPicked: isPicked,
                already: false,
                onChevron: {
                    if isExpanded {
                        expanded.remove(name)
                    } else {
                        expanded.insert(name)
                    }
                },
                onPick: { toggle(name) })

            if isExpanded {
                ForEach(submodels, id: \.self) { sub in
                    let already = existingMembers.contains(sub)
                    row(name: sub,
                        indent: 1,
                        showChevron: false,
                        isExpanded: false,
                        isPicked: picked.contains(sub),
                        already: already,
                        onChevron: {},
                        onPick: { if !already { toggle(sub) } })
                }
            }
        }
    }

    private func toggle(_ n: String) {
        if picked.contains(n) {
            picked.remove(n)
        } else {
            picked.insert(n)
        }
    }

    @ViewBuilder
    private func row(name: String,
                      indent: Int,
                      showChevron: Bool,
                      isExpanded: Bool,
                      isPicked: Bool,
                      already: Bool,
                      onChevron: @escaping () -> Void,
                      onPick: @escaping () -> Void) -> some View {
        HStack(spacing: 8) {
            if indent > 0 {
                Spacer().frame(width: CGFloat(indent) * 18)
            }
            if showChevron {
                Button {
                    onChevron()
                } label: {
                    Image(systemName: isExpanded ? "chevron.down" : "chevron.right")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .frame(width: 16)
                }
                .buttonStyle(.plain)
            } else {
                Spacer().frame(width: 16)
            }
            Button {
                onPick()
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: isPicked
                           ? "checkmark.circle.fill"
                           : (already ? "checkmark.circle" : "circle"))
                        .foregroundStyle(already
                                          ? AnyShapeStyle(.secondary)
                                          : (isPicked
                                              ? AnyShapeStyle(Color.accentColor)
                                              : AnyShapeStyle(.secondary)))
                    Image(systemName: name.contains("/") ? "square.on.square" : "cube")
                        .foregroundStyle(.tertiary)
                    Text(displayName(name))
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .foregroundStyle(already ? .secondary : .primary)
                    if already {
                        Text("(already a member)")
                            .font(.caption2)
                            .foregroundStyle(.tertiary)
                    }
                    Spacer()
                }
            }
            .buttonStyle(.plain)
            .disabled(already)
        }
        .padding(.vertical, 4)
    }

    private func displayName(_ n: String) -> String {
        // Submodel rows are indented under their parent — show
        // just the trailing portion to keep visual hierarchy clean.
        if let slash = n.firstIndex(of: "/") {
            return String(n[n.index(after: slash)...])
        }
        return n
    }
}

/// Phase J-4 (multi-select) — operations bar shown when 2+ models
/// are selected. Hosts Align ▾, Distribute ▾, Match Size ▾, and
/// Clear. Top-centered like the creation banner.
private struct MultiSelectActionBar: View {
    let selection: Set<String>
    let leader: String?
    let onAlign: (String) -> Void
    let onDistribute: (String) -> Void
    let onMatchSize: (String) -> Void
    let onFlip: (String) -> Void
    let onDuplicate: () -> Void
    let onGroup: () -> Void
    let onClear: () -> Void

    private var canDistribute: Bool { selection.count >= 3 }

    var body: some View {
        HStack(spacing: 10) {
            // Count + leader hint. Leader is shown so the user
            // knows which model the align/match ops target.
            VStack(alignment: .leading, spacing: 0) {
                Text("\(selection.count) selected")
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(.white)
                if let leader = leader, !leader.isEmpty {
                    Text("Leader: \(leader)")
                        .font(.caption2)
                        .foregroundStyle(.white.opacity(0.7))
                        .lineLimit(1)
                        .truncationMode(.middle)
                        .frame(maxWidth: 180, alignment: .leading)
                }
            }

            Divider()
                .frame(height: 24)
                .background(.white.opacity(0.3))

            Menu {
                Button("Align Left")   { onAlign("left") }
                Button("Align Right")  { onAlign("right") }
                Button("Align Top")    { onAlign("top") }
                Button("Align Bottom") { onAlign("bottom") }
                Divider()
                Button("Center Horizontal") { onAlign("centerH") }
                Button("Center Vertical")   { onAlign("centerV") }
                Button("Center Depth")      { onAlign("centerD") }
            } label: {
                Label("Align", systemImage: "align.horizontal.left")
                    .font(.caption.weight(.medium))
            }
            .menuStyle(.borderlessButton)
            .foregroundStyle(.white)

            Menu {
                Button("Distribute Horizontally") { onDistribute("horizontal") }
                Button("Distribute Vertically")   { onDistribute("vertical") }
                Button("Distribute Depth")        { onDistribute("depth") }
            } label: {
                Label("Distribute", systemImage: "rectangle.split.3x1")
                    .font(.caption.weight(.medium))
            }
            .menuStyle(.borderlessButton)
            .foregroundStyle(canDistribute ? .white : .white.opacity(0.4))
            .disabled(!canDistribute)

            Menu {
                Button("Same Width")  { onMatchSize("width") }
                Button("Same Height") { onMatchSize("height") }
                Button("Same Depth")  { onMatchSize("depth") }
                Divider()
                Button("Match All")   { onMatchSize("all") }
            } label: {
                Label("Match Size", systemImage: "rectangle.expand.vertical")
                    .font(.caption.weight(.medium))
            }
            .menuStyle(.borderlessButton)
            .foregroundStyle(.white)

            Menu {
                Button("Flip Horizontal") { onFlip("horizontal") }
                Button("Flip Vertical")   { onFlip("vertical") }
            } label: {
                Label("Flip", systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right")
                    .font(.caption.weight(.medium))
            }
            .menuStyle(.borderlessButton)
            .foregroundStyle(.white)

            Divider()
                .frame(height: 24)
                .background(.white.opacity(0.3))

            Button {
                onDuplicate()
            } label: {
                Label("Duplicate", systemImage: "plus.square.on.square")
                    .font(.caption.weight(.medium))
            }
            .buttonStyle(.plain)
            .foregroundStyle(.white)

            Button {
                onGroup()
            } label: {
                Label("Group", systemImage: "square.stack.3d.up")
                    .font(.caption.weight(.medium))
            }
            .buttonStyle(.plain)
            .foregroundStyle(.white)

            Divider()
                .frame(height: 24)
                .background(.white.opacity(0.3))

            Button {
                onClear()
            } label: {
                Image(systemName: "xmark.circle.fill")
                    .font(.body)
                    .foregroundStyle(.white.opacity(0.8))
            }
            .buttonStyle(.plain)
        }
        .padding(.horizontal, 14)
        .padding(.vertical, 8)
        .background(.black.opacity(0.55), in: Capsule())
        .shadow(radius: 3, y: 2)
    }
}

private struct InlineModelActionBar: View {
    @Environment(SequencerViewModel.self) var viewModel
    let modelName: String
    let summaryToken: Int
    let onPropertyChange: () -> Void
    let onRequestDelete: () -> Void
    let onRequestDuplicate: () -> Void

    var body: some View {
        // `TimelineView(.animation)` refreshes its content every
        // CADisplayLink tick — exactly when we want to recompute
        // the screen anchor (matches Metal redraw cadence).
        // GeometryReader gives us the canvas height for clamping
        // the bottom-anchored bar above the viewport's lower edge.
        GeometryReader { geo in
            TimelineView(.animation) { _ in
                anchoredBar(canvasHeight: geo.size.height)
            }
        }
    }

    private var locked: Bool {
        guard let summary = viewModel.document.modelLayoutSummary(modelName) else {
            return false
        }
        return (summary["locked"] as? NSNumber)?.boolValue ?? false
    }

    @ViewBuilder
    private func anchoredBar(canvasHeight: CGFloat) -> some View {
        if let value = XLightsBridgeBox.bridgeForLayoutEditor()?
                                       .screenAnchorPoint(forModel: modelName,
                                                          for: viewModel.document) {
            let anchor = value.cgPointValue
            HStack(spacing: 6) {
                Button {
                    let newLocked = !locked
                    _ = viewModel.document.setLayoutModelProperty(
                        modelName, key: "locked",
                        value: NSNumber(value: newLocked))
                    onPropertyChange()
                } label: {
                    Label(locked ? "Locked" : "Unlocked",
                          systemImage: locked ? "lock.fill" : "lock.open")
                        .labelStyle(.titleAndIcon)
                        .font(.caption2.weight(.medium))
                }
                .buttonStyle(.borderedProminent)
                .tint(locked ? .red.opacity(0.85) : .blue.opacity(0.85))
                .controlSize(.mini)

                Text(modelName)
                    .font(.caption2.weight(.semibold))
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .frame(maxWidth: 140)

                Button {
                    // Fit Selected — reuses the existing notification
                    // the canvas controls overlay uses for its own
                    // viewfinder button. Quick "where is this model?"
                    // affordance for users whose canvas is panned.
                    NotificationCenter.default.post(name: .previewFitModel,
                                                     object: "LayoutEditor",
                                                     userInfo: ["name": modelName])
                } label: {
                    Image(systemName: "viewfinder")
                        .font(.caption)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)

                Button {
                    onRequestDuplicate()
                } label: {
                    Image(systemName: "plus.square.on.square")
                        .font(.caption)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.blue)

                Button(role: .destructive) {
                    onRequestDelete()
                } label: {
                    Image(systemName: "trash")
                        .font(.caption)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.red)

                Button {
                    viewModel.layoutSelectSingle(nil)
                } label: {
                    Image(systemName: "xmark.circle.fill")
                        .font(.body)
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
            }
            .padding(.horizontal, 8)
            .padding(.vertical, 4)
            .background(.regularMaterial, in: Capsule())
            .shadow(radius: 2, y: 1)
            // Position the bar BELOW the model's screen-bottom
            // anchor with a small offset. Bottom-anchor avoids
            // overlap with the gizmo handles (Y axis arrow, rotate
            // ring, shear puck) which all live at or above the
            // model's top edge. Reads the GeometryReader's height
            // to clamp the bar above the canvas's bottom edge so
            // it stays visible when the model is near the bottom.
            .position(x: anchor.x,
                       y: min(canvasHeight - 28, anchor.y + 30))
            .transition(.opacity)
        }
    }
}

// Phase J-2 (touch UX) — model-name label overlay. Renders one
// small Text view per on-screen model at its projected centre.
// The bridge does a single batched query each frame returning
// `[(name, anchor)]`; off-screen / behind-camera models are
// filtered out at the bridge so SwiftUI never sees them.
//
// Cost: ~one bridge call + N Text views per frame. Cheap for
// typical 10–50 model shows; verify with a 200+ model show if
// needed.
private struct ModelLabelsOverlay: View {
    @Environment(SequencerViewModel.self) var viewModel

    var body: some View {
        TimelineView(.animation(minimumInterval: 1.0 / 30.0)) { _ in
            content
        }
    }

    @ViewBuilder
    private var content: some View {
        if let bridge = XLightsBridgeBox.bridgeForLayoutEditor() {
            let anchors = bridge.modelLabelAnchors(for: viewModel.document)
            ForEach(0..<anchors.count, id: \.self) { i in
                let entry = anchors[i]
                if let name = entry["name"] as? String,
                   let value = entry["anchor"] as? NSValue {
                    let p = value.cgPointValue
                    Text(name)
                        .font(.caption2.weight(.medium))
                        .foregroundStyle(.white)
                        .padding(.horizontal, 4)
                        .padding(.vertical, 1)
                        .background(Color.black.opacity(0.45),
                                    in: RoundedRectangle(cornerRadius: 3))
                        .fixedSize()
                        .position(x: p.x, y: p.y)
                }
            }
        }
    }
}

/// Bridge resolver. The XLMetalBridge instance is created inside
/// PreviewPaneView's coordinator; it doesn't live on the view
/// model. Maintain a tiny registry keyed by preview name so
/// detached views (e.g. the inline action bar) can reach the
/// active bridge without an explicit injection.
@MainActor
enum XLightsBridgeBox {
    private static var byName: [String: WeakBridge] = [:]
    private struct WeakBridge { weak var bridge: XLMetalBridge? }

    static func register(_ bridge: XLMetalBridge, forPreviewName name: String) {
        byName[name] = WeakBridge(bridge: bridge)
    }
    static func unregister(previewName name: String) {
        byName.removeValue(forKey: name)
    }
    static func bridgeForLayoutEditor() -> XLMetalBridge? {
        byName["LayoutEditor"]?.bridge
    }
}

// Phase J-2 (touch UX) — typed target for the long-press
// contextual menu. The `.confirmationDialog` switches on this to
// decide which buttons to show. Mirrors the keys produced by
// `XLMetalBridge.inspectHandleAtScreenPoint:`.
private enum LayoutContextMenuTarget: Equatable {
    case vertex(modelName: String, vertexIndex: Int)
    case segment(modelName: String, segmentIndex: Int, hasCurve: Bool)
    case curveControl(modelName: String, segmentIndex: Int)

    var title: String {
        switch self {
        case .vertex(_, let i):            return "Point \(i + 1)"
        case .segment(_, let i, _):        return "Segment \(i + 1)"
        case .curveControl(_, let i):      return "Curve on Segment \(i + 1)"
        }
    }
}

// Phase J-2 (touch UX) — bottom-anchored tool toolbar.
//
// Replaces desktop's CentreCycle ("tap the orange centre sphere
// to advance axis_tool") with a persistent picker so the active
// tool is always visible + reachable in 44pt touch targets.
// Plus persistent modifier toggles — Uniform / Lock Axis —
// that replace held-key modifiers from the desktop UI.
//
// See `plans/phase-j-touch-ux.md` for the design rationale.
private struct LayoutEditorToolToolbar: View {
    @Bindable var settings: PreviewSettings
    let selectedModelName: String
    let onToolChange: (String) -> Void

    var body: some View {
        HStack(spacing: 16) {
            toolPicker
            Divider().frame(height: 28)
            modifierToggles
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.regularMaterial, in: RoundedRectangle(cornerRadius: 12))
        .shadow(radius: 2, y: 1)
    }

    private var toolPicker: some View {
        // Radio-style. The third position (Scale) cycles through
        // XY_TRANS / Elevate when those make sense for the model
        // type, but the toolbar surface only exposes the three
        // common tools — XY_TRANS / Elevate are still reachable
        // via subclass auto-promotion in `SetActiveHandle`.
        HStack(spacing: 4) {
            toolButton(tool: "translate", label: "Move",  systemImage: "arrow.up.and.down.and.arrow.left.and.right")
            toolButton(tool: "rotate",    label: "Rotate", systemImage: "arrow.triangle.2.circlepath")
            toolButton(tool: "scale",     label: "Scale",  systemImage: "arrow.up.left.and.arrow.down.right")
        }
    }

    private func toolButton(tool: String, label: String, systemImage: String) -> some View {
        let isActive = settings.axisTool == tool
        return Button {
            settings.axisTool = tool
            // Free up axis-specific modifiers when leaving the
            // mode that consumes them, so a stale Lock Axis from
            // a previous tool doesn't haunt the next gesture.
            if tool == "rotate" {
                settings.lockAxis = 0
            }
            onToolChange(tool)
        } label: {
            VStack(spacing: 2) {
                Image(systemName: systemImage).font(.system(size: 18))
                Text(label).font(.caption2)
            }
            .frame(minWidth: 44, minHeight: 36)
            .padding(.horizontal, 4)
        }
        .buttonStyle(.plain)
        .background(isActive ? Color.accentColor.opacity(0.25) : Color.clear,
                    in: RoundedRectangle(cornerRadius: 6))
        .foregroundStyle(isActive ? Color.accentColor : Color.primary)
    }

    @ViewBuilder
    private var modifierToggles: some View {
        // Uniform — only meaningful in Scale mode (mirrors the
        // desktop convention where Shift = aspect-lock during a
        // resize). Hidden otherwise.
        if settings.axisTool == "scale" {
            Toggle(isOn: $settings.uniformModifier) {
                Label("Uniform", systemImage: "lock.fill")
                    .labelStyle(.titleAndIcon)
                    .font(.caption2)
            }
            .toggleStyle(.button)
            .controlSize(.small)
        }

        // Lock Axis — visible in Move + Scale (matches the desktop
        // axis-arrow constrain-to-axis semantics). Hidden in
        // Rotate where the axis is already implicit in the gizmo.
        if settings.axisTool == "translate" || settings.axisTool == "scale" {
            HStack(spacing: 2) {
                Text("Axis:").font(.caption2).foregroundStyle(.secondary)
                axisChip(label: "Free", value: 0)
                axisChip(label: "X",    value: 1)
                axisChip(label: "Y",    value: 2)
                axisChip(label: "Z",    value: 3)
            }
        }

        // Snap — visible in Move (only place where rounding-to-grid
        // matters). Mirrors the existing canvas-controls Snap
        // toggle so users have it without opening the gear menu.
        if settings.axisTool == "translate" {
            Toggle(isOn: $settings.snapToGrid) {
                Label("Snap", systemImage: "square.grid.3x3")
                    .labelStyle(.titleAndIcon)
                    .font(.caption2)
            }
            .toggleStyle(.button)
            .controlSize(.small)
        }
    }

    private func axisChip(label: String, value: Int) -> some View {
        let isActive = settings.lockAxis == value
        return Button {
            settings.lockAxis = value
        } label: {
            Text(label)
                .font(.caption2.weight(.medium))
                .frame(minWidth: 28, minHeight: 24)
        }
        .buttonStyle(.plain)
        .background(isActive ? Color.accentColor.opacity(0.3) : Color.gray.opacity(0.15),
                    in: RoundedRectangle(cornerRadius: 4))
        .foregroundStyle(isActive ? Color.accentColor : Color.primary)
    }
}

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

            // J-2 UX — model-name labels (SwiftUI overlay).
            // Lives in 2D and 3D; bridge filters off-screen
            // models from the per-frame label list.
            Toggle(isOn: $settings.showModelLabels) {
                Text("Labels").font(.caption2)
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
/// J-8 — Hex string ↔ SwiftUI Color helpers for the Tag Color
/// picker. Desktop stores tag colours as `#RRGGBB`; the picker
/// round-trips through these to keep the on-disk representation
/// unchanged. Returns nil for unparseable strings so the call
/// site can fall back to a sensible default (.black).
fileprivate extension Color {
    init?(hexString: String) {
        var s = hexString.trimmingCharacters(in: .whitespacesAndNewlines)
        if s.hasPrefix("#") { s.removeFirst() }
        guard s.count == 6 || s.count == 8,
              let v = UInt32(s, radix: 16) else { return nil }
        let r, g, b, a: Double
        if s.count == 8 {
            r = Double((v >> 24) & 0xff) / 255.0
            g = Double((v >> 16) & 0xff) / 255.0
            b = Double((v >>  8) & 0xff) / 255.0
            a = Double( v        & 0xff) / 255.0
        } else {
            r = Double((v >> 16) & 0xff) / 255.0
            g = Double((v >>  8) & 0xff) / 255.0
            b = Double( v        & 0xff) / 255.0
            a = 1.0
        }
        self.init(.sRGB, red: r, green: g, blue: b, opacity: a)
    }

    /// Emits `#RRGGBB`; alpha is dropped (desktop tag colours don't
    /// carry alpha and we don't want a round-trip to introduce one).
    func toHexString() -> String {
        let ui = UIColor(self)
        var r: CGFloat = 0, g: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
        ui.getRed(&r, green: &g, blue: &b, alpha: &a)
        let ir = Int((r * 255).rounded())
        let ig = Int((g * 255).rounded())
        let ib = Int((b * 255).rounded())
        return String(format: "#%02X%02X%02X", ir, ig, ib)
    }
}

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
