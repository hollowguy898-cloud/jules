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
// L4: Layout Transformer (deduplicated)
//
// After pipeline unification, L4 only handles packed bitfield detection.
// SoA and hot/cold splitting have been moved to their dedicated
// PreLLVMPass implementations (AoSToSoAPass and HotColdSplitterPass).
//
// This eliminates the duplication that caused the old pipeline to
// clobber metadata: both L4 and the Track 2 passes used to call
// registerStruct() which overwrote the StructMeta, causing data loss.
// ============================================================================

// Helper: check if a TypeId is a bool type
static bool isBoolType(TypeId tid) {
    if (tid.isNull()) return false;
    if (tid->getKind() != TypeKind::Primitive) return false;
    const auto& prim = static_cast<const PrimitiveType&>(*tid);
    return prim.primitiveKind() == PrimitiveKind::Bool;
}

// Helper: compute alignment for a struct
static uint32_t computeStructAlignment(StructDecl& sd) {
    uint32_t max_align = 1;
    for (const auto& field : sd.fields()) {
        if (field.type.isNull()) continue;
        uint64_t sz = (field.type->bitWidth() + 7) / 8;
        uint32_t field_align = static_cast<uint32_t>(std::min(sz, (uint64_t)16));
        if (field_align > max_align) max_align = field_align;
    }
    return max_align;
}

// ============================================================================
// Public entry point — only packed bitfield now
// ============================================================================
void LayoutTransformer::transform(Program& program, TypeTable& type_table, MetadataMap& meta) {
    (void)type_table;

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
                sm.field_is_hot.push_back(false);
            }
            meta.registerStruct(sd.name(), std::move(sm));
        }

        // Apply packed bitfield (the only remaining L4 transform)
        if (detectPackedBitfieldCandidate(sd, meta)) {
            applyPackedBitfield(sd, meta);
        }
    }
}

// ============================================================================
// Packed Bitfield Detection
// ============================================================================
bool LayoutTransformer::detectPackedBitfieldCandidate(StructDecl& sd, MetadataMap& meta) const {
    (void)meta;
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
// Apply Packed Bitfield
// ============================================================================
void LayoutTransformer::applyPackedBitfield(StructDecl& sd, MetadataMap& meta) {
    int consecutive_bools = 0;
    int total_bools = 0;

    for (const auto& field : sd.fields()) {
        if (isBoolType(field.type)) {
            consecutive_bools++;
            total_bools++;
        } else {
            consecutive_bools = 0;
        }
    }

    // Use mergeStructMeta instead of registerStruct (no clobbering)
    MetadataMap::StructMeta smeta;
    smeta.name = sd.name();

    // Only set PackedBitfield if no higher-priority transform exists
    const MetadataMap::StructMeta* existing = meta.getStructMeta(sd.name());
    if (existing && existing->transform.kind != TransformKind::None) {
        // A higher-priority transform (SoA, HotColdSplit, FieldReorder)
        // already applied — just add bitfield info as a complementary detail
        // We don't override the transform kind
        return;
    }

    smeta.transform.kind = TransformKind::PackedBitfield;
    smeta.transform.struct_name = sd.name();
    smeta.transform.detail = "Packed bitfield: " +
                             std::to_string(total_bools) + " bools -> 1 byte";
    meta.mergeStructMeta(sd.name(), std::move(smeta));

    // Set node metadata for the packed bitfield info
    NodeMeta& node_meta = meta.getOrCreate(&sd);
    if (node_meta.layout_transform.kind == TransformKind::None) {
        node_meta.layout_transform.kind = TransformKind::PackedBitfield;
        node_meta.layout_transform.detail = smeta.transform.detail;
        node_meta.layout_transform.struct_name = sd.name();
    }
}

} // namespace tether
