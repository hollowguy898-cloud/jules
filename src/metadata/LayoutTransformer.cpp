#include "metadata/LayoutTransformer.h"
#include "metadata/MetaTypes.h"
#include "ast/AST.h"
#include "sema/Type.h"
#include "parser/Parser.h"

#include <algorithm>
#include <cctype>
#include <set>

namespace tether {

// ============================================================================
// L4: Layout Transformer Implementation
// ============================================================================

// Helper: check if a TypeId is a primitive numeric type (f32, f64, i8-i64, u8-u64, bool)
static bool isPrimitiveNumeric(TypeId tid) {
    if (tid.isNull()) return false;
    if (tid->getKind() != TypeKind::Primitive) return false;
    const auto& prim = static_cast<const PrimitiveType&>(*tid);
    auto kind = prim.primitiveKind();
    // All primitives except Void are numeric for our purposes
    return kind != PrimitiveKind::Void;
}

// Helper: check if a TypeId is a bool type
static bool isBoolType(TypeId tid) {
    if (tid.isNull()) return false;
    if (tid->getKind() != TypeKind::Primitive) return false;
    const auto& prim = static_cast<const PrimitiveType&>(*tid);
    return prim.primitiveKind() == PrimitiveKind::Bool;
}

// Helper: check if a TypeId is a pointer-like type (pointer, reference, mut ref)
static bool isPointerType(TypeId tid) {
    if (tid.isNull()) return false;
    auto kind = tid->getKind();
    return kind == TypeKind::Pointer ||
           kind == TypeKind::Reference ||
           kind == TypeKind::MutReference ||
           kind == TypeKind::SmartPointer;
}

// Helper: check if a TypeId is a slice type
static bool isSliceType(TypeId tid) {
    if (tid.isNull()) return false;
    return tid->getKind() == TypeKind::Slice;
}

// Helper: check if a TypeId is a struct type larger than 32 bytes
static bool isLargeStructType(TypeId tid) {
    if (tid.isNull()) return false;
    if (tid->getKind() == TypeKind::Struct) {
        const auto& st = static_cast<const StructType&>(*tid);
        // Struct bitWidth is the sum of all field widths
        return st.bitWidth() > 32 * 8; // > 32 bytes = > 256 bits
    }
    return false;
}

// Helper: check if a field name suggests cold/debug/metadata data
static bool isColdFieldName(const std::string& name) {
    // Convert to lowercase for comparison
    std::string lower = name;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });

    static const std::set<std::string> cold_names = {
        "debug", "name", "desc", "info", "meta", "tag", "label", "comment"
    };
    return cold_names.count(lower) > 0;
}

// Helper: compute the size of a type in bytes
static uint64_t typeSizeBytes(TypeId tid) {
    if (tid.isNull()) return 0;
    return (tid->bitWidth() + 7) / 8;
}

// Helper: compute alignment for a struct
static uint32_t computeStructAlignment(StructDecl& sd) {
    uint32_t max_align = 1;
    for (const auto& field : sd.fields()) {
        if (field.type.isNull()) continue;
        uint64_t sz = typeSizeBytes(field.type);
        // Alignment is typically the natural size of the type, capped at 16
        uint32_t field_align = static_cast<uint32_t>(std::min(sz, (uint64_t)16));
        if (field_align > max_align) max_align = field_align;
        // Pointers/references are 8-byte aligned
        if (isPointerType(field.type) || isSliceType(field.type)) {
            if (8 > max_align) max_align = 8;
        }
    }
    return max_align;
}

// ============================================================================
// Public entry point
// ============================================================================

void LayoutTransformer::transform(Program& program, TypeTable& type_table, MetadataMap& meta) {
    (void)type_table; // Reserved for future type-aware transforms
    // Iterate over all StructDecls in the program and apply layout transforms
    for (auto& decl : program) {
        if (!decl) continue;
        if (decl->getKind() != NodeKind::StructDecl) continue;

        auto& sd = static_cast<StructDecl&>(*decl);

        // Register struct metadata if not already present
        if (!meta.getStructMeta(sd.name())) {
            MetadataMap::StructMeta sm;
            sm.name = sd.name();
            sm.layout = LayoutKind::AoS;
            sm.alignment = sd.alignment();
            for (const auto& field : sd.fields()) {
                sm.field_names.push_back(field.name);
                sm.field_is_hot.push_back(false); // will be filled in later
            }
            meta.registerStruct(sd.name(), std::move(sm));
        }

        // Check candidates in priority order. A struct can receive multiple
        // transforms, but we apply the most impactful one first.

        // 1. Packed bitfield (most localized transform)
        if (detectPackedBitfieldCandidate(sd, meta)) {
            applyPackedBitfield(sd, meta);
        }

        // 2. SoA transform (changes the whole data layout)
        if (detectSoACandidate(sd, meta)) {
            applySoATransform(sd, meta);
        }

        // 3. Hot/cold split (can coexist with SoA — hybrid SoA + hot/cold)
        if (detectHotColdSplitCandidate(sd, meta)) {
            applyHotColdSplit(sd, meta);
        }
    }
}

