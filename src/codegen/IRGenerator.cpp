#include "codegen/IRGenerator.h"

#include <cassert>
#include <cctype>
#include <cmath>
#include <cstring>
#include <algorithm>
#include <iomanip>
#include <limits>
#include <sstream>
#include <set>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
IRGenerator::IRGenerator(const std::vector<std::unique_ptr<TopLevel>>& program,
                         TypeTable& type_table,
                         MetadataMap* meta_map)
    : program_(program)
    , type_table_(type_table)
    , meta_map_(meta_map)
{}

// ============================================================================
// zeroConstant – return the LLVM IR zero constant for a given LLVM type
// ============================================================================
std::string IRGenerator::zeroConstant(const std::string& llvm_type) const {
    if (llvm_type == "double") return "0.0";
    if (llvm_type == "float")  return "0.0";
    if (llvm_type == "void")   return "";  // caller should use "ret void"
    return "0";
}

// ============================================================================
// sanitizeName – replace characters illegal in LLVM identifiers with '_'
// ============================================================================
std::string IRGenerator::sanitizeName(const std::string& name) const {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$') {
            result += c;
        } else {
            result += '_';
        }
    }
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) {
        result = "_" + result;
    }
    if (result.empty()) result = "_";
    return result;
}

// ============================================================================
// isAggregateType
// ============================================================================
bool IRGenerator::isAggregateType(TypeId type) const {
    if (!type) return false;
    switch (type->getKind()) {
        case TypeKind::Struct:
        case TypeKind::Slice:
        case TypeKind::Array:       // Array is aggregate { ptr, i64 }
            return true;
        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            return sp.smartPointerKind() != SmartPointerKind::Box;
        }
        case TypeKind::Error: {
            // Niched error types (pointer niche or integer niche) are NOT
            // aggregates — they're single scalar values. Only the struct
            // fallback representation is an aggregate.
            auto& err = cast<ErrorType>(type);
            TypeId succ = err.successType();
            if (succ && (isa<PointerType>(succ) || isa<ReferenceType>(succ) ||
                         isa<MutReferenceType>(succ))) {
                return false;  // Pointer niche: single ptr
            }
            if (succ && isa<SmartPointerType>(succ)) {
                auto& sp = cast<SmartPointerType>(succ);
                if (sp.smartPointerKind() == SmartPointerKind::Box) {
                    return false;  // Box niche: single ptr
                }
                return true;  // Rc/Arc: aggregate struct
            }
            if (succ && isa<PrimitiveType>(succ)) {
                auto& prim = cast<PrimitiveType>(succ);
                if (prim.isInteger() && prim.bitWidth() <= 32) {
                    return false;  // Integer niche: single i64
                }
                if (prim.isBool()) {
                    return false;  // Bool niche: single i8
                }
            }
            return true;  // Struct fallback: aggregate { T, i1 }
        }
        case TypeKind::Stride:
        case TypeKind::Tensor:
            return true;
        case TypeKind::Shape:
            return true;  // shape is an array of i64 — aggregate
        case TypeKind::Poison:
            return false;
        case TypeKind::SimdVector:
            return false;  // Vectors are SSA-friendly — not aggregates
        default:
            return false;
    }
}

// ============================================================================
// typeSizeBytes / typeAlignmentBytes
// ============================================================================
uint64_t IRGenerator::typeSizeBytes(TypeId type) const {
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
        case TypeKind::Array:      return 16;  // { ptr, i64 } same layout as slice
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            // BUG FIX: Empty structs have size 1 (like in C), not 0
            if (st.fields().empty()) return 1;
            uint64_t total = 0;
            for (const auto& f : st.fields()) {
                uint64_t fa = typeAlignmentBytes(f.type);
                if (fa == 0) fa = 1;  // guard against null/zero alignment
                total = ((total + fa - 1) / fa) * fa;
                uint64_t fs = typeSizeBytes(f.type);
                if (fs == 0) fs = 1;  // guard against null/zero size
                total += fs;
            }
            return total;
        }
        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            switch (sp.smartPointerKind()) {
                case SmartPointerKind::Box: return 8;
                case SmartPointerKind::Rc:
                case SmartPointerKind::Arc: return 16;
            }
            return 8;
        }
        case TypeKind::Error: {
            auto& err = cast<ErrorType>(type);
            // BUG FIX: Guard against null success type
            if (err.successType().isNull()) return 2; // i1 flag + padding
            TypeId succ = err.successType();
            // Niched representations are smaller:
            // - Pointer niche: just a ptr (8 bytes)
            // - Integer niche: i64 (8 bytes)
            // - Bool niche: i8 (1 byte)
            // - Struct fallback: { T, i1 }
            if (succ && (isa<PointerType>(succ) || isa<ReferenceType>(succ) ||
                         isa<MutReferenceType>(succ))) {
                return 8;  // Pointer niche: single ptr
            }
            if (succ && isa<SmartPointerType>(succ)) {
                auto& sp = cast<SmartPointerType>(succ);
                if (sp.smartPointerKind() == SmartPointerKind::Box) {
                    return 8;  // Box niche: single ptr
                }
            }
            if (succ && isa<PrimitiveType>(succ)) {
                auto& prim = cast<PrimitiveType>(succ);
                if (prim.isInteger() && prim.bitWidth() <= 32) {
                    return 8;  // Integer niche: i64
                }
                if (prim.isBool()) {
                    return 1;  // Bool niche: i8
                }
            }
            // Struct fallback: traditional { T, i1 }
            uint64_t vs = typeSizeBytes(succ);
            uint64_t va = typeAlignmentBytes(succ);
            if (va == 0) va = 1;
            vs = ((vs + va - 1) / va) * va;
            return vs + 1;
        }
        case TypeKind::Fn: return 8;
        case TypeKind::Poison: return 0;
        // BUG FIX: Handle AlignedType — size is same as inner type
        case TypeKind::Aligned: {
            auto& at = cast<AlignedType>(type);
            return at.inner().isNull() ? 0 : typeSizeBytes(at.inner());
        }
        // BUG FIX: Handle OpaqueType — size is sizeBytes()
        case TypeKind::Opaque: {
            auto& ot = cast<OpaqueType>(type);
            return ot.sizeBytes();
        }
        // SimdVectorType: element_size * count
        case TypeKind::SimdVector: {
            auto& sv = cast<SimdVectorType>(type);
            return sv.elementType().isNull() ? 0 : typeSizeBytes(sv.elementType()) * sv.count();
        }
        default: return 8;
    }
}

uint64_t IRGenerator::typeAlignmentBytes(TypeId type) const {
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
        case TypeKind::Array:      return 8;  // { ptr, i64 } alignment
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            uint64_t ma = 1;
            for (const auto& f : st.fields()) ma = std::max(ma, typeAlignmentBytes(f.type));
            return ma;
        }
        case TypeKind::SmartPointer: return 8;
        case TypeKind::Error:        return 8;
        case TypeKind::Fn:           return 8;
        case TypeKind::Poison:        return 1;
        // BUG FIX: Handle AlignedType — alignment comes from the inner type
        case TypeKind::Aligned: {
            auto& at = cast<AlignedType>(type);
            if (at.inner().isNull()) return 1;
            // Use the max of the inner alignment and the explicit alignment
            uint64_t inner_a = typeAlignmentBytes(at.inner());
            return std::max<uint64_t>(inner_a, at.alignment());
        }
        // BUG FIX: Handle OpaqueType — alignment is max(1, sizeBytes/8)
        case TypeKind::Opaque: {
            auto& ot = cast<OpaqueType>(type);
            return std::max<uint64_t>(1, ot.sizeBytes() / 8);
        }
        // SimdVectorType: vectors need 16/32-byte alignment for best perf
        case TypeKind::SimdVector: {
            auto& sv = cast<SimdVectorType>(type);
            uint64_t elem_align = sv.elementType().isNull() ? 1 : typeAlignmentBytes(sv.elementType());
            uint64_t natural = elem_align * sv.count();
            // Vectors up to 16 bytes align to 16; larger align to 32
            if (natural <= 16) return 16;
            if (natural <= 32) return 32;
            return 64;
        }
        default:                     return 8;
    }
}

// ============================================================================
// llvmType
// ============================================================================
std::string IRGenerator::llvmType(TypeId type) {
    if (!type) return "ptr";
    switch (type->getKind()) {
        case TypeKind::Primitive: {
            auto& prim = cast<PrimitiveType>(type);
            switch (prim.primitiveKind()) {
                case PrimitiveKind::U8:    return "i8";
                case PrimitiveKind::U16:   return "i16";
                case PrimitiveKind::U32:   return "i32";
                case PrimitiveKind::U64:   return "i64";
                case PrimitiveKind::USize: return "i64";
                case PrimitiveKind::I8:    return "i8";
                case PrimitiveKind::I16:   return "i16";
                case PrimitiveKind::I32:   return "i32";
                case PrimitiveKind::I64:   return "i64";
                case PrimitiveKind::ISize: return "i64";
                case PrimitiveKind::F32:   return "float";
                case PrimitiveKind::F64:   return "double";
                case PrimitiveKind::Bool:  return "i1";
                case PrimitiveKind::Void:  return "void";
                default:                   return "i8";
            }
        }
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutReference:
        case TypeKind::Allocator:
            return "ptr";

        case TypeKind::Slice: {
            auto key = type->toString();
            auto it = needed_types_.find(key);
            if (it != needed_types_.end()) return it->second.llvm_name;
            return "{ ptr, i64 }";
        }
        case TypeKind::Array: {
            // Arrays use the same { ptr, i64 } layout as slices, but
            // the length is a compile-time constant. This enables
            // bounds check elimination and stack allocation for small arrays.
            auto& arr = cast<ArrayType>(type);
            auto key = type->toString();
            auto it = needed_types_.find(key);
            if (it != needed_types_.end()) return it->second.llvm_name;
            std::string ln = "%array." + sanitizeName(arr.toString());
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            return ln;
        }
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            auto key = type->toString();
            auto it = needed_types_.find(key);
            if (it != needed_types_.end()) return it->second.llvm_name;
            // BUG FIX: Empty structs need to return a valid LLVM type
            if (st.fields().empty()) return "%struct." + sanitizeName(st.name());
            return "%struct." + sanitizeName(st.name());
        }
        case TypeKind::Enum:
            return "i32";

        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            if (sp.smartPointerKind() == SmartPointerKind::Box) return "ptr";
            auto key = type->toString();
            auto it = needed_types_.find(key);
            if (it != needed_types_.end()) return it->second.llvm_name;
            std::string prefix = sp.smartPointerKind() == SmartPointerKind::Rc ? "rc" : "arc";
            return "%" + prefix + "." + sanitizeName(sp.pointee()->toString());
        }
        case TypeKind::Error: {
            auto& err = cast<ErrorType>(type);
            auto key = type->toString();
            auto it = needed_types_.find(key);
            if (it != needed_types_.end()) return it->second.llvm_name;
            TypeId succ = err.successType();

            // --- Niched representation for pointer-like types ---
            // When the success type is a pointer/reference/Box, we can use a
            // single ptr with the lowest bit as error flag (valid pointers are
            // always aligned, so bit 0 = 0 on success, bit 0 = 1 on error).
            if (succ && (isa<PointerType>(succ) || isa<ReferenceType>(succ) ||
                         isa<MutReferenceType>(succ))) {
                std::string ln = "%error.niched." + sanitizeName(succ->toString());
                needed_types_[key] = {ln, "ptr"};  // Just a pointer
                type_emit_order_.push_back(key);
                return ln;
            }
            // Box<T> is also a pointer
            if (succ && isa<SmartPointerType>(succ)) {
                auto& sp = cast<SmartPointerType>(succ);
                if (sp.smartPointerKind() == SmartPointerKind::Box) {
                    std::string ln = "%error.niched." + sanitizeName(succ->toString());
                    needed_types_[key] = {ln, "ptr"};
                    type_emit_order_.push_back(key);
                    return ln;
                }
            }

            // --- Niched representation for small integers ---
            // When the success type is an integer <= 32 bits, use i64 with
            // the high bit as the error flag. This eliminates the { i32, i1 }
            // struct (which has padding waste) in favor of a single i64.
            if (succ && isa<PrimitiveType>(succ)) {
                auto& prim = cast<PrimitiveType>(succ);
                if (prim.isInteger() && prim.bitWidth() <= 32) {
                    std::string ln = "%error.niched." + sanitizeName(succ->toString());
                    needed_types_[key] = {ln, "i64"};
                    type_emit_order_.push_back(key);
                    return ln;
                }
                // Bool fits in i8 with high bit as error flag
                if (prim.isBool()) {
                    std::string ln = "%error.niched." + sanitizeName(succ->toString());
                    needed_types_[key] = {ln, "i8"};
                    type_emit_order_.push_back(key);
                    return ln;
                }
            }

            // --- Struct fallback for aggregates ---
            std::string vt = llvmType(succ);
            return (vt == "void") ? "{ i1 }" : "{ " + vt + ", i1 }";
        }
        case TypeKind::Fn:
            return "ptr";
        case TypeKind::Poison:
            return "i32";  // stub type for poison – keeps LLVM IR structurally valid
        default:
            break;
    }

    // Handle AlignedType and OpaqueType (not in the switch above)
    if (isa<AlignedType>(type)) {
        auto& at = cast<AlignedType>(type);
        return llvmType(at.inner());
    }
    if (isa<OpaqueType>(type)) {
        auto& ot = cast<OpaqueType>(type);
        if (ot.sizeBytes() == 0) return "i8"; // default opaque size
        return "i" + std::to_string(ot.sizeBytes() * 8);
    }
    // ShapeType: emitted as an array of i64 (one per dimension)
    if (isa<ShapeType>(type)) {
        auto& st = cast<ShapeType>(type);
        if (st.dimensions().empty()) return "i64";
        return "[" + std::to_string(st.dimensions().size()) + " x i64]";
    }
    // StrideType: emitted as an array of i64 (one per stride) + i8 layout flag
    if (isa<StrideType>(type)) {
        auto& st = cast<StrideType>(type);
        if (st.strides().empty()) return "{ i64, i8 }";
        std::string result = "{ [" + std::to_string(st.strides().size()) + " x i64], i8 }";
        return result;
    }
    // TensorType: emitted as a struct { ptr, shape, stride }
    if (isa<TensorType>(type)) {
        auto& tt = cast<TensorType>(type);
        auto key = type->toString();
        auto it = needed_types_.find(key);
        if (it != needed_types_.end()) return it->second.llvm_name;
        return "%tensor." + sanitizeName(tt.toString());
    }
    // SimdVectorType: emitted as LLVM vector type <N x T>
    if (isa<SimdVectorType>(type)) {
        auto& sv = cast<SimdVectorType>(type);
        std::string elem_type = llvmType(sv.elementType());
        uint32_t count = sv.count();
        // LLVM vector type: <N x T>
        return "<" + std::to_string(count) + " x " + elem_type + ">";
    }

    return "ptr";
}

// ============================================================================
// llvmReturnType
//   Returns the LLVM IR return type for a function.
//   Zig-style: error-returning functions return just the success type;
//   the error code is communicated via an out-parameter (ptr %err_slot).
// ============================================================================
std::string IRGenerator::llvmReturnType(TypeId type, bool can_error) {
    if (can_error) {
        if (!type || !isa<ErrorType>(type)) {
            // Defensive: error-returning function but type is not ErrorType
            // (can happen during error-resilient compilation). Emit a fallback.
            return "i64";
        }
        auto& err = cast<ErrorType>(type);
        return llvmType(err.successType());
    }
    return llvmType(type);
}

// ============================================================================
// llvmParamType
// ============================================================================
std::string IRGenerator::llvmParamType(TypeId type) {
    return llvmType(type);
}

// ============================================================================
// emitTypeCast – emit a type conversion instruction and return the new register.
// Returns the original value if no conversion is needed, or a new register
// containing the converted value.
// ============================================================================
std::string IRGenerator::emitTypeCast(const std::string& val,
                                       TypeId from_type, TypeId to_type) {
    if (!from_type || !to_type) return val;
    std::string from_ll = llvmType(from_type);
    std::string to_ll = llvmType(to_type);
    if (from_ll == to_ll) return val;
    if (from_ll.empty() || to_ll.empty()) return val;

    std::string conv = nextReg();

    // bool → integer/float
    if (from_ll == "i1") {
        if (to_ll == "double" || to_ll == "float") {
            body_ss_ << "  " << conv << " = uitofp i1 " << val << " to " << to_ll << "\n";
        } else {
            body_ss_ << "  " << conv << " = zext i1 " << val << " to " << to_ll << "\n";
        }
        return conv;
    }

    // integer → float/double
    if ((to_ll == "double" || to_ll == "float") &&
        !(from_ll == "double" || from_ll == "float")) {
        bool is_signed = from_type->isSigned();
        body_ss_ << "  " << conv << " = " << (is_signed ? "sitofp" : "uitofp")
                 << " " << from_ll << " " << val << " to " << to_ll << "\n";
        return conv;
    }

    // float/double → integer
    if ((from_ll == "double" || from_ll == "float") &&
        !(to_ll == "double" || to_ll == "float")) {
        bool is_signed = to_type->isSigned();
        body_ss_ << "  " << conv << " = " << (is_signed ? "fptosi" : "fptoui")
                 << " " << from_ll << " " << val << " to " << to_ll << "\n";
        return conv;
    }

    // float → float
    if ((from_ll == "double" || from_ll == "float") &&
        (to_ll == "double" || to_ll == "float")) {
        body_ss_ << "  " << conv << " = fptrunc " << from_ll << " " << val
                 << " to " << to_ll << "\n";
        return conv;
    }

    // integer → integer
    uint64_t from_bits = from_type->bitWidth();
    uint64_t to_bits = to_type->bitWidth();
    if (to_bits < from_bits) {
        // Shrinking: use trunc
        body_ss_ << "  " << conv << " = trunc " << from_ll << " " << val
                 << " to " << to_ll << "\n";
    } else if (to_bits > from_bits) {
        // Extending: use sext for signed, zext for unsigned
        bool is_signed = from_type->isSigned();
        body_ss_ << "  " << conv << " = " << (is_signed ? "sext" : "zext")
                 << " " << from_ll << " " << val << " to " << to_ll << "\n";
    }
    // Same size: no-op (shouldn't happen since from_ll == to_ll was checked)
    return conv;
}

// ============================================================================
// nextReg / nextLabel / makeAllocaName
// ============================================================================
std::string IRGenerator::nextReg() {
    return "%" + std::to_string(reg_counter_++);
}

std::string IRGenerator::nextLabel(const std::string& hint) {
    return sanitizeName(hint) + "." + std::to_string(label_counter_++);
}

std::string IRGenerator::makeAllocaName(const std::string& source_name) {
    std::string base = "%" + sanitizeName(source_name) + ".addr";
    std::string name = base;
    int suffix = 1;
    while (used_alloca_names_.count(name)) {
        name = base + "." + std::to_string(suffix++);
    }
    used_alloca_names_.insert(name);
    return name;
}

