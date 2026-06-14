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

#include <list>
#include <string>

// Abstract node in the import-mapping tree, exposing only the surface that
// AutoMapper needs. The desktop's xLightsImportModelNode (a wxDataView tree
// store node) implements this interface; the iPad will provide its own
// concrete type backed by SwiftUI/ObjC++ state. AutoMapper itself is wx-free
// and works against this interface.
//
// Three node levels are represented in the same class via the
// (model, strand, node) triple. A bare model has empty strand and node; a
// strand or submodel has a non-empty strand and empty node; a node-level
// item has all three populated.
class ImportMappingNode {
public:
    virtual ~ImportMappingNode() = default;

    virtual const std::string& GetCoreModel() const = 0;
    virtual const std::string& GetCoreStrand() const = 0;
    virtual const std::string& GetCoreNode() const = 0;

    // Source name currently mapped onto this destination node. Empty if
    // unmapped.
    virtual const std::string& GetMapping() const = 0;

    virtual std::list<std::string> GetAliases() const = 0;

    virtual bool IsGroup() const = 0;

    // Simplified model "class" (e.g. "SingingFace", "SpiralTree") as
    // computed by Model::DetermineClass. Empty if not applicable/unknown.
    // Default empty so hosts that don't compute this aren't forced to
    // implement it.
    virtual std::string GetModelClass() const { return std::string(); }

    // Resolved model "type" tag - the DisplayAsType string (e.g. "Arches",
    // "Tree 360", "Matrix"). Empty if not applicable/unknown. Default empty
    // so hosts that don't compute this aren't forced to implement it.
    virtual std::string GetModelType() const { return std::string(); }

    // True if this is a real singing prop: a Custom model with at least one
    // faceInfo entry of Type="NodeRange" that has actual Mouth-*/Eye-*/etc
    // node ranges defined (not just an empty/unused face). Default false so
    // hosts that don't compute this aren't forced to implement it.
    virtual bool IsSingingProp() const { return false; }

    // True if this is a "floodlight" model: a non-group, single-line model
    // with one node per string. Default false so hosts that don't compute
    // this aren't forced to implement it.
    virtual bool IsFloodlight() const { return false; }

    // True if this is a ModelGroup whose members are all floodlight models
    // (IsFloodlight). Default false so hosts that don't compute this aren't
    // forced to implement it.
    virtual bool IsFloodGroup() const { return false; }

    // Structural size info for Custom models - lit node count and the
    // CustomWidth/CustomHeight grid dimensions. Default 0 (unknown) so hosts
    // that don't compute this aren't forced to implement it. Used by
    // AutoMapper::RunCustomDimensionMatch (QuikMap Phase 96) to prefer a
    // same-type Custom vendor model with a similar node count/shape over a
    // blind first-available pairing.
    virtual int GetNodeCount() const { return 0; }
    virtual int GetWidth() const { return 0; }
    virtual int GetHeight() const { return 0; }
    virtual int GetStrandCount() const { return 0; }

    // True if QuikMap has determined this destination root (and its
    // subtree) should be skipped entirely - e.g. a DMX model or a group
    // containing DMX props (QuikMap Phase 0, see AutoMapper::RunSkipDMX).
    // Default false/no-op so hosts that don't need this aren't forced to
    // implement it.
    virtual bool IsSkipped() const { return false; }
    virtual void SetSkipped(bool skipped) {}

    // Composite "model[/strand[/node]]" name. Already exists on
    // xLightsImportModelNode.
    virtual std::string GetModelName() const = 0;

    // Record a mapping. `mappingModelType` is one of the model-type strings
    // (e.g. "Strand", "Node", "SubModel", "Unknown") that downstream UI may
    // surface in its column.
    virtual void Map(const std::string& mapTo, const std::string& mappingModelType) = 0;

    // Short human-readable label for how this mapping was chosen (e.g.
    // "Exact", "Alias", "Community", "Submodel", "Fuzzy", "Catchall",
    // "Manual"). Surfaced in the desktop UI's "Mapping Rule" column.
    // Default no-op/empty so hosts that don't need this (e.g. a future iPad
    // implementation) aren't forced to implement it.
    virtual void SetMappingRule(const std::string& rule) {}
    virtual std::string GetMappingRule() const { return std::string(); }

    virtual unsigned int GetChildCount() const = 0;
    virtual ImportMappingNode* GetNthChild(unsigned int n) = 0;
};

// A flat source candidate from the incoming sequence. The dialog (or iPad
// equivalent) builds the available list before calling AutoMapper, lowering
// + trimming the canonical form once so the matcher loop doesn't repeat
// work. `displayName` preserves the caller's preferred casing — that's the
// string written into ImportMappingNode::Map. `modelType` is the resolved
// model-type tag (e.g. "Model", "ModelGroup", "SubModel") used when this
// source maps onto a destination model — caller resolves it from the
// layout once instead of having AutoMapper call back through wx code.
struct AvailableSource {
    std::string displayName;
    std::string canonicalName;
    std::string modelType;
    // Simplified model "class" (e.g. "SingingFace", "SpiralTree") for
    // bare-model entries, as computed by Model::DetermineClass. Empty for
    // strand/node entries or when unknown.
    std::string modelClass;
    // Resolved model "type" tag - the DisplayAsType string (e.g. "Arches",
    // "Tree 360", "Matrix") for bare-model entries, as recorded on the
    // ImportChannel. Empty for strand/node entries or when unknown.
    std::string displayType;
    // True if this bare-model entry is a real singing prop (see
    // ImportMappingNode::IsSingingProp). False for strand/node entries.
    bool isSingingProp{ false };
    // True if this bare-model entry is a floodlight (see
    // ImportMappingNode::IsFloodlight). False for strand/node entries.
    bool isFloodlight{ false };
    // True if this is a ModelGroup whose members are all floodlights (see
    // ImportMappingNode::IsFloodGroup).
    bool isFloodGroup{ false };
    bool selected{ false };
    // Lightweight per-source timeline summary surfaced in the import mapping UI
    // (the iPad analogue of the desktop per-row timeline column). `effectCount`
    // is the number of source effects under this entry; `durationMs` is the end
    // time of its last effect. Both default 0 for readers that don't compute
    // them.
    int effectCount{ 0 };
    int durationMs{ 0 };
    // Structural size info for Custom models, mirroring
    // ImportMappingNode::GetNodeCount/GetWidth/GetHeight/GetStrandCount.
    // Empty/zero for strand/node entries or when unknown. See
    // AutoMapper::RunCustomDimensionMatch (QuikMap Phase 96).
    int nodeCount{ 0 };
    int width{ 0 };
    int height{ 0 };
    int strandCount{ 0 };
};
