#include "opt/FieldReorderer.h"

#include <algorithm>
#include <cstdint>
#include <cassert>

namespace tether {

// ============================================================================
// Helper: align up
// ============================================================================
static uint64_t alignUp(uint64_t offset, uint64_t alignment) {
    return (offset + alignment - 1) / alignment * alignment;
}

// ============================================================================
// Constructor
// ============================================================================
FieldReorderer::FieldReorderer(TypeTable& type_table)
    : type_table_(type_table) {}

// ============================================================================
// fieldSizeBytes - return the size in bytes for a given type
// ============================================================================
uint64_t FieldReorderer::fieldSizeBytes(TypeId type) const {
    if (type.isNull()) return 0;

    switch (type->getKind()) {
        case TypeKind::Primitive: {
            const auto& prim = cast<PrimitiveType>(type);
            switch (prim.primitiveKind()) {
                case PrimitiveKind::U8:    return 1;
                case PrimitiveKind::U16:   return 2;
                case PrimitiveKind::U32:   return 4;
                case PrimitiveKind::U64:   return 8;
                case PrimitiveKind::USize: return 8;
                case PrimitiveKind::I8:    return 1;
                case PrimitiveKind::I16:   return 2;
                case PrimitiveKind::I32:   return 4;
                case PrimitiveKind::I64:   return 8;
                case PrimitiveKind::ISize: return 8;
                case PrimitiveKind::F32:   return 4;
                case PrimitiveKind::F64:   return 8;
                case PrimitiveKind::Bool:  return 1;
                case PrimitiveKind::Void:  return 0;
                default:                   return 0;
            }
        }
        case TypeKind::Pointer:
            return 8;
        case TypeKind::Reference:
        case TypeKind::MutReference:
            return 8;
        case TypeKind::Slice:
            return 16; // ptr + len
        case TypeKind::Struct: {
            const auto& st = cast<StructType>(type);
            uint64_t offset = 0;
            uint64_t max_align = 1;
            for (const auto& field : st.fields()) {
                uint64_t fa = fieldAlignment(field.type);
                max_align = std::max(max_align, fa);
                offset = alignUp(offset, fa);
                offset += fieldSizeBytes(field.type);
            }
            // Final struct size is aligned to max field alignment
            offset = alignUp(offset, max_align);
            return offset;
        }
        case TypeKind::Enum:
            return 4;
        case TypeKind::SmartPointer: {
            const auto& sp = cast<SmartPointerType>(type);
            switch (sp.smartPointerKind()) {
                case SmartPointerKind::Box: return 8;
                case SmartPointerKind::Rc:  return 16;
                case SmartPointerKind::Arc: return 16;
                default:                    return 8;
            }
        }
        case TypeKind::Error: {
            const auto& err = cast<ErrorType>(type);
            // success size + 1 (for discriminant flag), aligned
            uint64_t success_size = fieldSizeBytes(err.successType());
            uint64_t success_align = fieldAlignment(err.successType());
            uint64_t offset = alignUp(success_size, success_align);
            offset += 1; // flag byte
            uint64_t max_align = std::max(success_align, uint64_t(1));
            offset = alignUp(offset, max_align);
            return offset;
        }
        case TypeKind::Fn:
            // Function types are not data; treat as pointer-sized
            return 8;
        case TypeKind::Allocator:
            return 8;
        case TypeKind::Poison:
            return 0; // error recovery type, no meaningful size
        default:
            return 0;
    }
}

// ============================================================================
// fieldAlignment - return the alignment requirement for a given type
// ============================================================================
uint64_t FieldReorderer::fieldAlignment(TypeId type) const {
    if (type.isNull()) return 1;

    switch (type->getKind()) {
        case TypeKind::Primitive: {
            const auto& prim = cast<PrimitiveType>(type);
            switch (prim.primitiveKind()) {
                case PrimitiveKind::U8:    return 1;
                case PrimitiveKind::U16:   return 2;
                case PrimitiveKind::U32:   return 4;
                case PrimitiveKind::U64:   return 8;
                case PrimitiveKind::USize: return 8;
                case PrimitiveKind::I8:    return 1;
                case PrimitiveKind::I16:   return 2;
                case PrimitiveKind::I32:   return 4;
                case PrimitiveKind::I64:   return 8;
                case PrimitiveKind::ISize: return 8;
                case PrimitiveKind::F32:   return 4;
                case PrimitiveKind::F64:   return 8;
                case PrimitiveKind::Bool:  return 1;
                case PrimitiveKind::Void:  return 1;
                default:                   return 1;
            }
        }
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutReference:
            return 8;
        case TypeKind::Slice:
            return 8; // slice = ptr + len, alignment of ptr
        case TypeKind::Struct: {
            const auto& st = cast<StructType>(type);
            uint64_t max_align = 1;
            for (const auto& field : st.fields()) {
                max_align = std::max(max_align, fieldAlignment(field.type));
            }
            return max_align;
        }
        case TypeKind::Enum:
            return 4;
        case TypeKind::SmartPointer: {
            const auto& sp = cast<SmartPointerType>(type);
            switch (sp.smartPointerKind()) {
                case SmartPointerKind::Box: return 8;
                case SmartPointerKind::Rc:  return 8;
                case SmartPointerKind::Arc: return 8;
                default:                    return 8;
            }
        }
        case TypeKind::Error: {
            const auto& err = cast<ErrorType>(type);
            uint64_t success_align = fieldAlignment(err.successType());
            return std::max(success_align, uint64_t(1));
        }
        case TypeKind::Fn:
            return 8;
        case TypeKind::Allocator:
            return 8;
        case TypeKind::Poison:
            return 1; // error recovery type, minimal alignment
        default:
            return 1;
    }
}

// ============================================================================
// computeLayoutSize - compute the padded layout size for a given field ordering
// ============================================================================
uint64_t FieldReorderer::computeLayoutSize(
    const std::vector<StructFieldDecl>& fields,
    const std::vector<int>& order) const
{
    uint64_t offset = 0;
    uint64_t max_align = 1;
    for (int idx : order) {
        uint64_t fa = fieldAlignment(fields[idx].type);
        uint64_t fs = fieldSizeBytes(fields[idx].type);
        max_align = std::max(max_align, fa);
        offset = alignUp(offset, fa);
        offset += fs;
    }
    offset = alignUp(offset, max_align);
    return offset;
}

// ============================================================================
// analyze - analyze a single struct and return the reordering result
// ============================================================================
ReorderResult FieldReorderer::analyze(StructDecl& decl) {
    ReorderResult result;
    result.struct_name = decl.name();

    const auto& fields = decl.fields();
    int n = static_cast<int>(fields.size());

    // Build the original order
    result.original_order.resize(n);
    for (int i = 0; i < n; ++i) {
        result.original_order[i] = i;
    }

    // Compute original layout size
    result.original_size = computeLayoutSize(fields, result.original_order);

    // Build sorted order: stable sort by alignment descending
    result.reordered_order.resize(n);
    for (int i = 0; i < n; ++i) {
        result.reordered_order[i] = i;
    }
    std::stable_sort(result.reordered_order.begin(), result.reordered_order.end(),
        [this, &fields](int a, int b) {
            uint64_t align_a = fieldAlignment(fields[a].type);
            uint64_t align_b = fieldAlignment(fields[b].type);
            if (align_a != align_b) return align_a > align_b;
            // Preserve original declaration order for same-alignment fields
            return a < b;
        });

    // Compute reordered layout size
    result.reordered_size = computeLayoutSize(fields, result.reordered_order);

    result.was_improved = result.reordered_size < result.original_size;

    return result;
}

// ============================================================================
// analyzeAll - analyze all structs in the program
// ============================================================================
std::vector<ReorderResult> FieldReorderer::analyzeAll(Program& program) {
    std::vector<ReorderResult> results;
    for (auto& top_level : program) {
        if (top_level->getKind() == NodeKind::StructDecl) {
            auto& decl = cast<StructDecl>(*top_level);
            results.push_back(analyze(decl));
        }
    }
    return results;
}

// ============================================================================
// apply - reorder the fields of a StructDecl according to the result
// ============================================================================
void FieldReorderer::apply(StructDecl& decl, const ReorderResult& result) {
    if (result.reordered_order == result.original_order) {
        return; // No reordering needed
    }

    auto& fields = decl.fields();
    std::vector<StructFieldDecl> new_fields;
    new_fields.reserve(fields.size());
    for (int idx : result.reordered_order) {
        new_fields.push_back(std::move(fields[idx]));
    }
    fields = std::move(new_fields);
}

} // namespace tether