// ============================================================================
// collectNeededTypes
// ============================================================================
void IRGenerator::collectNeededTypes(TypeId type) {
    if (!type) return;
    auto key = type->toString();
    if (needed_types_.count(key)) return;

    switch (type->getKind()) {
        case TypeKind::Struct: {
            auto& st = cast<StructType>(type);
            for (const auto& f : st.fields()) collectNeededTypes(f.type);
            std::string ln = "%struct." + sanitizeName(st.name());
            std::string body;
            // BUG FIX: Empty structs must emit at least { i8 } because LLVM
            // does not allow zero-element struct types.
            if (st.fields().empty()) {
                body = "{ i8 }";
            } else {
                // Check if this struct has a PackedBitfield transform.
                // If so, merge consecutive bool fields into a single integer type.
                bool is_packed_bitfield = false;
                if (meta_map_) {
                    auto* smeta = meta_map_->getStructMeta(st.name());
                    if (smeta && smeta->transform.kind == TransformKind::PackedBitfield) {
                        is_packed_bitfield = true;
                    }
                }

                body = "{ ";
                if (is_packed_bitfield) {
                    // Merge consecutive bool fields into a single integer type.
                    // Walk fields, accumulating consecutive bools, then emit as iN.
                    size_t i = 0;
                    bool first = true;
                    while (i < st.fields().size()) {
                        // Check if this field is bool
                        if (st.fields()[i].type && st.fields()[i].type->isBool()) {
                            // Count consecutive bools
                            size_t bool_count = 0;
                            while (i < st.fields().size() &&
                                   st.fields()[i].type && st.fields()[i].type->isBool()) {
                                ++bool_count;
                                ++i;
                            }
                            // Merge into the smallest integer type that fits
                            if (!first) body += ", ";
                            first = false;
                            if (bool_count <= 1) {
                                body += "i1";
                            } else if (bool_count <= 8) {
                                body += "i8";
                            } else if (bool_count <= 16) {
                                body += "i16";
                            } else if (bool_count <= 32) {
                                body += "i32";
                            } else {
                                body += "i64";
                            }
                        } else {
                            // Non-bool field: emit normally
                            if (!first) body += ", ";
                            first = false;
                            body += llvmType(st.fields()[i].type);
                            ++i;
                        }
                    }
                } else {
                    // Normal struct: emit each field individually
                    for (size_t i = 0; i < st.fields().size(); ++i) {
                        if (i > 0) body += ", ";
                        body += llvmType(st.fields()[i].type);
                    }
                }
                body += " }";
            }
            needed_types_[key] = {ln, body};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Slice: {
            auto& sl = cast<SliceType>(type);
            TypeId elem = sl.element();
            collectNeededTypes(elem);
            std::string elem_name = elem.isNull() ? "unknown" : elem->toString();
            std::string ln = "%slice." + sanitizeName(elem_name);
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Array: {
            auto& arr = cast<ArrayType>(type);
            TypeId elem = arr.element();
            collectNeededTypes(elem);
            std::string ln = "%array." + sanitizeName(arr.toString());
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            TypeId pt = sp.pointee();
            collectNeededTypes(pt);
            if (sp.smartPointerKind() == SmartPointerKind::Box) break;
            std::string pt_name = pt.isNull() ? "unknown" : pt->toString();
            std::string prefix = sp.smartPointerKind() == SmartPointerKind::Rc ? "rc" : "arc";
            std::string ln = "%" + prefix + "." + sanitizeName(pt_name);
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Error: {
            auto& err = cast<ErrorType>(type);
            TypeId st = err.successType();
            collectNeededTypes(st);

            // Determine niche representation for this error type
            bool use_pointer_niche = st && (isa<PointerType>(st) || isa<ReferenceType>(st) ||
                                            isa<MutReferenceType>(st));
            bool use_box_niche = false;
            if (st && isa<SmartPointerType>(st)) {
                auto& sp = cast<SmartPointerType>(st);
                use_box_niche = sp.smartPointerKind() == SmartPointerKind::Box;
            }
            bool use_integer_niche = false;
            bool use_bool_niche = false;
            if (st && isa<PrimitiveType>(st)) {
                auto& prim = cast<PrimitiveType>(st);
                if (prim.isInteger() && prim.bitWidth() <= 32) {
                    use_integer_niche = true;
                } else if (prim.isBool()) {
                    use_bool_niche = true;
                }
            }

            std::string st_name = st.isNull() ? "unknown" : st->toString();
            std::string ln;
            std::string body;

            if (use_pointer_niche || use_box_niche) {
                // Pointer niche: single ptr, error = low-bit-set sentinel
                ln = "%error.niched." + sanitizeName(st_name);
                body = "ptr";
            } else if (use_integer_niche) {
                // Integer niche: i64 with high bit as error flag
                ln = "%error.niched." + sanitizeName(st_name);
                body = "i64";
            } else if (use_bool_niche) {
                // Bool niche: i8 with high bit as error flag
                ln = "%error.niched." + sanitizeName(st_name);
                body = "i8";
            } else {
                // Struct fallback: traditional { T, i1 }
                std::string vt = llvmType(st);
                body = (vt == "void") ? "{ i1 }" : "{ " + vt + ", i1 }";
                ln = "%error." + sanitizeName(st_name);
            }

            needed_types_[key] = {ln, body};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Pointer: {
            collectNeededTypes(cast<PointerType>(type).pointee());
            break;
        }
        case TypeKind::Reference: {
            collectNeededTypes(cast<ReferenceType>(type).referent());
            break;
        }
        case TypeKind::MutReference: {
            collectNeededTypes(cast<MutReferenceType>(type).referent());
            break;
        }
        case TypeKind::Fn: {
            auto& ft = cast<FnType>(type);
            for (const auto& p : ft.params()) collectNeededTypes(p.type);
            collectNeededTypes(ft.returnType());
            if (ft.canError()) collectNeededTypes(ft.errorType());
            break;
        }
        case TypeKind::Poison:
            break;  // nothing to collect for poison types
        default: break;
    }
}

// ============================================================================
// getMetadataId
// ============================================================================
int IRGenerator::getMetadataId(const std::string& key) {
    auto it = metadata_map_.find(key);
    if (it != metadata_map_.end()) return it->second;
    int id = metadata_counter_++;
    metadata_map_[key] = id;
    metadata_entries_.push_back({id, "!{!\"" + key + "\"}"});
    return id;
}

// ============================================================================
// emitModuleHeader
// ============================================================================
void IRGenerator::emitModuleHeader() {
    module_out_ << "; ModuleID = 'jules'\n";
    module_out_ << "source_filename = \"jules\"\n";
    module_out_ << "target triple = \"x86_64-pc-linux-gnu\"\n";
    module_out_ << "target datalayout = \"e-m:e-p270:32:32-p271:32:32-p272:64:64-i64:64-f80:128-n8:16:32:64-S128\"\n\n";
}

// ============================================================================
// emitTypeDefinitions
// ============================================================================
void IRGenerator::emitTypeDefinitions() {
    for (const auto& key : type_emit_order_) {
        auto it = needed_types_.find(key);
        if (it == needed_types_.end()) continue;
        module_out_ << it->second.llvm_name << " = type " << it->second.body << "\n";
    }
    if (!type_emit_order_.empty()) module_out_ << "\n";
}

// ============================================================================
// emitRuntimeDecls
// ============================================================================
void IRGenerator::emitRuntimeDecls() {
    bool any = false;
    if (needed_runtime_.count("malloc")) {
        module_out_ << "declare ptr @malloc(i64) nounwind allockind(\"alloc,uninitialized\") allocsize(0)\n"; any = true;
    }
    if (needed_runtime_.count("free")) {
        module_out_ << "declare void @free(ptr nocapture writeonly) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("realloc")) {
        module_out_ << "declare ptr @realloc(ptr, i64) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("printf")) {
        module_out_ << "declare i32 @printf(ptr, ...)\n"; any = true;
    }
    if (needed_runtime_.count("memcpy")) {
        module_out_ << "declare void @llvm.memcpy.p0.p0.i64(ptr noalias nocapture writeonly, "
                       "ptr noalias nocapture readonly, i64, i1 immarg)\n"; any = true;
    }
    if (needed_runtime_.count("memset")) {
        module_out_ << "declare void @llvm.memset.p0.i64(ptr nocapture writeonly, "
                       "i8, i64, i1 immarg)\n"; any = true;
    }
    if (needed_runtime_.count("atomic_add")) {
        module_out_ << "declare i64 @llvm.atomicrmw.add.i64.p0(ptr, i64)\n"; any = true;
    }
    if (needed_runtime_.count("atomic_sub")) {
        module_out_ << "declare i64 @llvm.atomicrmw.sub.i64.p0(ptr, i64)\n"; any = true;
    }
    if (needed_runtime_.count("tether_yield")) {
        module_out_ << "declare void @tether_yield(i64)\n"; any = true;
    }
    if (needed_runtime_.count("prefetch")) {
        module_out_ << "declare void @llvm.prefetch.p0(ptr nocapture readonly, i32 immarg, i32 immarg, i32 immarg)\n"; any = true;
    }
    if (needed_runtime_.count("tether_spawn")) {
        module_out_ << "%struct.TetherTaskHandle = type { ptr }\n";
        module_out_ << "declare %struct.TetherTaskHandle @tether_spawn(ptr, ptr, i64)\n"; any = true;
    }
    if (needed_runtime_.count("tether_task_await")) {
        module_out_ << "declare ptr @tether_task_await(%struct.TetherTaskHandle)\n"; any = true;
    }
    if (needed_runtime_.count("tether_task_is_done")) {
        module_out_ << "declare i32 @tether_task_is_done(%struct.TetherTaskHandle)\n"; any = true;
    }
    if (needed_runtime_.count("tether_taskpool_init")) {
        module_out_ << "declare void @tether_taskpool_init(i32)\n"; any = true;
    }
    if (needed_runtime_.count("tether_taskpool_shutdown")) {
        module_out_ << "declare void @tether_taskpool_shutdown()\n"; any = true;
    }
    if (needed_runtime_.count("tether_deopt")) {
        module_out_ << "declare void @tether_deopt(i64, ptr) noreturn cold\n"; any = true;
    }
    // Benchmarking intrinsics — black_box prevents optimizer from eliminating code
    if (needed_runtime_.count("tether_black_box_i64")) {
        module_out_ << "declare void @tether_black_box_i64(i64) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("tether_black_box_f64")) {
        module_out_ << "declare void @tether_black_box_f64(double) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("tether_black_box_ptr")) {
        module_out_ << "declare void @tether_black_box_ptr(ptr) nounwind\n"; any = true;
    }
    // Volatile memory access intrinsics — for MMIO and hardware register access
    if (needed_runtime_.count("tether_volatile_read_i64")) {
        module_out_ << "declare i64 @tether_volatile_read_i64(ptr) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("tether_volatile_read_f64")) {
        module_out_ << "declare double @tether_volatile_read_f64(ptr) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("tether_volatile_write_i64")) {
        module_out_ << "declare void @tether_volatile_write_i64(ptr, i64) nounwind\n"; any = true;
    }
    if (needed_runtime_.count("tether_volatile_write_f64")) {
        module_out_ << "declare void @tether_volatile_write_f64(ptr, double) nounwind\n"; any = true;
    }
    // Optimized string equality — checks length first, then pointer
    // equality (for interned strings), then falls back to memcmp.
    // Returns i1 (bool).
    if (needed_runtime_.count("tether_str_eq")) {
        module_out_ << "declare i1 @tether_str_eq(ptr nocapture readonly, i64, ptr nocapture readonly, i64) nounwind readonly\n"; any = true;
    }
    // Zero-copy string slice — just adjusts pointer and length, no memcpy.
    // Returns { ptr, i64 } (a slice).
    if (needed_runtime_.count("tether_str_slice")) {
        module_out_ << "declare { ptr, i64 } @tether_str_slice(ptr nocapture readonly, i64, i64, i64) nounwind readonly\n"; any = true;
    }
    // SIMD vector reduce intrinsics — LLVM intrinsics, always available
    if (needed_runtime_.count("llvm_vector_reduce")) {
        // Float reductions (v4f32)
        module_out_ << "declare float @llvm.vector.reduce.fadd.v4f32(float, <4 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmul.v4f32(float, <4 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmin.v4f32(<4 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmax.v4f32(<4 x float>)\n";
        // Float reductions (v2f64)
        module_out_ << "declare double @llvm.vector.reduce.fadd.v2f64(double, <2 x double>)\n";
        module_out_ << "declare double @llvm.vector.reduce.fmul.v2f64(double, <2 x double>)\n";
        module_out_ << "declare double @llvm.vector.reduce.fmin.v2f64(<2 x double>)\n";
        module_out_ << "declare double @llvm.vector.reduce.fmax.v2f64(<2 x double>)\n";
        // Integer reductions (v4i32)
        module_out_ << "declare i32 @llvm.vector.reduce.add.v4i32(<4 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.mul.v4i32(<4 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.smin.v4i32(<4 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.smax.v4i32(<4 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.umin.v4i32(<4 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.umax.v4i32(<4 x i32>)\n";
        // Integer reductions (v2i64)
        module_out_ << "declare i64 @llvm.vector.reduce.add.v2i64(<2 x i64>)\n";
        module_out_ << "declare i64 @llvm.vector.reduce.mul.v2i64(<2 x i64>)\n";
        module_out_ << "declare i64 @llvm.vector.reduce.smin.v2i64(<2 x i64>)\n";
        module_out_ << "declare i64 @llvm.vector.reduce.smax.v2i64(<2 x i64>)\n";
        module_out_ << "declare i64 @llvm.vector.reduce.umin.v2i64(<2 x i64>)\n";
        module_out_ << "declare i64 @llvm.vector.reduce.umax.v2i64(<2 x i64>)\n";
        // Integer reductions (v8i32)
        module_out_ << "declare i32 @llvm.vector.reduce.add.v8i32(<8 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.mul.v8i32(<8 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.smin.v8i32(<8 x i32>)\n";
        module_out_ << "declare i32 @llvm.vector.reduce.smax.v8i32(<8 x i32>)\n";
        // Float reductions (v8f32)
        module_out_ << "declare float @llvm.vector.reduce.fadd.v8f32(float, <8 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmul.v8f32(float, <8 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmin.v8f32(<8 x float>)\n";
        module_out_ << "declare float @llvm.vector.reduce.fmax.v8f32(<8 x float>)\n";
        // Masked gather/scatter (v4f32)
        module_out_ << "declare <4 x float> @llvm.masked.gather.v4f32.v4p0(<4 x ptr>, i32, <4 x i1>, <4 x float>)\n";
        module_out_ << "declare void @llvm.masked.scatter.v4f32.v4p0(<4 x float>, <4 x ptr>, i32, <4 x i1>)\n";
        // Masked gather/scatter (v2f64)
        module_out_ << "declare <2 x double> @llvm.masked.gather.v2f64.v2p0(<2 x ptr>, i32, <2 x i1>, <2 x double>)\n";
        module_out_ << "declare void @llvm.masked.scatter.v2f64.v2p0(<2 x double>, <2 x ptr>, i32, <2 x i1>)\n";
        // Masked gather/scatter (v4i32)
        module_out_ << "declare <4 x i32> @llvm.masked.gather.v4i32.v4p0(<4 x ptr>, i32, <4 x i1>, <4 x i32>)\n";
        module_out_ << "declare void @llvm.masked.scatter.v4i32.v4p0(<4 x i32>, <4 x ptr>, i32, <4 x i1>)\n";
        // Masked gather/scatter (v8f32)
        module_out_ << "declare <8 x float> @llvm.masked.gather.v8f32.v8p0(<8 x ptr>, i32, <8 x i1>, <8 x float>)\n";
        module_out_ << "declare void @llvm.masked.scatter.v8f32.v8p0(<8 x float>, <8 x ptr>, i32, <8 x i1>)\n";
        any = true;
    }
    if (any) module_out_ << "\n";
}

// ============================================================================
// String-constant helpers
// ============================================================================
static std::string escapeLLVMString(const std::string& s) {
    std::string result;
    for (unsigned char c : s) {
        if (c == '\\') { result += "\\\\"; }
        else if (c == '"') { result += "\\22"; }
        else if (c >= 0x20 && c < 0x7F) { result += c; }
        else {
            char buf[8];
            std::snprintf(buf, sizeof(buf), "\\%02X", c);
            result += buf;
        }
    }
    return result;
}

void IRGenerator::emitStringConstants() {
    if (string_constants_.empty()) return;
    std::vector<std::pair<std::string, int>> sorted(string_constants_.begin(),
                                                     string_constants_.end());
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.second < b.second; });
    for (const auto& [str, idx] : sorted) {
        std::string escaped = escapeLLVMString(str);
        size_t len = str.size() + 1;
        module_out_ << "@.str." << idx
                    << " = private unnamed_addr constant ["
                    << len << " x i8] c\"" << escaped << "\\00\"\n";
    }
    module_out_ << "\n";
}

// ============================================================================
// emitTBAAMetadataForField — emit !tbaa metadata on struct field loads/stores
//
// Queries the MetadataMap for TBAA type info for the given struct name and
// field name. Returns a string like ", !tbaa !5" to append to a load/store,
// or empty string if no TBAA info is available.
// ============================================================================
std::string IRGenerator::emitTBAAMetadataForField(const std::string& struct_name,
                                                    const std::string& field_name) {
    if (!meta_map_) return "";

    // Look up the access tag for this struct+field.
    // In LLVM 17+, TBAA access tags have the format:
    //   !N = !{!base_type, !access_type, i64 offset, i64 size}
    // We pre-register both type descriptors and access tags.
    // The access tag key format is "access.STRUCT_NAME.FIELD_NAME".
    std::string access_key = "access." + struct_name + "." + field_name;
    auto it = metadata_map_.find(access_key);
    if (it != metadata_map_.end()) {
        return ", !tbaa !" + std::to_string(it->second);
    }

    // Fall back to the type descriptor (field-level)
    std::string key = "field." + struct_name + "." + field_name;
    it = metadata_map_.find(key);
    if (it != metadata_map_.end()) {
        return ", !tbaa !" + std::to_string(it->second);
    }

    // Fall back to struct-level TBAA
    std::string struct_key = "struct." + struct_name;
    it = metadata_map_.find(struct_key);
    if (it != metadata_map_.end()) {
        return ", !tbaa !" + std::to_string(it->second);
    }
    return "";
}

// ============================================================================
// emitTBAATypeAndAccessTags — emit TBAA type descriptor + access tag pair
//
// For LLVM 17+, TBAA has two kinds of metadata nodes:
//   1. Type descriptors: !N = !{!"name", !parent, i64 offset}
//   2. Access tags:      !N = !{!base_type, !access_type, i64 offset, i64 size}
//
// Loads/stores must reference access tags, not type descriptors.
// This function ensures both are pre-registered and returns the access tag ref.
// ============================================================================
std::string IRGenerator::emitTBAATypeAndAccessTags(const std::string& struct_name,
                                                     const std::string& field_name,
                                                     uint64_t offset, uint64_t size) {
    if (!meta_map_) return "";

    // Ensure the access tag is registered (it should have been pre-registered
    // by preRegisterTBAAMetadata, but if not, register it now)
    std::string access_key = "access." + struct_name + "." + field_name;
    auto it = metadata_map_.find(access_key);
    if (it != metadata_map_.end()) {
        return ", !tbaa !" + std::to_string(it->second);
    }

    // Access tag not yet registered — allocate one on the fly
    int access_id = metadata_counter_++;
    metadata_map_[access_key] = access_id;
    return ", !tbaa !" + std::to_string(access_id);
}

// ============================================================================
// emitRangeMetadataForEnum — emit !range metadata on enum loads
// ============================================================================
std::string IRGenerator::emitRangeMetadataForEnum(TypeId type) {
    if (!meta_map_ || !type) return "";
    // Only emit range metadata for enum types
    if (!isa<EnumType>(type)) return "";

    auto& en = cast<EnumType>(type);
    uint32_t count = static_cast<uint32_t>(en.variants().size());
    if (count == 0) return "";

    // Create range metadata: !{i32 0, i32 COUNT}
    std::string key = "range.enum." + std::to_string(count);
    auto it = metadata_map_.find(key);
    if (it != metadata_map_.end()) {
        return ", !range !" + std::to_string(it->second);
    }

    int range_id = metadata_counter_++;
    metadata_entries_.push_back({range_id,
        "!{i32 0, i32 " + std::to_string(count) + "}"});
    metadata_map_[key] = range_id;
    return ", !range !" + std::to_string(range_id);
}

// ============================================================================
// emitMetadata
// ============================================================================
void IRGenerator::emitMetadata() {
    if (metadata_entries_.empty()) return;
    for (const auto& e : metadata_entries_) {
        module_out_ << "!" << e.id << " = " << e.content << "\n";
    }
    module_out_ << "\n";
}

// ============================================================================
// emitTetherFnMetadata — compute and record Tether metadata for a function
//
// This is called after emitting each function definition. It examines the
// MetadataMap's NodeMeta for the function and its parameters, computes a
// flags bitmask, and records it for later emission by emitTetherMetadata().
//
// The actual metadata nodes are emitted at the end of generate() so that
// all metadata entries can be collected into module-level named metadata.
// ============================================================================
void IRGenerator::emitTetherFnMetadata(FnDecl* fn) {
    if (!fn) return;

    // Only emit metadata for function definitions (not declarations)
    if (!fn->body()) return;

    TetherFnMeta meta;
    meta.fn_name = sanitizeName(fn->name());

    // --- Compute function-level flags ---
    uint32_t fn_flags = 0;
    auto* nm = meta_map_ ? meta_map_->get(fn) : nullptr;

    if (nm) {
        // noalias — function's return/this is noalias
        if (nm->aliasing == AliasingKind::NoAlias) {
            fn_flags |= 1u;  // kFnNoAlias = bit0
        }
        // readonly — function is readonly
        if (nm->llvm_meta.noalias && nm->ownership == OwnershipKind::Immutable) {
            fn_flags |= 2u;  // kFnReadOnly = bit1
        }
        // hot — function is on a hot path
        if (nm->llvm_meta.hot_path) {
            fn_flags |= 4u;  // kFnHot = bit2
        }
        // cold — function is on a cold path
        if (nm->llvm_meta.cold_path) {
            fn_flags |= 8u;  // kFnCold = bit3
        }
        // pure — function is memory(none)
        if (nm->purity == FunctionPurity::Pure || fn->isPure()) {
            fn_flags |= 16u;  // kFnPure = bit4
        }
        // noalloc — function is willreturn + nosync
        if (nm->purity == FunctionPurity::NoAlloc || fn->isNoalloc()) {
            fn_flags |= 32u;  // kFnNoAlloc = bit5
        }
        // vectorize — loop is safe to vectorize
        if (nm->llvm_meta.vectorization_safe) {
            meta.loop_flags |= 64u;  // kFnVectorize = bit6
        }
        // unroll — loop should be unrolled
        if (nm->has_loop_with_linear_access && nm->llvm_meta.hot_path) {
            meta.loop_flags |= 128u;  // kFnUnroll = bit7
        }
        // invariant_load — function has invariant load patterns
        if (nm->llvm_meta.nonnull) {
            fn_flags |= 256u;  // kFnInvariantLoad = bit8
        }
    }

    // Also check function-level properties from the AST
    if (fn->isPure() && !(fn_flags & 16u)) {
        fn_flags |= 16u;  // kFnPure
    }
    if (fn->isNoalloc() && !(fn_flags & 32u)) {
        fn_flags |= 32u;  // kFnNoAlloc
    }

    // Check for @simd directive
    if (fn->hasDirective(CompilerDirective::Simd)) {
        meta.loop_flags |= 64u;  // kFnVectorize
    }

    meta.fn_flags = fn_flags;

    // --- Compute parameter-level flags ---
    for (size_t i = 0; i < fn->paramCount(); ++i) {
        const auto& p = fn->params()[i];
        uint32_t pflags = 0;

        auto* pnm = meta_map_ ? meta_map_->get(&p) : nullptr;
        if (pnm) {
            // noalias — parameter is restrict/noalias
            if (pnm->aliasing == AliasingKind::NoAlias || pnm->is_restrict) {
                pflags |= 1u;  // kParamNoAlias = bit0
            }
            // readonly — parameter is only read
            if (pnm->ownership == OwnershipKind::Immutable ||
                pnm->llvm_meta.noalias) {
                // Use a heuristic: if the parameter's ownership is Immutable
                // (val declaration) or it's a shared reference, mark readonly
                if (p.type && isa<ReferenceType>(p.type)) {
                    pflags |= 2u;  // kParamReadOnly = bit1
                }
            }
            // nonnull — parameter is never null (Tether references are nonnull)
            if (p.type && (isa<PointerType>(p.type) ||
                           isa<ReferenceType>(p.type) ||
                           isa<MutReferenceType>(p.type))) {
                pflags |= 4u;  // kParamNonNull = bit2
            }
            // invariant_load — loads from this parameter are invariant
            if (pnm->llvm_meta.nonnull && pnm->aliasing == AliasingKind::NoAlias) {
                pflags |= 8u;  // kParamInvariantLoad = bit3
            }
        }

        // Also check parameter-level properties from the AST
        // (the IRGenerator already emits some of these as attributes, but the
        //  pass plugin may apply additional attributes or fix up ones that
        //  were missed)
        if (p.is_restrict) {
            pflags |= 1u;  // kParamNoAlias
        }

        // For &T (shared reference) parameters, always add readonly
        if (p.type && isa<ReferenceType>(p.type)) {
            pflags |= 2u;  // kParamReadOnly
        }

        // For &mut T (exclusive reference) parameters, add noalias
        if (p.type && isa<MutReferenceType>(p.type)) {
            pflags |= 1u;  // kParamNoAlias
        }

        // For pointer/reference types, add nonnull
        if (p.type && (isa<PointerType>(p.type) ||
                       isa<ReferenceType>(p.type) ||
                       isa<MutReferenceType>(p.type))) {
            pflags |= 4u;  // kParamNonNull
        }

        // Add nocapture for reference parameters (they don't escape the
        // function in the common case — the borrow checker ensures they
        // don't outlive the call)
        if (p.type && (isa<MutReferenceType>(p.type) || isa<ReferenceType>(p.type))) {
            pflags |= 32u;  // kParamNoCapture = bit5
        }

        // Add writeonly for &mut T params that are only written to
        // (from semantic analysis — for now, conservatively skip this;
        //  a future analysis pass can set this when the param is never read)

        meta.param_flags.push_back(pflags);
    }

    // --- Compute dereferenceable metadata for reference parameters ---
    // For &T and &mut T parameters where the referent has a known size,
    // record the (param_index, dereferenceable_bytes) pair so that
    // emitTetherMetadata() can emit !tether.dereferenceable metadata.
    for (size_t i = 0; i < fn->paramCount(); ++i) {
        const auto& p = fn->params()[i];
        if (!p.type) continue;

        // Check if this is a reference type (&T or &mut T)
        TypeId inner_type;
        if (isa<MutReferenceType>(p.type)) {
            inner_type = cast<MutReferenceType>(p.type).referent();
        } else if (isa<ReferenceType>(p.type)) {
            inner_type = cast<ReferenceType>(p.type).referent();
        } else {
            continue;
        }

        // Get the size of the inner type
        uint64_t type_size = 0;
        if (inner_type) {
            type_size = typeSizeBytes(inner_type);
        }

        if (type_size > 0) {
            meta.deref_params.push_back({i, type_size});
        }
    }

    // Only record if there's something to emit
    if (fn_flags != 0 || meta.loop_flags != 0 ||
        !meta.param_flags.empty()) {
        tether_fn_metas_.push_back(std::move(meta));
    }
}

// ============================================================================
// emitTetherMetadata — emit all accumulated Tether named metadata
//
// Called at the end of generate() after all top-level declarations have
// been emitted. Writes the module-level named metadata nodes that the
// TetherAttrPass LLVM plugin reads.
//
// Emits:
//   !tether.fns = !{!N, !M, ...}     — function name + flags
//   !tether.params = !{!P, !Q, ...}   — function name + param idx + param flags
//   !tether.loops = !{!R, !S, ...}    — function name + loop flags
//   !tether.dereferenceable = !{!D, ...} — function name + param idx + deref bytes
// ============================================================================
void IRGenerator::emitTetherMetadata() {
    if (tether_fn_metas_.empty()) return;

    // We need to emit individual metadata entries and then the named
    // metadata that references them. We track the start position so we
    // only emit the Tether-specific entries (not the ones already emitted
    // by emitMetadata()).

    size_t start_idx = metadata_entries_.size();  // entries before Tether metadata

    std::vector<int> fn_meta_ids;       // IDs for function metadata entries
    std::vector<int> param_meta_ids;    // IDs for parameter metadata entries
    std::vector<int> loop_meta_ids;     // IDs for loop metadata entries

    // Allocate metadata IDs for each function entry
    for (const auto& meta : tether_fn_metas_) {
        // Function metadata: !{!"name", i32 flags}
        int fn_id = metadata_counter_++;
        fn_meta_ids.push_back(fn_id);
        metadata_entries_.push_back({fn_id,
            "!{!\"" + meta.fn_name + "\", i32 " + std::to_string(meta.fn_flags) + "}"});

        // Parameter metadata: !{!"name", i32 param_idx, i32 param_flags}
        for (size_t pi = 0; pi < meta.param_flags.size(); ++pi) {
            if (meta.param_flags[pi] != 0) {
                int param_id = metadata_counter_++;
                param_meta_ids.push_back(param_id);
                metadata_entries_.push_back({param_id,
                    "!{!\"" + meta.fn_name + "\", i32 " + std::to_string(pi) +
                    ", i32 " + std::to_string(meta.param_flags[pi]) + "}"});
            }
        }

        // Loop metadata: !{!"name", i32 loop_flags}
        if (meta.loop_flags != 0) {
            int loop_id = metadata_counter_++;
            loop_meta_ids.push_back(loop_id);
            metadata_entries_.push_back({loop_id,
                "!{!\"" + meta.fn_name + "\", i32 " +
                std::to_string(meta.loop_flags) + "}"});
        }
    }

    // Emit only the Tether-specific metadata entries (starting from start_idx)
    module_out_ << "; Tether optimization metadata (consumed by TetherAttrPass)\n";
    for (size_t i = start_idx; i < metadata_entries_.size(); ++i) {
        const auto& e = metadata_entries_[i];
        module_out_ << "!" << e.id << " = " << e.content << "\n";
    }

    // Emit the named metadata that ties them together
    // !tether.fns = !{!N, !M, ...}
    if (!fn_meta_ids.empty()) {
        module_out_ << "!tether.fns = !{";
        for (size_t i = 0; i < fn_meta_ids.size(); ++i) {
            if (i > 0) module_out_ << ", ";
            module_out_ << "!" << fn_meta_ids[i];
        }
        module_out_ << "}\n";
    }

    // !tether.params = !{!P, !Q, ...}
    if (!param_meta_ids.empty()) {
        module_out_ << "!tether.params = !{";
        for (size_t i = 0; i < param_meta_ids.size(); ++i) {
            if (i > 0) module_out_ << ", ";
            module_out_ << "!" << param_meta_ids[i];
        }
        module_out_ << "}\n";
    }

    // !tether.loops = !{!R, !S, ...}
    if (!loop_meta_ids.empty()) {
        module_out_ << "!tether.loops = !{";
        for (size_t i = 0; i < loop_meta_ids.size(); ++i) {
            if (i > 0) module_out_ << ", ";
            module_out_ << "!" << loop_meta_ids[i];
        }
        module_out_ << "}\n";
    }

    // --- Emit !tether.dereferenceable metadata ---
    // For &mut T and &T parameters where T has a known size, emit
    // dereferenceable(N) metadata so LLVM knows the pointer is valid.
    {
        std::vector<int> deref_meta_ids;
        for (const auto& meta : tether_fn_metas_) {
            for (const auto& [param_idx, type_size] : meta.deref_params) {
                int deref_id = metadata_counter_++;
                deref_meta_ids.push_back(deref_id);
                // Emit the metadata node directly
                module_out_ << "!" << deref_id << " = !{!\""
                    << meta.fn_name << "\", i32 " << std::to_string(param_idx)
                    << ", i64 " << std::to_string(type_size) << "}\n";
            }
        }

        // Emit the named metadata
        if (!deref_meta_ids.empty()) {
            module_out_ << "!tether.dereferenceable = !{";
            for (size_t i = 0; i < deref_meta_ids.size(); ++i) {
                if (i > 0) module_out_ << ", ";
                module_out_ << "!" << deref_meta_ids[i];
            }
            module_out_ << "}\n";
        }
    }

    // !tether.tbaa = !{!T, !U, ...} — TBAA field access metadata
    // This is the metadata consumed by TetherAttrPass to generate
    // LLVM TBAA tags on load/store instructions. Without this, LLVM
    // must assume all pointer accesses may alias, which prevents
    // vectorization, LICM, and other optimizations on struct fields.
    if (meta_map_) {
        // Collect struct field TBAA info from the MetadataMap
        std::vector<std::tuple<std::string, std::string, uint64_t, uint64_t>> tbaa_entries;
        for (const auto& [key, id] : metadata_map_) {
            // Keys like "access.STRUCT.FIELD" indicate TBAA access tags
            if (key.find("access.") == 0) {
                // Parse the key: "access.StructName.FieldName"
                std::string rest = key.substr(7); // skip "access."
                auto dot_pos = rest.find('.');
                if (dot_pos != std::string::npos) {
                    std::string struct_name = rest.substr(0, dot_pos);
                    std::string field_name = rest.substr(dot_pos + 1);
                    // Compute offset and size from type info
                    // For now, use the metadata ID as a placeholder;
                    // the TetherAttrPass will compute actual offsets from
                    // the struct layout
                    tbaa_entries.push_back({struct_name, field_name, 0, 0});
                }
            }
        }

        if (!tbaa_entries.empty()) {
            std::vector<int> tbaa_meta_ids;
            for (const auto& [struct_name, field_name, offset, size] : tbaa_entries) {
                int tbaa_id = metadata_counter_++;
                tbaa_meta_ids.push_back(tbaa_id);
                metadata_entries_.push_back({tbaa_id,
                    "!{!\"" + struct_name + "\", !\"" + field_name +
                    "\", i64 " + std::to_string(offset) +
                    ", i64 " + std::to_string(size) + "}"});
            }

            // Emit the entries
            for (size_t i = start_idx; i < metadata_entries_.size(); ++i) {
                const auto& e = metadata_entries_[i];
                // Only emit entries that haven't been emitted yet
                if (e.id >= start_idx) {
                    // Already emitted above, skip
                }
            }

            // Emit the named metadata
            module_out_ << "!tether.tbaa = !{";
            for (size_t i = 0; i < tbaa_meta_ids.size(); ++i) {
                if (i > 0) module_out_ << ", ";
                module_out_ << "!" << tbaa_meta_ids[i];
            }
            module_out_ << "}\n";
        }
    }

    module_out_ << "\n";
}

// ============================================================================
// preRegisterTBAAMetadata — allocate TBAA metadata IDs before code emission
//
// This must be called before any code emission so that
// emitTBAAMetadataForField() can find the IDs in metadata_map_.
// The actual metadata text is emitted by emitMetaMapTBAA() at the end.
// ============================================================================
void IRGenerator::preRegisterTBAAMetadata() {
    if (!meta_map_) return;

    // Allocate root TBAA node ID
    int root_id = metadata_counter_++;
    metadata_map_["__tbaa_root"] = root_id;

    // Allocate alias scope domain and scope IDs
    int domain_id = metadata_counter_++;
    metadata_map_["__tbaa_domain"] = domain_id;
    int scope_id = metadata_counter_++;
    metadata_map_["__tbaa_scope"] = scope_id;

    // For each struct, allocate struct-level and per-field TBAA IDs.
    // For LLVM 17+, we emit BOTH type descriptors and access tags:
    //   Type descriptor: !N = !{!"field.x", !struct_id, i64 offset}
    //   Access tag:      !M = !{!struct_id, !N, i64 offset, i64 size}
    for (const auto& [name, sm] : meta_map_->structs()) {
        int struct_id = metadata_counter_++;
        metadata_map_["struct." + name] = struct_id;

        for (size_t i = 0; i < sm.field_names.size(); ++i) {
            // Type descriptor ID for this field
            int field_id = metadata_counter_++;
            metadata_map_["field." + name + "." + sm.field_names[i]] = field_id;

            // Access tag ID for this field (LLVM 17+ format)
            int access_id = metadata_counter_++;
            metadata_map_["access." + name + "." + sm.field_names[i]] = access_id;
        }
    }
}

// ============================================================================
// emitMetaMapTBAA — emit TBAA type descriptors from the metadata engine
//
// Emits the metadata text for IDs that were pre-allocated by
// preRegisterTBAAMetadata(). This is called at the end of generate()
// after all code has been emitted.
// ============================================================================
void IRGenerator::emitMetaMapTBAA() {
    if (!meta_map_) return;

    // Emit root TBAA node
    auto root_it = metadata_map_.find("__tbaa_root");
    if (root_it == metadata_map_.end()) return;

    int root_id = root_it->second;
    module_out_ << "!" << root_id << " = !{!\"Tether TBAA\"}\n";

    // Emit alias scope domain and scope
    auto domain_it = metadata_map_.find("__tbaa_domain");
    auto scope_it = metadata_map_.find("__tbaa_scope");
    if (domain_it != metadata_map_.end()) {
        module_out_ << "!" << domain_it->second << " = !{!\"tether-alias-domain\"}\n";
    }
    if (scope_it != metadata_map_.end() && domain_it != metadata_map_.end()) {
        module_out_ << "!" << scope_it->second << " = !{!\"tether-noalias-scope\", !"
                     << domain_it->second << "}\n";
    }

    // Build a lookup from struct name → StructDecl* so we can compute real offsets
    std::unordered_map<std::string, StructDecl*> struct_decls;
    for (const auto& tl : program_) {
        if (isa<StructDecl>(*tl)) {
            auto& sd = cast<StructDecl>(*tl);
            struct_decls[sd.name()] = &sd;
        }
    }

    // For each struct, emit struct-level type descriptor, per-field type
    // descriptors, AND per-field access tags (LLVM 17+ format).
    //
    // LLVM 17 TBAA format:
    //   Type descriptor: !N = !{!"name", !parent, i64 offset}
    //   Access tag:      !M = !{!base_type, !access_type, i64 offset, i64 size}
    //
    // Loads/stores must reference access tags, not type descriptors.
    for (const auto& [name, sm] : meta_map_->structs()) {
        auto struct_it = metadata_map_.find("struct." + name);
        if (struct_it == metadata_map_.end()) continue;

        int struct_id = struct_it->second;
        // Struct type descriptor: child of the root
        module_out_ << "!" << struct_id << " = !{!\"struct." << name
                     << "\", !" << root_id << ", i64 0}\n";

        // Compute real field offsets from the StructDecl's field types
        uint64_t offset = 0;
        for (size_t i = 0; i < sm.field_names.size(); ++i) {
            auto field_it = metadata_map_.find("field." + name + "." + sm.field_names[i]);
            if (field_it == metadata_map_.end()) continue;

            int field_id = field_it->second;

            // Compute the size of this field's type
            uint64_t field_size = 4;  // default: i32/float
            auto sd_it = struct_decls.find(name);
            if (sd_it != struct_decls.end()) {
                auto* sd = sd_it->second;
                if (i < sd->fields().size()) {
                    TypeId ft = sd->fields()[i].type;
                    if (ft) {
                        field_size = typeSizeBytes(ft);
                        if (field_size == 0) field_size = 4;  // safety default
                    }
                }
            }

            // Emit type descriptor: !N = !{!"field.x", !struct_id, i64 offset}
            module_out_ << "!" << field_id << " = !{!\"field."
                         << sm.field_names[i] << "\", !" << struct_id
                         << ", i64 " << offset << "}\n";

            // Emit access tag: !M = !{!struct_id, !field_id, i64 offset, i64 size}
            auto access_it = metadata_map_.find("access." + name + "." + sm.field_names[i]);
            if (access_it != metadata_map_.end()) {
                module_out_ << "!" << access_it->second << " = !{!" << struct_id
                             << ", !" << field_id << ", i64 " << offset
                             << ", i64 " << field_size << "}\n";
            }

            // Advance offset by the field size (aligned to natural alignment)
            uint64_t align = field_size;
            if (align < 1) align = 1;
            if (align > 8) align = 8;
            // Round offset up to alignment
            offset = (offset + align - 1) & ~(align - 1);
            offset += field_size;
        }
    }

    module_out_ << "\n";
}

// ============================================================================
// generate – main entry point
// ============================================================================
std::string IRGenerator::generate() {
    // 0. Reset per-generation state
    tether_fn_metas_.clear();

    // 1. Collect composite types
    for (const auto& tl : program_) {
        if (isa<FnDecl>(*tl)) {
            auto& fn = cast<FnDecl>(*tl);
            for (const auto& p : fn.params()) collectNeededTypes(p.type);
            collectNeededTypes(fn.returnType());
            if (fn.canError()) collectNeededTypes(fn.errorType());
        } else if (isa<StructDecl>(*tl)) {
            auto& sd = cast<StructDecl>(*tl);
            for (const auto& f : sd.fields()) collectNeededTypes(f.type);
        } else if (isa<ImplDecl>(*tl)) {
            // ImplDecl methods are regular functions — collect their types too
            auto& impl = cast<ImplDecl>(*tl);
            for (const auto& method : impl.methods()) {
                for (const auto& p : method->params()) collectNeededTypes(p.type);
                collectNeededTypes(method->returnType());
                if (method->canError()) collectNeededTypes(method->errorType());
            }
        }
    }

    // 1.5 Pre-register TBAA metadata IDs from the metadata engine.
    // This must happen BEFORE code emission so that emitTBAAMetadataForField()
    // can find the IDs in metadata_map_. The actual metadata text is emitted
    // at the end via emitMetaMapTBAA().
    if (meta_map_) {
        preRegisterTBAAMetadata();
    }

    // 2. Emit module header
    emitModuleHeader();

    // 3. Emit struct/composite type definitions
    emitTypeDefinitions();

    // 4. Emit string constants
    emitStringConstants();

    // 5. Emit each top-level declaration (before runtime declarations,
    //    so that needed_runtime_ is fully populated by the time we emit them)
    for (const auto& tl : program_) {
        emitTopLevel(tl.get());
    }

    // 6. Emit runtime declarations — must come AFTER top-level emission
    //    because function bodies may insert entries into needed_runtime_
    //    (e.g. tether_yield, memcpy, memset). LLVM IR allows forward
    //    references, so declaring these after their use is valid.
    emitRuntimeDecls();

    // 7. Emit metadata (IRGenerator's own + metadata engine's L5 block)
    emitMetadata();

    // 8. Emit TBAA type descriptors and alias scopes from the metadata engine
    // These provide the type-based alias analysis info that LLVM uses to
    // prove that different struct field accesses don't alias.
    if (meta_map_) {
        emitMetaMapTBAA();
    }

    // 9. Emit Tether optimization metadata for the LLVM pass plugin.
    // This emits named metadata (!tether.fns, !tether.params, !tether.loops)
    // that the TetherAttrPass reads to inject LLVM optimization attributes.
    // The metadata is always emitted (even without the pass plugin), because
    // it's valid LLVM IR that just adds module-level named metadata nodes.
    // If the TetherAttrPass plugin is not loaded, the metadata is harmless.
    emitTetherMetadata();

    return module_out_.str();
}

// ============================================================================
// emitTopLevel
// ============================================================================
void IRGenerator::emitTopLevel(TopLevel* tl) {
    switch (tl->getKind()) {
        case NodeKind::FnDecl:     emitFnDecl(&cast<FnDecl>(*tl)); break;
        case NodeKind::StructDecl: emitStructDecl(&cast<StructDecl>(*tl)); break;
        case NodeKind::EnumDecl:   emitEnumDecl(&cast<EnumDecl>(*tl)); break;
        case NodeKind::ImportDecl: break;

        // ---- TraitDecl: compile-time only, no codegen ----
        case NodeKind::TraitDecl: break;

        // ---- ImplDecl: emit each method as a regular function ----
        case NodeKind::ImplDecl: {
            auto& impl = cast<ImplDecl>(*tl);
            for (const auto& method : impl.methods()) {
                emitFnDecl(method.get());
            }
            break;
        }

        default: break;
    }
}

// ============================================================================
// emitStructDecl – type definitions already emitted by emitTypeDefinitions
// ============================================================================
void IRGenerator::emitStructDecl(StructDecl* sd) {
    // Type defs are handled in emitTypeDefinitions(). We do NOT re-emit here
    // even for aligned structs, because emitTypeDefinitions() already wrote
    // the type definition to module_out_ and we can't undo that.
    // Alignment info is preserved as a comment for readability only.
    if (sd && sd->alignment() > 0) {
        module_out_ << "; %struct." << sanitizeName(sd->name())
                    << " has alignment " << sd->alignment() << "\n";
    }

    // Emit SoA global arrays if this struct is SoA-transformed
    if (meta_map_ && sd) {
        auto* smeta = meta_map_->getStructMeta(sd->name());
        if (smeta && smeta->transform.kind == TransformKind::SoATransform) {
            // For each field, emit a global array
            for (size_t i = 0; i < sd->fields().size(); ++i) {
                const auto& field = sd->fields()[i];
                std::string field_type = llvmType(field.type);
                std::string array_name = sanitizeName(sd->name()) + "_" + field.name;
                // Default size: 1024 elements (will be resized at runtime)
                module_out_ << "@" << array_name
                           << " = global [1024 x " << field_type << "] zeroinitializer\n";
            }
        }
    }
}

// ============================================================================
// emitEnumDecl – emit variant constants
// ============================================================================
void IRGenerator::emitEnumDecl(EnumDecl* ed) {
    for (size_t i = 0; i < ed->variantCount(); ++i) {
        const auto& v = ed->variants()[i];
        int64_t val = v.value.has_value() ? v.value.value() : static_cast<int64_t>(i);
        module_out_ << "@" << sanitizeName(ed->name()) << "."
                    << sanitizeName(v.name)
                    << " = private constant i32 " << val << "\n";
    }
}

// ============================================================================
// emitFnDecl
// ============================================================================
void IRGenerator::emitFnDecl(FnDecl* fn) {
    // Reset per-function state
    reg_counter_ = 0;
    label_counter_ = 0;
    var_ssa_.clear();
    scope_stack_.clear();
    used_alloca_names_.clear();
    defer_stack_.clear();
    errdefer_stack_.clear();
    stack_box_allocas_.clear();  // Clear stack-allocated Box tracking
    current_fn_can_error_ = fn->canError();
    terminated_ = false;
    current_return_type_ = fn->returnType();
    current_can_error_ = fn->canError();
    // BUG FIX: Store the function's error type for the default terminator.
    // The default terminator needs the ErrorType to build the correct return
    // struct — using just current_return_type_ with can_error=true produces
    // a wrong fallback type like "{ i64, i1 }" instead of the actual type.
    current_fn_error_type_ = fn->errorType();
    current_fn_name_ = fn->name();
    current_ret_alloca_.clear();
    current_err_slot_.clear();
    caller_err_slot_.clear();
    current_fn_has_simd_ = fn->hasDirective(CompilerDirective::Simd);
    current_fn_has_tailcall_ = fn->hasDirective(CompilerDirective::Tailcall);
    deopt_counter_ = 0;
    current_block_label_ = "entry";

    alloca_ss_.clear();
    body_ss_.clear();

    // Build return type
    // Zig-style: error-returning functions return just the success type,
    // with error code communicated via an out-parameter (ptr %err_slot).
    std::string ret_type = llvmType(fn->returnType());

    // Build function signature
    // Linkage + inlining strategy:
    //   main      → external (must be visible to the linker entry point)
    //   pure      → alwaysinline (no side effects → always profitable to inline)
    //   small fn  → inlinehint (let LLVM know it's small and likely worth inlining)
    //   otherwise → external with inlinehint (allows PGO to profile functions
    //                while still hinting LLVM to inline when profitable)
    //
    // NOTE: We use external linkage (not internal) for non-main functions because:
    //   1. IR-level PGO (-fprofile-generate) requires visible symbols to profile
    //   2. Propeller/PGO code layout optimization needs function-level profiles
    //   3. LLVM's inliner still respects inlinehint/alwaysinline attributes
    //   4. -ffunction-sections + lld --gc-sections eliminates unused externals
    // Internal linkage prevented PGO from collecting profile data (0 functions
    // profiled), which is why we switched to external + inline attributes.
    std::string linkage;
    std::string inlining_attr;
    bool is_small_fn = fn->body() && fn->body()->stmts().size() <= 3;
    if (fn->name() == "main") {
        linkage = "";  // external: define dso_local
    } else if (fn->isPure()) {
        // Pure functions have no side effects → always profitable to inline.
        // Use alwaysinline for small pure fns, inlinehint for larger ones.
        linkage = "";  // external for PGO compatibility
        inlining_attr = is_small_fn ? " alwaysinline" : " inlinehint";
    } else if (fn->isInline()) {
        // Explicit inline keyword: force inlining at call site
        linkage = "";  // external for PGO compatibility
        inlining_attr = " alwaysinline";
    } else if (is_small_fn) {
        linkage = "";  // external for PGO compatibility
        inlining_attr = " alwaysinline";  // tiny functions → always inline
    } else {
        linkage = "";  // external: PGO-compatible, inlinehint guides inliner
        inlining_attr = " inlinehint";  // hint that inlining is profitable
    }

    std::string sig = "define " + linkage + "dso_local " + ret_type + " @" + sanitizeName(fn->name()) + "(";
    for (size_t i = 0; i < fn->paramCount(); ++i) {
        const auto& p = fn->params()[i];
        if (i > 0) sig += ", ";
        sig += llvmParamType(p.type);

        // --- Parameter attributes for LLVM optimization ---
        // These give LLVM the aliasing/null/alignment info it needs to
        // generate optimal code. Without these, LLVM must be conservative.
        bool is_ptr_type = p.type && (isa<PointerType>(p.type) ||
                                       isa<ReferenceType>(p.type) ||
                                       isa<MutReferenceType>(p.type) ||
                                       isa<AllocatorType>(p.type));
        bool is_slice_type = p.type && isa<SliceType>(p.type);

        // noalias: &mut T and &T params don't alias other pointers
        // (borrow checker guarantees this for &mut; &T is also safe because
        //  shared borrows can't mutate through the pointer)
        // Also: 'restrict' keyword adds noalias to any pointer parameter,
        // providing the HPC vectorization guarantee that the pointer does not
        // alias any other pointer in the function.
        if (p.type) {
            if (isa<MutReferenceType>(p.type)) {
                sig += " noalias";
            } else if (isa<ReferenceType>(p.type)) {
                sig += " noalias";  // shared borrows also don't alias mutable ones
            } else if (p.is_restrict && is_ptr_type) {
                // Explicit 'restrict' keyword on raw pointer parameters:
                // This is the programmer's guarantee that the pointer does not
                // alias any other pointer accessible from this function.
                // Critical for HPC auto-vectorization of tight loops.
                sig += " noalias";
            }
        }

        // nonnull: Tether references are never null (guaranteed by type system)
        if (is_ptr_type) {
            sig += " nonnull";
        }

        // readonly: &T (shared borrow) params can't be written through
        if (p.type && isa<ReferenceType>(p.type)) {
            sig += " readonly";
        }

        // dereferenceable(N): pointer points to at least N bytes
        // NOTE: dereferenceable is only valid on pointer-typed params, not
        // struct-typed params like slices. For slices, we use byval instead.
        if (is_ptr_type) {
            uint64_t deref_bytes = 0;
            if (isa<ReferenceType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<ReferenceType>(p.type).referent());
            } else if (isa<MutReferenceType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<MutReferenceType>(p.type).referent());
            } else if (isa<PointerType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<PointerType>(p.type).pointee());
            } else if (isa<AllocatorType>(p.type)) {
                deref_bytes = 40;  // TetherAllocator = 5 pointers = 40 bytes
            }
            if (deref_bytes > 0) {
                sig += " dereferenceable(" + std::to_string(deref_bytes) + ")";
            }
        }

        // align(N): propagate alignment from align(N) types
        if (p.type) {
            uint64_t align = 0;
            if (isa<AlignedType>(p.type)) {
                align = cast<AlignedType>(p.type).alignment();
            } else if (isa<ReferenceType>(p.type)) {
                TypeId inner = cast<ReferenceType>(p.type).referent();
                if (inner && isa<AlignedType>(inner))
                    align = cast<AlignedType>(inner).alignment();
            } else if (isa<MutReferenceType>(p.type)) {
                TypeId inner = cast<MutReferenceType>(p.type).referent();
                if (inner && isa<AlignedType>(inner))
                    align = cast<AlignedType>(inner).alignment();
            }
            if (align > 0 && align != 8) {  // 8 is default; skip for brevity
                sig += " align " + std::to_string(align);
            }
        }

        sig += " %" + sanitizeName(p.name);
    }
    // Zig-style: error-returning functions get an extra ptr %err_slot parameter
    if (fn->canError()) {
        if (fn->paramCount() > 0) sig += ", ";
        sig += "ptr noalias writeonly %__err_slot";  // err_slot is write-only by callee
    }
    sig += ")";

    // Function-level attributes
    if (fn->isPure()) {
        sig += " memory(none) nounwind";
    } else if (!fn->canError()) {
        // Add nounwind to functions that can't throw errors
        sig += " nounwind";
    }

    // noalloc function attribute: guarantees no heap allocation.
    // This is a stronger version of "nounwind" for allocation-free code.
    // Note: LLVM 17 does not support "alloc-family" as a function attribute,
    // so we simply skip emitting it. The noalloc guarantee is enforced by
    // the Tether compiler, not by LLVM.
    // (Do NOT emit a comment here — it would break the function signature.)

    // Inlining attribute (alwaysinline / inlinehint)
    sig += inlining_attr;

    // --- @tailcall directive: add "tail-call" function attribute ---
    // When a function has @tailcall, we add the "tail-call" attribute to
    // tell LLVM's optimizer to ensure tail call optimization. We also
    // emit musttail on self-recursive calls (handled in CallExpr emission).
    if (fn->hasDirective(CompilerDirective::Tailcall)) {
        sig += " \"tail-call\"";
    }

    // --- Metadata engine: hot/cold function attributes from L5 ---
    if (meta_map_) {
        auto* nm = meta_map_->get(fn);
        if (nm) {
            if (nm->llvm_meta.hot_path) sig += " hot";
            if (nm->llvm_meta.cold_path) sig += " cold";
        }
    }

    // --- Pre-LLVM optimization: opaque barrier function attributes ---
    // If the function takes or returns opaque types, add the
    // inaccessiblememonly attribute to prevent LLVM from making
    // aliasing assumptions across this function's calls.
    if (meta_map_) {
        auto* nm = meta_map_->get(fn);
        if (nm && nm->opaque_barrier) {
            if (!fn->isPure()) {  // Pure functions already have memory(none)
                sig += " inaccessiblememonly";
            }
        }
    }

    // Metadata for directives
    // NOTE: LLVM function definitions only support !prof and !dbg metadata
    // attachments. Arbitrary !N attachments are invalid LLVM IR and will
    // cause llvm-as to reject the module. Instead, we emit directive info
    // as comments and register the metadata node for the module-level
    // metadata section (where it IS valid).
    std::string metadata_comment;
    for (const auto& d : fn->directives()) {
        std::string key;
        switch (d) {
            case CompilerDirective::Superoptimize: key = "jules.superoptimize"; break;
            case CompilerDirective::Polly:         key = "jules.polly"; break;
            case CompilerDirective::Simd:          key = "jules.simd"; break;
            case CompilerDirective::Tailcall:       key = "jules.tailcall"; break;
        }
        int id = getMetadataId(key);
        metadata_comment += " ; directive:" + key + " (!" + std::to_string(id) + ")";
    }

    // No body → declaration only (use dso_local for all declarations)
    if (!fn->body()) {
        module_out_ << "declare dso_local " << ret_type << " @" << sanitizeName(fn->name()) << "(";
        for (size_t i = 0; i < fn->paramCount(); ++i) {
            const auto& p = fn->params()[i];
            if (i > 0) module_out_ << ", ";
            module_out_ << llvmParamType(p.type);
            // Same parameter attributes as definitions
            bool is_ptr_type = p.type && (isa<PointerType>(p.type) ||
                                           isa<ReferenceType>(p.type) ||
                                           isa<MutReferenceType>(p.type) ||
                                           isa<AllocatorType>(p.type));
            if (p.type) {
                if (isa<MutReferenceType>(p.type)) module_out_ << " noalias";
                else if (isa<ReferenceType>(p.type)) module_out_ << " noalias readonly";
            }
            if (is_ptr_type) module_out_ << " nonnull";
        }
        // Zig-style: error-returning function declarations also have the err_slot param
        if (fn->canError()) {
            if (fn->paramCount() > 0) module_out_ << ", ";
            module_out_ << "ptr noalias writeonly";
        }
        module_out_ << ")";
        if (fn->isPure()) module_out_ << " memory(none) nounwind";
        else if (!fn->canError()) module_out_ << " nounwind";
        module_out_ << "\n\n";
        return;
    }

    // --- Register parameters as SSA variables ---
    // Scalar parameters are tracked as SSA values (no alloca/load/store).
    // Aggregate parameters still need alloca + store because they're
    // accessed by pointer (GEP, memcpy, etc.).
    for (size_t i = 0; i < fn->paramCount(); ++i) {
        const auto& p = fn->params()[i];
        std::string ll = llvmType(p.type);
        std::string param_reg = "%" + sanitizeName(p.name);

        if (shouldUseSSA(p.type)) {
            // SSA: parameter value is directly available in its register
            var_ssa_[p.name] = SSAVarInfo{
                param_reg,    // current_value = the parameter register
                ll,           // llvm_type
                p.type,       // tether_type
                false,        // needs_alloca
                ""            // alloca_name (unused)
            };
        } else {
            // Aggregate / address-taken: use alloca + store
            std::string aname = makeAllocaName(p.name);
            alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";
            body_ss_ << "  store " << ll << " " << param_reg
                     << ", ptr " << aname << "\n";
            var_ssa_[p.name] = SSAVarInfo{
                param_reg,    // current_value (not used for alloca vars)
                ll,           // llvm_type
                p.type,       // tether_type
                true,         // needs_alloca
                aname         // alloca_name
            };
        }
    }

    // For error-returning functions, store the err_slot parameter name
    // so error-return paths can write the error code to it.
    // No need to alloca a result struct — the function returns just the
    // success type directly.
    if (fn->canError()) {
        current_err_slot_ = "%__err_slot";
    }

    // Emit the body
    emitBlockStmt(fn->body());

    // Default terminator if the block doesn't end with one
    if (!isTerminated()) {
        emitDeferBlocks();
        if (current_can_error_) {
            // Zig-style: no error on the default fallthrough path.
            // The err_slot was zero-initialized by the caller, so we just
            // return the zero-initialized success value (or void).
            std::string ll_val = llvmType(current_return_type_);
            if (ll_val == "void") {
                body_ss_ << "  ret void\n";
            } else if (isAggregateType(current_return_type_)) {
                // Aggregate: zero-initialize via memset, then load and return
                std::string zero = makeAllocaName("__zero_ret");
                alloca_ss_ << "  " << zero << " = alloca " << ll_val << "\n";
                needed_runtime_.insert("memset");
                body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << zero
                         << ", i8 0, i64 " << typeSizeBytes(current_return_type_)
                         << ", i1 false)\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load " << ll_val << ", ptr " << zero << "\n";
                body_ss_ << "  ret " << ll_val << " " << loaded << "\n";
            } else {
                body_ss_ << "  ret " << ll_val << " " << zeroConstant(ll_val) << "\n";
            }
        } else if (current_return_type_ && current_return_type_->isVoid()) {
            body_ss_ << "  ret void\n";
        } else {
            std::string rt = llvmType(current_return_type_);
            body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
        }
        setTerminated(true);
    }

    // Write the complete function
    module_out_ << sig << " {" << metadata_comment << "\n";
    module_out_ << "entry:\n";
    module_out_ << alloca_ss_.str();
    module_out_ << body_ss_.str();
    module_out_ << "}\n\n";

    // Emit Tether metadata for this function (records metadata for later
    // emission by emitTetherMetadata() at the end of generate()).
    emitTetherFnMetadata(fn);
}