// ============================================================================
// SoA Detection
// ============================================================================

bool LayoutTransformer::detectSoACandidate(StructDecl& sd, MetadataMap& meta) const {
    // Skip structs already marked as SoA
    if (sd.isSoA()) {
        return false;
    }

    // Already has a SoA transform applied
    const MetadataMap::StructMeta* existing = meta.getStructMeta(sd.name());
    if (existing && existing->layout == LayoutKind::SoA) {
        return false;
    }

    // Must have >= 2 fields of primitive types
    int primitive_count = 0;
    for (const auto& field : sd.fields()) {
        if (isPrimitiveNumeric(field.type)) {
            primitive_count++;
        }
    }
    if (primitive_count < 2) {
        return false;
    }

    // Must be accessed via linear traversal in at least one hot loop.
    // Check the metadata map for access patterns that reference this struct.
    int fields_in_loops = countFieldsAccessedInLoops(sd.name(), meta);
    if (fields_in_loops < 2) {
        return false;
    }

    // Fields must be accessed independently (not always all at once).
    // For now, we check if the access pattern count is less than the
    // total primitive field count — if fields were always accessed together,
    // there would be one access pattern touching all fields.
    // A more sophisticated check would analyze co-occurrence in the same
    // loop body. We approximate: if at least 2 fields are accessed in loops
    // but not all of them, they are accessed independently.
    //
    // For the initial implementation, we accept any struct with >= 2
    // primitive fields accessed in loops as a SoA candidate.
    return true;
}

int LayoutTransformer::countFieldsAccessedInLoops(
    const std::string& struct_name, MetadataMap& meta) const {
    // Check struct metadata for access patterns
    const MetadataMap::StructMeta* sm = meta.getStructMeta(struct_name);

    // Count fields that have been identified as accessed in the metadata.
    // The L3 layer (MemoryTopologyAnalyzer) writes access patterns into
    // node metadata. We also check if any NodeMeta entries reference
    // this struct's fields.

    // For now, we rely on the StructMeta's field_is_hot array.
    // If the struct was registered with no access data, we return 0.
    // The L3 analyzer would have populated this before L4 runs.
    if (!sm) return 0;

    int count = 0;
    for (bool hot : sm->field_is_hot) {
        if (hot) count++;
    }

    // If no fields are explicitly marked hot, but we have linear access
    // metadata in the struct, count all primitive fields as loop-accessed.
    // This handles the case where L3 detected the struct is traversed linearly.
    if (count == 0) {
        // Check if any metadata in the system references this struct's
        // variables with linear traversal.
        // We scan the struct's metadata for any indication of linear access.
        // For simplicity, if the struct has at least 2 primitive fields
        // and exists in the metadata, we assume it's accessed in loops
        // when other metadata entries show loop activity.
        //
        // A practical heuristic: check if there's any node in the program
        // that has linear access patterns referencing this struct type.
        // Since we can't iterate the node metadata map directly from here,
        // we check if the StructMeta has been registered with traversal info.
        // If count is still 0, we fall back to checking whether the
        // struct metadata has any non-default state.
        if (sm->alignment > 0) {
            // The struct was explicitly registered with alignment info,
            // suggesting it's been analyzed. Count primitive fields.
            for (const auto& field_name : sm->field_names) {
                (void)field_name; // We'd need to look up the type; skip for now
            }
        }
    }

    return count;
}

// ============================================================================
// Hot/Cold Split Detection
// ============================================================================

