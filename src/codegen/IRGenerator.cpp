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
                         ASTAnnotationMap* annotations)
    : program_(program)
    , type_table_(type_table)
    , annotations_(annotations)
{}

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
            return true;
        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            return sp.smartPointerKind() != SmartPointerKind::Box;
        }
        case TypeKind::Error:
            return true;
        case TypeKind::Poison:
            return false;
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
            uint64_t vs = typeSizeBytes(err.successType());
            uint64_t va = typeAlignmentBytes(err.successType());
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
        default:                     return 8;
    }
}

// ============================================================================
// llvmType
// ============================================================================
std::string IRGenerator::llvmType(TypeId type) const {
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
            std::string vt = llvmType(err.successType());
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

    return "ptr";
}

// ============================================================================
// llvmReturnType
//   Returns the LLVM IR return type for a function.
//   Zig-style: error-returning functions return just the success type;
//   the error code is communicated via an out-parameter (ptr %err_slot).
// ============================================================================
std::string IRGenerator::llvmReturnType(TypeId type, bool can_error) const {
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
std::string IRGenerator::llvmParamType(TypeId type) const {
    return llvmType(type);
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
                body = "{ ";
                for (size_t i = 0; i < st.fields().size(); ++i) {
                    if (i > 0) body += ", ";
                    body += llvmType(st.fields()[i].type);
                }
                body += " }";
            }
            needed_types_[key] = {ln, body};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Slice: {
            auto& sl = cast<SliceType>(type);
            collectNeededTypes(sl.element());
            std::string ln = "%slice." + sanitizeName(sl.element()->toString());
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::SmartPointer: {
            auto& sp = cast<SmartPointerType>(type);
            collectNeededTypes(sp.pointee());
            if (sp.smartPointerKind() == SmartPointerKind::Box) break;
            std::string prefix = sp.smartPointerKind() == SmartPointerKind::Rc ? "rc" : "arc";
            std::string ln = "%" + prefix + "." + sanitizeName(sp.pointee()->toString());
            needed_types_[key] = {ln, "{ ptr, i64 }"};
            type_emit_order_.push_back(key);
            break;
        }
        case TypeKind::Error: {
            auto& err = cast<ErrorType>(type);
            collectNeededTypes(err.successType());
            std::string vt = llvmType(err.successType());
            std::string body = (vt == "void") ? "{ i1 }" : "{ " + vt + ", i1 }";
            std::string ln = "%error." + sanitizeName(err.successType()->toString());
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
// generate – main entry point
// ============================================================================
std::string IRGenerator::generate() {
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
        }
    }

    // 2. Emit module header
    emitModuleHeader();

    // 3. Emit struct/composite type definitions
    emitTypeDefinitions();

    // 4. Emit string constants
    emitStringConstants();

    // 5. Emit runtime declarations
    emitRuntimeDecls();

    // 6. Emit each top-level declaration
    for (const auto& tl : program_) {
        emitTopLevel(tl.get());
    }

    // 7. Emit metadata
    emitMetadata();

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
        default: break;
    }
}