// ============================================================================
// emitDeferBlocks
// ============================================================================
void IRGenerator::emitDeferBlocks() {
    if (defer_stack_.empty()) return;
    for (int i = static_cast<int>(defer_stack_.size()) - 1; i >= 0; --i) {
        emitStmt(defer_stack_[i]);
    }
}

// ============================================================================
// emitErrdeferBlocks
// ============================================================================
void IRGenerator::emitErrdeferBlocks() {
    // Emit errdefer blocks in reverse order (LIFO, like defer)
    for (auto it = errdefer_stack_.rbegin(); it != errdefer_stack_.rend(); ++it) {
        emitStmt(*it);
    }
}

// ============================================================================
// emitAtomicRMW
// ============================================================================
std::string IRGenerator::emitAtomicRMW(const std::string& result_reg,
                                        const std::string& ll_type,
                                        const std::string& ptr_reg,
                                        const std::string& val_reg,
                                        BinaryOp op,
                                        AtomicStmt::Ordering ordering) {
    std::string ordering_str;
    switch (ordering) {
        case AtomicStmt::Ordering::Relaxed: ordering_str = "monotonic"; break;
        case AtomicStmt::Ordering::Acquire: ordering_str = "acquire"; break;
        case AtomicStmt::Ordering::Release: ordering_str = "release"; break;
        case AtomicStmt::Ordering::AcqRel:  ordering_str = "acq_rel"; break;
        case AtomicStmt::Ordering::SeqCst:  ordering_str = "seq_cst"; break;
        default: ordering_str = "seq_cst"; break;
    }

    std::string rmw_op;
    switch (op) {
        case BinaryOp::Add: rmw_op = "add"; break;
        case BinaryOp::Sub: rmw_op = "sub"; break;
        case BinaryOp::BitAnd: rmw_op = "and"; break;
        case BinaryOp::BitOr: rmw_op = "or"; break;
        case BinaryOp::BitXor: rmw_op = "xor"; break;
        case BinaryOp::Shl: rmw_op = "shl"; break;   // min
        case BinaryOp::Shr: rmw_op = "shr"; break;    // max
        default: rmw_op = "add"; break;
    }

    // BUG FIX: LLVM 15+ opaque-pointer syntax requires 'ptr' type keyword
    // before the pointer operand in atomicrmw
    body_ss_ << "  " << result_reg << " = atomicrmw " << rmw_op << " " << ll_type
             << " ptr " << ptr_reg << ", " << ll_type << " " << val_reg
             << " " << ordering_str << "\n";
    return result_reg;
}

// ============================================================================
// emitBlockStmt
// ============================================================================
void IRGenerator::emitBlockStmt(BlockStmt* block) {
    if (!block) return;
    pushScope();
    for (const auto& stmt : block->stmts()) {
        if (isTerminated()) break;
        emitStmt(stmt.get());
    }
    popScope();
}