bool LayoutTransformer::detectHotColdSplitCandidate(StructDecl& sd, MetadataMap& meta) const {
    // Must have >= 4 fields
    if (sd.fieldCount() < 4) {
        return false;
    }

    // Use computeFieldHotness to classify fields
    std::vector<bool> hotness = computeFieldHotness(sd, meta);

    int hot_count = 0;
    int cold_count = 0;
    for (bool h : hotness) {
        if (h) hot_count++;
        else cold_count++;
    }

    // Must have both hot and cold fields to be a split candidate
    if (hot_count == 0 || cold_count == 0) {
        return false;
    }

    return true;
}

// ============================================================================
// Packed Bitfield Detection
// ============================================================================

bool LayoutTransformer::detectPackedBitfieldCandidate(StructDecl& sd, MetadataMap& meta) const {
    (void)meta; // Bitfield detection is purely structural (field types only)
    // Must have >= 2 consecutive bool fields
    int consecutive_bools = 0;
    bool found_pair = false;

    for (const auto& field : sd.fields()) {
        if (isBoolType(field.type)) {
            consecutive_bools++;
            if (consecutive_bools >= 2) {
                found_pair = true;
                break;
            }
        } else {
            consecutive_bools = 0;
        }
    }

    return found_pair;
}

// ============================================================================
// Apply SoA Transform
// ============================================================================

void LayoutTransformer::applySoATransform(StructDecl& sd, MetadataMap& meta) {
    // Write to the struct's StructMeta
    MetadataMap::StructMeta* sm = meta.getStructMetaMutable(sd.name());
    if (!sm) {
        // Register if not present
        MetadataMap::StructMeta new_sm;
        new_sm.name = sd.name();
        new_sm.layout = LayoutKind::SoA;
        for (const auto& field : sd.fields()) {
            new_sm.field_names.push_back(field.name);
            new_sm.field_is_hot.push_back(false);
        }
        sm = meta.getStructMetaMutable(sd.name());
        if (!sm) {
            meta.registerStruct(sd.name(), std::move(new_sm));
            sm = meta.getStructMetaMutable(sd.name());
        }
    }

    sm->layout = LayoutKind::SoA;
    sm->transform.kind = TransformKind::SoATransform;
    sm->transform.struct_name = sd.name();
    sm->transform.detail = "Auto-detected SoA: fields accessed independently in linear loop";

    // Classify fields: primitive = hot (benefit from SoA), non-primitive = cold
    sm->transform.hot_fields.clear();
    sm->transform.cold_fields.clear();

    for (const auto& field : sd.fields()) {
        if (isPrimitiveNumeric(field.type)) {
            sm->transform.hot_fields.push_back(field.name);
        } else {
            sm->transform.cold_fields.push_back(field.name);
        }
    }

    // Update field hotness
    for (size_t i = 0; i < sd.fields().size() && i < sm->field_is_hot.size(); ++i) {
        sm->field_is_hot[i] = isPrimitiveNumeric(sd.fields()[i].type);
    }

    // Compute alignment
    uint32_t align = sd.alignment();
    if (align == 0) {
        align = computeStructAlignment(sd);
    }

    // Also set the struct node's NodeMeta
    NodeMeta& node_meta = meta.getOrCreate(&sd);
    node_meta.layout = LayoutKind::SoA;
    node_meta.alignment = align;
    node_meta.llvm_meta.align = align;
}

// ============================================================================
// Apply Hot/Cold Split
// ============================================================================

void LayoutTransformer::applyHotColdSplit(StructDecl& sd, MetadataMap& meta) {
    // Get or create struct metadata
    MetadataMap::StructMeta* sm = meta.getStructMetaMutable(sd.name());
    if (!sm) {
        MetadataMap::StructMeta new_sm;
        new_sm.name = sd.name();
        for (const auto& field : sd.fields()) {
            new_sm.field_names.push_back(field.name);
            new_sm.field_is_hot.push_back(false);
        }
        meta.registerStruct(sd.name(), std::move(new_sm));
        sm = meta.getStructMetaMutable(sd.name());
    }

    // Compute field hotness
    std::vector<bool> hotness = computeFieldHotness(sd, meta);

    sm->layout = LayoutKind::Hybrid;
    sm->transform.kind = TransformKind::HotColdSplit;
    sm->transform.struct_name = sd.name();

    // Classify fields into hot/cold
    sm->transform.hot_fields.clear();
    sm->transform.cold_fields.clear();

    int hot_count = 0;
    int cold_count = 0;

    for (size_t i = 0; i < sd.fields().size(); ++i) {
        const auto& field = sd.fields()[i];
        if (i < hotness.size() && hotness[i]) {
            sm->transform.hot_fields.push_back(field.name);
            hot_count++;
        } else {
            sm->transform.cold_fields.push_back(field.name);
            cold_count++;
        }
    }

    // Update field_is_hot in struct metadata
    for (size_t i = 0; i < hotness.size() && i < sm->field_is_hot.size(); ++i) {
        sm->field_is_hot[i] = hotness[i];
    }

    sm->transform.detail = "Hot/cold split: " + std::to_string(hot_count) +
                           " hot fields, " + std::to_string(cold_count) + " cold fields";

    // Set node metadata
    NodeMeta& node_meta = meta.getOrCreate(&sd);
    node_meta.layout = LayoutKind::Hybrid;
    uint32_t align = sd.alignment();
    if (align == 0) align = computeStructAlignment(sd);
    node_meta.alignment = align;
    node_meta.llvm_meta.align = align;
}

