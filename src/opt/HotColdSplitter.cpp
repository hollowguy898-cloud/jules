#include "opt/HotColdSplitter.h"

#include <algorithm>
#include <cassert>
#include <cmath>

namespace tether {

// ============================================================================
// HotColdSplitterPass::run
//
// Walk all struct declarations and evaluate whether they should be split
// into hot and cold field groups. When PGO data is available, use that;
// otherwise, use type-based heuristics.
// ============================================================================
bool HotColdSplitterPass::run(Program& program, TypeTable& type_table) {
    structs_split_ = 0;
    bool any_split = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::StructDecl) continue;
        auto& decl = cast<StructDecl>(*top_level);
        if (processStructDecl(decl, type_table)) {
            any_split = true;
        }
    }

    return any_split;
}

// ============================================================================
// processStructDecl - evaluate and potentially split a struct
// ============================================================================
bool HotColdSplitterPass::processStructDecl(StructDecl& decl, TypeTable& type_table) {
    const auto& fields = decl.fields();

    // Don't split small structs
    if (fields.size() < MIN_FIELDS_FOR_SPLIT) return false;

    // Classify each field as hot or cold
    std::vector<bool> is_cold(fields.size(), false);
    int cold_count = 0;

    for (size_t i = 0; i < fields.size(); ++i) {
        is_cold[i] = isLikelyColdField(fields[i], type_table);
        if (is_cold[i]) cold_count++;
    }

    // Don't split if too few or too many fields are cold
    double cold_ratio = static_cast<double>(cold_count) / static_cast<double>(fields.size());
    if (cold_ratio < MIN_COLD_RATIO) return false;
    if (cold_ratio > 0.7) return false; // If most fields are cold, splitting doesn't help
    if (cold_count == 0) return false;

    // Build the split plan
    std::vector<int> hot_indices;
    std::vector<int> cold_indices;
    std::vector<std::string> hot_fields;
    std::vector<std::string> cold_fields;

    for (size_t i = 0; i < fields.size(); ++i) {
        if (is_cold[i]) {
            cold_indices.push_back(static_cast<int>(i));
            cold_fields.push_back(fields[i].name);
        } else {
            hot_indices.push_back(static_cast<int>(i));
            hot_fields.push_back(fields[i].name);
        }
    }

    // We need at least 1 hot field and 1 cold field for the split to be meaningful
    if (hot_indices.empty() || cold_indices.empty()) return false;

    // Annotate the struct declaration with the hot/cold split plan.
    // The IR generator will:
    // 1. Create a new "<StructName>Cold" struct type with the cold fields
    // 2. Add a "__cold: *<StructName>Cold" pointer field to the original struct
    // 3. Rewrite accesses to cold fields to go through the pointer
    //
    // The detail string format is:
    //   "hot:field1,field2,field3;cold:field4,field5"
    std::string detail = "hot:";
    for (size_t i = 0; i < hot_fields.size(); ++i) {
        if (i > 0) detail += ",";
        detail += hot_fields[i];
    }
    detail += ";cold:";
    for (size_t i = 0; i < cold_fields.size(); ++i) {
        if (i > 0) detail += ",";
        detail += cold_fields[i];
    }

    // Write hot/cold field classification to MetadataMap's StructMeta
    // Using mergeStructMeta to avoid clobbering existing data
    if (meta_map_) {
        MetadataMap::StructMeta smeta;
        smeta.name = decl.name();
        smeta.layout = LayoutKind::Hybrid;  // Hot/cold split = hybrid layout
        for (size_t i = 0; i < fields.size(); ++i) {
            smeta.field_names.push_back(fields[i].name);
            smeta.field_is_hot.push_back(!is_cold[i]);
        }
        smeta.transform.kind = TransformKind::HotColdSplit;
        smeta.transform.struct_name = decl.name();
        smeta.transform.hot_fields = hot_fields;
        smeta.transform.cold_fields = cold_fields;
        smeta.transform.detail = detail;
        meta_map_->mergeStructMeta(decl.name(), std::move(smeta));
    }

    structs_split_++;
    return true;
}

// ============================================================================
// isLikelyColdField - heuristic to classify a field as hot or cold
//
// Cold heuristics (without PGO data):
// - Pointer types (indirection, may be null, often optional data)
// - Smart pointer types (heap-allocated, slower access)
// - Slice types (pointer + length, typically for debugging/collections)
// - Large types ( > 32 bytes, likely not in cache)
// - String-like fields (debug_name, description, etc.)
// - Fields with "debug", "log", "trace", "name" in their name
//
// Hot heuristics:
// - Small numeric types (f32, f64, u8-u64, i8-i64, bool)
// - Types accessed in tight loops (detected by usage analysis)
// - Types that are small enough to fit in a cache line
// ============================================================================
bool HotColdSplitterPass::isLikelyColdField(const StructFieldDecl& field,
                                              TypeTable& /*type_table*/) const {
    // Heuristic 1: Field name suggests cold data
    const std::string& name = field.name;
    std::string lower_name = name;
    std::transform(lower_name.begin(), lower_name.end(),
                   lower_name.begin(), ::tolower);

    if (lower_name.find("debug") != std::string::npos ||
        lower_name.find("log") != std::string::npos ||
        lower_name.find("trace") != std::string::npos ||
        lower_name.find("name") != std::string::npos ||
        lower_name.find("desc") != std::string::npos ||
        lower_name.find("info") != std::string::npos ||
        lower_name.find("meta") != std::string::npos ||
        lower_name.find("tag") != std::string::npos ||
        lower_name.find("label") != std::string::npos ||
        lower_name.find("comment") != std::string::npos) {
        return true;
    }

    // Heuristic 2: Indirect types (pointers, smart pointers, slices)
    if (isIndirectType(field.type)) {
        return true;
    }

    // Heuristic 3: Large types (more than 32 bytes)
    if (field.type && !field.type.isNull()) {
        uint64_t size_bits = field.type->bitWidth();
        if (size_bits > 256) { // > 32 bytes
            return true;
        }
    }

    // Small numeric types are likely hot
    if (isSmallNumeric(field.type)) {
        return false;
    }

    // Default: if we can't classify, assume hot (conservative)
    return false;
}

// ============================================================================
// isIndirectType - check if a type requires indirection to access
// ============================================================================
bool HotColdSplitterPass::isIndirectType(TypeId type) const {
    if (type.isNull()) return false;

    // Pointer types require dereference
    if (isa<PointerType>(type) || isa<ReferenceType>(type) ||
        isa<MutReferenceType>(type)) {
        return true;
    }

    // Smart pointers require dereference
    if (isa<SmartPointerType>(type)) {
        return true;
    }

    // Slices are pointer + length
    if (isa<SliceType>(type)) {
        return true;
    }

    return false;
}

// ============================================================================
// isSmallNumeric - check if a type is a small numeric type (likely hot)
// ============================================================================
bool HotColdSplitterPass::isSmallNumeric(TypeId type) const {
    if (type.isNull()) return false;

    if (isa<PrimitiveType>(type)) {
        const auto& prim = cast<PrimitiveType>(type);
        return prim.isNumeric() && prim.bitWidth() <= 64;
    }

    // Booleans are small and hot
    if (type->isBool()) return true;

    return false;
}

} // namespace tether