// ============================================================================
// emitStmt
// ============================================================================
void IRGenerator::emitStmt(Stmt* stmt) {
    if (!stmt || isTerminated()) return;

    switch (stmt->getKind()) {
        // ---- var decl ----
        case NodeKind::VarDeclStmt: {
            auto& vd = cast<VarDeclStmt>(*stmt);
            // Use declared type if available, otherwise infer from init expression
            TypeId var_type = vd.declaredType();
            if (var_type.isNull() && vd.hasInit()) var_type = vd.init()->getType();
            std::string ll = llvmType(var_type);

            // --- Pre-LLVM optimization: SROA (Scalar Replacement of Aggregates) ---
            // If this variable has sroa_eligible metadata, decompose it into
            // individual SSA variables for each field instead of using alloca.
            if (meta_map_ && var_type && isa<StructType>(var_type)) {
                const NodeMeta* nm = meta_map_->get(stmt);
                if (nm && nm->sroa_eligible) {
                    auto& st = cast<StructType>(var_type);
                    // Register SROA mapping for this variable
                    SROAVarInfo sroa_info;
                    sroa_info.struct_type_name = st.name();

                    // If there's an initializer, we need to extract each field's
                    // value from the StructInitExpr.
                    // First, emit the struct init to get a pointer to the value.
                    // Then extract each field via GEP+load.
                    std::string init_ptr;
                    if (vd.hasInit()) {
                        init_ptr = emitExpr(vd.init());
                    }

                    for (size_t fi = 0; fi < st.fields().size(); ++fi) {
                        const auto& field = st.fields()[fi];
                        std::string field_ssa_name = sroaFieldName(vd.name(), field.name);
                        std::string field_ll = llvmType(field.type);

                        sroa_info.field_names.push_back(field.name);
                        sroa_info.field_types.push_back(field.type);

                        if (vd.hasInit() && !init_ptr.empty()) {
                            // Extract field value from the initialized struct
                            std::string gep = nextReg();
                            body_ss_ << "  " << gep << " = getelementptr " << ll
                                     << ", ptr " << init_ptr << ", i32 0, i32 " << fi << "\n";
                            if (isAggregateType(field.type)) {
                                // Aggregate field — track as alloca pointer
                                registerVar(field_ssa_name, SSAVarInfo{
                                    "",            // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    true,          // needs_alloca
                                    gep            // alloca_name (points into the struct)
                                });
                            } else {
                                // Scalar field — load and track as SSA value
                                std::string loaded = nextReg();
                                body_ss_ << "  " << loaded << " = load " << field_ll
                                         << ", ptr " << gep << "\n";
                                registerVar(field_ssa_name, SSAVarInfo{
                                    loaded,        // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    false,         // needs_alloca
                                    ""             // alloca_name
                                });
                            }
                        } else {
                            // No initializer — zero-initialize each field
                            if (isAggregateType(field.type)) {
                                std::string aname = makeAllocaName(field_ssa_name);
                                alloca_ss_ << "  " << aname << " = alloca " << field_ll << "\n";
                                needed_runtime_.insert("memset");
                                body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << aname
                                         << ", i8 0, i64 " << typeSizeBytes(field.type)
                                         << ", i1 false)\n";
                                registerVar(field_ssa_name, SSAVarInfo{
                                    "",            // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    true,          // needs_alloca
                                    aname          // alloca_name
                                });
                            } else {
                                registerVar(field_ssa_name, SSAVarInfo{
                                    zeroConstant(field_ll),  // current_value
                                    field_ll,                // llvm_type
                                    field.type,              // tether_type
                                    false,                   // needs_alloca
                                    ""                       // alloca_name
                                });
                            }
                        }
                    }

                    sroa_vars_[vd.name()] = std::move(sroa_info);
                    break;  // SROA handled — skip the normal alloca path
                }
            }

            if (shouldUseSSA(var_type)) {
                // SSA: track value directly, no alloca
                std::string val;
                if (vd.hasInit()) {
                    val = emitExpr(vd.init());
                } else {
                    // Uninitialized scalar → use zero value
                    val = "0";
                }
                registerVar(vd.name(), SSAVarInfo{
                    val,          // current_value
                    ll,           // llvm_type
                    var_type,     // tether_type
                    false,        // needs_alloca
                    ""            // alloca_name
                });
            } else {
                // Aggregate / address-taken: alloca + store/memcpy
                std::string aname = makeAllocaName(vd.name());
                alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";
                registerVar(vd.name(), SSAVarInfo{
                    "",           // current_value (unused for alloca vars)
                    ll,           // llvm_type
                    var_type,     // tether_type
                    true,         // needs_alloca
                    aname         // alloca_name
                });
                if (vd.hasInit()) {
                    std::string val = emitExpr(vd.init());
                    if (isAggregateType(var_type)) {
                        body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << aname
                                 << ", ptr " << val << ", i64 " << typeSizeBytes(var_type)
                                 << ", i1 false)\n";
                        needed_runtime_.insert("memcpy");
                    } else {
                        body_ss_ << "  store " << ll << " " << val << ", ptr " << aname << "\n";
                    }
                }
            }
            break;
        }

        // ---- val decl ----
        case NodeKind::ValDeclStmt: {
            auto& vd = cast<ValDeclStmt>(*stmt);
            // Use declared type if available, otherwise infer from init expression
            TypeId val_type = vd.declaredType();
            if (val_type.isNull() && vd.hasInit()) val_type = vd.init()->getType();
            std::string ll = llvmType(val_type);

            // --- Pre-LLVM optimization: SROA (Scalar Replacement of Aggregates) ---
            // If this variable has sroa_eligible metadata, decompose it into
            // individual SSA variables for each field instead of using alloca.
            if (meta_map_ && val_type && isa<StructType>(val_type)) {
                const NodeMeta* nm = meta_map_->get(stmt);
                if (nm && nm->sroa_eligible) {
                    auto& st = cast<StructType>(val_type);
                    // Register SROA mapping for this variable
                    SROAVarInfo sroa_info;
                    sroa_info.struct_type_name = st.name();

                    // Emit the struct init to get a pointer to the value.
                    std::string init_ptr;
                    if (vd.hasInit()) {
                        init_ptr = emitExpr(vd.init());
                    }

                    for (size_t fi = 0; fi < st.fields().size(); ++fi) {
                        const auto& field = st.fields()[fi];
                        std::string field_ssa_name = sroaFieldName(vd.name(), field.name);
                        std::string field_ll = llvmType(field.type);

                        sroa_info.field_names.push_back(field.name);
                        sroa_info.field_types.push_back(field.type);

                        if (vd.hasInit() && !init_ptr.empty()) {
                            // Extract field value from the initialized struct
                            std::string gep = nextReg();
                            body_ss_ << "  " << gep << " = getelementptr " << ll
                                     << ", ptr " << init_ptr << ", i32 0, i32 " << fi << "\n";
                            if (isAggregateType(field.type)) {
                                registerVar(field_ssa_name, SSAVarInfo{
                                    "",            // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    true,          // needs_alloca
                                    gep            // alloca_name
                                });
                            } else {
                                std::string loaded = nextReg();
                                body_ss_ << "  " << loaded << " = load " << field_ll
                                         << ", ptr " << gep << "\n";
                                registerVar(field_ssa_name, SSAVarInfo{
                                    loaded,        // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    false,         // needs_alloca
                                    ""             // alloca_name
                                });
                            }
                        } else {
                            // No initializer — zero-initialize each field
                            if (isAggregateType(field.type)) {
                                std::string aname = makeAllocaName(field_ssa_name);
                                alloca_ss_ << "  " << aname << " = alloca " << field_ll << "\n";
                                needed_runtime_.insert("memset");
                                body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << aname
                                         << ", i8 0, i64 " << typeSizeBytes(field.type)
                                         << ", i1 false)\n";
                                registerVar(field_ssa_name, SSAVarInfo{
                                    "",            // current_value
                                    field_ll,      // llvm_type
                                    field.type,    // tether_type
                                    true,          // needs_alloca
                                    aname          // alloca_name
                                });
                            } else {
                                registerVar(field_ssa_name, SSAVarInfo{
                                    zeroConstant(field_ll),  // current_value
                                    field_ll,                // llvm_type
                                    field.type,              // tether_type
                                    false,                   // needs_alloca
                                    ""                       // alloca_name
                                });
                            }
                        }
                    }

                    sroa_vars_[vd.name()] = std::move(sroa_info);
                    break;  // SROA handled — skip the normal alloca path
                }
            }

            if (shouldUseSSA(val_type)) {
                // SSA: track value directly, no alloca
                std::string val;
                if (vd.hasInit()) {
                    val = emitExpr(vd.init());
                } else {
                    val = "0";
                }
                registerVar(vd.name(), SSAVarInfo{
                    val,          // current_value
                    ll,           // llvm_type
                    val_type,     // tether_type
                    false,        // needs_alloca
                    ""            // alloca_name
                });
            } else {
                // Aggregate / address-taken: alloca + store/memcpy
                std::string aname = makeAllocaName(vd.name());
                alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";
                registerVar(vd.name(), SSAVarInfo{
                    "",           // current_value (unused for alloca vars)
                    ll,           // llvm_type
                    val_type,     // tether_type
                    true,         // needs_alloca
                    aname         // alloca_name
                });
                if (vd.hasInit()) {
                    std::string val = emitExpr(vd.init());
                    if (isAggregateType(val_type)) {
                        body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << aname
                                 << ", ptr " << val << ", i64 " << typeSizeBytes(val_type)
                                 << ", i1 false)\n";
                        needed_runtime_.insert("memcpy");
                    } else {
                        body_ss_ << "  store " << ll << " " << val << ", ptr " << aname << "\n";
                    }
                }
            }
            break;
        }

        // ---- assignment ----
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            TypeId target_type = as.target()->getType();
            std::string val = emitExpr(as.value());
            TypeId val_type = as.value()->getType();

            // BUG FIX: Convert the value to the target type if they differ.
            if (val_type && target_type &&
                !isAggregateType(target_type) &&
                llvmType(val_type) != llvmType(target_type)) {
                val = emitTypeCast(val, val_type, target_type);
            }

            // Check if the target is a simple scalar variable (SSA-eligible)
            if (auto* ident = dyn_cast<IdentExpr>(as.target())) {
                SSAVarInfo* info = lookupVar(ident->name());
                if (info && !info->needs_alloca) {
                    // SSA assignment: just update the tracked value
                    updateVarValue(ident->name(), val);
                    break;
                }
            }

            // Fallback: alloca-based store via emitLValue
            std::string target_ptr = emitLValue(as.target());
            if (isAggregateType(target_type)) {
                body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << target_ptr
                         << ", ptr " << val << ", i64 " << typeSizeBytes(target_type)
                         << ", i1 false)\n";
                needed_runtime_.insert("memcpy");
            } else {
                body_ss_ << "  store " << llvmType(target_type) << " " << val
                         << ", ptr " << target_ptr << "\n";
            }
            break;
        }

        // ---- defer ----
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            defer_stack_.push_back(ds.stmt());
            break;
        }

        // ---- if ----
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);

            // --- Pre-LLVM optimization: select conversion for branchless code ---
            // When the metadata engine marks an if/else as profitable for select
            // conversion (both branches are cheap, no side effects), emit a
            // select instruction instead of a branch. This eliminates branch
            // misprediction overhead and enables vectorization.
            {
                auto* nm = meta_map_ ? meta_map_->get(&is) : nullptr;
                bool can_select = nm && nm->select_profit == SelectProfitability::Profitable
                                  && is.hasElse()
                                  && is.thenBlock() && !is.thenBlock()->stmts().empty()
                                  && is.elseBlock() && !is.elseBlock()->stmts().empty()
                                  && is.thenBlock()->stmts().size() == 1
                                  && is.elseBlock()->stmts().size() == 1
                                  && isa<ExprStmt>(is.thenBlock()->stmts()[0].get())
                                  && isa<ExprStmt>(is.elseBlock()->stmts()[0].get());

                if (can_select) {
                    auto& then_es = cast<ExprStmt>(*is.thenBlock()->stmts()[0]);
                    auto& else_es = cast<ExprStmt>(*is.elseBlock()->stmts()[0]);

                    // Only convert if both expressions have non-void types
                    TypeId then_type = then_es.expr()->getType();
                    TypeId else_type = else_es.expr()->getType();

                    if (then_type && else_type &&
                        !then_type->isVoid() && !else_type->isVoid() &&
                        !isAggregateType(then_type) && !isAggregateType(else_type)) {
                        std::string cond_val = emitExpr(is.condition());
                        // Ensure i1
                        if (is.condition()->getType() && !is.condition()->getType()->isBool()) {
                            std::string c = nextReg();
                            body_ss_ << "  " << c << " = icmp ne " << llvmType(is.condition()->getType())
                                     << " " << cond_val << ", 0\n";
                            cond_val = c;
                        }

                        // Snapshot SSA state before emitting then branch
                        SSASnapshot pre_then_snap = takeSnapshot();

                        // Emit then expression
                        std::string then_val = emitExpr(then_es.expr());

                        // Snapshot after then
                        SSASnapshot after_then_snap = takeSnapshot();

                        // Restore SSA to pre-then state for else branch
                        for (auto& [name, val] : pre_then_snap.values) {
                            updateVarValue(name, val);
                        }

                        // Emit else expression
                        std::string else_val = emitExpr(else_es.expr());

                        // Emit select instruction
                        std::string result_type = llvmType(then_type);
                        if (result_type != "void" && !result_type.empty()) {
                            std::string result = nextReg();
                            body_ss_ << "  " << result << " = select i1 " << cond_val
                                     << ", " << result_type << " " << then_val
                                     << ", " << result_type << " " << else_val << "\n";

                            // Merge SSA state: use after_then for vars not touched by else,
                            // and current values for else-touched vars. For simplicity,
                            // use after_then values for all vars and override with current.
                            for (auto& [name, val] : after_then_snap.values) {
                                updateVarValue(name, val);
                            }
                        }

                        break;  // Skip the rest of the if/else emission
                    }
                }
            }

            std::string cond = emitExpr(is.condition());
            // Ensure i1
            if (is.condition()->getType() && !is.condition()->getType()->isBool()) {
                std::string c = nextReg();
                body_ss_ << "  " << c << " = icmp ne " << llvmType(is.condition()->getType())
                         << " " << cond << ", 0\n";
                cond = c;
            }
            std::string then_l = nextLabel("then");
            std::string else_l = is.hasElse() ? nextLabel("else") : "";
            std::string merge_l = nextLabel("ifmerge");

            // --- Pre-LLVM optimization: cold path branch weights ---
            std::string cold_meta = emitColdPathMetadata(stmt);
            if (cold_meta.empty() && is.condition()) {
                cold_meta = emitColdPathMetadata(is.condition());
            }

            // --- Metadata engine: branch probability from L1/L5 ---
            // If the MetadataMap has branch probability info for this IfStmt,
            // emit !prof metadata. This supplements the cold_meta from the
            // pre-LLVM pipeline and takes priority when both exist.
            if (meta_map_ && cold_meta.empty()) {
                auto* nm = meta_map_->get(&is);
                if (nm && nm->branch_prob != BranchProbability::Unknown) {
                    int prof_id = getBranchWeightMetadataId(
                        nm->branch_prob == BranchProbability::Likely ? 80 : 20,
                        nm->branch_prob == BranchProbability::Likely ? 20 : 80
                    );
                    cold_meta = ", !prof !" + std::to_string(prof_id);
                }
            }

            // Snapshot SSA values before branching (for phi node emission)
            SSASnapshot pre_branch_snap = takeSnapshot();
            // Remember the label of the block that contains the branch instruction
            std::string branch_block = current_block_label_;

            // --- Speculative optimization: deoptimization guard ---
            // If the speculative optimizer determined that one branch is
            // almost never taken, route the unlikely path to a deopt block
            // instead of normal code. This eliminates the overhead of the
            // cold path entirely in the fast path.
            bool has_deopt_guard = false;
            bool then_is_unlikely = false;
            if (meta_map_) {
                auto* nm = meta_map_->get(stmt);
                if (nm && nm->has_speculative_assumption &&
                    nm->speculative_kind == AssumptionKind::BranchNeverTaken) {
                    has_deopt_guard = true;
                    // Determine which branch is unlikely
                    auto* if_nm = meta_map_->get(&is);
                    if (if_nm) {
                        then_is_unlikely = (if_nm->branch_prob == BranchProbability::Unlikely);
                    }
                }
            }

            if (has_deopt_guard) {
                // Speculative: route the unlikely branch to a deopt block
                int deopt_id = deopt_counter_++;
                std::string deopt_l = nextLabel("deopt");

                // BUG FIX: When there's no else branch, else_l is empty.
                // The "likely" path for a no-else if where then is unlikely
                // must be the merge label (fallthrough), not an empty label.
                // We need a concrete label for the likely path.
                std::string likely_l;
                if (then_is_unlikely) {
                    if (is.hasElse()) {
                        likely_l = else_l;
                    } else {
                        // No else block — the likely path falls through to merge
                        likely_l = merge_l;
                    }
                } else {
                    likely_l = then_l;
                }
                std::string unlikely_l = then_is_unlikely ? then_l : deopt_l;

                // Branch to likely path and deopt block
                if (then_is_unlikely) {
                    body_ss_ << "  br i1 " << cond << ", label %" << deopt_l
                             << ", label %" << likely_l << cold_meta << "\n";
                } else {
                    body_ss_ << "  br i1 " << cond << ", label %" << likely_l
                             << ", label %" << deopt_l << cold_meta << "\n";
                }
                setTerminated(false);

                // Emit the deoptimization block
                emitBlockLabel(deopt_l);
                needed_runtime_.insert("tether_deopt");
                body_ss_ << "  ; [speculative] deoptimization guard, deopt_id=" << deopt_id << "\n";
                body_ss_ << "  call void @tether_deopt(i64 " << deopt_id << ", ptr null)\n";
                body_ss_ << "  unreachable\n";
                setTerminated(true);

                // Emit the likely path
                if (then_is_unlikely && !is.hasElse()) {
                    // No else block and then is unlikely: the likely path is
                    // just the fallthrough (merge block). We don't emit a
                    // separate block here — the merge block IS the likely path.
                    // Just update SSA values from the pre-branch snapshot.
                    SSASnapshot likely_snap = takeSnapshot();
                    // The merge label will be emitted below
                    setTerminated(false);
                    // Mark that we still need the merge block
                    bool likely_terminated = false;

                    // Emit the merge block directly
                    emitBlockLabel(merge_l);
                    for (auto& [name, val] : pre_branch_snap.values) {
                        updateVarValue(name, val);
                    }
                    setTerminated(false);
                } else {
                    // Emit the likely path as a real block
                    emitBlockLabel(likely_l);
                    setTerminated(false);
                    if (then_is_unlikely && is.hasElse()) {
                        emitBlockStmt(is.elseBlock());
                    } else {
                        emitBlockStmt(is.thenBlock());
                    }
                    // Snapshot at end of likely path
                    SSASnapshot likely_snap = takeSnapshot();
                    bool likely_terminated = isTerminated();
                    std::string likely_exit_block = current_block_label_;
                    if (!isTerminated()) body_ss_ << "  br label %" << merge_l << "\n";
                    setTerminated(false);

                    // Emit the merge block
                    if (!likely_terminated) {
                        emitBlockLabel(merge_l);
                        // Just use the likely snap values (no phi needed with deopt)
                        for (auto& [name, val] : likely_snap.values) {
                            updateVarValue(name, val);
                        }
                        setTerminated(false);
                    } else {
                        setTerminated(true);
                    }
                }
                break;
            }

            if (is.hasElse()) {
                body_ss_ << "  br i1 " << cond << ", label %" << then_l
                         << ", label %" << else_l << cold_meta << "\n";
            } else {
                body_ss_ << "  br i1 " << cond << ", label %" << then_l
                         << ", label %" << merge_l << cold_meta << "\n";
            }
            setTerminated(false);

            emitBlockLabel(then_l);
            emitBlockStmt(is.thenBlock());
            // Snapshot SSA values at end of then-block
            SSASnapshot then_snap = takeSnapshot();
            // Remember which block branches to merge (for phi predecessor)
            bool then_terminated = isTerminated();
            std::string then_exit_block = current_block_label_;
            if (!isTerminated()) body_ss_ << "  br label %" << merge_l << "\n";
            setTerminated(false);

            SSASnapshot else_snap;
            std::string else_exit_block;
            bool else_terminated = false;
            if (is.hasElse()) {
                emitBlockLabel(else_l);
                // Restore SSA values to pre-branch state before emitting else
                for (auto& [name, val] : pre_branch_snap.values) {
                    updateVarValue(name, val);
                }
                emitBlockStmt(is.elseBlock());
                else_snap = takeSnapshot();
                else_terminated = isTerminated();
                else_exit_block = current_block_label_;
                if (!isTerminated()) body_ss_ << "  br label %" << merge_l << "\n";
                setTerminated(false);
            } else {
                // No else: the "else" path is the fallthrough from branch_block
                else_snap = pre_branch_snap;
                else_exit_block = branch_block;
            }

            // BUG FIX: Only emit the merge block and phi nodes if at least
            // one path actually branches to merge (is not terminated with
            // return/break). If both paths are terminated, the merge block
            // is unreachable.
            bool then_reaches_merge = !then_terminated;
            bool else_reaches_merge = !else_terminated;

            if (then_reaches_merge || else_reaches_merge) {
                emitBlockLabel(merge_l);
                // Emit phi nodes only for predecessors that actually branch to merge
                if (then_reaches_merge && else_reaches_merge) {
                    // Both paths reach merge — normal phi
                    emitPhiNodes(then_snap, then_exit_block, else_snap, else_exit_block);
                } else if (then_reaches_merge) {
                    // Only then path reaches merge — no phi needed, just use then values
                    for (auto& [name, val] : then_snap.values) {
                        updateVarValue(name, val);
                    }
                } else {
                    // Only else path reaches merge — no phi needed, just use else values
                    for (auto& [name, val] : else_snap.values) {
                        updateVarValue(name, val);
                    }
                }
                setTerminated(false);
            } else {
                // Both paths are terminated — merge block is unreachable.
                // Still need to set terminated state.
                setTerminated(true);
            }
            break;
        }

        // ---- while ----
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            std::string cond_l = nextLabel("while.cond");
            std::string body_l = nextLabel("while.body");
            std::string end_l  = nextLabel("while.end");

            loop_stack_.push_back({end_l, cond_l});

            // Before emitting the loop, demote any SSA variables that are
            // assigned inside the loop body, condition, or increment clause.
            // These need alloca-based tracking so LLVM's mem2reg can insert
            // proper phi nodes at the loop header.
            auto assigned_in_loop = collectAssignedVars(ws.body());
            if (ws.hasIncrement()) {
                auto incr_vars = collectAssignedVars(ws.increment());
                assigned_in_loop.insert(incr_vars.begin(), incr_vars.end());
            }
            // Also check condition for assignments (rare but valid)
            auto cond_vars = collectAssignedVars(ws.condition());
            assigned_in_loop.insert(cond_vars.begin(), cond_vars.end());
            for (const auto& name : assigned_in_loop) {
                demoteSSAToAlloca(name);
            }

            body_ss_ << "  br label %" << cond_l << "\n";

            emitBlockLabel(cond_l);
            setTerminated(false);
            std::string cond = emitExpr(ws.condition());
            if (ws.condition()->getType() && !ws.condition()->getType()->isBool()) {
                std::string c = nextReg();
                body_ss_ << "  " << c << " = icmp ne " << llvmType(ws.condition()->getType())
                         << " " << cond << ", 0\n";
                cond = c;
            }
            body_ss_ << "  br i1 " << cond << ", label %" << body_l
                     << ", label %" << end_l << "\n";

            emitBlockLabel(body_l);
            setTerminated(false);

            // --- Pre-LLVM optimization: yield check insertion ---
            emitYieldCheckIfAnnotated(&ws);

            // --- Pre-LLVM optimization: prefetch insertion ---
            emitPrefetchIfAnnotated(&ws);

            emitBlockStmt(ws.body());
            if (!isTerminated()) {
                if (ws.hasIncrement()) emitExpr(ws.increment());

                // Collect loop metadata from all sources:
                // 1. @simd directive (existing)
                // 2. Metadata engine L3/L5 (vectorizable, prefetch, unroll)
                bool has_any_loop_md = current_fn_has_simd_;
                int simd_md_id = 0;

                if (current_fn_has_simd_) {
                    simd_md_id = emitSimdLoopMetadata();
                }

                // Check MetadataMap for L3/L5 loop metadata
                int meta_loop_md_id = 0;
                if (meta_map_) {
                    auto* nm = meta_map_->get(&ws);
                    if (nm) {
                        bool needs_meta_md = false;
                        // L3: vectorization-safe from access pattern analysis
                        if (nm->llvm_meta.vectorization_safe && !current_fn_has_simd_) {
                            needs_meta_md = true;
                        }
                        // L3: access pattern says vectorizable
                        if (!nm->access_patterns.empty()) {
                            for (const auto& ap : nm->access_patterns) {
                                if (ap.vectorizable && !current_fn_has_simd_) {
                                    needs_meta_md = true;
                                    break;
                                }
                            }
                        }
                        // L3: prefetch distance hint
                        if (nm->llvm_meta.prefetch_distance > 0) {
                            needs_meta_md = true;
                        }
                        // L6: profile-guided unroll count
                        if (nm->profile.has_profile && nm->profile.loop_iteration_count >= 2) {
                            needs_meta_md = true;
                        }

                        if (needs_meta_md) {
                            // Build combined loop metadata entries
                            std::vector<int> entry_ids;
                            if (nm->llvm_meta.vectorization_safe || !nm->access_patterns.empty()) {
                                bool any_vec = nm->llvm_meta.vectorization_safe;
                                if (!any_vec) {
                                    for (const auto& ap : nm->access_patterns) {
                                        if (ap.vectorizable) { any_vec = true; break; }
                                    }
                                }
                                if (any_vec && !current_fn_has_simd_) {
                                    int vec_id = metadata_counter_++;
                                    metadata_entries_.push_back({vec_id,
                                        "!{!\"llvm.loop.vectorize.enable\", i1 true}"});
                                    entry_ids.push_back(vec_id);
                                }
                            }
                            if (nm->llvm_meta.prefetch_distance > 0) {
                                int pf_id = metadata_counter_++;
                                metadata_entries_.push_back({pf_id,
                                    "!{!\"tether.prefetch_distance\", i32 " +
                                    std::to_string(static_cast<int32_t>(nm->llvm_meta.prefetch_distance)) + "}"});
                                entry_ids.push_back(pf_id);
                            }
                            if (nm->profile.has_profile && nm->profile.loop_iteration_count >= 2) {
                                uint64_t unroll = nm->profile.loop_iteration_count;
                                if (unroll > 8) unroll = 8;
                                int unroll_id = metadata_counter_++;
                                metadata_entries_.push_back({unroll_id,
                                    "!{!\"llvm.loop.unroll.count\", i32 " +
                                    std::to_string(unroll) + "}"});
                                entry_ids.push_back(unroll_id);
                            }
                            if (!entry_ids.empty()) {
                                meta_loop_md_id = metadata_counter_++;
                                std::string combined = "distinct !{";
                                for (size_t i = 0; i < entry_ids.size(); ++i) {
                                    if (i > 0) combined += ", ";
                                    combined += "!" + std::to_string(entry_ids[i]);
                                }
                                combined += "}";
                                metadata_entries_.push_back({meta_loop_md_id, combined});
                                has_any_loop_md = true;
                            }
                        }
                    }
                }

                if (has_any_loop_md) {
                    // If we have both @simd and metadata engine hints,
                    // combine them into a single loop metadata node
                    if (simd_md_id && meta_loop_md_id) {
                        int combined_id = metadata_counter_++;
                        metadata_entries_.push_back({combined_id,
                            "distinct !{!" + std::to_string(simd_md_id) +
                            ", !" + std::to_string(meta_loop_md_id) + "}"});
                        body_ss_ << "  br label %" << cond_l
                                 << ", !llvm.loop !" << combined_id << "\n";
                    } else if (simd_md_id) {
                        body_ss_ << "  br label %" << cond_l
                                 << ", !llvm.loop !" << simd_md_id << "\n";
                    } else {
                        body_ss_ << "  br label %" << cond_l
                                 << ", !llvm.loop !" << meta_loop_md_id << "\n";
                    }
                } else {
                    body_ss_ << "  br label %" << cond_l << "\n";
                }
            }
            setTerminated(false);

            emitBlockLabel(end_l);
            setTerminated(false);
            loop_stack_.pop_back();
            break;
        }

        // ---- return ----
        case NodeKind::ReturnStmt: {
            auto& rs = cast<ReturnStmt>(*stmt);
            emitDeferBlocks();

            // Check if this is a tail call opportunity: the return value is
            // directly a CallExpr with matching return type. If so, we can
            // emit the call with `musttail` to guarantee LLVM eliminates the
            // stack frame (TCO). This applies even without the @tailcall
            // directive — any CallExpr in direct tail position qualifies.
            bool is_tail_call = false;
            if (rs.hasValue() && rs.value()->getKind() == NodeKind::CallExpr) {
                TypeId ret_type = current_return_type_;
                // Only for non-aggregate, non-error returns (musttail requires
                // the call result to be directly returned with matching types)
                if (ret_type && !isAggregateType(ret_type) && !current_can_error_) {
                    // The return type of the call must match the function's return type
                    TypeId call_type = rs.value()->getType();
                    // For non-error types, the call's type must match the return type
                    if (call_type && call_type == ret_type) {
                        is_tail_call = true;
                    }
                    // Also allow void-to-void tail calls
                    if (ret_type->isVoid() && call_type && call_type->isVoid()) {
                        is_tail_call = true;
                    }
                }
            }

            if (is_tail_call) {
                // Set the musttail flag so the CallExpr emitter prefixes the
                // call with `musttail`. The flag is consumed by the emitter.
                is_musttail_call_ = true;
                std::string val = emitExpr(rs.value());
                std::string ll = llvmType(current_return_type_);
                if (ll == "void") {
                    body_ss_ << "  ret void\n";
                } else {
                    body_ss_ << "  ret " << ll << " " << val << "\n";
                }
            } else if (current_can_error_) {
                std::string vt = llvmType(current_return_type_);
                if (rs.hasValue()) {
                    std::string val = emitExpr(rs.value());
                    // BUG FIX: If the expression type doesn't match the return type,
                    // insert a conversion (e.g. int→float).
                    TypeId val_type = rs.value()->getType();
                    if (val_type && current_return_type_ &&
                        !isAggregateType(current_return_type_) && vt != "void") {
                        val = emitTypeCast(val, val_type, current_return_type_);
                    }
                    if (isAggregateType(current_return_type_)) {
                        std::string loaded = nextReg();
                        body_ss_ << "  " << loaded << " = load " << vt << ", ptr " << val << "\n";
                        body_ss_ << "  ret " << vt << " " << loaded << "\n";
                    } else if (vt == "void") {
                        body_ss_ << "  ret void\n";
                    } else {
                        body_ss_ << "  ret " << vt << " " << val << "\n";
                    }
                } else {
                    if (vt == "void") {
                        body_ss_ << "  ret void\n";
                    } else {
                        body_ss_ << "  ret " << vt << " " << zeroConstant(vt) << "\n";
                    }
                }
            } else {
                if (rs.hasValue()) {
                    std::string val = emitExpr(rs.value());
                    std::string ll = llvmType(current_return_type_);
                    // BUG FIX: Same type conversion for non-error returns
                    TypeId val_type = rs.value()->getType();
                    if (val_type && current_return_type_ &&
                        !isAggregateType(current_return_type_) && ll != "void") {
                        val = emitTypeCast(val, val_type, current_return_type_);
                    }
                    if (isAggregateType(current_return_type_)) {
                        std::string loaded = nextReg();
                        body_ss_ << "  " << loaded << " = load " << ll << ", ptr " << val << "\n";
                        body_ss_ << "  ret " << ll << " " << loaded << "\n";
                    } else {
                        body_ss_ << "  ret " << ll << " " << val << "\n";
                    }
                } else {
                    body_ss_ << "  ret void\n";
                }
            }
            setTerminated(true);
            break;
        }

        // ---- break ----
        case NodeKind::BreakStmt: {
            if (!loop_stack_.empty()) {
                emitDeferBlocks();
                body_ss_ << "  br label %" << loop_stack_.back().break_label << "\n";
            }
            setTerminated(true);
            break;
        }

        // ---- continue ----
        case NodeKind::ContinueStmt: {
            if (!loop_stack_.empty()) {
                emitDeferBlocks();
                body_ss_ << "  br label %" << loop_stack_.back().continue_label << "\n";
            }
            setTerminated(true);
            break;
        }

        // ---- expression statement ----
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            emitExpr(es.expr());
            break;
        }

        // ---- block statement ----
        case NodeKind::BlockStmt: {
            emitBlockStmt(&cast<BlockStmt>(*stmt));
            break;
        }

        // ---- errdefer ----
        case NodeKind::ErrdeferStmt: {
            auto& es = static_cast<ErrdeferStmt&>(*stmt);
            // Push onto the errdefer stack; will be emitted on error paths only
            errdefer_stack_.push_back(es.stmt());
            break;
        }

        // ---- atomic ----
        case NodeKind::AtomicStmt: {
            auto& as = static_cast<AtomicStmt&>(*stmt);
            // Emit fence before the atomic operation
            std::string ordering_str;
            switch (as.ordering()) {
                case AtomicStmt::Ordering::Relaxed: ordering_str = "monotonic"; break;
                case AtomicStmt::Ordering::Acquire: ordering_str = "acquire"; break;
                case AtomicStmt::Ordering::Release: ordering_str = "release"; break;
                case AtomicStmt::Ordering::AcqRel:  ordering_str = "acq_rel"; break;
                case AtomicStmt::Ordering::SeqCst:  ordering_str = "seq_cst"; break;
                default: ordering_str = "seq_cst"; break;
            }
            body_ss_ << "  fence " << ordering_str << "\n";
            // Emit the inner statement
            emitStmt(as.inner());
            // Fence after for Acquire/SeqCst
            if (as.ordering() == AtomicStmt::Ordering::Acquire ||
                as.ordering() == AtomicStmt::Ordering::SeqCst) {
                body_ss_ << "  fence " << ordering_str << "\n";
            }
            break;
        }

        // ---- yield ----
        case NodeKind::YieldStmt: {
            auto& ys = static_cast<YieldStmt&>(*stmt);
            // Yield is a cooperative context switch - call the runtime yield function
            needed_runtime_.insert("tether_yield");
            if (ys.hasValue()) {
                std::string val = emitExpr(ys.value());
                // BUG FIX: Cast non-i64 values to i64 before passing to tether_yield.
                // tether_yield expects i64, but the yielded value may be i32, f64, etc.
                TypeId val_type = ys.value()->getType();
                if (val_type && !val_type->isVoid()) {
                    std::string ll = llvmType(val_type);
                    if (ll != "i64") {
                        std::string cast_val = nextReg();
                        if (ll == "f64" || ll == "f32") {
                            body_ss_ << "  " << cast_val << " = fptosi " << ll
                                     << " " << val << " to i64\n";
                        } else {
                            body_ss_ << "  " << cast_val << " = zext " << ll
                                     << " " << val << " to i64\n";
                        }
                        val = cast_val;
                    }
                }
                body_ss_ << "  call void @tether_yield(i64 " << val << ")\n";
            } else {
                body_ss_ << "  call void @tether_yield(i64 0)\n";
            }
            break;
        }

        // ---- match (exhaustive pattern matching on enums) ----
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            // Emit as an if-else chain (basic implementation)
            // TODO: Optimize with jump tables for dense enum discriminants
            std::string subject_val = emitExpr(ms.subject());
            TypeId subject_type = ms.subject()->getType();
            std::string ll_subject = llvmType(subject_type);

            // Take a snapshot before the match for phi node emission
            SSASnapshot entry_snap = takeSnapshot();

            std::string merge_label = nextLabel("match_merge");
            std::vector<std::string> arm_labels;
            for (size_t i = 0; i < ms.armCount(); ++i) {
                arm_labels.push_back(nextLabel("match_arm"));
            }

            // Branch from current block to the first arm
            if (!isTerminated() && !arm_labels.empty()) {
                body_ss_ << "  br label %" << arm_labels[0] << "\n";
            }

            for (size_t i = 0; i < ms.armCount(); ++i) {
                const auto& arm = ms.arms()[i];
                emitBlockLabel(arm_labels[i]);
                setTerminated(false);

                // Emit comparison: subject == pattern
                std::string pattern_val = emitExpr(arm.pattern.get());
                std::string cmp_reg = nextReg();
                body_ss_ << "  " << cmp_reg << " = icmp eq " << ll_subject
                         << " " << subject_val << ", " << pattern_val << "\n";

                std::string next_arm_label = (i + 1 < ms.armCount())
                    ? arm_labels[i + 1] : merge_label;
                std::string body_label = arm_labels[i] + ".body";

                body_ss_ << "  br i1 " << cmp_reg << ", label %" << body_label
                         << ", label %" << next_arm_label << "\n";

                emitBlockLabel(body_label);
                setTerminated(false);
                pushScope();
                emitBlockStmt(arm.body.get());
                popScope();
                if (!isTerminated()) {
                    body_ss_ << "  br label %" << merge_label << "\n";
                    setTerminated(true);
                }
            }

            // Merge point
            emitBlockLabel(merge_label);
            setTerminated(false);
            break;
        }

        // ---- spawn (async task dispatch via work-stealing pool) ----
        case NodeKind::SpawnStmt: {
            auto& sp = cast<SpawnStmt>(*stmt);
            // Emit as a tether_spawn runtime call.
            // The task expression must be a call expression: spawn func(args)
            // We emit it as: tether_spawn(task_fn, context_ptr, context_size)
            // where task_fn wraps the call and context holds the arguments.

            // For now, emit as a direct call (synchronous) with runtime spawn wrapper.
            // The full async implementation requires a trampoline function that
            // captures the closure context — this is a future enhancement.
            needed_runtime_.insert("tether_spawn");
            needed_runtime_.insert("tether_task_await");
            needed_runtime_.insert("tether_taskpool_init");

            // Emit the task expression as a regular call
            // (Full async would require extracting the callee and args into
            //  a heap-allocated context struct and generating a trampoline)
            std::string task_result = emitExpr(sp.task());

            // Add comment marking this as a spawn site for future async lowering
            body_ss_ << "  ; spawn site — currently synchronous\n";
            break;
        }

        default: break;
    }
}