// ============================================================================
// Apply Packed Bitfield
// ============================================================================

void LayoutTransformer::applyPackedBitfield(StructDecl& sd, MetadataMap& meta) {
    // Count consecutive bool fields
    int consecutive_bools = 0;
    int max_consecutive = 0;
    int total_bools = 0;

    for (const auto& field : sd.fields()) {
        if (isBoolType(field.type)) {
            consecutive_bools++;
            total_bools++;
            if (consecutive_bools > max_consecutive) {
                max_consecutive = consecutive_bools;
            }
        } else {
            consecutive_bools = 0;
        }
    }

    // Get or create struct metadata
    MetadataMap::StructMeta* sm = meta.getStructMetaMutable(sd.name());
    if (!sm) {
        MetadataMap::StructMeta new_sm;
        new_sm.name = sd.name();
        for (const auto& field : sd.fields()) {
            new_sm.field_names.push_back(field.name);
            new_sm.field_is_hot.push_back(false);
        }
        meta.registerStruct(sd.name(), std::move(new_sm));
        sm = meta.getStructMetaMutable(sd.name());
    }

    // Only set PackedBitfield if it hasn't been overridden by a more
    // significant transform (SoA or HotColdSplit).
    // If SoA or HotColdSplit was already applied, we keep that but still
    // add bitfield information.
    if (sm->transform.kind == TransformKind::None) {
        sm->transform.kind = TransformKind::PackedBitfield;
    }

    sm->transform.struct_name = sd.name();
    sm->transform.detail = "Packed bitfield: " +
                           std::to_string(total_bools) + " bools -> 1 byte";

    // Set node metadata for the packed bitfield info
    NodeMeta& node_meta = meta.getOrCreate(&sd);
    if (node_meta.layout_transform.kind == TransformKind::None) {
        node_meta.layout_transform.kind = TransformKind::PackedBitfield;
        node_meta.layout_transform.detail = sm->transform.detail;
        node_meta.layout_transform.struct_name = sd.name();
    }
}

// ============================================================================
// Compute Field Hotness
// ============================================================================

std::vector<bool> LayoutTransformer::computeFieldHotness(StructDecl& sd, MetadataMap& meta) const {
    std::vector<bool> hotness(sd.fields().size(), false);

    for (size_t i = 0; i < sd.fields().size(); ++i) {
        const auto& field = sd.fields()[i];
        TypeId ft = field.type;

        // String-like field names are cold
        if (isColdFieldName(field.name)) {
            hotness[i] = false;
            continue;
        }

        // Pointer/reference types are cold
        if (isPointerType(ft)) {
            hotness[i] = false;
            continue;
        }

        // Slice types are cold
        if (isSliceType(ft)) {
            hotness[i] = false;
            continue;
        }

        // Large struct types (>32 bytes) are cold
        if (isLargeStructType(ft)) {
            hotness[i] = false;
            continue;
        }

        // Small numeric types (f32, f64, i8-i64, u8-u64, bool) are hot
        if (isPrimitiveNumeric(ft)) {
            hotness[i] = true;
            continue;
        }

        // Default: cold (unknown types, enums, etc.)
        hotness[i] = false;
    }

    // Also consult existing metadata for field hotness from L3 analysis
    const MetadataMap::StructMeta* sm = meta.getStructMeta(sd.name());
    if (sm) {
        for (size_t i = 0; i < sm->field_is_hot.size() && i < hotness.size(); ++i) {
            // If L3 has explicitly marked a field as hot, respect that
            if (sm->field_is_hot[i]) {
                hotness[i] = true;
            }
        }
    }

    return hotness;
}

} // namespace tether
