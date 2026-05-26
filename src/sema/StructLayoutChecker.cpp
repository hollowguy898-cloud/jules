#include "sema/StructLayoutChecker.h"

#include <algorithm>
#include <cstdint>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
StructLayoutChecker::StructLayoutChecker(TypeTable& type_table)
    : type_table_(type_table)
{}

// ============================================================================
// fieldSizeBytes - compute the size in bytes of a type
// ============================================================================
uint64_t StructLayoutChecker::fieldSizeBytes(TypeId type) const {
    if (!type) return 0;
    switch (type->getKind()) {
        case TypeKind::Primitive: {
            auto& prim = cast<PrimitiveType>(type);
            if (prim.primitiveKind() == PrimitiveKind::Void) return 0;
            if (prim.primitiveKind() == PrimitiveKind::Bool) return 1;
            return prim.bitWidth() / 8;
        }
        case TypeKind::Enum:      return 4;
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutReference:
        case TypeKind::Allocator:  return 8;
        case TypeKind::Slice:      return 16;
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            uint64_t total = 0;
            for (const auto& f : st.fields()) {
                uint64_t fa = fieldAlignment(f.type);
                total = ((total + fa - 1) / fa) * fa;
                total += fieldSizeBytes(f.type);
            }
            uint64_t ma = fieldAlignment(type);
            total = ((total + ma - 1) / ma) * ma;
            return total;
        }
        case TypeKind::SmartPointer: return 8;
        case TypeKind::Error:        return 16;
        case TypeKind::Fn:           return 8;
        default:                     return 8;
    }
}

// ============================================================================
// fieldAlignment - compute the alignment in bytes of a type
// ============================================================================
uint64_t StructLayoutChecker::fieldAlignment(TypeId type) const {
    if (!type) return 1;
    switch (type->getKind()) {
        case TypeKind::Primitive: {
            auto& prim = cast<PrimitiveType>(type);
            if (prim.primitiveKind() == PrimitiveKind::Void) return 1;
            if (prim.primitiveKind() == PrimitiveKind::Bool) return 1;
            return std::max<uint64_t>(1, prim.bitWidth() / 8);
        }
        case TypeKind::Enum:      return 4;
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutReference:
        case TypeKind::Allocator:  return 8;
        case TypeKind::Slice:      return 8;
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            uint64_t ma = 1;
            for (const auto& f : st.fields()) ma = std::max(ma, fieldAlignment(f.type));
            return ma;
        }
        case TypeKind::SmartPointer: return 8;
        case TypeKind::Error:        return 8;
        case TypeKind::Fn:           return 8;
        default:                     return 8;
    }
}

// ============================================================================
// computePaddedSize - compute struct size with fields in declaration order
// ============================================================================
uint64_t StructLayoutChecker::computePaddedSize(const std::vector<StructFieldDecl>& fields) const {
    uint64_t offset = 0;
    uint64_t max_align = 1;
    for (const auto& f : fields) {
        uint64_t fa = fieldAlignment(f.type);
        max_align = std::max(max_align, fa);
        offset = ((offset + fa - 1) / fa) * fa;
        offset += fieldSizeBytes(f.type);
    }
    // Tail padding to overall struct alignment
    offset = ((offset + max_align - 1) / max_align) * max_align;
    return offset;
}

// ============================================================================
// computeOptimalSize - compute struct size with fields sorted by alignment desc
// ============================================================================
uint64_t StructLayoutChecker::computeOptimalSize(const std::vector<StructFieldDecl>& fields) const {
    // Sort fields by alignment descending (then by size descending as tiebreaker)
    std::vector<StructFieldDecl> sorted = fields;
    std::stable_sort(sorted.begin(), sorted.end(), [this](const StructFieldDecl& a, const StructFieldDecl& b) {
        uint64_t aa = fieldAlignment(a.type);
        uint64_t ab = fieldAlignment(b.type);
        if (aa != ab) return aa > ab;
        return fieldSizeBytes(a.type) > fieldSizeBytes(b.type);
    });

    uint64_t offset = 0;
    uint64_t max_align = 1;
    for (const auto& f : sorted) {
        uint64_t fa = fieldAlignment(f.type);
        max_align = std::max(max_align, fa);
        offset = ((offset + fa - 1) / fa) * fa;
        offset += fieldSizeBytes(f.type);
    }
    // Tail padding
    offset = ((offset + max_align - 1) / max_align) * max_align;
    return offset;
}

// ============================================================================
// check - check all struct declarations in the program
// ============================================================================
void StructLayoutChecker::check(Program& program) {
    for (const auto& tl : program) {
        if (!tl || tl->getKind() != NodeKind::StructDecl) continue;
        auto& sd = cast<StructDecl>(*tl);

        // Only check structs with more than one field
        if (sd.fields().size() <= 1) continue;

        uint64_t current_size = computePaddedSize(sd.fields());
        uint64_t optimal_size = computeOptimalSize(sd.fields());

        if (optimal_size < current_size) {
            LayoutWarning w;
            w.loc = sd.sourceLoc();
            w.struct_name = sd.name();
            w.current_size = current_size;
            w.optimal_size = optimal_size;
            w.message = "Reordering fields will shrink struct '" + sd.name() + "' from "
                       + std::to_string(current_size) + " bytes to "
                       + std::to_string(optimal_size)
                       + " bytes, doubling your CPU cache capacity";
            warnings_.push_back(std::move(w));
        }
    }
}

} // namespace tether