// ============================================================================
// emitExpr – dispatch on expression kind
// ============================================================================
std::string IRGenerator::emitExpr(Expr* expr) {
    if (!expr) return "0";

    // If the expression's type is PoisonType, emit a stub value instead of
    // trying to generate real code for a type that doesn't exist in LLVM.
    if (expr->hasType() && expr->getType() && isa<PoisonType>(expr->getType())) {
        auto reg = nextReg();
        body_ss_ << "  " << reg << " = add i32 0, 0 ; poison type stub\n";
        return reg;
    }

    // ========================================================================
    // Comptime-evaluated expression: emit the precomputed constant value
    // instead of the full expression tree. This eliminates entire categories
    // of runtime computation — the Zig philosophy: if it can be computed at
    // compile time, it IS computed at compile time.
    // ========================================================================
    if (meta_map_) {
        const NodeMeta* nm = meta_map_->get(expr);
        if (nm && nm->comptime_evaluated) {
            TypeId ty = expr->getType();
            std::string ll_type = ty ? llvmType(ty) : "i64";

            // Int comptime value
            if (nm->comptime_int_value != 0 || (ty && ty->isInteger())) {
                // Check if it's actually an int type (not just default 0)
                // We use a heuristic: if the type is integer or the value is non-zero
                // or the type is bool, treat as int
                if (ty && (ty->isInteger() || ty->isBool())) {
                    if (ty->isBool()) {
                        return nm->comptime_bool_value ? "true" : "false";
                    }
                    // Integer constant
                    if (ty->isFloat()) {
                        // Int stored as float context
                        double d = static_cast<double>(nm->comptime_int_value);
                        uint64_t bits;
                        std::memcpy(&bits, &d, sizeof(d));
                        std::ostringstream oss;
                        oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
                        return oss.str();
                    }
                    return std::to_string(nm->comptime_int_value);
                }
            }

            // Float comptime value
            if (ty && ty->isFloat()) {
                double d = nm->comptime_float_value;
                uint64_t bits;
                std::memcpy(&bits, &d, sizeof(d));
                std::ostringstream oss;
                oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
                return oss.str();
            }

            // Bool comptime value (for bool-typed expressions)
            if (ty && ty->isBool()) {
                return nm->comptime_bool_value ? "true" : "false";
            }

            // String comptime value
            if (!nm->comptime_string_value.empty()) {
                const std::string& s = nm->comptime_string_value;
                auto it = string_constants_.find(s);
                int idx;
                if (it != string_constants_.end()) {
                    idx = it->second;
                } else {
                    idx = string_counter_++;
                    string_constants_[s] = idx;
                }
                std::string reg = nextReg();
                size_t len = s.size() + 1;
                body_ss_ << "  " << reg << " = getelementptr [" << len << " x i8], ptr @.str."
                         << idx << ", i64 0, i64 0 ; comptime string\n";
                return reg;
            }

            // Fallback: if comptime_evaluated is set but we couldn't determine
            // the value type, fall through to normal codegen
        }
    }

    switch (expr->getKind()) {

    // ========================================================================
    // Literals
    // ========================================================================
    case NodeKind::IntLiteral: {
        auto& lit = cast<IntLiteral>(*expr);
        // BUG FIX: If the target type is a float/double, emit as a float constant.
        // LLVM IR requires float-typed constants (e.g. `ret double 2.0`),
        // not bare integers in float contexts.
        TypeId ty = expr->getType();
        if (ty && ty->isFloat()) {
            double d = static_cast<double>(lit.value());
            uint64_t bits;
            std::memcpy(&bits, &d, sizeof(d));
            std::ostringstream oss;
            oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
            return oss.str();
        }
        return std::to_string(lit.value());
    }

    case NodeKind::FloatLiteral: {
        auto& lit = cast<FloatLiteral>(*expr);
        double d = lit.value();
        uint64_t bits;
        std::memcpy(&bits, &d, sizeof(d));
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setfill('0') << std::setw(16) << bits;
        return oss.str();
    }

    case NodeKind::BoolLiteral: {
        return cast<BoolLiteral>(*expr).value() ? "true" : "false";
    }

    case NodeKind::StringLiteral: {
        auto& lit = cast<StringLiteral>(*expr);
        const std::string& s = lit.value();

        auto it = string_constants_.find(s);
        int idx;
        if (it != string_constants_.end()) {
            // String constant deduplication: reuse existing global constant
            idx = it->second;
        } else {
            idx = string_counter_++;
            string_constants_[s] = idx;
        }
        std::string reg = nextReg();
        size_t len = s.size() + 1;
        body_ss_ << "  " << reg << " = getelementptr [" << len << " x i8], ptr @.str."
                 << idx << ", i64 0, i64 0\n";
        return reg;
    }

    // ========================================================================
    // Identifier
    // ========================================================================
    case NodeKind::IdentExpr: {
        auto& id = cast<IdentExpr>(*expr);
        SSAVarInfo* info = lookupVar(id.name());
        if (info) {
            if (info->needs_alloca) {
                // Alloca-backed variable: return pointer for aggregates,
                // load scalar value for non-aggregates
                TypeId ty = expr->getType();
                if (isAggregateType(ty)) return info->alloca_name;
                std::string ll = llvmType(ty);
                std::string reg = nextReg();
                body_ss_ << "  " << reg << " = load " << ll << ", ptr " << info->alloca_name << "\n";
                return reg;
            } else {
                // SSA-tracked variable: return the current SSA value directly
                return info->current_value;
            }
        }
        // Global / function name
        return "@" + sanitizeName(id.name());
    }

    // ========================================================================
    // Binary expression
    // ========================================================================
    case NodeKind::BinaryExpr: {
        auto& bin = cast<BinaryExpr>(*expr);
        BinaryOp op = bin.op();

        // ---- Assignment operators ----
        if (op == BinaryOp::Assign) {
            TypeId target_type = bin.left()->getType();
            std::string val = emitExpr(bin.right());
            TypeId val_type = bin.right()->getType();

            // BUG FIX: Convert the value to the target type if they differ.
            // This handles cases like `counter: i32 = sum - 5` where the
            // expression type (i64) differs from the variable type (i32).
            if (val_type && target_type &&
                !isAggregateType(target_type) &&
                llvmType(val_type) != llvmType(target_type)) {
                val = emitTypeCast(val, val_type, target_type);
            }

            // Check if the target is a simple scalar variable (SSA-eligible)
            if (auto* ident = dyn_cast<IdentExpr>(bin.left())) {
                SSAVarInfo* info = lookupVar(ident->name());
                if (info && !info->needs_alloca) {
                    // SSA assignment: just update the tracked value
                    updateVarValue(ident->name(), val);
                    return val;
                }
            }

            // Fallback: alloca-based store via emitLValue
            std::string target_ptr = emitLValue(bin.left());
            if (isAggregateType(target_type)) {
                body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << target_ptr
                         << ", ptr " << val << ", i64 " << typeSizeBytes(target_type)
                         << ", i1 false)\n";
                needed_runtime_.insert("memcpy");
            } else {
                body_ss_ << "  store " << llvmType(target_type) << " " << val
                         << ", ptr " << target_ptr << "\n";
            }
            return val;
        }

        // Compound assignment: load, compute, store
        if (op >= BinaryOp::AddAssign && op <= BinaryOp::ShrAssign) {
            // For SSA variables, we can compute the new value directly
            // without going through emitLValue/emitExpr for the load.
            TypeId target_type = bin.left()->getType();
            std::string ll = llvmType(target_type);

            // Get the current value of the LHS
            std::string lhs;
            if (auto* ident = dyn_cast<IdentExpr>(bin.left())) {
                SSAVarInfo* info = lookupVar(ident->name());
                if (info && !info->needs_alloca) {
                    // SSA variable: use current value directly
                    lhs = info->current_value;
                } else {
                    // Alloca variable: load from alloca
                    lhs = emitExpr(bin.left());
                }
            } else {
                lhs = emitExpr(bin.left());
            }
            std::string rhs = emitExpr(bin.right());

            // Map compound op to its base op
            BinaryOp base_op;
            switch (op) {
                case BinaryOp::AddAssign: base_op = BinaryOp::Add; break;
                case BinaryOp::SubAssign: base_op = BinaryOp::Sub; break;
                case BinaryOp::MulAssign: base_op = BinaryOp::Mul; break;
                case BinaryOp::DivAssign: base_op = BinaryOp::Div; break;
                case BinaryOp::ModAssign: base_op = BinaryOp::Mod; break;
                case BinaryOp::AndAssign: base_op = BinaryOp::BitAnd; break;
                case BinaryOp::OrAssign:  base_op = BinaryOp::BitOr;  break;
                case BinaryOp::XorAssign: base_op = BinaryOp::BitXor; break;
                case BinaryOp::ShlAssign: base_op = BinaryOp::Shl;   break;
                case BinaryOp::ShrAssign: base_op = BinaryOp::Shr;   break;
                default: base_op = BinaryOp::Add; break;
            }

            // Compute the new value — use target_type as the operand type,
            // converting operands as needed to avoid type mismatches.
            TypeId rhs_type = bin.right()->getType();
            if (rhs_type && target_type && llvmType(rhs_type) != ll) {
                rhs = emitTypeCast(rhs, rhs_type, target_type);
            }
            TypeId lhs_type = bin.left()->getType();
            if (lhs_type && target_type && llvmType(lhs_type) != ll) {
                lhs = emitTypeCast(lhs, lhs_type, target_type);
            }

            std::string new_val;
            {
                std::string reg = nextReg();
                emitBinaryOp(reg, ll, lhs, base_op, rhs, target_type);
                new_val = reg;
            }

            // Store the result
            if (auto* ident = dyn_cast<IdentExpr>(bin.left())) {
                SSAVarInfo* info = lookupVar(ident->name());
                if (info && !info->needs_alloca) {
                    // SSA variable: just update the tracked value
                    updateVarValue(ident->name(), new_val);
                    return new_val;
                }
            }
            // Fallback: store via emitLValue
            std::string target_ptr = emitLValue(bin.left());
            body_ss_ << "  store " << ll << " " << new_val << ", ptr " << target_ptr << "\n";
            return new_val;
        }

        // ---- Regular binary operators ----
        std::string lhs = emitExpr(bin.left());
        std::string rhs = emitExpr(bin.right());
        // BUG FIX: Use the expression's type (common type) rather than just the
        // left operand's type. When operands have different types (e.g. i32 + i64),
        // the semantic analyzer promotes both to the common type (i64). We need
        // to emit conversions so both operands match the common type.
        TypeId result_type = expr->getType();
        TypeId lhs_type = bin.left()->getType();
        TypeId rhs_type = bin.right()->getType();

        // For comparison operators, the result type is i1 (bool), so we use
        // the common operand type instead. For arithmetic, the result type IS
        // the operand type.
        TypeId operand_type;
        bool is_cmp = (op >= BinaryOp::Eq && op <= BinaryOp::Ge);

        // ---- String slice comparison optimization ----
        // When comparing two string slices ([]u8) with == or !=, use the
        // optimized tether_str_eq runtime function which checks length first,
        // then pointer equality (for interned strings), then falls back to
        // memcmp. This makes interned string comparison O(1).
        if (is_cmp && (op == BinaryOp::Eq || op == BinaryOp::Ne)) {
            auto isStringSlice = [](TypeId t) -> bool {
                if (!t || !isa<SliceType>(t)) return false;
                TypeId elem = cast<SliceType>(t).element();
                if (elem.isNull() || !isa<PrimitiveType>(elem)) return false;
                return cast<PrimitiveType>(elem).primitiveKind() == PrimitiveKind::U8;
            };

            bool lhs_is_string_slice = isStringSlice(lhs_type);
            bool rhs_is_string_slice = isStringSlice(rhs_type);

            if (lhs_is_string_slice && rhs_is_string_slice) {
                // Emit call to tether_str_eq(ptr, len, ptr, len) -> i1
                // Both operands are slice values: { ptr, i64 }
                // We need to extract the ptr and len from each slice.
                std::string lhs_alloca = emitLValue(bin.left());
                std::string rhs_alloca = emitLValue(bin.right());

                // Extract lhs.ptr
                std::string lhs_ptr_ptr = nextReg();
                body_ss_ << "  " << lhs_ptr_ptr << " = getelementptr { ptr, i64 }, ptr "
                         << lhs_alloca << ", i32 0, i32 0\n";
                std::string lhs_ptr = nextReg();
                body_ss_ << "  " << lhs_ptr << " = load ptr, ptr " << lhs_ptr_ptr << "\n";

                // Extract lhs.len
                std::string lhs_len_ptr = nextReg();
                body_ss_ << "  " << lhs_len_ptr << " = getelementptr { ptr, i64 }, ptr "
                         << lhs_alloca << ", i32 0, i32 1\n";
                std::string lhs_len = nextReg();
                body_ss_ << "  " << lhs_len << " = load i64, ptr " << lhs_len_ptr << "\n";

                // Extract rhs.ptr
                std::string rhs_ptr_ptr = nextReg();
                body_ss_ << "  " << rhs_ptr_ptr << " = getelementptr { ptr, i64 }, ptr "
                         << rhs_alloca << ", i32 0, i32 0\n";
                std::string rhs_ptr = nextReg();
                body_ss_ << "  " << rhs_ptr << " = load ptr, ptr " << rhs_ptr_ptr << "\n";

                // Extract rhs.len
                std::string rhs_len_ptr = nextReg();
                body_ss_ << "  " << rhs_len_ptr << " = getelementptr { ptr, i64 }, ptr "
                         << rhs_alloca << ", i32 0, i32 1\n";
                std::string rhs_len = nextReg();
                body_ss_ << "  " << rhs_len << " = load i64, ptr " << rhs_len_ptr << "\n";

                // Call tether_str_eq
                needed_runtime_.insert("tether_str_eq");
                std::string eq_result = nextReg();
                body_ss_ << "  " << eq_result << " = call i1 @tether_str_eq(ptr "
                         << lhs_ptr << ", i64 " << lhs_len << ", ptr "
                         << rhs_ptr << ", i64 " << rhs_len << ")\n";

                // For !=, invert the result
                if (op == BinaryOp::Ne) {
                    std::string ne_result = nextReg();
                    body_ss_ << "  " << ne_result << " = xor i1 " << eq_result << ", true\n";
                    return ne_result;
                }
                return eq_result;
            }
        }

        if (is_cmp) {
            // For comparisons, determine the operand type from the common type
            // of the two operands. If they differ, use the larger type
            // (which is the common type from the semantic analyzer).
            if (lhs_type && rhs_type && lhs_type != rhs_type) {
                if (lhs_type->bitWidth() >= rhs_type->bitWidth()) {
                    operand_type = lhs_type;
                } else {
                    operand_type = rhs_type;
                }
            } else {
                operand_type = lhs_type;
            }
        } else {
            operand_type = result_type;
        }

        std::string ll = llvmType(operand_type);

        // Convert lhs to operand_type if needed
        if (lhs_type && operand_type && llvmType(lhs_type) != ll) {
            lhs = emitTypeCast(lhs, lhs_type, operand_type);
        }

        // Convert rhs to operand_type if needed
        if (rhs_type && operand_type && llvmType(rhs_type) != ll) {
            rhs = emitTypeCast(rhs, rhs_type, operand_type);
        }

        std::string reg = nextReg();
        emitBinaryOp(reg, ll, lhs, op, rhs, operand_type);
        return reg;
    }

    // ========================================================================
    // Unary expression
    // ========================================================================
    case NodeKind::UnaryExpr: {
        auto& un = cast<UnaryExpr>(*expr);
        TypeId ty = expr->getType();
        std::string ll = llvmType(ty);

        switch (un.op()) {
            case UnaryOp::Neg: {
                std::string operand = emitExpr(un.operand());
                std::string reg = nextReg();
                if (ty && ty->isFloat()) {
                    body_ss_ << "  " << reg << " = fneg " << ll << " " << operand << "\n";
                } else {
                    body_ss_ << "  " << reg << " = sub " << ll << " 0, " << operand << "\n";
                }
                return reg;
            }
            case UnaryOp::Not: {
                std::string operand = emitExpr(un.operand());
                std::string reg = nextReg();
                body_ss_ << "  " << reg << " = xor i1 " << operand << ", true\n";
                return reg;
            }
            case UnaryOp::BitNot: {
                std::string operand = emitExpr(un.operand());
                std::string reg = nextReg();
                body_ss_ << "  " << reg << " = xor " << ll << " " << operand << ", -1\n";
                return reg;
            }
            case UnaryOp::Deref: {
                std::string operand = emitExpr(un.operand());
                if (isAggregateType(ty)) return operand;
                std::string reg = nextReg();
                body_ss_ << "  " << reg << " = load " << ll << ", ptr " << operand << "\n";
                return reg;
            }
            case UnaryOp::Addr: {
                return emitLValue(un.operand());
            }
        }
        return "0";
    }

    // ========================================================================
    // Call expression
    // ========================================================================
    case NodeKind::CallExpr: {
        auto& call = cast<CallExpr>(*expr);
        Expr* callee_expr = call.callee();

        // Check for smart-pointer built-in methods
        if (auto* mem = dyn_cast<MemberExpr>(callee_expr)) {
            TypeId obj_type = mem->object()->getType();
            if (obj_type && isa<SmartPointerType>(obj_type)) {
                auto& sp = cast<SmartPointerType>(obj_type);
                const std::string& method = mem->field();
                std::string obj_val = emitExpr(mem->object());

                if (method == "new") {
                    // Box::new(val), Rc::new(val), Arc::new(val)
                    if (call.argCount() >= 1) {
                        switch (sp.smartPointerKind()) {
                            case SmartPointerKind::Box:
                                return emitBoxNew(call.args()[0].get(), sp.pointee());
                            case SmartPointerKind::Rc:
                                return emitRcNew(call.args()[0].get(), sp.pointee());
                            case SmartPointerKind::Arc:
                                return emitArcNew(call.args()[0].get(), sp.pointee());
                        }
                    }
                } else if (method == "drop") {
                    switch (sp.smartPointerKind()) {
                        case SmartPointerKind::Box: emitBoxDrop(obj_val); break;
                        case SmartPointerKind::Rc:  emitRcDrop(obj_val, sp.pointee()); break;
                        case SmartPointerKind::Arc:  emitArcDrop(obj_val, sp.pointee()); break;
                    }
                    return "0";
                } else if (method == "clone") {
                    switch (sp.smartPointerKind()) {
                        case SmartPointerKind::Rc:  return emitRcClone(obj_val, sp.pointee());
                        case SmartPointerKind::Arc: return emitArcClone(obj_val, sp.pointee());
                        default: break;
                    }
                }
            }
        }

        // Regular call
        std::string callee;
        if (auto* ident = dyn_cast<IdentExpr>(callee_expr)) {
            // --- Built-in intrinsics ---
            // black_box(val) — optimization barrier for benchmarking
            if (ident->name() == "black_box" || ident->name() == "@black_box") {
                if (call.argCount() >= 1) {
                    std::string val = emitExpr(call.args()[0].get());
                    TypeId arg_type = call.args()[0]->getType();
                    std::string ll = llvmType(arg_type);

                    if (ll == "double" || ll == "float") {
                        needed_runtime_.insert("tether_black_box_f64");
                        body_ss_ << "  call void @tether_black_box_f64(" << ll << " " << val << ")\n";
                    } else if (ll == "ptr") {
                        needed_runtime_.insert("tether_black_box_ptr");
                        body_ss_ << "  call void @tether_black_box_ptr(" << ll << " " << val << ")\n";
                    } else {
                        // Default: use i64 version, cast if needed
                        needed_runtime_.insert("tether_black_box_i64");
                        if (ll != "i64") {
                            std::string widened = nextReg();
                            body_ss_ << "  " << widened << " = zext " << ll << " " << val << " to i64\n";
                            val = widened;
                        }
                        body_ss_ << "  call void @tether_black_box_i64(i64 " << val << ")\n";
                    }
                    return val;  // Return the original value
                }
                return "0";
            }

            // volatile_read(ptr) — read a value from memory with volatile semantics
            if (ident->name() == "volatile_read" || ident->name() == "@volatile_read") {
                if (call.argCount() >= 1) {
                    std::string ptr_val = emitExpr(call.args()[0].get());
                    // Determine the type being read
                    TypeId read_type = expr->getType();
                    std::string ll = "i64"; // default
                    if (read_type) {
                        ll = llvmType(read_type);
                    }
                    std::string result = nextReg();
                    body_ss_ << "  " << result << " = load volatile " << ll << ", ptr " << ptr_val << "\n";
                    return result;
                }
                return "0";
            }

            // volatile_write(ptr, value) — write a value to memory with volatile semantics
            if (ident->name() == "volatile_write" || ident->name() == "@volatile_write") {
                if (call.argCount() >= 2) {
                    std::string ptr_val = emitExpr(call.args()[0].get());
                    std::string val = emitExpr(call.args()[1].get());
                    // Determine the type being written
                    TypeId write_type = call.args()[1]->getType();
                    std::string ll = "i64";
                    if (write_type) {
                        ll = llvmType(write_type);
                    }
                    body_ss_ << "  store volatile " << ll << " " << val << ", ptr " << ptr_val << "\n";
                    return "";
                }
                return "";
            }

            // ===== SIMD Vector Intrinsics =====
            // These emit LLVM vector operations directly — NOT function calls.

            // simd_load(ptr) — load a SIMD vector from aligned memory
            if (ident->name() == "simd_load" || ident->name() == "@simd_load") {
                if (call.argCount() >= 1) {
                    std::string ptr_val = emitExpr(call.args()[0].get());
                    TypeId result_type = expr->getType();
                    std::string ll = "i64";
                    if (!result_type.isNull()) ll = llvmType(result_type);
                    // Determine alignment from the vector type
                    std::string align_str = "";
                    if (!result_type.isNull() && isa<SimdVectorType>(result_type)) {
                        uint64_t align = typeAlignmentBytes(result_type);
                        if (align >= 16) align_str = ", align " + std::to_string(align);
                    }
                    std::string result = nextReg();
                    body_ss_ << "  " << result << " = load " << ll << ", ptr " << ptr_val << align_str << "\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "undef";
            }

            // simd_store(ptr, vec) — store a SIMD vector to aligned memory
            if (ident->name() == "simd_store" || ident->name() == "@simd_store") {
                if (call.argCount() >= 2) {
                    std::string ptr_val = emitExpr(call.args()[0].get());
                    std::string vec_val = emitExpr(call.args()[1].get());
                    TypeId vec_type = call.args()[1]->getType();
                    std::string ll = "i64";
                    if (!vec_type.isNull()) ll = llvmType(vec_type);
                    std::string align_str = "";
                    if (!vec_type.isNull() && isa<SimdVectorType>(vec_type)) {
                        uint64_t align = typeAlignmentBytes(vec_type);
                        if (align >= 16) align_str = ", align " + std::to_string(align);
                    }
                    body_ss_ << "  store " << ll << " " << vec_val << ", ptr " << ptr_val << align_str << "\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return "";
                }
                return "";
            }

            // simd_shuffle(vec_a, vec_b, mask) — shuffle vector elements
            if (ident->name() == "simd_shuffle" || ident->name() == "@simd_shuffle") {
                if (call.argCount() >= 3) {
                    std::string vec_a = emitExpr(call.args()[0].get());
                    std::string vec_b = emitExpr(call.args()[1].get());
                    // The mask is a vector of i32 constants
                    std::string mask_val = emitExpr(call.args()[2].get());
                    TypeId result_type = expr->getType();
                    std::string ll_a = "i64";
                    std::string ll_result = "i64";
                    if (!result_type.isNull()) {
                        ll_result = llvmType(result_type);
                    }
                    if (!call.args()[0]->getType().isNull()) {
                        ll_a = llvmType(call.args()[0]->getType());
                    }
                    std::string result = nextReg();
                    body_ss_ << "  " << result << " = shufflevector " << ll_a << " " << vec_a
                             << ", " << ll_a << " " << vec_b
                             << ", " << mask_val << "\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "undef";
            }

            // simd_reduce_add(vec) — horizontal sum reduction
            if (ident->name() == "simd_reduce_add" || ident->name() == "@simd_reduce_add") {
                if (call.argCount() >= 1) {
                    std::string vec_val = emitExpr(call.args()[0].get());
                    TypeId vec_type = call.args()[0]->getType();
                    std::string ll_vec = "<4 x float>";
                    std::string ll_elem = "float";
                    if (!vec_type.isNull() && isa<SimdVectorType>(vec_type)) {
                        auto& sv = cast<SimdVectorType>(vec_type);
                        ll_vec = llvmType(vec_type);
                        ll_elem = llvmType(sv.elementType());
                    }
                    std::string result = nextReg();
                    // LLVM vector reduce: @llvm.vector.reduce.fadd.v4f32(float start, <4 x float> vec)
                    // We use ordered reduction (acc = ((a[0] + a[1]) + a[2]) + a[3])
                    std::string zero = (ll_elem == "float" || ll_elem == "double") ? "0.0" : "0";
                    std::string reduce_op = (ll_elem == "float" || ll_elem == "double") ? "fadd" : "add";
                    body_ss_ << "  " << result << " = call " << ll_elem
                             << " @llvm.vector.reduce." << reduce_op << "." << ll_vec
                             << "(" << ll_elem << " " << zero << ", " << ll_vec << " " << vec_val << ")\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "0";
            }

            // simd_reduce_mul(vec) — horizontal product reduction
            if (ident->name() == "simd_reduce_mul" || ident->name() == "@simd_reduce_mul") {
                if (call.argCount() >= 1) {
                    std::string vec_val = emitExpr(call.args()[0].get());
                    TypeId vec_type = call.args()[0]->getType();
                    std::string ll_vec = "<4 x float>";
                    std::string ll_elem = "float";
                    if (!vec_type.isNull() && isa<SimdVectorType>(vec_type)) {
                        auto& sv = cast<SimdVectorType>(vec_type);
                        ll_vec = llvmType(vec_type);
                        ll_elem = llvmType(sv.elementType());
                    }
                    std::string result = nextReg();
                    std::string one = (ll_elem == "float" || ll_elem == "double") ? "1.0" : "1";
                    std::string reduce_op = (ll_elem == "float" || ll_elem == "double") ? "fmul" : "mul";
                    body_ss_ << "  " << result << " = call " << ll_elem
                             << " @llvm.vector.reduce." << reduce_op << "." << ll_vec
                             << "(" << ll_elem << " " << one << ", " << ll_vec << " " << vec_val << ")\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "1";
            }

            // simd_reduce_min(vec) — horizontal minimum
            if (ident->name() == "simd_reduce_min" || ident->name() == "@simd_reduce_min") {
                if (call.argCount() >= 1) {
                    std::string vec_val = emitExpr(call.args()[0].get());
                    TypeId vec_type = call.args()[0]->getType();
                    std::string ll_vec = "<4 x i32>";
                    std::string ll_elem = "i32";
                    std::string minmax_kind = "smin";
                    if (!vec_type.isNull() && isa<SimdVectorType>(vec_type)) {
                        auto& sv = cast<SimdVectorType>(vec_type);
                        ll_vec = llvmType(vec_type);
                        ll_elem = llvmType(sv.elementType());
                        if (sv.elementType()->isFloat()) minmax_kind = "fmin";
                        else if (sv.elementType()->isUnsigned()) minmax_kind = "umin";
                    }
                    std::string result = nextReg();
                    body_ss_ << "  " << result << " = call " << ll_elem
                             << " @llvm.vector.reduce." << minmax_kind << "." << ll_vec
                             << "(" << ll_vec << " " << vec_val << ")\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "0";
            }

            // simd_reduce_max(vec) — horizontal maximum
            if (ident->name() == "simd_reduce_max" || ident->name() == "@simd_reduce_max") {
                if (call.argCount() >= 1) {
                    std::string vec_val = emitExpr(call.args()[0].get());
                    TypeId vec_type = call.args()[0]->getType();
                    std::string ll_vec = "<4 x i32>";
                    std::string ll_elem = "i32";
                    std::string minmax_kind = "smax";
                    if (!vec_type.isNull() && isa<SimdVectorType>(vec_type)) {
                        auto& sv = cast<SimdVectorType>(vec_type);
                        ll_vec = llvmType(vec_type);
                        ll_elem = llvmType(sv.elementType());
                        if (sv.elementType()->isFloat()) minmax_kind = "fmax";
                        else if (sv.elementType()->isUnsigned()) minmax_kind = "umax";
                    }
                    std::string result = nextReg();
                    body_ss_ << "  " << result << " = call " << ll_elem
                             << " @llvm.vector.reduce." << minmax_kind << "." << ll_vec
                             << "(" << ll_vec << " " << vec_val << ")\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "0";
            }

            // simd_gather(ptrs) — gather load from multiple addresses
            if (ident->name() == "simd_gather" || ident->name() == "@simd_gather") {
                if (call.argCount() >= 1) {
                    // ptrs is a vector of pointers: <N x ptr>
                    std::string ptrs_val = emitExpr(call.args()[0].get());
                    TypeId result_type = expr->getType();
                    std::string ll_result = "<4 x float>";
                    if (!result_type.isNull()) ll_result = llvmType(result_type);
                    // Determine the pointer vector type from the argument
                    TypeId ptrs_type = call.args()[0]->getType();
                    std::string ll_ptrs = "<4 x ptr>";
                    if (!ptrs_type.isNull()) ll_ptrs = llvmType(ptrs_type);
                    // Determine vector count from result type
                    uint32_t vec_count = 4;
                    if (!result_type.isNull() && isa<SimdVectorType>(result_type)) {
                        vec_count = cast<SimdVectorType>(result_type).count();
                    }
                    std::string result = nextReg();
                    // LLVM masked gather: @llvm.masked.gather.v4f32.v4p0(<4 x ptr> ptrs, i32 align, <4 x i1> mask, <4 x float> passthru)
                    // Use all-ones mask and zeroinitializer passthru
                    std::string mask_str = "<";
                    for (uint32_t i = 0; i < vec_count; ++i) {
                        if (i > 0) mask_str += ", ";
                        mask_str += "i1 true";
                    }
                    mask_str += ">";
                    body_ss_ << "  " << result << " = call " << ll_result
                             << " @llvm.masked.gather." << ll_result << ".v" << vec_count << "p0("
                             << ll_ptrs << " " << ptrs_val << ", i32 16, " << mask_str << ", "
                             << ll_result << " zeroinitializer)\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return result;
                }
                return "undef";
            }

            // simd_scatter(vals, ptrs) — scatter store to multiple addresses
            if (ident->name() == "simd_scatter" || ident->name() == "@simd_scatter") {
                if (call.argCount() >= 2) {
                    std::string vals_val = emitExpr(call.args()[0].get());
                    std::string ptrs_val = emitExpr(call.args()[1].get());
                    TypeId val_type = call.args()[0]->getType();
                    std::string ll_val = "<4 x float>";
                    if (!val_type.isNull()) ll_val = llvmType(val_type);
                    // Determine the pointer vector type from the second argument
                    TypeId ptrs_type = call.args()[1]->getType();
                    std::string ll_ptrs = "<4 x ptr>";
                    if (!ptrs_type.isNull()) ll_ptrs = llvmType(ptrs_type);
                    // Determine vector count from val type
                    uint32_t vec_count = 4;
                    if (!val_type.isNull() && isa<SimdVectorType>(val_type)) {
                        vec_count = cast<SimdVectorType>(val_type).count();
                    }
                    std::string mask_str = "<";
                    for (uint32_t i = 0; i < vec_count; ++i) {
                        if (i > 0) mask_str += ", ";
                        mask_str += "i1 true";
                    }
                    mask_str += ">";
                    // LLVM masked scatter: @llvm.masked.scatter.v4f32.v4p0(<4 x float> vals, <4 x ptr> ptrs, i32 align, <4 x i1> mask)
                    body_ss_ << "  call void @llvm.masked.scatter." << ll_val << ".v" << vec_count << "p0("
                             << ll_val << " " << vals_val << ", " << ll_ptrs << " " << ptrs_val
                             << ", i32 16, " << mask_str << ")\n";
                    needed_runtime_.insert("llvm_vector_reduce");
                    return "";
                }
                return "";
            }

            // If this function has been monomorphized, use the mangled name
            // instead of the original name. The MonomorphizationPass records
            // concrete instantiations; the IRGenerator needs access to the
            // pass results to resolve the mangled name. For now, the pass
            // infrastructure is in place and the full integration (plumbing
            // the pass results through the pipeline to the IRGenerator) can
            // be done in a follow-up.
            // TODO: Check if monomorphization pass has a concrete instance
            //       for this call. If so, use the mangled name instead:
            //         callee = "@" + sanitizeName(monomorphized_name);
            callee = "@" + sanitizeName(ident->name());
        } else {
            callee = emitExpr(callee_expr);
        }

        // --- Pre-LLVM optimization: inline arena allocator lowering ---
        // If this call has an AllocatorInlined annotation, emit the
        // inline bump allocation code instead of a regular function call.
        {
            std::string inlined = emitInlineAllocatorIfAnnotated(&call, expr->getType());
            if (!inlined.empty()) return inlined;
        }

        // --- Pre-LLVM optimization: opaque barrier fence ---
        // If this call has an opaque barrier in the metadata map,
        // emit a fence before and after the call to prevent reordering.
        bool call_has_opaque_barrier = false;
        if (meta_map_) {
            auto* nm = meta_map_->get(&call);
            call_has_opaque_barrier = nm && nm->opaque_barrier;
        }

        // Emit arguments
        std::vector<std::string> arg_vals;
        std::vector<std::string> arg_types;
        for (const auto& arg : call.args()) {
            std::string val = emitExpr(arg.get());
            TypeId at = arg->getType();
            std::string ll = llvmType(at);
            if (isAggregateType(at)) {
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load " << ll << ", ptr " << val << "\n";
                arg_vals.push_back(loaded);
                arg_types.push_back(ll);
            } else {
                arg_vals.push_back(val);
                arg_types.push_back(ll);
            }
        }

        // Determine return type
        TypeId ret_type = expr->getType();
        bool callee_can_error = ret_type && isa<ErrorType>(ret_type);
        TypeId actual_ret = ret_type;
        std::string caller_err_slot_alloc;
        if (callee_can_error) {
            actual_ret = cast<ErrorType>(ret_type).successType();
            // Zig-style: allocate an i32 err_slot on the caller's stack
            caller_err_slot_alloc = makeAllocaName("__err_slot");
            alloca_ss_ << "  " << caller_err_slot_alloc << " = alloca i32\n";
            body_ss_ << "  store i32 0, ptr " << caller_err_slot_alloc << "\n";
        }
        std::string ret_ll = llvmType(actual_ret);

        std::string result;

        // --- Pre-LLVM optimization: opaque barrier fence (before call) ---
        if (call_has_opaque_barrier) {
            body_ss_ << "  fence acquire ; [opaque barrier] before FFI call\n";
        }

        // --- @tailcall directive: emit musttail for self-recursive calls ---
        // When the current function has @tailcall and this call is a
        // self-recursive call (calling the current function), emit
        // `musttail call` instead of `call`. This guarantees TCO at the
        // LLVM level — if LLVM can't satisfy it, it's a compile error.
        bool is_self_recursive_tailcall = false;
        if (current_fn_name_ == std::string(dyn_cast<IdentExpr>(callee_expr) ?
                     dyn_cast<IdentExpr>(callee_expr)->name() : "")) {
            // Check if the current function has @tailcall directive
            // We need to check the current FnDecl — we do this by checking
            // if the current function name matches and the directive is set
            // (we set a flag at emitFnDecl entry based on the FnDecl)
            is_self_recursive_tailcall = current_fn_has_tailcall_;
        }

        std::string call_keyword;
        if (is_self_recursive_tailcall) {
            call_keyword = "musttail call";
        } else if (is_musttail_call_) {
            // Tail call from ReturnStmt: the return value is directly this
            // call expression, so we can use musttail to guarantee TCO.
            call_keyword = "musttail call";
            is_musttail_call_ = false;  // consume the flag
        } else {
            call_keyword = "call";
        }

        // Emit the call — for void returns, don't assign to a register
        if (ret_ll == "void") {
            body_ss_ << "  " << call_keyword << " void " << callee << "(";
        } else {
            result = nextReg();
            body_ss_ << "  " << result << " = " << call_keyword << " " << ret_ll << " " << callee << "(";
        }
        for (size_t i = 0; i < arg_vals.size(); ++i) {
            if (i > 0) body_ss_ << ", ";
            body_ss_ << arg_types[i] << " " << arg_vals[i];
        }
        // Zig-style: pass err_slot as last argument for error-returning calls
        if (callee_can_error) {
            if (!arg_vals.empty()) body_ss_ << ", ";
            body_ss_ << "ptr " << caller_err_slot_alloc;
        }
        body_ss_ << ")\n";

        // --- Pre-LLVM optimization: opaque barrier fence (after call) ---
        if (call_has_opaque_barrier) {
            body_ss_ << "  fence release ; [opaque barrier] after FFI call\n";
        }

        // Error return: store success value in alloca and track err_slot
        // (The try-expression will check err_slot and load the success value)
        if (callee_can_error) {
            caller_err_slot_ = caller_err_slot_alloc;
            auto& err = cast<ErrorType>(ret_type);
            std::string vt = llvmType(err.successType());
            if (vt == "void") {
                return "";  // void result — err_slot is tracked in caller_err_slot_
            } else if (isAggregateType(err.successType())) {
                // Aggregate: store result in alloca, return pointer
                std::string ea = makeAllocaName("__err_ret");
                alloca_ss_ << "  " << ea << " = alloca " << vt << "\n";
                body_ss_ << "  store " << vt << " " << result << ", ptr " << ea << "\n";
                return ea;
            } else {
                // Scalar: store result in alloca, return pointer
                // (try-expression will load from this alloca)
                std::string ea = makeAllocaName("__err_ret");
                alloca_ss_ << "  " << ea << " = alloca " << vt << "\n";
                body_ss_ << "  store " << vt << " " << result << ", ptr " << ea << "\n";
                return ea;
            }
        }

        // Aggregate return: store in alloca
        if (isAggregateType(actual_ret)) {
            std::string ea = makeAllocaName("__agg_ret");
            alloca_ss_ << "  " << ea << " = alloca " << ret_ll << "\n";
            body_ss_ << "  store " << ret_ll << " " << result << ", ptr " << ea << "\n";
            return ea;
        }

        return result;
    }

    // ========================================================================
    // Member expression
    // ========================================================================
    case NodeKind::MemberExpr: {
        auto& mem = cast<MemberExpr>(*expr);
        TypeId obj_type = mem.object()->getType();

        // --- Pre-LLVM optimization: SoA-transformed access ---
        // If this member expression has a SoATransformed annotation,
        // emit the SoA field array access instead of the normal struct access.
        {
            std::string soa_result = emitSoAAccessIfAnnotated(expr);
            if (!soa_result.empty()) return soa_result;
        }

        // --- Pre-LLVM optimization: SROA field access ---
        // If the object of this MemberExpr is an SROA-decomposed variable
        // (e.g., p.x where p is SROA'd), look up the field's SSA value
        // directly instead of GEP+load.
        if (mem.object()->getKind() == NodeKind::IdentExpr) {
            auto& ident = cast<IdentExpr>(*mem.object());
            if (isSROAVariable(ident.name())) {
                std::string field_ssa = sroaFieldName(ident.name(), mem.field());
                SSAVarInfo* info = lookupVar(field_ssa);
                if (info) {
                    if (info->needs_alloca) {
                        // Aggregate field — return the alloca pointer
                        if (isAggregateType(info->tether_type)) return info->alloca_name;
                        std::string ll = info->llvm_type;
                        std::string reg = nextReg();
                        body_ss_ << "  " << reg << " = load " << ll << ", ptr "
                                 << info->alloca_name << "\n";
                        return reg;
                    } else {
                        // Scalar field — return the SSA value directly
                        return info->current_value;
                    }
                }
            }
        }

        // Smart pointer field access
        if (obj_type && isa<SmartPointerType>(obj_type)) {
            auto& sp = cast<SmartPointerType>(obj_type);
            if (sp.smartPointerKind() == SmartPointerKind::Box) {
                // Box<T>: deref the pointer and access the field on T
                std::string ptr_val = emitExpr(mem.object());
                TypeId pointee = sp.pointee();
                if (isa<StructType>(pointee)) {
                    auto& st = cast<StructType>(pointee);
                    int idx = st.fieldIndex(mem.field());
                    if (idx < 0) return "0";
                    std::string ll = llvmType(pointee);
                    std::string gep = nextReg();
                    body_ss_ << "  " << gep << " = getelementptr " << ll
                             << ", ptr " << ptr_val << ", i32 0, i32 " << idx << "\n";
                    TypeId ft = st.fields()[idx].type;
                    if (isAggregateType(ft)) return gep;
                    std::string loaded = nextReg();
                    std::string tbaa = emitTBAAMetadataForField(st.name(), mem.field());
                    body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep << tbaa << "\n";
                    return loaded;
                }
            }
            // Rc/Arc: struct { ptr, i64 }
            // Field "data" → ptr, field "refcount" → i64
            if (mem.field() == "data" || mem.field() == "ptr") {
                std::string obj_ptr = emitLValue(mem.object());
                std::string ll = llvmType(obj_type);
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 0\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load ptr, ptr " << gep << "\n";
                return loaded;
            }
            if (mem.field() == "refcount" || mem.field() == "count") {
                std::string obj_ptr = emitLValue(mem.object());
                std::string ll = llvmType(obj_type);
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 1\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load i64, ptr " << gep << "\n";
                return loaded;
            }
            return "0";
        }

        // Direct struct member
        if (obj_type && isa<StructType>(obj_type)) {
            auto& st = cast<StructType>(obj_type);
            int idx = st.fieldIndex(mem.field());
            if (idx < 0) return "0";
            std::string obj_ptr = emitLValue(mem.object());
            std::string ll = llvmType(obj_type);
            std::string gep = nextReg();
            body_ss_ << "  " << gep << " = getelementptr " << ll
                     << ", ptr " << obj_ptr << ", i32 0, i32 " << idx << "\n";
            TypeId ft = st.fields()[idx].type;
            if (isAggregateType(ft)) return gep;
            std::string loaded = nextReg();
            std::string tbaa = emitTBAAMetadataForField(st.name(), mem.field());
            body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep << tbaa << "\n";
            return loaded;
        }

        // Pointer / reference member (auto-deref)
        if (obj_type && (isa<PointerType>(obj_type) || isa<ReferenceType>(obj_type) ||
                        isa<MutReferenceType>(obj_type))) {
            TypeId deref_type;
            if (isa<PointerType>(obj_type)) deref_type = cast<PointerType>(obj_type).pointee();
            else if (isa<ReferenceType>(obj_type)) deref_type = cast<ReferenceType>(obj_type).referent();
            else deref_type = cast<MutReferenceType>(obj_type).referent();

            if (deref_type && isa<StructType>(deref_type)) {
                auto& st = cast<StructType>(deref_type);
                int idx = st.fieldIndex(mem.field());
                if (idx < 0) return "0";
                std::string ptr_val = emitExpr(mem.object());
                std::string ll = llvmType(deref_type);
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << ptr_val << ", i32 0, i32 " << idx << "\n";
                TypeId ft = st.fields()[idx].type;
                if (isAggregateType(ft)) return gep;
                std::string loaded = nextReg();
                std::string tbaa = emitTBAAMetadataForField(st.name(), mem.field());

                // Add !invariant.load for immutable reference access
                std::string inv_meta;
                if (isa<ReferenceType>(obj_type)) {
                    auto it = metadata_map_.find("__invariant_load");
                    if (it != metadata_map_.end()) {
                        inv_meta = ", !invariant.load !" + std::to_string(it->second);
                    }
                }

                body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep
                         << tbaa << inv_meta << "\n";
                return loaded;
            }

            // Enum variant access through pointer
            if (deref_type && isa<EnumType>(deref_type)) {
                auto& en = cast<EnumType>(deref_type);
                int idx = en.variantIndex(mem.field());
                if (idx >= 0) return std::to_string(idx);
            }
        }

        // Enum variant access (direct)
        if (obj_type && isa<EnumType>(obj_type)) {
            auto& en = cast<EnumType>(obj_type);
            int idx = en.variantIndex(mem.field());
            if (idx >= 0) return std::to_string(idx);
        }

        // Slice fields
        if (obj_type && isa<SliceType>(obj_type)) {
            std::string obj_ptr = emitLValue(mem.object());
            std::string ll = llvmType(obj_type);
            if (mem.field() == "ptr" || mem.field() == "data") {
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 0\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load ptr, ptr " << gep << "\n";
                return loaded;
            }
            if (mem.field() == "len" || mem.field() == "length") {
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 1\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load i64, ptr " << gep << "\n";
                return loaded;
            }
        }

        return "0";
    }

    // ========================================================================
    // Index expression
    // ========================================================================
    case NodeKind::IndexExpr: {
        auto& idx = cast<IndexExpr>(*expr);
        std::string index = emitExpr(idx.index());
        TypeId obj_type = idx.object()->getType();
        TypeId elem_type = expr->getType();

        // Slice indexing
        if (obj_type && isa<SliceType>(obj_type)) {
            std::string obj_ptr = emitLValue(idx.object());
            std::string ll = llvmType(obj_type);
            std::string dp = nextReg();
            body_ss_ << "  " << dp << " = getelementptr " << ll
                     << ", ptr " << obj_ptr << ", i32 0, i32 0\n";
            std::string data = nextReg();
            body_ss_ << "  " << data << " = load ptr, ptr " << dp << "\n";
            std::string ll_e = llvmType(elem_type);
            std::string ep = nextReg();
            body_ss_ << "  " << ep << " = getelementptr " << ll_e
                     << ", ptr " << data << ", i64 " << index << "\n";
            if (isAggregateType(elem_type)) return ep;
            std::string loaded = nextReg();
            body_ss_ << "  " << loaded << " = load " << ll_e << ", ptr " << ep << "\n";
            return loaded;
        }

        // Pointer indexing
        if (obj_type && isa<PointerType>(obj_type)) {
            auto& pt = cast<PointerType>(obj_type);
            std::string ptr_val = emitExpr(idx.object());
            std::string ll_e = llvmType(pt.pointee());
            std::string ep = nextReg();
            body_ss_ << "  " << ep << " = getelementptr " << ll_e
                     << ", ptr " << ptr_val << ", i64 " << index << "\n";
            if (isAggregateType(pt.pointee())) return ep;
            std::string loaded = nextReg();
            body_ss_ << "  " << loaded << " = load " << ll_e << ", ptr " << ep << "\n";
            return loaded;
        }

        // Array indexing (contiguous)
        {
            std::string obj_ptr = emitLValue(idx.object());
            std::string ll_e = llvmType(elem_type);
            std::string ep = nextReg();
            body_ss_ << "  " << ep << " = getelementptr " << ll_e
                     << ", ptr " << obj_ptr << ", i64 " << index << "\n";
            if (isAggregateType(elem_type)) return ep;
            std::string loaded = nextReg();
            body_ss_ << "  " << loaded << " = load " << ll_e << ", ptr " << ep << "\n";
            return loaded;
        }
    }

    // ========================================================================
    // Deref expression
    // ========================================================================
    case NodeKind::DerefExpr: {
        auto& deref = cast<DerefExpr>(*expr);
        std::string ptr_val = emitExpr(deref.operand());
        TypeId ty = expr->getType();
        if (isAggregateType(ty)) return ptr_val;
        std::string ll = llvmType(ty);
        std::string reg = nextReg();

        // Collect metadata annotations for this load
        std::string load_meta;

        // FIX 0.7: Add !invariant.load metadata when dereferencing an
        // immutable reference (&T / ReferenceType). This tells LLVM the
        // loaded value will not change, enabling more aggressive optimization.
        TypeId operand_type = deref.operand()->getType();
        if (operand_type && isa<ReferenceType>(operand_type)) {
            // Allocate an !invariant.load metadata node if not yet created
            auto it = metadata_map_.find("__invariant_load");
            if (it == metadata_map_.end()) {
                int inv_id = metadata_counter_++;
                metadata_map_["__invariant_load"] = inv_id;
                // The metadata node will be emitted by emitMetadata()
                // We need to add it to metadata_entries_ as well
                metadata_entries_.push_back({inv_id, "!{}"});
            }
            load_meta += ", !invariant.load !" + std::to_string(metadata_map_["__invariant_load"]);
        }

        // FIX 0.8: Add !nonnull metadata on smart pointer dereferences.
        // After Box.new(), Rc.new(), Arc.new(), the pointer is guaranteed
        // non-null. This helps LLVM eliminate null checks.
        if (operand_type && isa<SmartPointerType>(operand_type)) {
            auto it = metadata_map_.find("__nonnull");
            if (it == metadata_map_.end()) {
                int nn_id = metadata_counter_++;
                metadata_map_["__nonnull"] = nn_id;
                metadata_entries_.push_back({nn_id, "!{}"});
            }
            load_meta += ", !nonnull !" + std::to_string(metadata_map_["__nonnull"]);
        }

        body_ss_ << "  " << reg << " = load " << ll << ", ptr " << ptr_val
                 << load_meta << "\n";
        return reg;
    }

    // ========================================================================
    // Address-of expression
    // ========================================================================
    case NodeKind::AddrOfExpr: {
        auto& addr = cast<AddrOfExpr>(*expr);
        return emitLValue(addr.operand());
    }

    // ========================================================================
    // Cast expression
    // ========================================================================
    case NodeKind::CastExpr: {
        auto& ce = cast<CastExpr>(*expr);
        std::string val = emitExpr(ce.expr());
        TypeId from = ce.expr()->getType();
        TypeId to = ce.targetType();
        std::string from_ll = llvmType(from);
        std::string to_ll = llvmType(to);
        if (from_ll == to_ll) return val;

        std::string reg = nextReg();
        bool from_int  = from && (from->isInteger() || from->isBool());
        bool from_flt  = from && from->isFloat();
        bool from_ptr  = from && from->isPointerLike();
        bool to_int    = to   && (to->isInteger() || to->isBool());
        bool to_flt    = to   && to->isFloat();
        bool to_ptr    = to   && to->isPointerLike();

        if (from_int && to_int) {
            uint64_t fb = from->bitWidth(), tb = to->bitWidth();
            if (tb < fb)      body_ss_ << "  " << reg << " = trunc " << from_ll << " " << val << " to " << to_ll << "\n";
            else if (tb > fb) body_ss_ << "  " << reg << " = " << (from->isSigned() ? "sext" : "zext") << " " << from_ll << " " << val << " to " << to_ll << "\n";
            else return val;
        } else if (from_int && to_flt) {
            body_ss_ << "  " << reg << " = " << (from->isSigned() ? "sitofp" : "uitofp") << " " << from_ll << " " << val << " to " << to_ll << "\n";
        } else if (from_flt && to_int) {
            body_ss_ << "  " << reg << " = " << (to->isSigned() ? "fptosi" : "fptoui") << " " << from_ll << " " << val << " to " << to_ll << "\n";
        } else if (from_flt && to_flt) {
            uint64_t fb = from->bitWidth(), tb = to->bitWidth();
            if (tb < fb)      body_ss_ << "  " << reg << " = fptrunc " << from_ll << " " << val << " to " << to_ll << "\n";
            else if (tb > fb) body_ss_ << "  " << reg << " = fpext " << from_ll << " " << val << " to " << to_ll << "\n";
            else return val;
        } else if (from_int && to_ptr) {
            body_ss_ << "  " << reg << " = inttoptr " << from_ll << " " << val << " to ptr\n";
        } else if (from_ptr && to_int) {
            body_ss_ << "  " << reg << " = ptrtoint ptr " << val << " to " << to_ll << "\n";
        } else if (from_ptr && to_ptr) {
            body_ss_ << "  " << reg << " = bitcast ptr " << val << " to ptr\n";
        } else {
            body_ss_ << "  " << reg << " = bitcast " << from_ll << " " << val << " to " << to_ll << "\n";
        }
        return reg;
    }

    // ========================================================================
    // Struct init expression
    // ========================================================================
    case NodeKind::StructInitExpr: {
        auto& si = cast<StructInitExpr>(*expr);
        TypeId ty = expr->getType();

        // BUG FIX: Guard against null or non-struct types (e.g., PoisonType
        // from error-resilient compilation). Emit a zero-initialized alloca.
        if (!ty || !isa<StructType>(ty)) {
            std::string ll = llvmType(ty);
            std::string aname = makeAllocaName("__sinit_fb");
            alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";
            return aname;
        }

        std::string ll = llvmType(ty);
        std::string aname = makeAllocaName("__sinit");
        alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";

        // Zero-initialize
        needed_runtime_.insert("memset");
        body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << aname
                 << ", i8 0, i64 " << typeSizeBytes(ty) << ", i1 false)\n";

        auto& st = cast<StructType>(ty);
        for (const auto& init : si.inits()) {
            int fi = st.fieldIndex(init.field_name);
            if (fi < 0) continue;
            std::string val = emitExpr(init.value.get());
            TypeId ft = st.fields()[fi].type;
            std::string gep = nextReg();
            body_ss_ << "  " << gep << " = getelementptr " << ll
                     << ", ptr " << aname << ", i32 0, i32 " << fi << "\n";
            if (isAggregateType(ft)) {
                body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << gep
                         << ", ptr " << val << ", i64 " << typeSizeBytes(ft)
                         << ", i1 false)\n";
                needed_runtime_.insert("memcpy");
            } else {
                body_ss_ << "  store " << llvmType(ft) << " " << val << ", ptr " << gep << "\n";
            }
        }
        return aname;
    }

    // ========================================================================
    // Array init expression
    // ========================================================================
    case NodeKind::ArrayInitExpr: {
        auto& ai = cast<ArrayInitExpr>(*expr);
        TypeId ty = expr->getType();
        std::string ll = llvmType(ty);
        std::string aname = nextReg();
        alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";

        TypeId elem_type;
        if (!ai.elements().empty() && ai.elements()[0]->hasType()) {
            elem_type = ai.elements()[0]->getType();
        }
        if (elem_type) {
            std::string ll_e = llvmType(elem_type);
            for (size_t i = 0; i < ai.elementCount(); ++i) {
                std::string val = emitExpr(ai.elements()[i].get());
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll_e
                         << ", ptr " << aname << ", i64 " << i << "\n";
                if (isAggregateType(elem_type)) {
                    body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << gep
                             << ", ptr " << val << ", i64 " << typeSizeBytes(elem_type)
                             << ", i1 false)\n";
                    needed_runtime_.insert("memcpy");
                } else {
                    body_ss_ << "  store " << ll_e << " " << val << ", ptr " << gep << "\n";
                }
            }
        }
        return aname;
    }

    // ========================================================================
    // Sizeof expression
    // ========================================================================
    case NodeKind::SizeofExpr: {
        auto& sf = cast<SizeofExpr>(*expr);
        TypeId target = sf.isTypeOperand() ? sf.targetType() :
                        (sf.expr() ? sf.expr()->getType() : TypeId());
        return std::to_string(typeSizeBytes(target));
    }

    // ========================================================================
    // Unsafe expression
    // ========================================================================
    case NodeKind::UnsafeExpr: {
        auto& us = cast<UnsafeExpr>(*expr);
        return emitExpr(us.inner());
    }

    // ========================================================================
    // Poison expression – stub lowering
    // ========================================================================
    case NodeKind::PoisonExpr: {
        // Stub lowering: emit a dummy 0 value so LLVM IR stays valid.
        // This allows the rest of the function to be compiled and analyzed.
        auto reg = nextReg();
        body_ss_ << "  " << reg << " = add i32 0, 0 ; poison stub\n";
        return reg;
    }

    // ========================================================================
    // Try expression – Zig-style error propagation
    // ========================================================================
    case NodeKind::TryExpr: {
        auto& te = static_cast<TryExpr&>(*expr);
        // Emit the operand
        std::string result = emitExpr(te.operand());

        // Defensive: if the operand type is not a proper ErrorType (e.g., due
        // to prior semantic errors), treat try as a no-op passthrough.
        TypeId operand_type = te.operand()->getType();
        if (!operand_type || !isa<ErrorType>(operand_type)) {
            // Not a proper error union — just pass through the result
            return result;
        }

        auto& err = cast<ErrorType>(operand_type);
        std::string vt = llvmType(err.successType());

        // Zig-style: Check the err_slot for errors instead of extracting
        // from a return struct.  The caller_err_slot_ was set by the
        // CallExpr handler when it emitted the call.
        std::string err_code = nextReg();
        body_ss_ << "  " << err_code << " = load i32, ptr " << caller_err_slot_ << "\n";
        std::string err_flag_reg = nextReg();
        body_ss_ << "  " << err_flag_reg << " = icmp ne i32 " << err_code << ", 0\n";

        // For non-void, load the success value from the alloca
        std::string value_reg;
        if (vt != "void") {
            if (isAggregateType(err.successType())) {
                value_reg = result;  // already a pointer to the value
            } else {
                value_reg = nextReg();
                body_ss_ << "  " << value_reg << " = load " << vt << ", ptr " << result << "\n";
            }
        }

        std::string err_label = nextLabel("try_err");
        std::string ok_label = nextLabel("try_ok");
        // Error paths are cold: annotate with branch weight metadata
        int try_prof_id = getBranchWeightMetadataId(1, 10000);
        body_ss_ << "  br i1 " << err_flag_reg << ", label %" << err_label
                 << ", label %" << ok_label
                 << ", !prof !" << try_prof_id << "\n";

        body_ss_ << err_label << ":\n";
        // On error: emit errdefer blocks and propagate the error
        emitErrdeferBlocks();
        emitDeferBlocks();
        if (!isTerminated()) {
            if (current_can_error_) {
                // Zig-style: store error code to current function's err_slot
                // and return zero/poison of the success type
                body_ss_ << "  store i32 " << err_code << ", ptr " << current_err_slot_ << "\n";
                std::string crt = llvmType(current_return_type_);
                if (crt == "void") {
                    body_ss_ << "  ret void\n";
                } else {
                    body_ss_ << "  ret " << crt << " " << zeroConstant(crt) << "\n";
                }
            } else {
                // Non-error function returning early from try — return zero
                std::string rt = llvmType(current_return_type_);
                body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
            }
        }
        setTerminated(true);

        body_ss_ << ok_label << ":\n";
        setTerminated(false);
        if (vt == "void") return "";  // no value to extract
        return value_reg;
    }

    // ========================================================================
    // Comptime expression – compile-time enforcement (passthrough at codegen)
    // ========================================================================
    case NodeKind::ComptimeExpr: {
        auto& ce = cast<ComptimeExpr>(*expr);
        // comptime enforcement is a compile-time concern, not a codegen concern.
        // The semantic analyzer already verified this evaluates at compile time.
        // Just emit the inner expression.
        return emitExpr(ce.inner());
    }

    // ========================================================================
    // Reduce expression – hardware-native parallel reduction tree
    // ========================================================================
    case NodeKind::ReduceExpr: {
        auto& re = cast<ReduceExpr>(*expr);
        TypeId result_type = expr->getType();
        std::string ll_result = llvmType(result_type);

        // Determine the neutral element based on the reduction op
        std::string identity;
        std::string llvm_op;
        switch (re.op()) {
            case ReduceExpr::ReduceOp::Add:
                identity = "0";
                llvm_op = "add";
                break;
            case ReduceExpr::ReduceOp::Mul:
                identity = "1";
                llvm_op = "mul";
                break;
            case ReduceExpr::ReduceOp::Max:
                // Use MIN_INT as identity for max reduction
                if (result_type && result_type->isFloat()) {
                    identity = "0xFFF0000000000000"; // -inf
                    llvm_op = "fmax";
                } else if (result_type && result_type->isSigned()) {
                    identity = "-9223372036854775808"; // INT64_MIN
                    llvm_op = "smax";
                } else {
                    identity = "0";
                    llvm_op = "umax";
                }
                break;
            case ReduceExpr::ReduceOp::Min:
                if (result_type && result_type->isFloat()) {
                    identity = "0x7FF0000000000000"; // +inf
                    llvm_op = "fmin";
                } else if (result_type && result_type->isSigned()) {
                    identity = "9223372036854775807"; // INT64_MAX
                    llvm_op = "smin";
                } else {
                    identity = "18446744073709551615"; // UINT64_MAX
                    llvm_op = "umin";
                }
                break;
            case ReduceExpr::ReduceOp::And:
                identity = "true";
                llvm_op = "and";
                break;
            case ReduceExpr::ReduceOp::Or:
                identity = "false";
                llvm_op = "or";
                break;
            case ReduceExpr::ReduceOp::BitAnd:
                identity = "-1"; // all bits set
                llvm_op = "and";
                break;
            case ReduceExpr::ReduceOp::BitOr:
                identity = "0";
                llvm_op = "or";
                break;
            default:
                identity = "0";
                llvm_op = "add";
                break;
        }

        // Emit the iterable (must produce a slice or array pointer)
        std::string iterable_reg = emitExpr(re.iterable());

        // Emit axis if present (for multi-dimensional reductions, future use)
        if (re.hasAxis()) {
            std::string axis_reg = emitExpr(re.axis());
            body_ss_ << "  ; reduce axis = " << axis_reg << "\n";
        }

        // Create an accumulator variable (SSA-tracked)
        std::string acc_name = "__reduce_acc_" + std::to_string(reg_counter_);
        std::string acc_alloca = makeAllocaName(acc_name);
        alloca_ss_ << "  " << acc_alloca << " = alloca " << ll_result << "\n";
        body_ss_ << "  store " << ll_result << " " << identity << ", ptr " << acc_alloca << "\n";

        // Get the iterable type to determine how to iterate
        TypeId iterable_type = re.iterable()->getType();

        // For slice types, extract ptr and len
        if (iterable_type && isa<SliceType>(iterable_type)) {
            // iterable_reg is a pointer to a { ptr, i64 } struct
            std::string data_ptr = nextReg();
            std::string len_ptr = nextReg();
            std::string data_val = nextReg();
            std::string len_val = nextReg();

            body_ss_ << "  " << data_ptr << " = getelementptr { ptr, i64 }, ptr " << iterable_reg << ", i64 0, i32 0\n";
            body_ss_ << "  " << data_val << " = load ptr, ptr " << data_ptr << "\n";
            body_ss_ << "  " << len_ptr << " = getelementptr { ptr, i64 }, ptr " << iterable_reg << ", i64 0, i32 1\n";
            body_ss_ << "  " << len_val << " = load i64, ptr " << len_ptr << "\n";

            // Emit reduction loop
            std::string loop_label = nextLabel("reduce_loop");
            std::string body_label = nextLabel("reduce_body");
            std::string exit_label = nextLabel("reduce_exit");

            // Loop counter alloca
            std::string i_alloca = makeAllocaName("__reduce_i");
            alloca_ss_ << "  " << i_alloca << " = alloca i64\n";
            body_ss_ << "  store i64 0, ptr " << i_alloca << "\n";

            body_ss_ << "  br label %" << loop_label << "\n";

            // Loop header
            body_ss_ << loop_label << ":\n";
            std::string i_val = nextReg();
            body_ss_ << "  " << i_val << " = load i64, ptr " << i_alloca << "\n";
            std::string cmp_reg = nextReg();
            body_ss_ << "  " << cmp_reg << " = icmp ult i64 " << i_val << ", " << len_val << "\n";
            body_ss_ << "  br i1 " << cmp_reg << ", label %" << body_label << ", label %" << exit_label << "\n";

            // Loop body: load element, accumulate
            body_ss_ << body_label << ":\n";
            std::string elem_ptr = nextReg();
            std::string elem_val = nextReg();
            std::string acc_val = nextReg();
            std::string new_acc = nextReg();

            body_ss_ << "  " << elem_ptr << " = getelementptr " << ll_result << ", ptr " << data_val << ", i64 " << i_val << "\n";
            body_ss_ << "  " << elem_val << " = load " << ll_result << ", ptr " << elem_ptr << "\n";
            body_ss_ << "  " << acc_val << " = load " << ll_result << ", ptr " << acc_alloca << "\n";

            // Emit the reduction operation
            if (llvm_op == "fmax" || llvm_op == "fmin") {
                body_ss_ << "  " << new_acc << " = call " << ll_result << " @llvm." << llvm_op << "." << ll_result << "(" << ll_result << " " << acc_val << ", " << ll_result << " " << elem_val << ")\n";
            } else if (llvm_op == "smax" || llvm_op == "smin" || llvm_op == "umax" || llvm_op == "umin") {
                // LLVM integer min/max via icmp + select
                std::string cmp_kind = (llvm_op == "smax") ? "sgt" :
                                       (llvm_op == "smin") ? "slt" :
                                       (llvm_op == "umax") ? "ugt" : "ult";
                std::string minmax_cmp = nextReg();
                body_ss_ << "  " << minmax_cmp << " = icmp " << cmp_kind << " " << ll_result << " " << acc_val << ", " << elem_val << "\n";
                body_ss_ << "  " << new_acc << " = select i1 " << minmax_cmp << ", " << ll_result << " " << acc_val << ", " << ll_result << " " << elem_val << "\n";
            } else if (ll_result == "i1") {
                body_ss_ << "  " << new_acc << " = " << llvm_op << " i1 " << acc_val << ", " << elem_val << "\n";
            } else if (result_type && result_type->isFloat()) {
                std::string float_op = (llvm_op == "add") ? "fadd" : (llvm_op == "mul") ? "fmul" : llvm_op;
                body_ss_ << "  " << new_acc << " = " << float_op << " " << ll_result << " " << acc_val << ", " << elem_val << "\n";
            } else {
                body_ss_ << "  " << new_acc << " = " << llvm_op << " " << ll_result << " " << acc_val << ", " << elem_val << "\n";
            }

            body_ss_ << "  store " << ll_result << " " << new_acc << ", ptr " << acc_alloca << "\n";

            // Increment counter
            std::string next_i = nextReg();
            body_ss_ << "  " << next_i << " = add i64 " << i_val << ", 1\n";
            body_ss_ << "  store i64 " << next_i << ", ptr " << i_alloca << "\n";
            body_ss_ << "  br label %" << loop_label << "\n";

            // Exit
            body_ss_ << exit_label << ":\n";
            std::string final_acc = nextReg();
            body_ss_ << "  " << final_acc << " = load " << ll_result << ", ptr " << acc_alloca << "\n";
            return final_acc;
        }

        // For non-slice types (single value), just return the value
        return iterable_reg;
    }

    default: break;
    }

    return "0";
}

// ============================================================================
// emitBinaryOp – emit a single binary operation instruction
// ============================================================================
void IRGenerator::emitBinaryOp(const std::string& result_reg,
                               const std::string& ll_type,
                               const std::string& lhs,
                               BinaryOp op,
                               const std::string& rhs,
                               TypeId result_type) {
    // Determine if we're working with integers, floats, or bools
    bool is_float = result_type && result_type->isFloat();
    [[maybe_unused]] bool is_bool  = result_type && result_type->isBool();
    bool is_signed = result_type && result_type->isSigned();
    [[maybe_unused]] bool is_unsigned = result_type && result_type->isUnsigned();

    switch (op) {
        // ---- Arithmetic ----
        case BinaryOp::Add:
            if (is_float) body_ss_ << "  " << result_reg << " = fadd " << ll_type << " " << lhs << ", " << rhs << "\n";
            else          body_ss_ << "  " << result_reg << " = add " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Sub:
            if (is_float) body_ss_ << "  " << result_reg << " = fsub " << ll_type << " " << lhs << ", " << rhs << "\n";
            else          body_ss_ << "  " << result_reg << " = sub " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Mul:
            if (is_float) body_ss_ << "  " << result_reg << " = fmul " << ll_type << " " << lhs << ", " << rhs << "\n";
            else          body_ss_ << "  " << result_reg << " = mul " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Div:
            if (is_float)     body_ss_ << "  " << result_reg << " = fdiv " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = sdiv " << ll_type << " " << lhs << ", " << rhs << "\n";
            else              body_ss_ << "  " << result_reg << " = udiv " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Mod:
            if (is_float)     body_ss_ << "  " << result_reg << " = frem " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = srem " << ll_type << " " << lhs << ", " << rhs << "\n";
            else              body_ss_ << "  " << result_reg << " = urem " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;

        // ---- Logical ----
        case BinaryOp::And:
            body_ss_ << "  " << result_reg << " = and i1 " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Or:
            body_ss_ << "  " << result_reg << " = or i1 " << lhs << ", " << rhs << "\n";
            break;

        // ---- Bitwise ----
        case BinaryOp::BitAnd:
            body_ss_ << "  " << result_reg << " = and " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::BitOr:
            body_ss_ << "  " << result_reg << " = or " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::BitXor:
            body_ss_ << "  " << result_reg << " = xor " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Shl:
            body_ss_ << "  " << result_reg << " = shl " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Shr:
            if (is_signed) body_ss_ << "  " << result_reg << " = ashr " << ll_type << " " << lhs << ", " << rhs << "\n";
            else           body_ss_ << "  " << result_reg << " = lshr " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;

        // ---- Comparison ----
        case BinaryOp::Eq:
            if (is_float) body_ss_ << "  " << result_reg << " = fcmp oeq " << ll_type << " " << lhs << ", " << rhs << "\n";
            else          body_ss_ << "  " << result_reg << " = icmp eq " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Ne:
            if (is_float) body_ss_ << "  " << result_reg << " = fcmp one " << ll_type << " " << lhs << ", " << rhs << "\n";
            else          body_ss_ << "  " << result_reg << " = icmp ne " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Lt:
            if (is_float)      body_ss_ << "  " << result_reg << " = fcmp olt " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = icmp slt " << ll_type << " " << lhs << ", " << rhs << "\n";
            else               body_ss_ << "  " << result_reg << " = icmp ult " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Le:
            if (is_float)      body_ss_ << "  " << result_reg << " = fcmp ole " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = icmp sle " << ll_type << " " << lhs << ", " << rhs << "\n";
            else               body_ss_ << "  " << result_reg << " = icmp ule " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Gt:
            if (is_float)      body_ss_ << "  " << result_reg << " = fcmp ogt " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = icmp sgt " << ll_type << " " << lhs << ", " << rhs << "\n";
            else               body_ss_ << "  " << result_reg << " = icmp ugt " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
        case BinaryOp::Ge:
            if (is_float)      body_ss_ << "  " << result_reg << " = fcmp oge " << ll_type << " " << lhs << ", " << rhs << "\n";
            else if (is_signed) body_ss_ << "  " << result_reg << " = icmp sge " << ll_type << " " << lhs << ", " << rhs << "\n";
            else               body_ss_ << "  " << result_reg << " = icmp uge " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;

        default:
            // Fallback
            body_ss_ << "  " << result_reg << " = add " << ll_type << " " << lhs << ", " << rhs << "\n";
            break;
    }
}

// ============================================================================
// emitLValue
// ============================================================================
std::string IRGenerator::emitLValue(Expr* expr) {
    if (!expr) {
        // BUG FIX: Return a valid alloca instead of "null" string
        std::string fallback = makeAllocaName("__fallback");
        alloca_ss_ << "  " << fallback << " = alloca i8\n";
        return fallback;
    }

    switch (expr->getKind()) {
        case NodeKind::IdentExpr: {
            auto& id = cast<IdentExpr>(*expr);
            SSAVarInfo* info = lookupVar(id.name());
            if (info && info->needs_alloca) {
                return info->alloca_name;
            }
            // SSA variable needs an alloca for address-of — create one on demand
            if (info && !info->needs_alloca) {
                // Materialize: create an alloca, store current value, mark as alloca-backed
                std::string aname = makeAllocaName(id.name());
                alloca_ss_ << "  " << aname << " = alloca " << info->llvm_type << "\n";
                body_ss_ << "  store " << info->llvm_type << " " << info->current_value
                         << ", ptr " << aname << "\n";
                info->alloca_name = aname;
                info->needs_alloca = true;
                return aname;
            }
            return "@" + sanitizeName(id.name());
        }

        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            TypeId obj_type = mem.object()->getType();

            // --- Pre-LLVM optimization: SROA field lvalue ---
            // If the object is an SROA-decomposed variable, look up the
            // field's alloca/pointer directly instead of GEP on the struct.
            if (mem.object()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*mem.object());
                if (isSROAVariable(ident.name())) {
                    std::string field_ssa = sroaFieldName(ident.name(), mem.field());
                    SSAVarInfo* info = lookupVar(field_ssa);
                    if (info) {
                        if (info->needs_alloca) {
                            return info->alloca_name;
                        }
                        // SSA-tracked scalar field — materialize an alloca
                        std::string aname = makeAllocaName(field_ssa);
                        alloca_ss_ << "  " << aname << " = alloca " << info->llvm_type << "\n";
                        body_ss_ << "  store " << info->llvm_type << " " << info->current_value
                                 << ", ptr " << aname << "\n";
                        info->alloca_name = aname;
                        info->needs_alloca = true;
                        return aname;
                    }
                }
            }

            // Pointer/reference deref
            if (obj_type && (isa<PointerType>(obj_type) || isa<ReferenceType>(obj_type) ||
                            isa<MutReferenceType>(obj_type))) {
                TypeId deref_type;
                if (isa<PointerType>(obj_type)) deref_type = cast<PointerType>(obj_type).pointee();
                else if (isa<ReferenceType>(obj_type)) deref_type = cast<ReferenceType>(obj_type).referent();
                else deref_type = cast<MutReferenceType>(obj_type).referent();

                if (deref_type && isa<StructType>(deref_type)) {
                    auto& st = cast<StructType>(deref_type);
                    int idx = st.fieldIndex(mem.field());
                    if (idx < 0) {
                        // BUG FIX: field not found — return fallback alloca
                        std::string fallback = makeAllocaName("__fb_field");
                        alloca_ss_ << "  " << fallback << " = alloca i8\n";
                        return fallback;
                    }
                    std::string ptr_val = emitExpr(mem.object());
                    std::string ll = llvmType(deref_type);
                    std::string gep = nextReg();
                    body_ss_ << "  " << gep << " = getelementptr " << ll
                             << ", ptr " << ptr_val << ", i32 0, i32 " << idx << "\n";
                    return gep;
                }
            }

            // Direct struct
            if (obj_type && isa<StructType>(obj_type)) {
                auto& st = cast<StructType>(obj_type);
                int idx = st.fieldIndex(mem.field());
                if (idx < 0) {
                    // BUG FIX: field not found — return fallback alloca
                    std::string fallback = makeAllocaName("__fb_field2");
                    alloca_ss_ << "  " << fallback << " = alloca i8\n";
                    return fallback;
                }
                std::string obj_ptr = emitLValue(mem.object());
                std::string ll = llvmType(obj_type);
                std::string gep = nextReg();
                body_ss_ << "  " << gep << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 " << idx << "\n";
                return gep;
            }

            // BUG FIX: Unknown member target — return fallback alloca
            {
                std::string fallback = makeAllocaName("__fb_mem");
                alloca_ss_ << "  " << fallback << " = alloca i8\n";
                return fallback;
            }
        }

        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            std::string index = emitExpr(idx.index());
            TypeId obj_type = idx.object()->getType();

            // Slice
            if (obj_type && isa<SliceType>(obj_type)) {
                std::string obj_ptr = emitLValue(idx.object());
                std::string ll = llvmType(obj_type);
                std::string dp = nextReg();
                body_ss_ << "  " << dp << " = getelementptr " << ll
                         << ", ptr " << obj_ptr << ", i32 0, i32 0\n";
                std::string data = nextReg();
                body_ss_ << "  " << data << " = load ptr, ptr " << dp << "\n";
                TypeId elem_type = expr->getType();
                std::string ll_e = llvmType(elem_type);
                std::string ep = nextReg();
                body_ss_ << "  " << ep << " = getelementptr " << ll_e
                         << ", ptr " << data << ", i64 " << index << "\n";
                return ep;
            }

            // Pointer
            if (obj_type && isa<PointerType>(obj_type)) {
                auto& pt = cast<PointerType>(obj_type);
                std::string ptr_val = emitExpr(idx.object());
                std::string ll_e = llvmType(pt.pointee());
                std::string ep = nextReg();
                body_ss_ << "  " << ep << " = getelementptr " << ll_e
                         << ", ptr " << ptr_val << ", i64 " << index << "\n";
                return ep;
            }

            // Array
            {
                std::string obj_ptr = emitLValue(idx.object());
                TypeId elem_type = expr->getType();
                std::string ll_e = llvmType(elem_type);
                std::string ep = nextReg();
                body_ss_ << "  " << ep << " = getelementptr " << ll_e
                         << ", ptr " << obj_ptr << ", i64 " << index << "\n";
                return ep;
            }
        }

        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return emitExpr(deref.operand());
        }

        case NodeKind::StructInitExpr:
        case NodeKind::ArrayInitExpr:
            return emitExpr(expr); // already returns a pointer

        default:
            break;
    }
    return emitExpr(expr);
}

// ============================================================================
// Smart pointer emission
// ============================================================================

std::string IRGenerator::emitBoxNew(Expr* value, TypeId pointee_type) {
    std::string ll = llvmType(pointee_type);
    uint64_t size = typeSizeBytes(pointee_type);

    // --- Pre-LLVM optimization: StackAllocated ---
    // If the EscapeAnalysis pass marked this Box as non-escaping
    // in the MetadataMap, use alloca instead of malloc. This
    // eliminates heap allocation overhead entirely for short-lived
    // Boxes. LLVM CANNOT do this because malloc/free are opaque
    // external calls.
    bool is_stack_allocated = false;
    if (meta_map_) {
        auto* nm = meta_map_->get(value);
        is_stack_allocated = nm && nm->stack_allocated;
    }
    if (is_stack_allocated) {
        // Stack-allocated Box: alloca + store (no malloc/free)
        std::string stack_ptr = makeAllocaName("__stack_box");
        alloca_ss_ << "  " << stack_ptr << " = alloca " << ll << "\n";
        std::string val = emitExpr(value);
        if (isAggregateType(pointee_type)) {
            body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << stack_ptr
                     << ", ptr " << val << ", i64 " << size << ", i1 false)\n";
            needed_runtime_.insert("memcpy");
        } else {
            body_ss_ << "  store " << ll << " " << val << ", ptr " << stack_ptr << "\n";
        }
        stack_box_allocas_.insert(stack_ptr);
        return stack_ptr;
    }

    // Default: heap allocation via malloc
    needed_runtime_.insert("malloc");
    std::string mr = nextReg();
    body_ss_ << "  " << mr << " = call ptr @malloc(i64 " << size << ")\n";

    std::string val = emitExpr(value);
    if (isAggregateType(pointee_type)) {
        body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << mr
                 << ", ptr " << val << ", i64 " << size << ", i1 false)\n";
        needed_runtime_.insert("memcpy");
    } else {
        body_ss_ << "  store " << ll << " " << val << ", ptr " << mr << "\n";
    }
    return mr;
}

void IRGenerator::emitBoxDrop(const std::string& ptr_val) {
    // If this Box was stack-allocated (by emitBoxNew with StackAllocated
    // annotation), skip the free — the alloca is automatically reclaimed
    // when the function returns.
    if (stack_box_allocas_.count(ptr_val)) {
        // Stack-allocated Box: no free needed, just mark as dropped
        // (could add lifetime.end intrinsic for more precise stack slot reuse)
        body_ss_ << "  ; stack-allocated Box.drop — no free needed for " << ptr_val << "\n";
        return;
    }
    needed_runtime_.insert("free");
    body_ss_ << "  call void @free(ptr " << ptr_val << ")\n";
}

std::string IRGenerator::emitRcNew(Expr* value, TypeId pointee_type) {
    needed_runtime_.insert("malloc");
    uint64_t val_size = typeSizeBytes(pointee_type);
    uint64_t total = val_size + 8;

    std::string mr = nextReg();
    body_ss_ << "  " << mr << " = call ptr @malloc(i64 " << total << ")\n";

    // refcount = 1
    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << mr << ", i64 0\n";
    body_ss_ << "  store i64 1, ptr " << rcg << "\n";

    // value at offset 8
    std::string vg = nextReg();
    body_ss_ << "  " << vg << " = getelementptr i8, ptr " << mr << ", i64 8\n";
    std::string val = emitExpr(value);
    std::string ll = llvmType(pointee_type);
    if (isAggregateType(pointee_type)) {
        body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << vg
                 << ", ptr " << val << ", i64 " << val_size << ", i1 false)\n";
        needed_runtime_.insert("memcpy");
    } else {
        body_ss_ << "  store " << ll << " " << val << ", ptr " << vg << "\n";
    }

    // Build Rc struct { ptr, i64 }
    TypeId rc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Rc);
    std::string rct = llvmType(rc_type);
    std::string ea = makeAllocaName("__rc");
    alloca_ss_ << "  " << ea << " = alloca " << rct << "\n";

    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << rct << ", ptr " << ea << ", i32 0, i32 0\n";
    body_ss_ << "  store ptr " << mr << ", ptr " << pg << "\n";

    std::string cg = nextReg();
    body_ss_ << "  " << cg << " = getelementptr " << rct << ", ptr " << ea << ", i32 0, i32 1\n";
    body_ss_ << "  store i64 1, ptr " << cg << "\n";

    return ea;
}

std::string IRGenerator::emitRcClone(const std::string& rc_ptr, TypeId pointee_type) {
    TypeId rc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Rc);
    std::string rct = llvmType(rc_type);

    // Load data pointer
    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << rct << ", ptr " << rc_ptr << ", i32 0, i32 0\n";
    std::string dp = nextReg();
    body_ss_ << "  " << dp << " = load ptr, ptr " << pg << "\n";

    // Increment refcount
    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << dp << ", i64 0\n";
    std::string old = nextReg();
    body_ss_ << "  " << old << " = load i64, ptr " << rcg << "\n";
    std::string nw = nextReg();
    body_ss_ << "  " << nw << " = add i64 " << old << ", 1\n";
    body_ss_ << "  store i64 " << nw << ", ptr " << rcg << "\n";

    // Copy struct
    std::string na = makeAllocaName("__rc_clone");
    alloca_ss_ << "  " << na << " = alloca " << rct << "\n";
    body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << na << ", ptr " << rc_ptr
             << ", i64 " << typeSizeBytes(rc_type) << ", i1 false)\n";
    needed_runtime_.insert("memcpy");
    return na;
}