// ============================================================================
// emitStructDecl – type definitions already emitted by emitTypeDefinitions
// ============================================================================
void IRGenerator::emitStructDecl(StructDecl* sd) {
    // Type defs are handled in emitTypeDefinitions(), but if the struct has
    // a non-default alignment, we need to re-emit with the alignment info.
    if (sd && sd->alignment() > 0) {
        // BUG FIX: Build the correct key matching needed_types_ format.
        // The key is type->toString() (e.g. "struct:Name{...}"), not sd->name().
        // Since we need the type from the type table, build the body directly.
        std::string body_str;
        std::string ll_name = "%struct." + sanitizeName(sd->name());
        for (size_t i = 0; i < sd->fieldCount(); ++i) {
            if (i > 0) body_str += ", ";
            body_str += llvmType(sd->fields()[i].type);
        }
        // Remove old entry with the non-aligned key if it exists, to avoid
        // duplicate type definitions. Search by ll_name prefix.
        for (auto it = needed_types_.begin(); it != needed_types_.end(); ++it) {
            if (it->second.llvm_name == ll_name) {
                needed_types_.erase(it);
                break;
            }
        }
        // Also remove from emit order
        type_emit_order_.erase(
            std::remove_if(type_emit_order_.begin(), type_emit_order_.end(),
                [&](const std::string& k) {
                    auto it = needed_types_.find(k);
                    return it != needed_types_.end() && it->second.llvm_name == ll_name;
                }),
            type_emit_order_.end());

        module_out_ << ll_name << " = type <{ " << body_str << " }> ; align " << sd->alignment() << "\n";
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
    //   otherwise → internal (allows aggressive inlining by LLVM)
    std::string linkage;
    std::string inlining_attr;
    bool is_small_fn = fn->body() && fn->body()->stmts().size() <= 3;
    if (fn->name() == "main") {
        linkage = "";  // external: define dso_local
    } else if (fn->isPure()) {
        // Pure functions have no side effects → always profitable to inline.
        // Use alwaysinline for small pure fns, inlinehint for larger ones.
        linkage = "internal ";  // internal > linkonce_odr for inlining
        inlining_attr = is_small_fn ? " alwaysinline" : " inlinehint";
    } else if (is_small_fn) {
        linkage = "internal ";
        inlining_attr = " alwaysinline";  // tiny functions → always inline
    } else {
        linkage = "internal ";  // internal linkage enables aggressive inlining
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
                                       isa<SliceType>(p.type) ||
                                       isa<AllocatorType>(p.type));

        // noalias: &mut T and &T params don't alias other pointers
        // (borrow checker guarantees this for &mut; &T is also safe because
        //  shared borrows can't mutate through the pointer)
        if (p.type) {
            if (isa<MutReferenceType>(p.type)) {
                sig += " noalias";
            } else if (isa<ReferenceType>(p.type)) {
                sig += " noalias";  // shared borrows also don't alias mutable ones
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
        if (is_ptr_type) {
            uint64_t deref_bytes = 0;
            if (isa<ReferenceType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<ReferenceType>(p.type).referent());
            } else if (isa<MutReferenceType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<MutReferenceType>(p.type).referent());
            } else if (isa<PointerType>(p.type)) {
                deref_bytes = typeSizeBytes(cast<PointerType>(p.type).pointee());
            } else if (isa<SliceType>(p.type)) {
                deref_bytes = 16;  // { ptr, i64 } = 16 bytes
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

    // Inlining attribute (alwaysinline / inlinehint)
    sig += inlining_attr;

    // --- Pre-LLVM optimization: opaque barrier function attributes ---
    // If the function takes or returns opaque types, add the
    // inaccessiblememonly attribute to prevent LLVM from making
    // aliasing assumptions across this function's calls.
    if (hasOpaqueBarrierAnnotation(fn)) {
        if (!fn->isPure()) {  // Pure functions already have memory(none)
            sig += " inaccessiblememonly";
        }
    }

    // Metadata for directives
    std::string metadata_suffix;
    for (const auto& d : fn->directives()) {
        std::string key;
        switch (d) {
            case CompilerDirective::Superoptimize: key = "jules.superoptimize"; break;
            case CompilerDirective::Polly:         key = "jules.polly"; break;
            case CompilerDirective::Simd:          key = "jules.simd"; break;
        }
        int id = getMetadataId(key);
        metadata_suffix += " !" + std::to_string(id);
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
                                           isa<SliceType>(p.type) ||
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
                std::string zero = nextReg();
                alloca_ss_ << "  " << zero << " = alloca " << ll_val << "\n";
                needed_runtime_.insert("memset");
                body_ss_ << "  call void @llvm.memset.p0.i64(ptr " << zero
                         << ", i8 0, i64 " << typeSizeBytes(current_return_type_)
                         << ", i1 false)\n";
                std::string loaded = nextReg();
                body_ss_ << "  " << loaded << " = load " << ll_val << ", ptr " << zero << "\n";
                body_ss_ << "  ret " << ll_val << " " << loaded << "\n";
            } else {
                body_ss_ << "  ret " << ll_val << " 0\n";
            }
        } else if (current_return_type_ && current_return_type_->isVoid()) {
            body_ss_ << "  ret void\n";
        } else {
            body_ss_ << "  ret " << llvmType(current_return_type_) << " 0\n";
        }
        setTerminated(true);
    }

    // Write the complete function
    module_out_ << sig << metadata_suffix << " {\n";
    module_out_ << "entry:\n";
    module_out_ << alloca_ss_.str();
    module_out_ << body_ss_.str();
    module_out_ << "}\n\n";
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

            // Snapshot SSA values before branching (for phi node emission)
            SSASnapshot pre_branch_snap = takeSnapshot();
            // Remember the label of the block that contains the branch instruction
            std::string branch_block = current_block_label_;

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
            std::string then_exit_block = current_block_label_;
            if (!isTerminated()) body_ss_ << "  br label %" << merge_l << "\n";
            setTerminated(false);

            SSASnapshot else_snap;
            std::string else_exit_block;
            if (is.hasElse()) {
                emitBlockLabel(else_l);
                // Restore SSA values to pre-branch state before emitting else
                for (auto& [name, val] : pre_branch_snap.values) {
                    updateVarValue(name, val);
                }
                emitBlockStmt(is.elseBlock());
                else_snap = takeSnapshot();
                else_exit_block = current_block_label_;
                if (!isTerminated()) body_ss_ << "  br label %" << merge_l << "\n";
                setTerminated(false);
            } else {
                // No else: the "else" path is the fallthrough from branch_block
                else_snap = pre_branch_snap;
                else_exit_block = branch_block;
            }

            emitBlockLabel(merge_l);
            // Emit phi nodes for variables that differ between then and else paths
            emitPhiNodes(then_snap, then_exit_block, else_snap, else_exit_block);
            setTerminated(false);
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
                // @simd loop vectorization metadata
                if (current_fn_has_simd_) {
                    int loop_md_id = emitSimdLoopMetadata();
                    body_ss_ << "  br label %" << cond_l << ", !llvm.loop !" << loop_md_id << "\n";
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

            if (current_can_error_) {
                // Zig-style: return just the success value directly.
                // No struct wrapping needed — the err_slot stays at 0 (no error),
                // as initialized by the caller.  Success value returns in a register!
                std::string vt = llvmType(current_return_type_);
                if (rs.hasValue()) {
                    std::string val = emitExpr(rs.value());
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
                        body_ss_ << "  ret " << vt << " 0\n";
                    }
                }
            } else {
                if (rs.hasValue()) {
                    std::string val = emitExpr(rs.value());
                    std::string ll = llvmType(current_return_type_);
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

    switch (expr->getKind()) {

    // ========================================================================
    // Literals
    // ========================================================================
    case NodeKind::IntLiteral: {
        auto& lit = cast<IntLiteral>(*expr);
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

            // Compute the new value
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
        // For comparison operators, the operand type differs from the result type (i1).
        // icmp/fcmp operate on the operand type, not the result bool type.
        TypeId operand_type = bin.left()->getType();
        std::string ll = llvmType(operand_type);
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
        // If this call has an OpaqueBarrier annotation, emit a fence
        // before and after the call to prevent reordering.
        bool call_has_opaque_barrier = annotations_ &&
            annotations_->hasAnnotation(&call, ASTAnnotationKind::OpaqueBarrier);

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
            caller_err_slot_alloc = nextReg();
            alloca_ss_ << "  " << caller_err_slot_alloc << " = alloca i32\n";
            body_ss_ << "  store i32 0, ptr " << caller_err_slot_alloc << "\n";
        }
        std::string ret_ll = llvmType(actual_ret);

        std::string result;

        // --- Pre-LLVM optimization: opaque barrier fence (before call) ---
        if (call_has_opaque_barrier) {
            body_ss_ << "  fence acquire ; [opaque barrier] before FFI call\n";
        }

        // Emit the call — for void returns, don't assign to a register
        if (ret_ll == "void") {
            body_ss_ << "  call void " << callee << "(";
        } else {
            result = nextReg();
            body_ss_ << "  " << result << " = call " << ret_ll << " " << callee << "(";
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
                std::string ea = nextReg();
                alloca_ss_ << "  " << ea << " = alloca " << vt << "\n";
                body_ss_ << "  store " << vt << " " << result << ", ptr " << ea << "\n";
                return ea;
            } else {
                // Scalar: store result in alloca, return pointer
                // (try-expression will load from this alloca)
                std::string ea = nextReg();
                alloca_ss_ << "  " << ea << " = alloca " << vt << "\n";
                body_ss_ << "  store " << vt << " " << result << ", ptr " << ea << "\n";
                return ea;
            }
        }

        // Aggregate return: store in alloca
        if (isAggregateType(actual_ret)) {
            std::string ea = nextReg();
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
                    body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep << "\n";
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
            body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep << "\n";
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
                body_ss_ << "  " << loaded << " = load " << llvmType(ft) << ", ptr " << gep << "\n";
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
        body_ss_ << "  " << reg << " = load " << ll << ", ptr " << ptr_val << "\n";
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
    // Select expression (branchless conditional)
    // ========================================================================
    case NodeKind::SelectExpr: {
        auto& sel = cast<SelectExpr>(*expr);
        std::string cond = emitExpr(sel.condition());
        if (sel.condition()->getType() && !sel.condition()->getType()->isBool()) {
            std::string c = nextReg();
            body_ss_ << "  " << c << " = icmp ne " << llvmType(sel.condition()->getType())
                     << " " << cond << ", 0\n";
            cond = c;
        }
        std::string tv = emitExpr(sel.trueExpr());
        std::string fv = emitExpr(sel.falseExpr());
        std::string ll = llvmType(expr->getType());
        std::string reg = nextReg();
        body_ss_ << "  " << reg << " = select i1 " << cond << ", " << ll << " " << tv
                 << ", " << ll << " " << fv << "\n";
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
            std::string aname = nextReg();
            alloca_ss_ << "  " << aname << " = alloca " << ll << "\n";
            return aname;
        }

        std::string ll = llvmType(ty);
        std::string aname = nextReg();
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
                    body_ss_ << "  ret " << crt << " 0\n";
                }
            } else {
                // Non-error function returning early from try — return zero
                body_ss_ << "  ret " << llvmType(current_return_type_) << " 0\n";
            }
        }
        setTerminated(true);

        body_ss_ << ok_label << ":\n";
        setTerminated(false);
        if (vt == "void") return "";  // no value to extract
        return value_reg;
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
        std::string fallback = nextReg();
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
                        std::string fallback = nextReg();
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
                    std::string fallback = nextReg();
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
                std::string fallback = nextReg();
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
    // If the EscapeAnalysis pass annotated this Box as non-escaping,
    // use alloca instead of malloc. This eliminates heap allocation
    // overhead entirely for short-lived Boxes. LLVM CANNOT do this
    // because malloc/free are opaque external calls.
    if (annotations_ && annotations_->hasAnnotation(value, ASTAnnotationKind::StackAllocated)) {
        // Stack-allocated Box: alloca + store (no malloc/free)
        std::string stack_ptr = nextReg();
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
    std::string ea = nextReg();
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
    std::string na = nextReg();
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
    std::string ea = nextReg();
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
    std::string na = nextReg();
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
    std::string vt = llvmType(err.successType());

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
            body_ss_ << "  ret " << crt << " 0\n";
        }
    } else {
        // Non-error-returning function — return zero of the actual return type
        if (current_return_type_ && !current_return_type_->isVoid()) {
            body_ss_ << "  ret " << llvmType(current_return_type_) << " 0\n";
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
// ColdPath annotation from the ErrorPathSeparator pass, this method
// creates LLVM branch weight metadata that tells the optimizer the
// likely execution direction.
//
// The annotation detail string format is:
//   "try_error_branch:COLD:HOT"   (e.g., "try_error_branch:1:1000")
//   "catch_block"
//   "errdefer_cleanup"
//   "coalesced_defer:N"
//   "error_function:NAME"
//
// Returns a string like ", !prof !5" to append to a branch instruction,
// or empty string if no annotation exists.
// ============================================================================
std::string IRGenerator::emitColdPathMetadata(const ASTNode* node) {
    if (!annotations_ || !node) return "";

    auto* anns = annotations_->getAnnotations(node);
    if (!anns) return "";

    for (const auto& ann : *anns) {
        if (ann.kind != ASTAnnotationKind::ColdPath) continue;

        // Parse the detail to extract branch weights
        uint32_t cold_w = 1;
        uint32_t hot_w = 1000;

        // Detail format: "try_error_branch:COLD:HOT"
        if (ann.detail.find("try_error_branch:") == 0) {
            // Parse "try_error_branch:1:1000"
            size_t first_colon = ann.detail.find(':', 17); // after "try_error_branch:"
            if (first_colon != std::string::npos) {
                try {
                    cold_w = static_cast<uint32_t>(std::stoul(ann.detail.substr(17, first_colon - 17)));
                    hot_w = static_cast<uint32_t>(std::stoul(ann.detail.substr(first_colon + 1)));
                } catch (...) {
                    cold_w = 1; hot_w = 1000;
                }
            }
        } else if (ann.detail == "catch_block" ||
                   ann.detail == "errdefer_cleanup" ||
                   ann.detail.find("coalesced_defer:") == 0) {
            cold_w = 1;
            hot_w = 2000;
        } else if (ann.detail.find("error_function:") == 0) {
            cold_w = 1;
            hot_w = 500;
        } else {
            // Generic cold path annotation
            cold_w = 1;
            hot_w = 1000;
        }

        int prof_id = getBranchWeightMetadataId(cold_w, hot_w);
        return ", !prof !" + std::to_string(prof_id);
    }

    return "";
}

// ============================================================================
// emitPrefetchIfAnnotated – emit prefetch intrinsics for aligned loop access
//
// When a WhileStmt has a PrefetchSite annotation from the PrefetchInserter
// pass, this method emits:
//   call void @llvm.prefetch(ptr %next_addr, i32 0, i32 3, i32 1)
//
// The annotation detail format is:
//   "align:ALIGNMENT:distance:DISTANCE"
//
// This is called at the top of the loop body, before any other statements.
// ============================================================================
void IRGenerator::emitPrefetchIfAnnotated(WhileStmt* loop) {
    if (!annotations_ || !loop) return;

    auto* anns = annotations_->getAnnotations(loop);
    if (!anns) return;

    for (const auto& ann : *anns) {
        if (ann.kind != ASTAnnotationKind::PrefetchSite) continue;

        // Parse detail: "align:64:distance:8"
        uint32_t align = 64;
        int distance = 8;

        if (ann.detail.find("align:") == 0) {
            size_t pos = 6;
            size_t colon = ann.detail.find(':', pos);
            if (colon != std::string::npos) {
                try {
                    align = static_cast<uint32_t>(std::stoul(ann.detail.substr(pos, colon - pos)));
                } catch (...) {}
            }
            // Look for ":distance:N"
            size_t dist_pos = ann.detail.find(":distance:");
            if (dist_pos != std::string::npos) {
                try {
                    distance = std::stoi(ann.detail.substr(dist_pos + 10));
                } catch (...) {}
            }
        }

        // Emit the prefetch intrinsic declaration
        needed_runtime_.insert("prefetch");

        // Emit a prefetch for the next iteration's data.
        // In practice, the IR generator would need to know the array base address
        // and the loop index to compute the prefetch address. Since we don't have
        // that information at the annotation level, we emit a placeholder comment
        // and the prefetch call structure. The LLVM optimizer will handle the rest
        // when combined with the alignment metadata on the data.
        body_ss_ << "  ; [prefetch] align=" << align << " distance=" << distance
                 << " (PrefetchSite annotation)\n";

        // Emit the llvm.prefetch intrinsic - this tells LLVM to prefetch
        // the cache line containing the address. The actual address computation
        // is done by the loop's GEP instructions which LLVM can analyze.
        // We emit a generic prefetch at the loop body entry point.
        // The 4th arg: 1 = data prefetch (not instruction)
        body_ss_ << "  call void @llvm.prefetch.p0(ptr null, i32 0, i32 3, i32 1)"
                 << " ; prefetch hint (alignment-guided)\n";
        break;  // Only process the first PrefetchSite annotation
    }
}

// ============================================================================
// emitYieldCheckIfAnnotated – emit cooperative yield checks in loops
//
// When a WhileStmt has a YieldPoint annotation from the YieldPointInserter
// pass, this method emits a counter-based yield check:
//
//   %counter = alloca i64
//   store i64 0, ptr %counter
//   ; ... at top of loop body:
//   %val = load i64, ptr %counter
//   %inc = add i64 %val, 1
//   store i64 %inc, ptr %counter
//   %mod = srem i64 %inc, INTERVAL
//   %should = icmp eq i64 %mod, 0
//   br i1 %should, label %yield.check, label %yield.skip
// yield.check:
//   call void @tether_yield(i64 0)
//   br label %yield.skip
// yield.skip:
//
// The annotation detail format is:
//   "loop_yield:interval:N"
// ============================================================================
void IRGenerator::emitYieldCheckIfAnnotated(WhileStmt* loop) {
    if (!annotations_ || !loop) return;

    auto* anns = annotations_->getAnnotations(loop);
    if (!anns) return;

    for (const auto& ann : *anns) {
        if (ann.kind != ASTAnnotationKind::YieldPoint) continue;

        // Parse interval from detail: "loop_yield:interval:256"
        int interval = 256;
        if (ann.detail.find("loop_yield:interval:") == 0) {
            try {
                interval = std::stoi(ann.detail.substr(20));
            } catch (...) {
                interval = 256;
            }
        }

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
        break;  // Only process the first YieldPoint annotation
    }
}

// ============================================================================
// hasOpaqueBarrierAnnotation – check if a function has an opaque barrier
// ============================================================================
bool IRGenerator::hasOpaqueBarrierAnnotation(FnDecl* fn) const {
    if (!annotations_ || !fn) return false;
    return annotations_->hasAnnotation(fn, ASTAnnotationKind::OpaqueBarrier);
}

// ============================================================================
// emitInlineAllocatorIfAnnotated – inline arena bump allocation
//
// When a CallExpr has an AllocatorInlined annotation from the
// AllocatorLowerer pass, emit inline bump allocation code:
//
//   %old = load ptr, ptr %arena.offset
//   %aligned = add ptr %old, ALIGN_UP(size, 8)
//   store ptr %aligned, ptr %arena.offset
//   ; result = %old (pointer to newly allocated memory)
//
// The annotation detail format is:
//   "arena_inline:SIZE"
// ============================================================================
std::string IRGenerator::emitInlineAllocatorIfAnnotated(CallExpr* call, TypeId /*ret_type*/) {
    if (!annotations_ || !call) return "";

    auto* anns = annotations_->getAnnotations(call);
    if (!anns) return "";

    for (const auto& ann : *anns) {
        if (ann.kind != ASTAnnotationKind::AllocatorInlined) continue;

        // Parse size from detail: "arena_inline:16"
        uint64_t alloc_size = 8;
        if (ann.detail.find("arena_inline:") == 0) {
            try {
                alloc_size = std::stoull(ann.detail.substr(13));
            } catch (...) {
                alloc_size = 8;
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

    return "";
}

// ============================================================================
// hasSoAAnnotation – check if an expression has a SoA transform annotation
// ============================================================================
bool IRGenerator::hasSoAAnnotation(Expr* expr) const {
    if (!annotations_ || !expr) return false;
    return annotations_->hasAnnotation(expr, ASTAnnotationKind::SoATransformed);
}

// ============================================================================
// emitSoAAccessIfAnnotated – emit SoA-transformed array access
//
// When a MemberExpr has a SoATransformed annotation from the AoS→SoA pass,
// rewrite the access pattern:
//
// Original: data[i].field  →  load %struct.Vec3, ptr %gep
// SoA:      data_field[i]  →  load float, ptr %gep
//
// The annotation detail format is:
//   "STRUCT_NAME:FIELD_ARRAY_NAME" (e.g., "Vec3:data_x")
// ============================================================================
std::string IRGenerator::emitSoAAccessIfAnnotated(Expr* expr) {
    if (!annotations_ || !expr) return "";

    auto* anns = annotations_->getAnnotations(expr);
    if (!anns) return "";

    for (const auto& ann : *anns) {
        if (ann.kind != ASTAnnotationKind::SoATransformed) continue;

        // Parse detail: "Vec3:data_x"
        std::string field_array_name;
        size_t colon_pos = ann.detail.find(':');
        if (colon_pos != std::string::npos) {
            field_array_name = ann.detail.substr(colon_pos + 1);
        }

        if (field_array_name.empty()) continue;

        // For a MemberExpr on an IndexExpr, we need to:
        // 1. Get the index value from the IndexExpr
        // 2. Access the SoA field array at that index
        if (auto* member = dyn_cast<MemberExpr>(expr)) {
            if (auto* index = dyn_cast<IndexExpr>(member->object())) {
                // Emit the index value
                std::string idx_val = emitExpr(index->index());

                // Emit the SoA array access: @structname_fieldname[idx]
                std::string soa_array = "@" + sanitizeName(field_array_name);
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
        break;
    }

    return "";
}

} // namespace tether