void IRGenerator::emitRcDrop(const std::string& rc_ptr, TypeId pointee_type) {
    needed_runtime_.insert("free");
    TypeId rc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Rc);
    std::string rct = llvmType(rc_type);

    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << rct << ", ptr " << rc_ptr << ", i32 0, i32 0\n";
    std::string dp = nextReg();
    body_ss_ << "  " << dp << " = load ptr, ptr " << pg << "\n";

    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << dp << ", i64 0\n";
    std::string old = nextReg();
    body_ss_ << "  " << old << " = load i64, ptr " << rcg << "\n";
    std::string nw = nextReg();
    body_ss_ << "  " << nw << " = sub i64 " << old << ", 1\n";
    body_ss_ << "  store i64 " << nw << ", ptr " << rcg << "\n";

    std::string iz = nextReg();
    body_ss_ << "  " << iz << " = icmp eq i64 " << nw << ", 0\n";
    std::string fl = nextLabel("rc.free");
    std::string dl = nextLabel("rc.done");
    body_ss_ << "  br i1 " << iz << ", label %" << fl << ", label %" << dl << "\n";
    body_ss_ << fl << ":\n";
    body_ss_ << "  call void @free(ptr " << dp << ")\n";
    body_ss_ << "  br label %" << dl << "\n";
    body_ss_ << dl << ":\n";
    setTerminated(false);
}

std::string IRGenerator::emitArcNew(Expr* value, TypeId pointee_type) {
    needed_runtime_.insert("malloc");
    uint64_t val_size = typeSizeBytes(pointee_type);
    uint64_t total = val_size + 8;

    std::string mr = nextReg();
    body_ss_ << "  " << mr << " = call ptr @malloc(i64 " << total << ")\n";

    // Atomic store refcount = 1
    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << mr << ", i64 0\n";
    body_ss_ << "  store atomic i64 1, ptr " << rcg << " release\n";

    // Value at offset 8
    std::string vg = nextReg();
    body_ss_ << "  " << vg << " = getelementptr i8, ptr " << mr << ", i64 8\n";
    std::string val = emitExpr(value);
    std::string ll = llvmType(pointee_type);
    if (isAggregateType(pointee_type)) {
        body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << vg
                 << ", ptr " << val << ", i64 " << val_size << ", i1 false)\n";
        needed_runtime_.insert("memcpy");
    } else {
        body_ss_ << "  store " << ll << " " << val << ", ptr " << vg << "\n";
    }

    // Build Arc struct { ptr, i64 }
    TypeId arc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Arc);
    std::string at = llvmType(arc_type);
    std::string ea = makeAllocaName("__arc");
    alloca_ss_ << "  " << ea << " = alloca " << at << "\n";

    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << at << ", ptr " << ea << ", i32 0, i32 0\n";
    body_ss_ << "  store ptr " << mr << ", ptr " << pg << "\n";

    std::string cg = nextReg();
    body_ss_ << "  " << cg << " = getelementptr " << at << ", ptr " << ea << ", i32 0, i32 1\n";
    body_ss_ << "  store i64 1, ptr " << cg << "\n";

    return ea;
}

std::string IRGenerator::emitArcClone(const std::string& arc_ptr, TypeId pointee_type) {
    needed_runtime_.insert("atomic_add");
    TypeId arc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Arc);
    std::string at = llvmType(arc_type);

    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << at << ", ptr " << arc_ptr << ", i32 0, i32 0\n";
    std::string dp = nextReg();
    body_ss_ << "  " << dp << " = load ptr, ptr " << pg << "\n";

    // Atomic increment
    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << dp << ", i64 0\n";
    std::string old = nextReg();
    // BUG FIX: atomicrmw type must be i64 (the integer type), not ptr
    body_ss_ << "  " << old << " = atomicrmw add i64 ptr " << rcg << ", i64 1 acquire\n";

    // Copy struct
    std::string na = makeAllocaName("__arc_clone");
    alloca_ss_ << "  " << na << " = alloca " << at << "\n";
    body_ss_ << "  call void @llvm.memcpy.p0.p0.i64(ptr " << na << ", ptr " << arc_ptr
             << ", i64 " << typeSizeBytes(arc_type) << ", i1 false)\n";
    needed_runtime_.insert("memcpy");
    return na;
}

void IRGenerator::emitArcDrop(const std::string& arc_ptr, TypeId pointee_type) {
    needed_runtime_.insert("free");
    needed_runtime_.insert("atomic_sub");
    TypeId arc_type = type_table_.getSmartPointer(pointee_type, SmartPointerKind::Arc);
    std::string at = llvmType(arc_type);

    std::string pg = nextReg();
    body_ss_ << "  " << pg << " = getelementptr " << at << ", ptr " << arc_ptr << ", i32 0, i32 0\n";
    std::string dp = nextReg();
    body_ss_ << "  " << dp << " = load ptr, ptr " << pg << "\n";

    // Atomic decrement
    std::string rcg = nextReg();
    body_ss_ << "  " << rcg << " = getelementptr i64, ptr " << dp << ", i64 0\n";
    std::string old = nextReg();
    // BUG FIX: atomicrmw type must be i64 (the integer type), not ptr
    body_ss_ << "  " << old << " = atomicrmw sub i64 ptr " << rcg << ", i64 1 release\n";

    // If old was 1, free
    std::string iz = nextReg();
    body_ss_ << "  " << iz << " = icmp eq i64 " << old << ", 1\n";
    std::string fl = nextLabel("arc.free");
    std::string dl = nextLabel("arc.done");
    body_ss_ << "  br i1 " << iz << ", label %" << fl << ", label %" << dl << "\n";
    body_ss_ << fl << ":\n";
    body_ss_ << "  call void @free(ptr " << dp << ")\n";
    body_ss_ << "  br label %" << dl << "\n";
    body_ss_ << dl << ":\n";
    setTerminated(false);
}

// ============================================================================
// Error handling
// ============================================================================
std::string IRGenerator::emitErrorCheck(const std::string& result_ptr, TypeId error_type) {
    auto& err = cast<ErrorType>(error_type);
    TypeId succ = err.successType();
    std::string vt = llvmType(succ);

    // Determine if this error type uses niche optimization
    bool use_pointer_niche = succ && (isa<PointerType>(succ) || isa<ReferenceType>(succ) ||
                                      isa<MutReferenceType>(succ));
    bool use_box_niche = false;
    if (succ && isa<SmartPointerType>(succ)) {
        auto& sp = cast<SmartPointerType>(succ);
        use_box_niche = sp.smartPointerKind() == SmartPointerKind::Box;
    }
    bool use_integer_niche = false;
    bool use_bool_niche = false;
    if (succ && isa<PrimitiveType>(succ)) {
        auto& prim = cast<PrimitiveType>(succ);
        if (prim.isInteger() && prim.bitWidth() <= 32) {
            use_integer_niche = true;
        } else if (prim.isBool()) {
            use_bool_niche = true;
        }
    }

    if (use_pointer_niche || use_box_niche) {
        // ====================================================================
        // Pointer niche: check if the lowest bit of the pointer is set
        // Valid pointers are always aligned, so bit 0 = 0 on success,
        // bit 0 = 1 on error (sentinel value).
        // ====================================================================
        std::string ptr_val = nextReg();
        body_ss_ << "  " << ptr_val << " = load ptr, ptr " << result_ptr << "\n";
        std::string intptr_val = nextReg();
        body_ss_ << "  " << intptr_val << " = ptrtoint ptr " << ptr_val << " to i64\n";
        std::string ef = nextReg();
        body_ss_ << "  " << ef << " = icmp ne i64 " << intptr_val << ", 0\n";
        // Actually: check the low bit — error if bit 0 is set
        std::string low_bit = nextReg();
        body_ss_ << "  " << low_bit << " = and i64 " << intptr_val << ", 1\n";
        std::string ef2 = nextReg();
        body_ss_ << "  " << ef2 << " = icmp ne i64 " << low_bit << ", 0\n";

        std::string err_l = nextLabel("error.propagate");
        std::string ok_l  = nextLabel("error.ok");
        int prof_id = getBranchWeightMetadataId(1, 10000);
        body_ss_ << "  br i1 " << ef2 << ", label %" << err_l << ", label %" << ok_l
                 << ", !prof !" << prof_id << "\n";

        // Error block: extract error code from the pointer (bits 1..31 shifted right)
        body_ss_ << err_l << ":\n";
        setTerminated(false);
        emitDeferBlocks();
        std::string err_code = nextReg();
        body_ss_ << "  " << err_code << " = lshr i64 " << intptr_val << ", 1\n";
        std::string err_code_i32 = nextReg();
        body_ss_ << "  " << err_code_i32 << " = trunc i64 " << err_code << " to i32\n";
        if (current_can_error_) {
            body_ss_ << "  store i32 " << err_code_i32 << ", ptr " << current_err_slot_ << "\n";
            std::string crt = llvmType(current_return_type_);
            if (crt == "void") {
                body_ss_ << "  ret void\n";
            } else {
                body_ss_ << "  ret " << crt << " " << zeroConstant(crt) << "\n";
            }
        } else {
            if (current_return_type_ && !current_return_type_->isVoid()) {
                std::string rt = llvmType(current_return_type_);
                body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
            } else {
                body_ss_ << "  ret void\n";
            }
        }
        setTerminated(true);

        // OK block: return the pointer value as-is (it's a valid pointer)
        body_ss_ << ok_l << ":\n";
        setTerminated(false);
        return ptr_val;

    } else if (use_integer_niche) {
        // ====================================================================
        // Integer niche: check if the high bit of the i64 is set
        // Success values fit in the lower 32 bits, error has the 63rd bit set.
        // ====================================================================
        std::string ival = nextReg();
        body_ss_ << "  " << ival << " = load i64, ptr " << result_ptr << "\n";
        std::string high_bit = nextReg();
        body_ss_ << "  " << high_bit << " = ashr i64 " << ival << ", 63\n";
        std::string ef = nextReg();
        body_ss_ << "  " << ef << " = icmp ne i64 " << high_bit << ", 0\n";

        std::string err_l = nextLabel("error.propagate");
        std::string ok_l  = nextLabel("error.ok");
        int prof_id = getBranchWeightMetadataId(1, 10000);
        body_ss_ << "  br i1 " << ef << ", label %" << err_l << ", label %" << ok_l
                 << ", !prof !" << prof_id << "\n";

        // Error block: extract error code from lower 32 bits
        body_ss_ << err_l << ":\n";
        setTerminated(false);
        emitDeferBlocks();
        std::string err_code = nextReg();
        body_ss_ << "  " << err_code << " = trunc i64 " << ival << " to i32\n";
        if (current_can_error_) {
            body_ss_ << "  store i32 " << err_code << ", ptr " << current_err_slot_ << "\n";
            std::string crt = llvmType(current_return_type_);
            if (crt == "void") {
                body_ss_ << "  ret void\n";
            } else {
                body_ss_ << "  ret " << crt << " " << zeroConstant(crt) << "\n";
            }
        } else {
            if (current_return_type_ && !current_return_type_->isVoid()) {
                std::string rt = llvmType(current_return_type_);
                body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
            } else {
                body_ss_ << "  ret void\n";
            }
        }
        setTerminated(true);

        // OK block: truncate i64 to the actual integer type
        body_ss_ << ok_l << ":\n";
        setTerminated(false);
        std::string result = nextReg();
        body_ss_ << "  " << result << " = trunc i64 " << ival << " to " << vt << "\n";
        return result;

    } else if (use_bool_niche) {
        // ====================================================================
        // Bool niche: check if the high bit of the i8 is set
        // Success: 0 or 1 in the lower bit, error has bit 7 set.
        // ====================================================================
        std::string ival = nextReg();
        body_ss_ << "  " << ival << " = load i8, ptr " << result_ptr << "\n";
        std::string high_bit = nextReg();
        body_ss_ << "  " << high_bit << " = ashr i8 " << ival << ", 7\n";
        std::string ef = nextReg();
        body_ss_ << "  " << ef << " = icmp ne i8 " << high_bit << ", 0\n";

        std::string err_l = nextLabel("error.propagate");
        std::string ok_l  = nextLabel("error.ok");
        int prof_id = getBranchWeightMetadataId(1, 10000);
        body_ss_ << "  br i1 " << ef << ", label %" << err_l << ", label %" << ok_l
                 << ", !prof !" << prof_id << "\n";

        // Error block
        body_ss_ << err_l << ":\n";
        setTerminated(false);
        emitDeferBlocks();
        std::string err_code = nextReg();
        body_ss_ << "  " << err_code << " = zext i8 " << ival << " to i32\n";
        if (current_can_error_) {
            body_ss_ << "  store i32 " << err_code << ", ptr " << current_err_slot_ << "\n";
            std::string crt = llvmType(current_return_type_);
            if (crt == "void") {
                body_ss_ << "  ret void\n";
            } else {
                body_ss_ << "  ret " << crt << " " << zeroConstant(crt) << "\n";
            }
        } else {
            if (current_return_type_ && !current_return_type_->isVoid()) {
                std::string rt = llvmType(current_return_type_);
                body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
            } else {
                body_ss_ << "  ret void\n";
            }
        }
        setTerminated(true);

        // OK block: truncate to i1
        body_ss_ << ok_l << ":\n";
        setTerminated(false);
        std::string result = nextReg();
        body_ss_ << "  " << result << " = trunc i8 " << ival << " to i1\n";
        return result;

    } else {
        // ====================================================================
        // Struct fallback: traditional err_slot check (Zig-style)
        // ====================================================================
        // Zig-style: Check the err_slot for errors instead of extracting from
        // a return struct.  The caller_err_slot_ was set by the CallExpr handler.
        std::string err_code = nextReg();
        body_ss_ << "  " << err_code << " = load i32, ptr " << caller_err_slot_ << "\n";
        std::string ef = nextReg();
        body_ss_ << "  " << ef << " = icmp ne i32 " << err_code << ", 0\n";

        std::string err_l = nextLabel("error.propagate");
        std::string ok_l  = nextLabel("error.ok");
        // Error paths are cold: annotate with branch weight metadata so LLVM
        // places error blocks in cold sections and optimizes the happy path.
        int prof_id = getBranchWeightMetadataId(1, 10000);
        body_ss_ << "  br i1 " << ef << ", label %" << err_l << ", label %" << ok_l
                 << ", !prof !" << prof_id << "\n";

        // Error block
        body_ss_ << err_l << ":\n";
        setTerminated(false);
        emitDeferBlocks();
        if (current_can_error_) {
            // Zig-style: store error code to current function's err_slot
            // and return zero/poison of the success type
            body_ss_ << "  store i32 " << err_code << ", ptr " << current_err_slot_ << "\n";
            std::string crt = llvmType(current_return_type_);
            if (crt == "void") {
                body_ss_ << "  ret void\n";
            } else {
                body_ss_ << "  ret " << crt << " " << zeroConstant(crt) << "\n";
            }
        } else {
            // Non-error-returning function — return zero of the actual return type
            if (current_return_type_ && !current_return_type_->isVoid()) {
                std::string rt = llvmType(current_return_type_);
                body_ss_ << "  ret " << rt << " " << zeroConstant(rt) << "\n";
            } else {
                body_ss_ << "  ret void\n";
            }
        }
        setTerminated(true);

        // OK block
        body_ss_ << ok_l << ":\n";
        setTerminated(false);

        if (vt == "void") return "";
        // result_ptr is a pointer to an alloca containing the success value
        if (isAggregateType(err.successType())) return result_ptr;
        std::string vr = nextReg();
        body_ss_ << "  " << vr << " = load " << vt << ", ptr " << result_ptr << "\n";
        return vr;
    }
}

// ============================================================================
// Allocator helpers
// ============================================================================
std::string IRGenerator::emitAllocatorCall(Expr* allocator_expr, TypeId alloc_type,
                                           const std::string& count_reg) {
    std::string ap = emitExpr(allocator_expr);

    // Load the alloc function pointer (field 0 of the vtable)
    std::string afn_g = nextReg();
    body_ss_ << "  " << afn_g << " = getelementptr ptr, ptr " << ap << ", i32 0\n";
    std::string afn = nextReg();
    body_ss_ << "  " << afn << " = load ptr, ptr " << afn_g << "\n";

    uint64_t elem_size = typeSizeBytes(alloc_type);
    std::string ts = nextReg();
    body_ss_ << "  " << ts << " = mul i64 " << elem_size << ", " << count_reg << "\n";

    std::string result = nextReg();
    body_ss_ << "  " << result << " = call ptr " << afn << "(i64 " << ts << ")\n";
    return result;
}

// ============================================================================
// emitSimdLoopMetadata – create @simd loop vectorization metadata
// ============================================================================
int IRGenerator::emitSimdLoopMetadata() {
    // Check if we already have a cached SIMD loop metadata node
    auto it = metadata_map_.find("llvm.loop.simd");
    if (it != metadata_map_.end()) return it->second;

    // Create individual metadata entries for each vectorization hint.
    // LLVM expects specific types for each option:
    //   vectorize.enable    → i1 true (boolean flag)
    //   vectorize.width     → i32 N   (vector width in elements)
    //   interleave.count    → i32 N   (interleave factor)
    //   unroll.enable       → i1 true (force unroll small SIMD loops)
    //   unroll.count        → i32 N   (unroll factor)
    int enable_id     = metadata_counter_++;
    int width_id      = metadata_counter_++;
    int interleave_id = metadata_counter_++;
    int unroll_en_id  = metadata_counter_++;
    int unroll_cnt_id = metadata_counter_++;

    metadata_entries_.push_back({enable_id,
        "!{!\"llvm.loop.vectorize.enable\", i1 true}"});
    metadata_entries_.push_back({width_id,
        "!{!\"llvm.loop.vectorize.width\", i32 4}"});
    metadata_entries_.push_back({interleave_id,
        "!{!\"llvm.loop.interleave.count\", i32 4}"});
    metadata_entries_.push_back({unroll_en_id,
        "!{!\"llvm.loop.unroll.enable\", i1 true}"});
    metadata_entries_.push_back({unroll_cnt_id,
        "!{!\"llvm.loop.unroll.count\", i32 4}"});

    // Create the loop metadata grouping node — contains all the property nodes.
    //   !4 = !{!1, !2, !3, !4, !5}
    int group_id = metadata_counter_++;
    std::string group_content = "!{!" + std::to_string(enable_id)
        + ", !" + std::to_string(width_id)
        + ", !" + std::to_string(interleave_id)
        + ", !" + std::to_string(unroll_en_id)
        + ", !" + std::to_string(unroll_cnt_id) + "}";
    metadata_entries_.push_back({group_id, group_content});

    // Create the self-referencing distinct metadata node that LLVM requires.
    // The !llvm.loop annotation must point to a distinct node that references
    // itself as the first operand.  This is how LLVM identifies the start of
    // a loop metadata chain.
    //   !5 = distinct !{!5, !4}
    int loop_id = metadata_counter_++;
    std::string loop_content = "distinct !{!" + std::to_string(loop_id)
        + ", !" + std::to_string(group_id) + "}";
    metadata_entries_.push_back({loop_id, loop_content});

    metadata_map_["llvm.loop.simd"] = loop_id;
    return loop_id;
}

// ============================================================================
// getBranchWeightMetadataId – create branch weight profile metadata
// ============================================================================
int IRGenerator::getBranchWeightMetadataId(uint32_t cold_weight, uint32_t hot_weight) {
    std::string key = "branch_weights." + std::to_string(cold_weight) + "." + std::to_string(hot_weight);
    auto it = metadata_map_.find(key);
    if (it != metadata_map_.end()) return it->second;

    int prof_id = metadata_counter_++;
    metadata_entries_.push_back({prof_id,
        "!{!\"branch_weights\", i32 " + std::to_string(cold_weight) +
        ", i32 " + std::to_string(hot_weight) + "}"});
    metadata_map_[key] = prof_id;
    return prof_id;
}

// ============================================================================
// emitColdPathMetadata – emit !prof metadata for cold/hot branch hints
//
// When a node (typically a TryExpr, IfStmt, or ErrdeferStmt) has a
// cold_path flag in the MetadataMap from the ErrorPathSeparator pass,
// this method creates LLVM branch weight metadata that tells the
// optimizer the likely execution direction.
//
// Returns a string like ", !prof !5" to append to a branch instruction,
// or empty string if no cold path metadata exists.
// ============================================================================
std::string IRGenerator::emitColdPathMetadata(const ASTNode* node) {
    if (!meta_map_ || !node) return "";

    auto* nm = meta_map_->get(node);
    if (!nm || !nm->llvm_meta.cold_path) return "";

    // Use default cold path weights: 1 (cold) : 1000 (hot)
    int prof_id = getBranchWeightMetadataId(1, 1000);
    return ", !prof !" + std::to_string(prof_id);
}

// ============================================================================
// emitPrefetchIfAnnotated – emit prefetch intrinsics for aligned loop access
//
// When a WhileStmt has a prefetch_distance > 0 in the MetadataMap
// from the PrefetchInserter pass, this method emits:
//   call void @llvm.prefetch(ptr %next_addr, i32 0, i32 3, i32 1)
//
// This is called at the top of the loop body, before any other statements.
// ============================================================================
void IRGenerator::emitPrefetchIfAnnotated(WhileStmt* loop) {
    if (!meta_map_ || !loop) return;

    auto* nm = meta_map_->get(loop);
    if (!nm || nm->llvm_meta.prefetch_distance <= 0) return;

    int distance = static_cast<int>(nm->llvm_meta.prefetch_distance);

    // Emit the prefetch intrinsic declaration
    needed_runtime_.insert("prefetch");

    body_ss_ << "  ; [prefetch] distance=" << distance
             << " (MetadataMap prefetch hint)\n";

    // Find the array variable and loop index from access patterns
    // Access patterns are stored in nm->access_patterns
    if (!nm->access_patterns.empty()) {
        // Use the first access pattern to find the array + index
        const auto& ap = nm->access_patterns.front();

        // Look for the index variable in the loop body
        // Walk the while condition to find "i < N" pattern
        // Then compute prefetch address as: base_addr + (i + distance) * element_size

        // For now, emit a more useful prefetch using the GEP we can compute
        // from the access pattern's variable name
        std::string var_name = ap.variable_name;
        if (!var_name.empty()) {
            // Get the base address of the array variable
            std::string base_addr = getVarValue(var_name);
            if (!base_addr.empty()) {
                // Emit: prefetch(base + distance * cache_line_size)
                std::string prefetch_addr = nextReg();
                body_ss_ << "  " << prefetch_addr << " = getelementptr i8, ptr "
                         << base_addr << ", i64 " << (distance * 64) << "\n";
                body_ss_ << "  call void @llvm.prefetch.p0(ptr " << prefetch_addr
                         << ", i32 0, i32 3, i32 1)\n";
                return;
            }
        }
    }

    // Fallback: emit with null (old behavior, better than nothing for the
    // prefetch declaration to be present)
    body_ss_ << "  call void @llvm.prefetch.p0(ptr null, i32 0, i32 3, i32 1)\n";
}

// ============================================================================
// emitYieldCheckIfAnnotated – emit cooperative yield checks in loops
//
// When a WhileStmt has yield_point = true in the MetadataMap
// from the YieldPointInserter pass, this method emits a counter-based
// yield check.
// ============================================================================
void IRGenerator::emitYieldCheckIfAnnotated(WhileStmt* loop) {
    if (!meta_map_ || !loop) return;

    auto* nm = meta_map_->get(loop);
    if (!nm || !nm->yield_point) return;

    // Use a default interval of 256 iterations
    int interval = 256;

    needed_runtime_.insert("tether_yield");

    // Create a hidden counter alloca for this loop's yield check
    std::string counter_alloca = makeAllocaName("__yield_counter_" + current_fn_name_);
    alloca_ss_ << "  " << counter_alloca << " = alloca i64\n";
    body_ss_ << "  store i64 0, ptr " << counter_alloca << "\n";

    // At the top of the loop body: increment counter and check
    std::string counter_val = nextReg();
    body_ss_ << "  " << counter_val << " = load i64, ptr " << counter_alloca << "\n";
    std::string incremented = nextReg();
    body_ss_ << "  " << incremented << " = add i64 " << counter_val << ", 1\n";
    body_ss_ << "  store i64 " << incremented << ", ptr " << counter_alloca << "\n";
    std::string mod_val = nextReg();
    body_ss_ << "  " << mod_val << " = srem i64 " << incremented << ", " << interval << "\n";
    std::string should_yield = nextReg();
    body_ss_ << "  " << should_yield << " = icmp eq i64 " << mod_val << ", 0\n";

    std::string yield_check_l = nextLabel("yield.check");
    std::string yield_skip_l = nextLabel("yield.skip");

    body_ss_ << "  br i1 " << should_yield << ", label %" << yield_check_l
             << ", label %" << yield_skip_l << "\n";
    setTerminated(false);

    body_ss_ << yield_check_l << ":\n";
    body_ss_ << "  call void @tether_yield(i64 0)\n";
    body_ss_ << "  br label %" << yield_skip_l << "\n";
    setTerminated(false);

    body_ss_ << yield_skip_l << ":\n";
    setTerminated(false);
}

// ============================================================================
// hasOpaqueBarrierAnnotation – check if a function has an opaque barrier
//
// Now checks the MetadataMap instead of the ASTAnnotationMap.
// ============================================================================
bool IRGenerator::hasOpaqueBarrierAnnotation(FnDecl* fn) const {
    if (!meta_map_ || !fn) return false;
    auto* nm = meta_map_->get(fn);
    return nm && nm->opaque_barrier;
}

// ============================================================================
// emitInlineAllocatorIfAnnotated – inline arena bump allocation
//
// When a CallExpr has allocator_inlined = true in the MetadataMap
// from the AllocatorLowerer pass, emit inline bump allocation code.
// ============================================================================
std::string IRGenerator::emitInlineAllocatorIfAnnotated(CallExpr* call, TypeId /*ret_type*/) {
    if (!meta_map_ || !call) return "";

    auto* nm = meta_map_->get(call);
    if (!nm || !nm->allocator_inlined) return "";

    // Use a default allocation size of 8 bytes
    // (The exact size is not stored in the MetadataMap; the IR generator
    // can compute it from the call's type information.)
    uint64_t alloc_size = 8;

    // Try to infer the allocation size from the call's return type
    if (call->hasType()) {
        TypeId ret_type = call->getType();
        if (ret_type && !ret_type.isNull()) {
            uint64_t computed_size = typeSizeBytes(ret_type);
            if (computed_size > 0) alloc_size = computed_size;
        }
    }

    // Align up to 16 bytes (common arena alignment)
    uint64_t aligned_size = ((alloc_size + 15) / 16) * 16;

    // Get the callee (should be allocator.alloc or allocator.create)
    // The object is the allocator variable
    std::string callee_str;
    if (auto* member = dyn_cast<MemberExpr>(call->callee())) {
        callee_str = emitLValue(member->object());
    } else {
        callee_str = emitExpr(call->callee());
    }

    // Emit inline arena bump allocation:
    //   %old = load ptr, ptr %allocator.offset
    //   %aligned = add ptr %old, SIZE
    //   store ptr %aligned, ptr %allocator.offset
    //   result = %old
    std::string old_ptr = nextReg();
    body_ss_ << "  " << old_ptr << " = load ptr, ptr " << callee_str << "\n";

    std::string new_ptr = nextReg();
    body_ss_ << "  " << new_ptr << " = getelementptr i8, ptr " << old_ptr
             << ", i64 " << aligned_size << "\n";
    body_ss_ << "  store ptr " << new_ptr << ", ptr " << callee_str << "\n";

    // Zero-initialize the allocated memory
    needed_runtime_.insert("memset");
    body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << old_ptr
             << ", i8 0, i64 " << alloc_size << ", i1 false)\n";

    return old_ptr;
}

// ============================================================================
// hasSoAAnnotation – check if an expression has a SoA transform annotation
//
// Now checks the MetadataMap instead of the ASTAnnotationMap.
// ============================================================================
bool IRGenerator::hasSoAAnnotation(Expr* expr) const {
    if (!meta_map_ || !expr) return false;
    auto* nm = meta_map_->get(expr);
    return nm && nm->soa_transformed;
}

// ============================================================================
// emitSoAAccessIfAnnotated – emit SoA-transformed array access
//
// When a MemberExpr has soa_transformed = true in the MetadataMap
// from the AoS→SoA pass, rewrite the access pattern.
// ============================================================================
std::string IRGenerator::emitSoAAccessIfAnnotated(Expr* expr) {
    if (!meta_map_ || !expr) return "";

    auto* nm = meta_map_->get(expr);
    if (!nm || !nm->soa_transformed) return "";

    // For a MemberExpr on an IndexExpr, we need to:
    // 1. Get the index value from the IndexExpr
    // 2. Access the SoA field array at that index
    if (auto* member = dyn_cast<MemberExpr>(expr)) {
        if (auto* index = dyn_cast<IndexExpr>(member->object())) {
            // Emit the index value
            std::string idx_val = emitExpr(index->index());

            // Look up the field array name from the struct metadata
            std::string field_array_name;
            if (index->object()->hasType()) {
                TypeId obj_type = index->object()->getType();
                if (obj_type && isa<StructType>(obj_type)) {
                    auto& st = cast<StructType>(obj_type);
                    field_array_name = sanitizeName(st.name()) + "_" + member->field();
                }
            }
            if (field_array_name.empty()) {
                field_array_name = sanitizeName(member->field());
            }

            // Emit the SoA array access: @structname_fieldname[idx]
            std::string soa_array = "@" + field_array_name;
            std::string gep = nextReg();

            // Get the element type from the expression's type
            std::string ll = llvmType(expr->getType());

            body_ss_ << "  " << gep << " = getelementptr " << ll
                     << ", ptr " << soa_array << ", i64 " << idx_val << "\n";

            if (isAggregateType(expr->getType())) return gep;

            std::string loaded = nextReg();
            body_ss_ << "  " << loaded << " = load " << ll << ", ptr " << gep << "\n";
            return loaded;
        }
    }

    // Fallback: if we can't rewrite, return empty and let normal emission happen
    return "";
}

// ============================================================================
// hasSpeculativeAssumption – check if a node has a speculative assumption
// ============================================================================
bool IRGenerator::hasSpeculativeAssumption(const ASTNode* node) const {
    if (!meta_map_ || !node) return false;
    auto* nm = meta_map_->get(node);
    return nm && nm->has_speculative_assumption;
}

// ============================================================================
// emitDeoptGuardForBranch – emit a deoptimization guard for a branch
//
// When the speculative optimizer has determined that one branch is almost
// never taken (BranchNeverTaken assumption), we emit the likely path as
// the fast path and route the unlikely path to a deoptimization block
// that calls tether_deopt() and is marked unreachable.
//
// Pattern:
//   br i1 %cond, label %likely, label %deopt.N
// deopt.N:
//   call void @tether_deopt(i64 N, ptr null)
//   unreachable
// likely:
//   ; ... fast-path code ...
// ============================================================================
std::string IRGenerator::emitDeoptGuardForBranch(const ASTNode* node,
                                                   const std::string& cond,
                                                   const std::string& likely_label,
                                                   const std::string& unlikely_label) {
    if (!meta_map_ || !node) return unlikely_label;

    auto* nm = meta_map_->get(node);
    if (!nm || !nm->has_speculative_assumption) return unlikely_label;

    // Only emit deopt guard for BranchNeverTaken assumptions
    if (nm->speculative_kind != AssumptionKind::BranchNeverTaken) return unlikely_label;

    int deopt_id = deopt_counter_++;
    std::string deopt_label = nextLabel("deopt");

    // Emit the branch: likely path is normal, unlikely path goes to deopt
    body_ss_ << "  br i1 " << cond << ", label %" << likely_label
             << ", label %" << deopt_label << "\n";
    setTerminated(false);

    // Emit the deoptimization block — PASS the same label we just branched to.
    // Without this, emitDeoptBlock would call nextLabel() again, generating a
    // different label and leaving the branch above pointing at a non-existent
    // block (the "empty label bug").
    emitDeoptBlock(deopt_id, deopt_label);

    // Return the likely label (caller should emit fast-path code there)
    return likely_label;
}

// ============================================================================
// emitDeoptBlock – emit a deoptimization block
//
// Emits a basic block that calls the runtime deopt handler and is marked
// unreachable. LLVM will place this block out-of-line (cold section).
//
// If `label` is provided (non-empty), uses that label instead of generating
// a new one. This is critical: callers may have already emitted a branch
// instruction targeting a specific label, so we must reuse that exact label
// rather than calling nextLabel() again (which would produce a different label
// and leave the branch pointing at a non-existent block — the empty label bug).
// ============================================================================
void IRGenerator::emitDeoptBlock(int deopt_id, const std::string& label) {
    std::string deopt_label = label.empty() ? nextLabel("deopt") : label;
    emitBlockLabel(deopt_label);

    // Declare the deopt runtime function
    needed_runtime_.insert("tether_deopt");

    body_ss_ << "  ; [speculative] deoptimization guard, deopt_id=" << deopt_id << "\n";
    body_ss_ << "  call void @tether_deopt(i64 " << deopt_id << ", ptr null)\n";
    body_ss_ << "  unreachable\n";
    setTerminated(true);
}

} // namespace tether
