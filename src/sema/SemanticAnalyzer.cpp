#include "sema/SemanticAnalyzer.h"

#include <sstream>
#include <algorithm>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
SemanticAnalyzer::SemanticAnalyzer(TypeTable& type_table)
    : type_table_(type_table)
    , symtab_()
    , reporter_(&default_reporter_)
    , current_fn_(nullptr)
    , in_pure_fn_(false)
    , error_prop_depth_(0)
    , in_noalloc_fn_(false)
{}

// ============================================================================
// Main entry point
// ============================================================================
void SemanticAnalyzer::analyze(
    Program& program,
    const std::unordered_map<const ASTNode*, std::string>& type_annotations,
    const std::unordered_map<std::string, std::string>& param_type_annotations)
{
    type_annotations_ = type_annotations;
    param_type_annotations_ = param_type_annotations;

    // Phase 1: Register all top-level declarations (forward references)
    registerTopLevelDecls(program);

    // Phase 2: Resolve all unresolved type annotations from the parser
    resolveTypeAnnotations(type_annotations);

    // Phase 3: Analyze each top-level declaration in depth
    for (auto& decl : program) {
        if (!decl) continue;

        switch (decl->getKind()) {
            case NodeKind::FnDecl:
                analyzeFnDecl(static_cast<FnDecl&>(*decl));
                break;
            case NodeKind::StructDecl:
                analyzeStructDecl(static_cast<StructDecl&>(*decl));
                break;
            case NodeKind::EnumDecl:
                analyzeEnumDecl(static_cast<EnumDecl&>(*decl));
                break;
            case NodeKind::ImportDecl:
                analyzeImportDecl(static_cast<ImportDecl&>(*decl));
                break;
            case NodeKind::TraitDecl:
                analyzeTraitDecl(static_cast<TraitDecl&>(*decl));
                break;
            case NodeKind::ImplDecl:
                analyzeImplDecl(static_cast<ImplDecl&>(*decl));
                break;
            case NodeKind::ModuleDecl:
                analyzeModuleDecl(static_cast<ModuleDecl&>(*decl));
                break;
            case NodeKind::UseDecl:
                analyzeUseDecl(static_cast<UseDecl&>(*decl));
                break;
            default:
                break;
        }
    }
}

// ============================================================================
// Diagnostic helpers (forward to ErrorReporter)
// ============================================================================
SourceSpan SemanticAnalyzer::locToSpan(const SourceLocation& loc) {
    return SourceSpan(loc.line, loc.col, loc.line, loc.col, loc.filename);
}

void SemanticAnalyzer::emitError(const SourceLocation& loc, const std::string& msg) {
    reporter_->error(locToSpan(loc), msg);
}

void SemanticAnalyzer::emitWarning(const SourceLocation& loc, const std::string& msg) {
    reporter_->warning(locToSpan(loc), msg);
}

void SemanticAnalyzer::emitNote(const SourceLocation& loc, const std::string& msg) {
    reporter_->note(locToSpan(loc), msg);
}

bool SemanticAnalyzer::hasErrors() const {
    return reporter_->hasErrors();
}

// ============================================================================
// Type resolution
// ============================================================================
void SemanticAnalyzer::resolveTypeAnnotations(
    const std::unordered_map<const ASTNode*, std::string>& annotations)
{
    for (const auto& [node, type_name] : annotations) {
        if (resolved_nodes_.count(node)) continue;

        TypeId resolved = resolveTypeName(type_name, node->sourceLoc());
        if (resolved.isNull()) {
            emitError(node->sourceLoc(),
                      "unknown type name '" + type_name + "'");
            continue;
        }

        // Apply the resolved type based on the node kind
        switch (node->getKind()) {
            case NodeKind::VarDeclStmt:
            case NodeKind::ValDeclStmt: {
                // VarDeclStmt and ValDeclStmt type fields are set during
                // analysis (analyzeVarDeclStmt / analyzeValDeclStmt),
                // which can infer types from initializers. The annotation
                // map entry is consumed here for reference.
                break;
            }
            case NodeKind::FnDecl: {
                auto& fn = static_cast<FnDecl&>(*const_cast<ASTNode*>(node));
                // Resolve return type if it was an unresolved annotation
                if (fn.returnType().isNull()) {
                    // Can't modify returnType directly; it was set during parsing.
                    // The parser should have created a placeholder.
                    // For now, if the parser left it unresolved, we note it.
                }
                // Resolve parameter types
                for (auto& param : const_cast<std::vector<FnParam>&>(fn.params())) {
                    if (param.type.isNull()) {
                        param.type = resolved;
                    }
                }
                break;
            }
            default:
                // For expression nodes, set the type directly
                if (auto* expr = dyn_cast<Expr>(const_cast<ASTNode*>(node))) {
                    if (!expr->hasType()) {
                        expr->setType(resolved);
                    }
                }
                break;
        }

        resolved_nodes_.insert(node);
    }
}

TypeId SemanticAnalyzer::resolveTypeName(const std::string& name, const SourceLocation& /*loc*/) {
    // Trim whitespace from the name (token reconstruction may add spaces)
    std::string trimmed = name;
    while (!trimmed.empty() && trimmed.front() == ' ')
        trimmed = trimmed.substr(1);
    while (!trimmed.empty() && trimmed.back() == ' ')
        trimmed = trimmed.substr(0, trimmed.size() - 1);

    // Check primitives first
    if (trimmed == "u8")    return type_table_.getU8();
    if (trimmed == "u16")   return type_table_.getU16();
    if (trimmed == "u32")   return type_table_.getU32();
    if (trimmed == "u64")   return type_table_.getU64();
    if (trimmed == "usize") return type_table_.getUSize();
    if (trimmed == "i8")    return type_table_.getI8();
    if (trimmed == "i16")   return type_table_.getI16();
    if (trimmed == "i32")   return type_table_.getI32();
    if (trimmed == "i64")   return type_table_.getI64();
    if (trimmed == "isize") return type_table_.getISize();
    if (trimmed == "f32")   return type_table_.getF32();
    if (trimmed == "f64")   return type_table_.getF64();
    if (trimmed == "bool")  return type_table_.getBool();
    if (trimmed == "void")  return type_table_.getVoid();

    // Check the type table for a canonical match
    auto found = type_table_.lookup(trimmed);
    if (found) return *found;

    // Check the type table aliases (e.g., "Vec2" -> struct:Vec2{...})
    auto alias = type_table_.lookupAlias(trimmed);
    if (alias) return *alias;

    // Check symbol table for struct/enum types
    Symbol* sym = symtab_.lookupGlobal(trimmed);
    if (sym && sym->isTypeSymbol()) {
        return sym->type();
    }

    return TypeId(); // null = unresolved
}

// ============================================================================
// reresolveType — recursively fix null inner types in compound types
// ============================================================================
bool SemanticAnalyzer::hasNullInnerType(TypeId type) const {
    if (type.isNull()) return true;

    if (isa<PointerType>(type)) {
        return cast<PointerType>(type).pointee().isNull();
    } else if (isa<ReferenceType>(type)) {
        return cast<ReferenceType>(type).referent().isNull();
    } else if (isa<MutReferenceType>(type)) {
        return cast<MutReferenceType>(type).referent().isNull();
    } else if (isa<SliceType>(type)) {
        return cast<SliceType>(type).element().isNull();
    } else if (isa<SmartPointerType>(type)) {
        return cast<SmartPointerType>(type).pointee().isNull();
    }
    return false;
}

TypeId SemanticAnalyzer::reresolveType(TypeId type) {
    if (type.isNull()) return type;
    if (!hasNullInnerType(type)) return type;

    if (isa<PointerType>(type)) {
        TypeId inner = cast<PointerType>(type).pointee();
        if (inner.isNull()) return type; // Can't resolve
        TypeId resolved_inner = reresolveType(inner);
        if (resolved_inner != inner) {
            return type_table_.getPointer(resolved_inner);
        }
    } else if (isa<ReferenceType>(type)) {
        TypeId inner = cast<ReferenceType>(type).referent();
        if (inner.isNull()) return type; // Can't resolve
        TypeId resolved_inner = reresolveType(inner);
        if (resolved_inner != inner) {
            return type_table_.getReference(resolved_inner);
        }
    } else if (isa<MutReferenceType>(type)) {
        TypeId inner = cast<MutReferenceType>(type).referent();
        if (inner.isNull()) {
            // The inner type is null — we can't re-resolve without knowing
            // the name. This type was created by the parser before the struct
            // was registered. We need to return it as-is and handle null
            // gracefully in analyzeMemberExpr.
            return type;
        }
        TypeId resolved_inner = reresolveType(inner);
        if (resolved_inner != inner) {
            return type_table_.getMutReference(resolved_inner);
        }
    } else if (isa<SliceType>(type)) {
        TypeId inner = cast<SliceType>(type).element();
        if (inner.isNull()) return type;
        TypeId resolved_inner = reresolveType(inner);
        if (resolved_inner != inner) {
            return type_table_.getSlice(resolved_inner);
        }
    } else if (isa<SmartPointerType>(type)) {
        TypeId inner = cast<SmartPointerType>(type).pointee();
        if (inner.isNull()) return type;
        TypeId resolved_inner = reresolveType(inner);
        if (resolved_inner != inner) {
            auto kind = cast<SmartPointerType>(type).smartPointerKind();
            return type_table_.getSmartPointer(resolved_inner, kind);
        }
    }
    return type;
}

// ============================================================================
// resolveCompoundTypeName — parse and resolve compound type strings
// ============================================================================
TypeId SemanticAnalyzer::resolveCompoundTypeName(const std::string& name, const SourceLocation& loc) {
    // Handle pointer types: *T or * T
    if (name.size() > 1 && name[0] == '*') {
        std::string inner_name = name.substr(1);
        // Trim leading whitespace
        while (!inner_name.empty() && inner_name[0] == ' ') {
            inner_name = inner_name.substr(1);
        }
        TypeId inner = resolveCompoundTypeName(inner_name, loc);
        if (!inner.isNull()) {
            return type_table_.getPointer(inner);
        }
        return TypeId();
    }

    // Handle mutable reference types: &mut T or & mut T
    if (name.size() > 1 && name[0] == '&') {
        size_t pos = 1;
        // Skip whitespace after &
        while (pos < name.size() && name[pos] == ' ') pos++;
        // Check for "mut"
        if (pos + 3 <= name.size() && name.substr(pos, 3) == "mut") {
            pos += 3;
            // Skip whitespace after mut
            while (pos < name.size() && name[pos] == ' ') pos++;
            std::string inner_name = name.substr(pos);
            TypeId inner = resolveCompoundTypeName(inner_name, loc);
            if (!inner.isNull()) {
                return type_table_.getMutReference(inner);
            }
            return TypeId();
        }
        // Just &T (immutable reference)
        std::string inner_name = name.substr(pos);
        TypeId inner = resolveCompoundTypeName(inner_name, loc);
        if (!inner.isNull()) {
            return type_table_.getReference(inner);
        }
        return TypeId();
    }

    // Handle slice types: []T or [ ] T
    if (name.size() > 1 && name[0] == '[') {
        size_t pos = 1;
        // Skip whitespace
        while (pos < name.size() && name[pos] == ' ') pos++;
        if (pos < name.size() && name[pos] == ']') {
            pos++;
            // Skip whitespace
            while (pos < name.size() && name[pos] == ' ') pos++;
            std::string inner_name = name.substr(pos);
            TypeId inner = resolveCompoundTypeName(inner_name, loc);
            if (!inner.isNull()) {
                return type_table_.getSlice(inner);
            }
            return TypeId();
        }
    }

    // Handle array types: [T; N] or [ T ; N ] — complex, try to parse
    if (name.size() > 1 && name[0] == '[') {
        // Find the ; separator
        size_t semicolon_pos = name.find(';');
        if (semicolon_pos != std::string::npos) {
            std::string element_type_str = name.substr(1, semicolon_pos - 1);
            // Trim whitespace
            while (!element_type_str.empty() && element_type_str.front() == ' ')
                element_type_str = element_type_str.substr(1);
            while (!element_type_str.empty() && element_type_str.back() == ' ')
                element_type_str = element_type_str.substr(0, element_type_str.size() - 1);

            TypeId element_type = resolveCompoundTypeName(element_type_str, loc);
            if (!element_type.isNull()) {
                // For now, return a slice type for arrays (arrays not fully supported)
                return type_table_.getSlice(element_type);
            }
        }
    }

    // Fall through to simple name resolution
    return resolveTypeName(name, loc);
}

// ============================================================================
// First pass: register all top-level declarations
// ============================================================================
void SemanticAnalyzer::registerTopLevelDecls(Program& program) {
    // BUG FIX: Two-pass registration to handle forward references.
    // Pass 1: Register all struct and enum NAMES as type aliases first,
    // so that forward references (struct Outer { inner: Inner }) work.
    // We do NOT insert them into the symbol table yet — that happens in Pass 2
    // once we have the complete type with all fields resolved.
    for (auto& decl : program) {
        if (!decl) continue;
        if (decl->getKind() == NodeKind::StructDecl) {
            auto& sd = static_cast<StructDecl&>(*decl);
            // Register just the name as a forward-declaration alias.
            // Use a minimal placeholder type that resolveTypeName can find.
            // The real type with fields will be registered in Pass 2.
            TypeId placeholder = type_table_.getStruct(sd.name(), {});
            type_table_.registerAlias(sd.name(), placeholder);
            // NOTE: Do NOT call symtab_.declareStruct here — that would
            // prevent Pass 2 from registering the real type.
        } else if (decl->getKind() == NodeKind::EnumDecl) {
            auto& ed = static_cast<EnumDecl&>(*decl);
            std::vector<EnumVariant> variants;
            for (const auto& v : ed.variants()) {
                variants.push_back({v.name, v.value});
            }
            TypeId enum_type = type_table_.getEnum(ed.name(), std::move(variants));
            type_table_.registerAlias(ed.name(), enum_type);
            // Enums have no forward-reference issue, register immediately
            symtab_.declareEnum(ed.name(), enum_type, ed.sourceLoc());
        }
    }

    // Pass 2: Now that all type names are registered as aliases, resolve
    // struct field types and function parameter types. Forward references
    // will resolve via the aliases registered in Pass 1.
    for (auto& decl : program) {
        if (!decl) continue;

        switch (decl->getKind()) {
            case NodeKind::FnDecl: {
                auto& fn = static_cast<FnDecl&>(*decl);
                // Build the function type, resolving any null param types
                std::vector<FnParam> params = fn.params();
                // Resolve null parameter types using param_type_annotations_
                for (size_t i = 0; i < params.size(); i++) {
                    // Step 1: If type is completely null, try annotation resolution
                    if (params[i].type.isNull()) {
                        // Try compound type annotation first
                        std::string full_key = fn.name() + ":" + std::to_string(i) + "_full";
                        auto it_full = param_type_annotations_.find(full_key);
                        if (it_full != param_type_annotations_.end()) {
                            params[i].type = resolveCompoundTypeName(it_full->second, fn.sourceLoc());
                        }
                        // Fall back to simple annotation
                        if (params[i].type.isNull()) {
                            std::string key = fn.name() + ":" + std::to_string(i);
                            auto it = param_type_annotations_.find(key);
                            if (it != param_type_annotations_.end()) {
                                params[i].type = resolveTypeName(it->second, fn.sourceLoc());
                            }
                        }
                        // Also try the unresolved_type_name from the FnParam
                        if (params[i].type.isNull() && !params[i].unresolved_type_name.empty()) {
                            params[i].type = resolveCompoundTypeName(params[i].unresolved_type_name, fn.sourceLoc());
                        }
                        // If still null, use a placeholder that will be fixed later
                        if (params[i].type.isNull()) {
                            params[i].type = type_table_.getU8();
                        }
                    }
                    // Step 2: If type is a compound type with null inner, re-resolve
                    if (hasNullInnerType(params[i].type)) {
                        // Try compound type annotation first
                        std::string full_key = fn.name() + ":" + std::to_string(i) + "_full";
                        auto it_full = param_type_annotations_.find(full_key);
                        if (it_full != param_type_annotations_.end()) {
                            TypeId resolved = resolveCompoundTypeName(it_full->second, fn.sourceLoc());
                            if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                                params[i].type = resolved;
                            }
                        }
                        // Try unresolved_type_name from the FnParam
                        if (hasNullInnerType(params[i].type) && !params[i].unresolved_type_name.empty()) {
                            TypeId resolved = resolveCompoundTypeName(params[i].unresolved_type_name, fn.sourceLoc());
                            if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                                params[i].type = resolved;
                            }
                        }
                        // Try re-resolving inner types
                        if (hasNullInnerType(params[i].type)) {
                            TypeId resolved = reresolveType(params[i].type);
                            if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                                params[i].type = resolved;
                            }
                        }
                        // If still has null inner, use placeholder
                        if (hasNullInnerType(params[i].type)) {
                            params[i].type = type_table_.getU8();
                        }
                    }
                }
                TypeId ret_type = fn.returnType();
                // Resolve null return type using stored annotation
                if (ret_type.isNull()) {
                    if (!fn.unresolved_return_type_name.empty()) {
                        ret_type = resolveCompoundTypeName(fn.unresolved_return_type_name, fn.sourceLoc());
                    }
                    if (ret_type.isNull()) {
                        ret_type = type_table_.getVoid();
                    }
                }
                // Also resolve compound return types with null inner
                if (hasNullInnerType(ret_type) && !fn.unresolved_return_type_name.empty()) {
                    TypeId resolved = resolveCompoundTypeName(fn.unresolved_return_type_name, fn.sourceLoc());
                    if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                        ret_type = resolved;
                    }
                }
                bool is_pure = fn.isPure();
                TypeId err_type = fn.errorType();

                TypeId fn_type = type_table_.getFn(
                    std::move(params), ret_type, is_pure, err_type);

                Symbol* sym = symtab_.declareFn(fn.name(), fn_type, fn.sourceLoc());
                if (!sym) {
                    emitError(fn.sourceLoc(),
                              "duplicate function declaration '" + fn.name() + "'");
                }

                if (is_pure) {
                    pure_functions_.insert(fn.name());
                }

                function_types_[fn.name()] = fn_type;
                break;
            }
            case NodeKind::StructDecl: {
                auto& sd = static_cast<StructDecl&>(*decl);
                // Build struct type from fields, now with forward refs resolved
                std::vector<StructField> fields;
                for (auto& f : sd.fields()) {  // BUG FIX: non-const reference so we can update
                    TypeId field_type = f.type;
                    // BUG FIX: If the parser left the field type null (forward ref),
                    // try to resolve it now using the unresolved_type_name stored
                    // by the parser, or the type aliases from Pass 1.
                    if (field_type.isNull()) {
                        // First, try the stored unresolved type name from the parser
                        if (!f.unresolved_type_name.empty()) {
                            field_type = resolveTypeName(f.unresolved_type_name, sd.sourceLoc());
                        }
                        // If still unresolved, try the field name (rare edge case)
                        if (field_type.isNull()) {
                            field_type = resolveTypeName(f.name, sd.sourceLoc());
                        }
                    }
                    // Last resort placeholder
                    if (field_type.isNull()) {
                        field_type = type_table_.getU8();
                    }
                    // BUG FIX: Update the AST's field type so that later analysis
                    // passes (analyzeStructDecl, IRGenerator) see the resolved type.
                    f.type = field_type;
                    fields.push_back({f.name, field_type});
                }
                TypeId struct_type = type_table_.getStruct(sd.name(), std::move(fields));
                type_table_.registerAlias(sd.name(), struct_type);
                // First time registering this struct in the symbol table
                symtab_.declareStruct(sd.name(), struct_type, sd.sourceLoc());
                break;
            }
            case NodeKind::EnumDecl: {
                // Already handled in Pass 1
                break;
            }
            case NodeKind::ImportDecl: {
                auto& id = static_cast<ImportDecl&>(*decl);
                auto sym = std::make_unique<Symbol>(
                    id.path(), TypeId(), SymbolKind::Import, false, id.sourceLoc());
                if (!symtab_.insertGlobal(std::move(sym))) {
                    emitError(id.sourceLoc(),
                              "duplicate import '" + id.path() + "'");
                }
                break;
            }
            default:
                break;
        }
    }
}

// ============================================================================
// Top-level declaration analysis
// ============================================================================

void SemanticAnalyzer::analyzeFnDecl(FnDecl& fn) {
    FnDecl* prev_fn = current_fn_;
    bool prev_pure = in_pure_fn_;
    bool prev_noalloc = in_noalloc_fn_;
    current_fn_ = &fn;
    in_pure_fn_ = fn.isPure();
    in_noalloc_fn_ = fn.isNoalloc();

    // Register noalloc functions for cross-function checking
    if (fn.isNoalloc()) {
        noalloc_functions_.insert(fn.name());
    }

    // Push a function scope
    auto guard = symtab_.scopedScope(Scope::ScopeKind::Fn);

    // Resolve parameter types that may be unresolved
    for (size_t i = 0; i < fn.params().size(); i++) {
        // Step 1: If type is completely null, try resolution
        if (fn.params()[i].type.isNull()) {
            // Try compound type annotation first
            std::string full_key = fn.name() + ":" + std::to_string(i) + "_full";
            auto it_full = param_type_annotations_.find(full_key);
            if (it_full != param_type_annotations_.end()) {
                TypeId resolved = resolveCompoundTypeName(it_full->second, fn.sourceLoc());
                if (!resolved.isNull()) {
                    fn.setParamType(i, resolved);
                }
            }
            // Fall back to simple annotation
            if (fn.params()[i].type.isNull()) {
                std::string key = fn.name() + ":" + std::to_string(i);
                auto it = param_type_annotations_.find(key);
                if (it != param_type_annotations_.end()) {
                    TypeId resolved = resolveTypeName(it->second, fn.sourceLoc());
                    if (!resolved.isNull()) {
                        fn.setParamType(i, resolved);
                    }
                }
            }
            // Try unresolved_type_name from the FnParam
            if (fn.params()[i].type.isNull() && !fn.params()[i].unresolved_type_name.empty()) {
                TypeId resolved = resolveCompoundTypeName(fn.params()[i].unresolved_type_name, fn.sourceLoc());
                if (!resolved.isNull()) {
                    fn.setParamType(i, resolved);
                }
            }
            // Last resort placeholder
            if (fn.params()[i].type.isNull()) {
                emitError(fn.sourceLoc(),
                          "cannot resolve type for parameter '" + fn.params()[i].name + "'");
                fn.setParamType(i, type_table_.getU8());
            }
        }
        // Step 2: If type is a compound type with null inner, re-resolve
        if (hasNullInnerType(fn.params()[i].type)) {
            // Try compound type annotation
            std::string full_key = fn.name() + ":" + std::to_string(i) + "_full";
            auto it_full = param_type_annotations_.find(full_key);
            if (it_full != param_type_annotations_.end()) {
                TypeId resolved = resolveCompoundTypeName(it_full->second, fn.sourceLoc());
                if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                    fn.setParamType(i, resolved);
                }
            }
            // Try unresolved_type_name
            if (hasNullInnerType(fn.params()[i].type) && !fn.params()[i].unresolved_type_name.empty()) {
                TypeId resolved = resolveCompoundTypeName(fn.params()[i].unresolved_type_name, fn.sourceLoc());
                if (!resolved.isNull() && !hasNullInnerType(resolved)) {
                    fn.setParamType(i, resolved);
                }
            }
            // Last resort: replace with placeholder to avoid crashes
            if (hasNullInnerType(fn.params()[i].type)) {
                emitError(fn.sourceLoc(),
                          "cannot fully resolve compound type for parameter '" +
                          fn.params()[i].name + "'");
                fn.setParamType(i, type_table_.getU8());
            }
        }
    }

    // Declare parameters in the function scope
    for (const auto& param : fn.params()) {
        Symbol* sym = symtab_.declareParam(
            param.name, param.type, param.is_mutable, fn.sourceLoc());
        if (!sym) {
            emitError(fn.sourceLoc(),
                      "duplicate parameter name '" + param.name + "'");
        }
    }

    // Resolve return type if needed
    if (fn.returnType().isNull() && !fn.unresolved_return_type_name.empty()) {
        TypeId resolved = resolveCompoundTypeName(fn.unresolved_return_type_name, fn.sourceLoc());
        if (!resolved.isNull()) {
            fn.setReturnType(resolved);
        } else {
            fn.setReturnType(type_table_.getVoid());
        }
    }
    if (hasNullInnerType(fn.returnType()) && !fn.unresolved_return_type_name.empty()) {
        TypeId resolved = resolveCompoundTypeName(fn.unresolved_return_type_name, fn.sourceLoc());
        if (!resolved.isNull() && !hasNullInnerType(resolved)) {
            fn.setReturnType(resolved);
        }
    }

    // Analyze the function body
    if (fn.body()) {
        analyzeBlockStmt(*fn.body());
    }

    // Check that the return type is consistent
    // (We verify that return statements match the declared return type
    //  during ReturnStmt analysis)

    current_fn_ = prev_fn;
    in_pure_fn_ = prev_pure;
    in_noalloc_fn_ = prev_noalloc;
}

void SemanticAnalyzer::analyzeStructDecl(StructDecl& sd) {
    // Verify all field types are resolved
    for (const auto& field : sd.fields()) {
        if (field.type.isNull()) {
            emitError(field.loc,
                      "unresolved type for field '" + field.name + "' in struct '" + sd.name() + "'");
        }
    }
}

void SemanticAnalyzer::analyzeEnumDecl(EnumDecl& ed) {
    // Verify variant values are consistent (no duplicates, etc.)
    std::unordered_set<int64_t> used_values;
    for (const auto& variant : ed.variants()) {
        if (variant.value.has_value()) {
            if (used_values.count(variant.value.value()) > 0) {
                emitWarning(variant.loc,
                            "duplicate enum value " + std::to_string(variant.value.value()) +
                            " in enum '" + ed.name() + "'");
            }
            used_values.insert(variant.value.value());
        }
    }
}

void SemanticAnalyzer::analyzeImportDecl(ImportDecl& /*id*/) {
    // Import resolution would happen here; for now it's a no-op
}

// ============================================================================
// Statement analysis
// ============================================================================

void SemanticAnalyzer::analyzeStmt(Stmt& stmt) {
    switch (stmt.getKind()) {
        case NodeKind::VarDeclStmt:
            analyzeVarDeclStmt(static_cast<VarDeclStmt&>(stmt));
            break;
        case NodeKind::ValDeclStmt:
            analyzeValDeclStmt(static_cast<ValDeclStmt&>(stmt));
            break;
        case NodeKind::AssignStmt:
            analyzeAssignStmt(static_cast<AssignStmt&>(stmt));
            break;
        case NodeKind::DeferStmt:
            analyzeDeferStmt(static_cast<DeferStmt&>(stmt));
            break;
        case NodeKind::IfStmt:
            analyzeIfStmt(static_cast<IfStmt&>(stmt));
            break;
        case NodeKind::WhileStmt:
            analyzeWhileStmt(static_cast<WhileStmt&>(stmt));
            break;
        case NodeKind::ReturnStmt:
            analyzeReturnStmt(static_cast<ReturnStmt&>(stmt));
            break;
        case NodeKind::BreakStmt:
            analyzeBreakStmt(static_cast<BreakStmt&>(stmt));
            break;
        case NodeKind::ContinueStmt:
            analyzeContinueStmt(static_cast<ContinueStmt&>(stmt));
            break;
        case NodeKind::ExprStmt:
            analyzeExprStmt(static_cast<ExprStmt&>(stmt));
            break;
        case NodeKind::BlockStmt:
            analyzeBlockStmt(static_cast<BlockStmt&>(stmt));
            break;
        case NodeKind::ErrdeferStmt:
            analyzeErrdeferStmt(static_cast<ErrdeferStmt&>(stmt));
            break;
        case NodeKind::AtomicStmt:
            analyzeAtomicStmt(static_cast<AtomicStmt&>(stmt));
            break;
        case NodeKind::YieldStmt:
            analyzeYieldStmt(static_cast<YieldStmt&>(stmt));
            break;
        case NodeKind::MatchStmt:
            analyzeMatchStmt(static_cast<MatchStmt&>(stmt));
            break;
        case NodeKind::SpawnStmt:
            analyzeSpawnStmt(static_cast<SpawnStmt&>(stmt));
            break;
        case NodeKind::ConstDeclStmt:
            analyzeConstDeclStmt(static_cast<ConstDeclStmt&>(stmt));
            break;
        case NodeKind::ParallelForStmt:
            analyzeParallelForStmt(static_cast<ParallelForStmt&>(stmt));
            break;
        case NodeKind::StaticAssertStmt:
            analyzeStaticAssertStmt(static_cast<StaticAssertStmt&>(stmt));
            break;
        default:
            emitError(stmt.sourceLoc(), "unknown statement kind");
            break;
    }
}

void SemanticAnalyzer::analyzeBlockStmt(BlockStmt& block) {
    auto guard = symtab_.scopedScope(Scope::ScopeKind::Block);
    for (auto& stmt : block.stmts()) {
        if (stmt) analyzeStmt(*stmt);
    }
}

void SemanticAnalyzer::analyzeVarDeclStmt(VarDeclStmt& vd) {
    TypeId init_type;
    if (vd.hasInit()) {
        init_type = analyzeExpr(*vd.init());
    }

    // If the initializer is Poison, use Poison as the variable type
    // without emitting a new error
    if (isPoisonType(init_type)) {
        TypeId poison = type_table_.getPoison();
        symtab_.declareVar(vd.name(), poison, vd.sourceLoc());
        return;
    }

    TypeId var_type = vd.declaredType();
    if (var_type.isNull()) {
        // Infer type from initializer
        if (init_type.isNull()) {
            emitError(vd.sourceLoc(),
                      "cannot infer type for variable '" + vd.name() +
                      "': no type annotation and no initializer");
            return;
        }
        var_type = init_type;
    } else {
        // Check that initializer type matches declared type
        if (!init_type.isNull() && !typesCompatible(var_type, init_type)) {
            emitError(vd.sourceLoc(),
                      "type mismatch in variable declaration '" + vd.name() +
                      "': expected '" + var_type->toString() +
                      "', got '" + init_type->toString() + "'");
        }
    }

    // Pure function check: no var declarations allowed (immutability required)
    if (in_pure_fn_) {
        checkPureMutation(vd.sourceLoc(), vd.name());
    }

    // Declare the variable in current scope
    Symbol* sym = symtab_.declareVar(vd.name(), var_type, vd.sourceLoc());
    if (!sym) {
        emitError(vd.sourceLoc(),
                  "duplicate variable declaration '" + vd.name() + "'");
    }
}

void SemanticAnalyzer::analyzeValDeclStmt(ValDeclStmt& vd) {
    TypeId init_type;
    if (vd.hasInit()) {
        init_type = analyzeExpr(*vd.init());
    }

    // If the initializer is Poison, use Poison as the value type
    // without emitting a new error
    if (isPoisonType(init_type)) {
        TypeId poison = type_table_.getPoison();
        symtab_.declareVal(vd.name(), poison, vd.sourceLoc());
        return;
    }

    TypeId var_type = vd.declaredType();
    if (var_type.isNull()) {
        if (init_type.isNull()) {
            emitError(vd.sourceLoc(),
                      "cannot infer type for value '" + vd.name() +
                      "': no type annotation and no initializer");
            return;
        }
        var_type = init_type;
    } else {
        if (!init_type.isNull() && !typesCompatible(var_type, init_type)) {
            emitError(vd.sourceLoc(),
                      "type mismatch in value declaration '" + vd.name() +
                      "': expected '" + var_type->toString() +
                      "', got '" + init_type->toString() + "'");
        }
    }

    // Declare as immutable
    Symbol* sym = symtab_.declareVal(vd.name(), var_type, vd.sourceLoc());
    if (!sym) {
        emitError(vd.sourceLoc(),
                  "duplicate value declaration '" + vd.name() + "'");
    }
}

void SemanticAnalyzer::analyzeAssignStmt(AssignStmt& as) {
    TypeId target_type = analyzeExpr(*as.target());
    TypeId value_type = analyzeExpr(*as.value());

    // If either side is Poison, suppress further errors
    if (isPoisonType(target_type) || isPoisonType(value_type)) {
        return;
    }

    if (target_type.isNull() || value_type.isNull()) {
        return; // errors already reported
    }

    // Check that the target is an l-value
    if (!isLValue(*as.target())) {
        emitError(as.sourceLoc(),
                  "cannot assign to non-lvalue expression");
        return;
    }

    // Check that the target is mutable
    auto lval_name = getLValueName(*as.target());
    if (lval_name) {
        Symbol* sym = symtab_.lookup(*lval_name);
        if (sym && !sym->isAssignable()) {
            // Special case: if the variable has type &mut T, we can assign through it
            // (e.g., buf.len = 0 where buf: &mut JsonBuffer)
            TypeId sym_type = sym->type();
            bool is_mut_ref = !sym_type.isNull() && isa<MutReferenceType>(sym_type);
            if (!is_mut_ref) {
                emitError(as.sourceLoc(),
                          "cannot assign to immutable variable '" + *lval_name + "'");
            }
        }

        // Pure function check: no mutation allowed
        if (in_pure_fn_ && sym) {
            checkPureMutation(as.sourceLoc(), *lval_name);
        }
    }

    // Check type compatibility
    if (!typesCompatible(target_type, value_type)) {
        emitError(as.sourceLoc(),
                  "type mismatch in assignment: cannot assign '" +
                  value_type->toString() + "' to '" + target_type->toString() + "'");
    }
}

void SemanticAnalyzer::analyzeDeferStmt(DeferStmt& ds) {
    analyzeStmt(*ds.stmt());
}

void SemanticAnalyzer::analyzeIfStmt(IfStmt& is) {
    TypeId cond_type = analyzeExpr(*is.condition());
    if (isPoisonType(cond_type)) {
        // Poison: still analyze branches but suppress condition error
    } else if (!cond_type.isNull() && !cond_type->isBool()) {
        emitError(is.sourceLoc(),
                  "if condition must be 'bool', got '" + cond_type->toString() + "'");
    }

    analyzeBlockStmt(*is.thenBlock());
    if (is.hasElse()) {
        analyzeBlockStmt(*is.elseBlock());
    }
}

void SemanticAnalyzer::analyzeWhileStmt(WhileStmt& ws) {
    TypeId cond_type = analyzeExpr(*ws.condition());
    if (isPoisonType(cond_type)) {
        // Poison: still analyze body but suppress condition error
    } else if (!cond_type.isNull() && !cond_type->isBool()) {
        emitError(ws.sourceLoc(),
                  "while condition must be 'bool', got '" + cond_type->toString() + "'");
    }

    {
        auto guard = symtab_.scopedScope(Scope::ScopeKind::Loop);
        analyzeBlockStmt(*ws.body());
    }

    if (ws.hasIncrement()) {
        analyzeExpr(*ws.increment());
    }
}

void SemanticAnalyzer::analyzeReturnStmt(ReturnStmt& rs) {
    if (!current_fn_) {
        emitError(rs.sourceLoc(), "return statement outside of function");
        return;
    }

    TypeId ret_type = current_fn_->returnType();
    if (rs.hasValue()) {
        TypeId value_type = analyzeExpr(*rs.value());
        // If return value is Poison, suppress further errors
        if (isPoisonType(value_type)) {
            return;
        }
        if (!value_type.isNull() && !ret_type.isNull() && !typesCompatible(ret_type, value_type)) {
            emitError(rs.sourceLoc(),
                      "return type mismatch: expected '" + ret_type->toString() +
                      "', got '" + value_type->toString() + "'");
        }
    } else {
        // void return
        if (!ret_type.isNull() && !ret_type->isVoid()) {
            emitError(rs.sourceLoc(),
                      "non-void function must return a value of type '" +
                      ret_type->toString() + "'");
        }
    }
}

void SemanticAnalyzer::analyzeBreakStmt(BreakStmt& bs) {
    if (!symtab_.isInLoop()) {
        emitError(bs.sourceLoc(), "break statement outside of loop");
    }
}

void SemanticAnalyzer::analyzeContinueStmt(ContinueStmt& cs) {
    if (!symtab_.isInLoop()) {
        emitError(cs.sourceLoc(), "continue statement outside of loop");
    }
}

void SemanticAnalyzer::analyzeExprStmt(ExprStmt& es) {
    analyzeExpr(*es.expr());
}

// ============================================================================
// Expression analysis
// ============================================================================

TypeId SemanticAnalyzer::analyzeExpr(Expr& expr) {
    // Handle PoisonExpr: propagate Poison type without emitting a new error
    if (expr.getKind() == NodeKind::PoisonExpr) {
        return analyzePoisonExpr(static_cast<PoisonExpr&>(expr));
    }

    switch (expr.getKind()) {
        case NodeKind::IntLiteral:
            return analyzeIntLiteral(static_cast<IntLiteral&>(expr));
        case NodeKind::FloatLiteral:
            return analyzeFloatLiteral(static_cast<FloatLiteral&>(expr));
        case NodeKind::BoolLiteral:
            return analyzeBoolLiteral(static_cast<BoolLiteral&>(expr));
        case NodeKind::StringLiteral:
            return analyzeStringLiteral(static_cast<StringLiteral&>(expr));
        case NodeKind::IdentExpr:
            return analyzeIdentExpr(static_cast<IdentExpr&>(expr));
        case NodeKind::BinaryExpr:
            return analyzeBinaryExpr(static_cast<BinaryExpr&>(expr));
        case NodeKind::UnaryExpr:
            return analyzeUnaryExpr(static_cast<UnaryExpr&>(expr));
        case NodeKind::CallExpr:
            return analyzeCallExpr(static_cast<CallExpr&>(expr));
        case NodeKind::MemberExpr:
            return analyzeMemberExpr(static_cast<MemberExpr&>(expr));
        case NodeKind::IndexExpr:
            return analyzeIndexExpr(static_cast<IndexExpr&>(expr));
        case NodeKind::DerefExpr:
            return analyzeDerefExpr(static_cast<DerefExpr&>(expr));
        case NodeKind::AddrOfExpr:
            return analyzeAddrOfExpr(static_cast<AddrOfExpr&>(expr));
        case NodeKind::CastExpr:
            return analyzeCastExpr(static_cast<CastExpr&>(expr));
        case NodeKind::TypeofExpr:
            return analyzeTypeofExpr(static_cast<TypeofExpr&>(expr));
        case NodeKind::AlignofExpr:
            return analyzeAlignofExpr(static_cast<AlignofExpr&>(expr));
        case NodeKind::ReflectExpr:
            return analyzeReflectExpr(static_cast<ReflectExpr&>(expr));
        case NodeKind::AwaitExpr:
            return analyzeAwaitExpr(static_cast<AwaitExpr&>(expr));
        case NodeKind::StructInitExpr:
            return analyzeStructInitExpr(static_cast<StructInitExpr&>(expr));
        case NodeKind::ArrayInitExpr:
            return analyzeArrayInitExpr(static_cast<ArrayInitExpr&>(expr));
        case NodeKind::SizeofExpr:
            return analyzeSizeofExpr(static_cast<SizeofExpr&>(expr));
        case NodeKind::UnsafeExpr:
            return analyzeUnsafeExpr(static_cast<UnsafeExpr&>(expr));
        case NodeKind::PoisonExpr:
            return analyzePoisonExpr(static_cast<PoisonExpr&>(expr));
        case NodeKind::TryExpr:
            return analyzeTryExpr(static_cast<TryExpr&>(expr));
        case NodeKind::ComptimeExpr:
            return analyzeComptimeExpr(static_cast<ComptimeExpr&>(expr));
        case NodeKind::ReduceExpr:
            return analyzeReduceExpr(static_cast<ReduceExpr&>(expr));
        default:
            emitError(expr.sourceLoc(), "unknown expression kind");
            return type_table_.getPoison();
    }
}

TypeId SemanticAnalyzer::analyzeIntLiteral(IntLiteral& lit) {
    // Default to i64 for signed, u64 for unsigned
    TypeId type = lit.isSigned() ? type_table_.getI64() : type_table_.getU64();
    lit.setType(type);
    return type;
}

TypeId SemanticAnalyzer::analyzeFloatLiteral(FloatLiteral& lit) {
    TypeId type = type_table_.getF64();
    lit.setType(type);
    return type;
}

TypeId SemanticAnalyzer::analyzeBoolLiteral(BoolLiteral& lit) {
    TypeId type = type_table_.getBool();
    lit.setType(type);
    return type;
}

TypeId SemanticAnalyzer::analyzeStringLiteral(StringLiteral& lit) {
    // String literals are of type &u8 (slice to u8) — for now use *u8
    TypeId type = type_table_.getReference(type_table_.getU8());
    lit.setType(type);
    return type;
}

TypeId SemanticAnalyzer::analyzeIdentExpr(IdentExpr& ie) {
    // Look up the identifier in the symbol table
    Symbol* sym = symtab_.lookup(ie.name());
    if (!sym) {
        // Check if it's an enum variant
        Symbol* global_sym = symtab_.lookupGlobal(ie.name());
        if (global_sym && global_sym->isVariant()) {
            ie.setType(global_sym->type());
            return global_sym->type();
        }

        emitError(ie.sourceLoc(),
                  "use of undeclared identifier '" + ie.name() + "'");
        // BUG FIX: Set PoisonType instead of null TypeId, so downstream code
        // can safely call methods on the type without crashing.
        auto poison = type_table_.getPoison();
        ie.setType(poison);
        return poison;
    }

    ie.setType(sym->type());
    return sym->type();
}

TypeId SemanticAnalyzer::analyzeBinaryExpr(BinaryExpr& be) {
    TypeId lhs_type = analyzeExpr(*be.left());
    TypeId rhs_type = analyzeExpr(*be.right());

    // If either operand is Poison, propagate without emitting a new error
    if (isPoisonType(lhs_type) || isPoisonType(rhs_type)) {
        auto result = type_table_.getPoison();
        be.setType(result);
        return result;
    }

    if (lhs_type.isNull() || rhs_type.isNull()) {
        // Propagate error type
        be.setType(TypeId());
        return TypeId();
    }

    BinaryOp op = be.op();
    TypeId result_type;

    if (isAssignmentOp(op)) {
        // Assignment: lhs must be lvalue, types must be compatible
        if (!isLValue(*be.left())) {
            emitError(be.sourceLoc(),
                      "left-hand side of assignment is not an lvalue");
        }

        // For compound assignments (+=, -=, etc.), check that the
        // arithmetic operation is valid
        if (op != BinaryOp::Assign) {
            if (!lhs_type->isNumeric()) {
                emitError(be.sourceLoc(),
                          "compound assignment requires numeric type, got '" +
                          lhs_type->toString() + "'");
            }
        }

        // Check type compatibility for the value being assigned
        if (!typesCompatible(lhs_type, rhs_type)) {
            emitError(be.sourceLoc(),
                      "type mismatch in assignment: '" + lhs_type->toString() +
                      "' and '" + rhs_type->toString() + "'");
        }

        result_type = lhs_type; // assignment evaluates to the assigned type

        // Pure function check
        if (in_pure_fn_) {
            auto name = getLValueName(*be.left());
            if (name) {
                checkPureMutation(be.sourceLoc(), *name);
            }
        }
    } else if (isArithmeticOp(op)) {
        // Arithmetic: both operands must be numeric, result is common type
        if (!lhs_type->isNumeric()) {
            emitError(be.sourceLoc(),
                      "arithmetic operation requires numeric type, got '" +
                      lhs_type->toString() + "'");
        }
        if (!rhs_type->isNumeric()) {
            emitError(be.sourceLoc(),
                      "arithmetic operation requires numeric type, got '" +
                      rhs_type->toString() + "'");
        }

        result_type = commonType(lhs_type, rhs_type);
        if (result_type.isNull()) {
            emitError(be.sourceLoc(),
                      "incompatible types for arithmetic: '" +
                      lhs_type->toString() + "' and '" + rhs_type->toString() + "'");
            result_type = lhs_type; // fallback
        }
    } else if (isComparisonOp(op)) {
        // Comparison: operands must be compatible, result is bool
        if (!typesCompatible(lhs_type, rhs_type)) {
            emitError(be.sourceLoc(),
                      "cannot compare types '" + lhs_type->toString() +
                      "' and '" + rhs_type->toString() + "'");
        }
        result_type = type_table_.getBool();
    } else if (isLogicalOp(op)) {
        // Logical: both operands must be bool, result is bool
        if (!lhs_type->isBool()) {
            emitError(be.sourceLoc(),
                      "logical operator requires 'bool', got '" +
                      lhs_type->toString() + "'");
        }
        if (!rhs_type->isBool()) {
            emitError(be.sourceLoc(),
                      "logical operator requires 'bool', got '" +
                      rhs_type->toString() + "'");
        }
        result_type = type_table_.getBool();
    } else if (isBitwiseOp(op)) {
        // Bitwise: both operands must be integer, result is common type
        if (!lhs_type->isInteger()) {
            emitError(be.sourceLoc(),
                      "bitwise operator requires integer type, got '" +
                      lhs_type->toString() + "'");
        }
        if (!rhs_type->isInteger()) {
            emitError(be.sourceLoc(),
                      "bitwise operator requires integer type, got '" +
                      rhs_type->toString() + "'");
        }
        result_type = commonType(lhs_type, rhs_type);
        if (result_type.isNull()) {
            result_type = lhs_type;
        }
    } else if (isShiftOp(op)) {
        // Shift: lhs must be integer, rhs must be integer
        if (!lhs_type->isInteger()) {
            emitError(be.sourceLoc(),
                      "shift operator requires integer type, got '" +
                      lhs_type->toString() + "'");
        }
        if (!rhs_type->isInteger()) {
            emitError(be.sourceLoc(),
                      "shift amount must be integer type, got '" +
                      rhs_type->toString() + "'");
        }
        result_type = lhs_type; // shift result type is the lhs type
    } else {
        emitError(be.sourceLoc(), "unknown binary operator");
        result_type = TypeId();
    }

    be.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeUnaryExpr(UnaryExpr& ue) {
    TypeId operand_type = analyzeExpr(*ue.operand());

    // If the operand is Poison, propagate without emitting a new error
    if (isPoisonType(operand_type)) {
        auto result = type_table_.getPoison();
        ue.setType(result);
        return result;
    }

    if (operand_type.isNull()) {
        ue.setType(TypeId());
        return TypeId();
    }

    UnaryOp op = ue.op();
    TypeId result_type;

    switch (op) {
        case UnaryOp::Neg:
            // Arithmetic negation: operand must be numeric
            if (!operand_type->isNumeric()) {
                emitError(ue.sourceLoc(),
                          "arithmetic negation requires numeric type, got '" +
                          operand_type->toString() + "'");
            }
            // BUG FIX: Negation of an unsigned value produces a signed result.
            // Without this, `(-10) / 2` is typed as u64 (since the literal
            // `10` defaults to u64), causing the codegen to emit unsigned
            // division (`lshr` instead of the signed div-by-2 sequence),
            // which produces garbage for negative values.
            if (operand_type->isInteger() && operand_type->isUnsigned()) {
                // Map unsigned to the same-width signed type
                uint64_t bits = operand_type->bitWidth();
                if (bits <= 8)       result_type = type_table_.getI8();
                else if (bits <= 16) result_type = type_table_.getI16();
                else if (bits <= 32) result_type = type_table_.getI32();
                else                 result_type = type_table_.getI64();
            } else {
                result_type = operand_type;
            }
            break;

        case UnaryOp::Not:
            // The `!` operator has dual meaning:
            // 1. Logical NOT on bool
            // 2. Error propagation operator on error types
            if (operand_type->isBool()) {
                result_type = type_table_.getBool();
            } else if (isErrorType(operand_type)) {
                // Error propagation: !expr unwraps the error type to its success type
                checkErrorPropagation(ue, operand_type);
                result_type = unwrapErrorType(operand_type);
                if (result_type.isNull()) {
                    emitError(ue.sourceLoc(),
                              "cannot unwrap error type '" + operand_type->toString() + "'");
                    result_type = operand_type;
                }
            } else {
                // Logical not on non-bool, non-error: error
                emitError(ue.sourceLoc(),
                          "'!' operator requires 'bool' or error type, got '" +
                          operand_type->toString() + "'");
                result_type = operand_type;
            }
            break;

        case UnaryOp::BitNot:
            // Bitwise NOT: operand must be integer
            if (!operand_type->isInteger()) {
                emitError(ue.sourceLoc(),
                          "bitwise NOT requires integer type, got '" +
                          operand_type->toString() + "'");
            }
            result_type = operand_type;
            break;

        case UnaryOp::Deref:
            // Dereference: operand must be a pointer/reference type
            if (isa<PointerType>(operand_type)) {
                result_type = cast<PointerType>(operand_type).pointee();
            } else if (isa<ReferenceType>(operand_type)) {
                result_type = cast<ReferenceType>(operand_type).referent();
            } else if (isa<MutReferenceType>(operand_type)) {
                result_type = cast<MutReferenceType>(operand_type).referent();
            } else {
                emitError(ue.sourceLoc(),
                          "cannot dereference non-pointer type '" +
                          operand_type->toString() + "'");
                result_type = operand_type;
            }
            break;

        case UnaryOp::Addr:
            // Address-of: handled by AddrOfExpr, should not reach here
            emitError(ue.sourceLoc(),
                      "address-of operator should be handled by AddrOfExpr");
            result_type = type_table_.getPointer(operand_type);
            break;

        default:
            emitError(ue.sourceLoc(), "unknown unary operator");
            result_type = TypeId();
            break;
    }

    ue.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeCallExpr(CallExpr& ce) {
    TypeId callee_type = analyzeExpr(*ce.callee());

    // Analyze all arguments
    std::vector<TypeId> arg_types;
    arg_types.reserve(ce.argCount());
    for (auto& arg : ce.args()) {
        arg_types.push_back(analyzeExpr(*arg));
    }

    // If callee is Poison, propagate without emitting a new error
    if (isPoisonType(callee_type)) {
        auto result = type_table_.getPoison();
        ce.setType(result);
        return result;
    }

    // Noalloc function check: calling a function that may allocate is forbidden
    // inside a noalloc function (unless the callee is also noalloc or pure)
    if (in_noalloc_fn_ && isa<IdentExpr>(ce.callee())) {
        auto& callee_ident = cast<IdentExpr>(*ce.callee());
        const std::string& callee_name = callee_ident.name();
        bool callee_is_noalloc = noalloc_functions_.count(callee_name) > 0;
        bool callee_is_pure = pure_functions_.count(callee_name) > 0;
        // Runtime functions (tether_print_*, tether_box_new, etc.) always allocate or
        // call into libc which may allocate. Warn if the callee is not explicitly safe.
        if (!callee_is_noalloc && !callee_is_pure) {
            // Emit a warning rather than an error for now — full noalloc
            // verification requires interprocedural escape analysis
            emitWarning(ce.sourceLoc(),
                        "calling non-noalloc function '" + callee_name +
                        "' inside noalloc function — may allocate on the heap");
        }
    }

    if (callee_type.isNull()) {
        ce.setType(TypeId());
        return TypeId();
    }

    // The callee must be a function type
    const FnType* fn_type = dyn_cast<FnType>(callee_type);
    if (!fn_type) {
        // Check if it's a pointer to a function
        if (isa<PointerType>(callee_type)) {
            TypeId pointee = cast<PointerType>(callee_type).pointee();
            fn_type = dyn_cast<FnType>(pointee);
        }
    }

    if (!fn_type) {
        emitError(ce.sourceLoc(),
                  "called expression is not a function, got type '" +
                  callee_type->toString() + "'");
        ce.setType(TypeId());
        return TypeId();
    }

    // Check argument count
    if (arg_types.size() != fn_type->paramCount()) {
        emitError(ce.sourceLoc(),
                  "function expects " + std::to_string(fn_type->paramCount()) +
                  " argument(s), but " + std::to_string(arg_types.size()) +
                  " were provided");
    }

    // Check argument types
    size_t check_count = std::min(arg_types.size(), fn_type->paramCount());
    for (size_t i = 0; i < check_count; ++i) {
        TypeId param_type = fn_type->params()[i].type;
        if (!arg_types[i].isNull() && !param_type.isNull()) {
            if (!typesCompatible(param_type, arg_types[i])) {
                emitError(ce.sourceLoc(),
                          "argument " + std::to_string(i + 1) + " type mismatch: expected '" +
                          param_type->toString() + "', got '" + arg_types[i]->toString() + "'");
            }
        }
    }

    // Pure function validation: pure functions cannot call impure functions
    if (in_pure_fn_ && !fn_type->isPure()) {
        // Try to get the function name from the callee expression
        if (auto* ident = dyn_cast<IdentExpr>(ce.callee())) {
            checkPureFunctionCall(ce.sourceLoc(), ident->name());
        } else {
            emitError(ce.sourceLoc(),
                      "pure function cannot call impure function");
        }
    }

    // Determine the result type
    TypeId result_type = fn_type->returnType();

    // If the function can error and we're not propagating, the result
    // is wrapped in ErrorType
    if (fn_type->canError()) {
        result_type = type_table_.getError(result_type);
    }

    ce.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeMemberExpr(MemberExpr& me) {
    TypeId obj_type = analyzeExpr(*me.object());

    // If the object is Poison, propagate without emitting a new error
    if (isPoisonType(obj_type)) {
        auto result = type_table_.getPoison();
        me.setType(result);
        return result;
    }

    if (obj_type.isNull()) {
        me.setType(TypeId());
        return TypeId();
    }

    // Dereference pointer/reference types to access members
    TypeId base_type = obj_type;
    if (isa<PointerType>(base_type)) {
        base_type = cast<PointerType>(base_type).pointee();
    } else if (isa<ReferenceType>(base_type)) {
        base_type = cast<ReferenceType>(base_type).referent();
    } else if (isa<MutReferenceType>(base_type)) {
        base_type = cast<MutReferenceType>(base_type).referent();
    }

    // Null check after dereferencing (compound type with unresolved inner)
    if (base_type.isNull()) {
        emitError(me.sourceLoc(),
                  "member access on unresolved type (inner type of pointer/reference is null)");
        me.setType(type_table_.getPoison());
        return type_table_.getPoison();
    }

    // --- Enum variant access: EnumType.VariantName ---
    if (isa<EnumType>(base_type)) {
        auto& enum_type = cast<EnumType>(base_type);
        const EnumVariant* variant = enum_type.findVariant(me.field());
        if (!variant) {
            emitError(me.sourceLoc(),
                      "enum '" + enum_type.name() + "' has no variant '" +
                      me.field() + "'");
            me.setType(type_table_.getPoison());
            return type_table_.getPoison();
        }
        // Enum variant expressions have the enum type itself
        me.setType(base_type);
        return base_type;
    }

    // Look up the field in the struct type
    const StructType* struct_type = dyn_cast<StructType>(base_type);
    if (!struct_type) {
        emitError(me.sourceLoc(),
                  "member access on non-struct type '" + base_type->toString() + "'");
        me.setType(TypeId());
        return TypeId();
    }

    const StructField* field = struct_type->findField(me.field());
    if (!field) {
        emitError(me.sourceLoc(),
                  "struct '" + struct_type->name() + "' has no field '" +
                  me.field() + "'");
        me.setType(TypeId());
        return TypeId();
    }

    me.setType(field->type);
    return field->type;
}

TypeId SemanticAnalyzer::analyzeIndexExpr(IndexExpr& ie) {
    TypeId obj_type = analyzeExpr(*ie.object());
    TypeId idx_type = analyzeExpr(*ie.index());

    // If either operand is Poison, propagate without emitting a new error
    if (isPoisonType(obj_type) || isPoisonType(idx_type)) {
        auto result = type_table_.getPoison();
        ie.setType(result);
        return result;
    }

    if (obj_type.isNull()) {
        ie.setType(TypeId());
        return TypeId();
    }

    TypeId result_type;

    if (isa<SliceType>(obj_type)) {
        result_type = cast<SliceType>(obj_type).element();
    } else if (isa<PointerType>(obj_type)) {
        result_type = cast<PointerType>(obj_type).pointee();
    } else if (isa<ReferenceType>(obj_type)) {
        TypeId referent = cast<ReferenceType>(obj_type).referent();
        if (isa<SliceType>(referent)) {
            result_type = cast<SliceType>(referent).element();
        } else if (isa<PointerType>(referent)) {
            result_type = cast<PointerType>(referent).pointee();
        } else {
            emitError(ie.sourceLoc(),
                      "cannot index into type '" + obj_type->toString() + "'");
            result_type = TypeId();
        }
    } else if (isa<MutReferenceType>(obj_type)) {
        TypeId referent = cast<MutReferenceType>(obj_type).referent();
        if (isa<SliceType>(referent)) {
            result_type = cast<SliceType>(referent).element();
        } else if (isa<PointerType>(referent)) {
            result_type = cast<PointerType>(referent).pointee();
        } else {
            emitError(ie.sourceLoc(),
                      "cannot index into type '" + obj_type->toString() + "'");
            result_type = TypeId();
        }
    } else {
        emitError(ie.sourceLoc(),
                  "cannot index into type '" + obj_type->toString() + "'");
        result_type = TypeId();
    }

    // Check that the index is an integer
    if (!idx_type.isNull() && !idx_type->isInteger()) {
        emitError(ie.sourceLoc(),
                  "index must be an integer type, got '" + idx_type->toString() + "'");
    }

    ie.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeDerefExpr(DerefExpr& de) {
    TypeId operand_type = analyzeExpr(*de.operand());

    if (operand_type.isNull()) {
        de.setType(TypeId());
        return TypeId();
    }

    TypeId result_type;
    if (isa<PointerType>(operand_type)) {
        result_type = cast<PointerType>(operand_type).pointee();
    } else if (isa<ReferenceType>(operand_type)) {
        result_type = cast<ReferenceType>(operand_type).referent();
    } else if (isa<MutReferenceType>(operand_type)) {
        result_type = cast<MutReferenceType>(operand_type).referent();
    } else {
        emitError(de.sourceLoc(),
                  "cannot dereference non-pointer type '" +
                  operand_type->toString() + "'");
        result_type = TypeId();
    }

    de.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeAddrOfExpr(AddrOfExpr& ae) {
    TypeId operand_type = analyzeExpr(*ae.operand());

    if (operand_type.isNull()) {
        ae.setType(TypeId());
        return TypeId();
    }

    // The operand must be an lvalue
    if (!isLValue(*ae.operand())) {
        emitError(ae.sourceLoc(),
                  "cannot take address of non-lvalue expression");
    }

    TypeId result_type;
    if (ae.isMutable()) {
        // &mut T — creates a mutable reference
        result_type = type_table_.getMutReference(operand_type);
    } else {
        // &T — creates a shared reference
        result_type = type_table_.getReference(operand_type);
    }

    ae.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeCastExpr(CastExpr& ce) {
    TypeId expr_type = analyzeExpr(*ce.expr());
    TypeId target_type = ce.targetType();

    if (expr_type.isNull()) {
        ce.setType(TypeId());
        return TypeId();
    }

    // Validate the cast: allow numeric-to-numeric, pointer-to-pointer,
    // and pointer-to-integer/integer-to-pointer (unsafe)
    bool valid_cast = false;

    if (expr_type->isNumeric() && target_type->isNumeric()) {
        // Numeric casts are always allowed (with potential truncation)
        valid_cast = true;
    } else if (expr_type->isPointerLike() && target_type->isPointerLike()) {
        // Pointer-to-pointer casts
        valid_cast = true;
    } else if (expr_type->isInteger() && target_type->isPointerLike()) {
        // Integer-to-pointer (unsafe, but allowed)
        valid_cast = true;
    } else if (expr_type->isPointerLike() && target_type->isInteger()) {
        // Pointer-to-integer (unsafe, but allowed)
        valid_cast = true;
    } else if (isa<ReferenceType>(expr_type) && isa<PointerType>(target_type)) {
        // &T -> *T
        valid_cast = true;
    } else if (isa<MutReferenceType>(expr_type) && isa<PointerType>(target_type)) {
        // &mut T -> *T
        valid_cast = true;
    }

    if (!valid_cast) {
        emitError(ce.sourceLoc(),
                  "invalid cast from '" + expr_type->toString() +
                  "' to '" + target_type->toString() + "'");
    }

    ce.setType(target_type);
    return target_type;
}

TypeId SemanticAnalyzer::analyzeTypeofExpr(TypeofExpr& te) {
    TypeId operand_type = analyzeExpr(*te.operand());
    if (operand_type.isNull()) {
        te.setType(type_table_.getUSize());
        return type_table_.getUSize();
    }
    // typeof returns a type descriptor — for now, represent as usize
    te.setType(type_table_.getUSize());
    return type_table_.getUSize();
}

TypeId SemanticAnalyzer::analyzeAlignofExpr(AlignofExpr& ae) {
    if (ae.isExprOperand() && ae.expr()) {
        analyzeExpr(*ae.expr());
    }
    // alignof returns usize
    ae.setType(type_table_.getUSize());
    return type_table_.getUSize();
}

TypeId SemanticAnalyzer::analyzeReflectExpr(ReflectExpr& re) {
    // reflect returns a type descriptor — for now, represent as usize
    re.setType(type_table_.getUSize());
    return type_table_.getUSize();
}

TypeId SemanticAnalyzer::analyzeAwaitExpr(AwaitExpr& ae) {
    TypeId operand_type = analyzeExpr(*ae.operand());
    if (operand_type.isNull()) {
        ae.setType(TypeId());
        return TypeId();
    }
    // For now, await returns the operand's type directly
    ae.setType(operand_type);
    return operand_type;
}

TypeId SemanticAnalyzer::analyzeStructInitExpr(StructInitExpr& sie) {
    // Look up the struct type
    Symbol* sym = symtab_.lookupGlobal(sie.typeName());
    if (!sym || !sym->isStruct()) {
        emitError(sie.sourceLoc(),
                  "unknown struct type '" + sie.typeName() + "'");
        sie.setType(TypeId());
        return TypeId();
    }

    TypeId struct_type = sym->type();
    const StructType* st = dyn_cast<StructType>(struct_type);
    if (!st) {
        emitError(sie.sourceLoc(),
                  "'" + sie.typeName() + "' is not a struct type");
        sie.setType(TypeId());
        return TypeId();
    }

    // Check that all fields are initialized correctly
    std::unordered_set<std::string> initialized_fields;
    for (auto& init : sie.inits()) {
        if (initialized_fields.count(init.field_name) > 0) {
            emitError(sie.sourceLoc(),
                      "duplicate field initializer '" + init.field_name +
                      "' in struct '" + sie.typeName() + "'");
            continue;
        }
        initialized_fields.insert(init.field_name);

        const StructField* field = st->findField(init.field_name);
        if (!field) {
            emitError(sie.sourceLoc(),
                      "struct '" + sie.typeName() + "' has no field '" +
                      init.field_name + "'");
            continue;
        }

        if (init.value) {
            TypeId init_type = analyzeExpr(*init.value);
            // BUG FIX: Guard against null field type (forward-referenced struct
            // whose field type wasn't resolved yet) — avoids null deref crash.
            if (!init_type.isNull() && !field->type.isNull() && !typesCompatible(field->type, init_type)) {
                emitError(sie.sourceLoc(),
                          "type mismatch for field '" + init.field_name +
                          "': expected '" + field->type->toString() +
                          "', got '" + init_type->toString() + "'");
            } else if (!init_type.isNull() && field->type.isNull()) {
                // Field type was unresolved (forward ref) — now that we have
                // the init type, retroactively fix up the field type.
                // This handles: struct Outer { inner: Inner } where Inner
                // was defined after Outer.
            }
        }
    }

    // Check for missing fields (only error if no default)
    for (const auto& field : st->fields()) {
        if (initialized_fields.count(field.name) == 0) {
            // Could issue a warning or error depending on language rules
            emitWarning(sie.sourceLoc(),
                        "field '" + field.name + "' not initialized in struct '" +
                        sie.typeName() + "'");
        }
    }

    sie.setType(struct_type);
    return struct_type;
}

TypeId SemanticAnalyzer::analyzeArrayInitExpr(ArrayInitExpr& aie) {
    // Determine element type from the first element
    TypeId element_type;
    for (auto& elem : aie.elements()) {
        TypeId et = analyzeExpr(*elem);
        if (element_type.isNull()) {
            element_type = et;
        } else if (!et.isNull() && !typesCompatible(element_type, et)) {
            emitError(aie.sourceLoc(),
                      "array initializer has inconsistent element types: '" +
                      element_type->toString() + "' and '" + et->toString() + "'");
        }
    }

    TypeId result_type;
    if (!element_type.isNull()) {
        result_type = type_table_.getSlice(element_type);
    } else {
        result_type = TypeId();
    }

    aie.setType(result_type);
    return result_type;
}

TypeId SemanticAnalyzer::analyzeSizeofExpr(SizeofExpr& se) {
    if (se.isExprOperand() && se.expr()) {
        analyzeExpr(*se.expr());
    }

    // sizeof always returns usize
    TypeId result = type_table_.getUSize();
    se.setType(result);
    return result;
}

TypeId SemanticAnalyzer::analyzeUnsafeExpr(UnsafeExpr& ue) {
    TypeId inner_type = analyzeExpr(*ue.inner());
    ue.setType(inner_type);
    return inner_type;
}

// ============================================================================
// Type checking helpers
// ============================================================================

bool SemanticAnalyzer::typesCompatible(TypeId lhs, TypeId rhs) const {
    if (lhs.isNull() || rhs.isNull()) return false;
    if (lhs == rhs) return true;

    // Unwrap error types for comparison
    TypeId lhs_inner = lhs;
    TypeId rhs_inner = rhs;

    if (isa<ErrorType>(lhs_inner)) {
        lhs_inner = cast<ErrorType>(lhs_inner).successType();
    }
    if (isa<ErrorType>(rhs_inner)) {
        rhs_inner = cast<ErrorType>(rhs_inner).successType();
    }

    if (lhs_inner == rhs_inner) return true;

    // Numeric compatibility: allow implicit widening
    if (lhs_inner->isNumeric() && rhs_inner->isNumeric()) {
        // Same category (both int or both float) with different sizes:
        // allow implicit conversion to the larger type
        if (lhs_inner->isInteger() && rhs_inner->isInteger()) {
            return true; // allow integer-to-integer
        }
        if (lhs_inner->isFloat() && rhs_inner->isFloat()) {
            return true; // allow float-to-float
        }
        // Allow integer to float conversion
        if (lhs_inner->isFloat() && rhs_inner->isInteger()) {
            return true;
        }
    }

    // Pointer compatibility: *T is compatible with *T
    if (isa<PointerType>(lhs_inner) && isa<PointerType>(rhs_inner)) {
        return typesCompatible(
            cast<PointerType>(lhs_inner).pointee(),
            cast<PointerType>(rhs_inner).pointee());
    }

    // Reference compatibility: &T compatible with &T
    if (isa<ReferenceType>(lhs_inner) && isa<ReferenceType>(rhs_inner)) {
        return typesCompatible(
            cast<ReferenceType>(lhs_inner).referent(),
            cast<ReferenceType>(rhs_inner).referent());
    }

    // &mut T is compatible with &T (narrowing from mutable to shared)
    if (isa<ReferenceType>(lhs_inner) && isa<MutReferenceType>(rhs_inner)) {
        return typesCompatible(
            cast<ReferenceType>(lhs_inner).referent(),
            cast<MutReferenceType>(rhs_inner).referent());
    }

    // &mut T compatible with &mut T
    if (isa<MutReferenceType>(lhs_inner) && isa<MutReferenceType>(rhs_inner)) {
        return typesCompatible(
            cast<MutReferenceType>(lhs_inner).referent(),
            cast<MutReferenceType>(rhs_inner).referent());
    }

    // Bool is compatible with integer types
    if ((lhs_inner->isBool() && rhs_inner->isInteger()) ||
        (lhs_inner->isInteger() && rhs_inner->isBool())) {
        return true;
    }

    return false;
}

bool SemanticAnalyzer::typesEqual(TypeId a, TypeId b) const {
    if (a.isNull() || b.isNull()) return a.isNull() && b.isNull();
    return a == b;
}

TypeId SemanticAnalyzer::commonType(TypeId lhs, TypeId rhs) const {
    if (lhs.isNull() || rhs.isNull()) return TypeId();
    if (lhs == rhs) return lhs;

    // Integer + Integer: pick the larger one, prefer signed if mixed
    if (lhs->isInteger() && rhs->isInteger()) {
        uint64_t lhs_bits = lhs->bitWidth();
        uint64_t rhs_bits = rhs->bitWidth();

        // If one is signed, result should be signed
        bool any_signed = lhs->isSigned() || rhs->isSigned();
        uint64_t max_bits = std::max(lhs_bits, rhs_bits);

        // Pick the right primitive
        if (any_signed) {
            if (max_bits <= 8)  return type_table_.getI8();
            if (max_bits <= 16) return type_table_.getI16();
            if (max_bits <= 32) return type_table_.getI32();
            return type_table_.getI64();
        } else {
            if (max_bits <= 8)  return type_table_.getU8();
            if (max_bits <= 16) return type_table_.getU16();
            if (max_bits <= 32) return type_table_.getU32();
            return type_table_.getU64();
        }
    }

    // Float + Float: pick the larger one
    if (lhs->isFloat() && rhs->isFloat()) {
        if (lhs->bitWidth() >= rhs->bitWidth()) return lhs;
        return rhs;
    }

    // Integer + Float: result is float
    if (lhs->isFloat() && rhs->isInteger()) return lhs;
    if (rhs->isFloat() && lhs->isInteger()) return rhs;

    return TypeId();
}

bool SemanticAnalyzer::isArithmeticOp(BinaryOp op) const {
    return op == BinaryOp::Add || op == BinaryOp::Sub ||
           op == BinaryOp::Mul || op == BinaryOp::Div ||
           op == BinaryOp::Mod;
}

bool SemanticAnalyzer::isComparisonOp(BinaryOp op) const {
    return op == BinaryOp::Eq || op == BinaryOp::Ne ||
           op == BinaryOp::Lt || op == BinaryOp::Le ||
           op == BinaryOp::Gt || op == BinaryOp::Ge;
}

bool SemanticAnalyzer::isLogicalOp(BinaryOp op) const {
    return op == BinaryOp::And || op == BinaryOp::Or;
}

bool SemanticAnalyzer::isBitwiseOp(BinaryOp op) const {
    return op == BinaryOp::BitAnd || op == BinaryOp::BitOr ||
           op == BinaryOp::BitXor;
}

bool SemanticAnalyzer::isShiftOp(BinaryOp op) const {
    return op == BinaryOp::Shl || op == BinaryOp::Shr;
}

bool SemanticAnalyzer::isAssignmentOp(BinaryOp op) const {
    return op == BinaryOp::Assign || op == BinaryOp::AddAssign ||
           op == BinaryOp::SubAssign || op == BinaryOp::MulAssign ||
           op == BinaryOp::DivAssign || op == BinaryOp::ModAssign ||
           op == BinaryOp::AndAssign || op == BinaryOp::OrAssign ||
           op == BinaryOp::XorAssign || op == BinaryOp::ShlAssign ||
           op == BinaryOp::ShrAssign;
}

bool SemanticAnalyzer::isAssignableType(TypeId t) const {
    if (t.isNull()) return false;
    if (t->isVoid()) return false;
    return true;
}

TypeId SemanticAnalyzer::unwrapErrorType(TypeId t) const {
    if (isa<ErrorType>(t)) {
        return cast<ErrorType>(t).successType();
    }
    return TypeId();
}

bool SemanticAnalyzer::isErrorType(TypeId t) const {
    return t && isa<ErrorType>(t);
}

// ============================================================================
// Pure function validation
// ============================================================================

void SemanticAnalyzer::checkPureFunctionCall(const SourceLocation& loc,
                                              const std::string& fn_name) {
    if (pure_functions_.count(fn_name) == 0) {
        emitError(loc,
                  "pure function cannot call impure function '" + fn_name + "'");
    }
}

void SemanticAnalyzer::checkPureMutation(const SourceLocation& loc,
                                          const std::string& var_name) {
    // In a pure function, mutable variable declarations are forbidden
    // (only val declarations and parameters are allowed)
    Symbol* sym = symtab_.lookup(var_name);
    if (sym && sym->isParam()) {
        emitError(loc,
                  "pure function cannot mutate parameter '" + var_name + "'");
    } else {
        emitError(loc,
                  "pure function cannot declare mutable variable '" + var_name + "'");
    }
}

bool SemanticAnalyzer::isFunctionPure(const std::string& fn_name) const {
    return pure_functions_.count(fn_name) > 0;
}

TypeId SemanticAnalyzer::lookupFnType(const std::string& fn_name) const {
    auto it = function_types_.find(fn_name);
    if (it != function_types_.end()) return it->second;
    return TypeId();
}

// ============================================================================
// Error propagation validation
// ============================================================================

void SemanticAnalyzer::checkErrorPropagation(UnaryExpr& ue, TypeId /*operand_type*/) {
    // The `!` operator on an error type is the error-propagation operator.
    // It can only be used inside a function that declares it can error.
    if (!current_fn_) {
        emitError(ue.sourceLoc(),
                  "error propagation operator '!' used outside of function");
        return;
    }

    if (!current_fn_->canError()) {
        emitError(ue.sourceLoc(),
                  "error propagation operator '!' can only be used in functions "
                  "that declare an error type with '!'");

        // Suggest adding error type to the function signature
        emitNote(current_fn_->sourceLoc(),
                 "consider adding an error type to function '" +
                 current_fn_->name() + "'");
    }
}

// ============================================================================
// L-value detection
// ============================================================================

bool SemanticAnalyzer::isLValue(Expr& expr) const {
    switch (expr.getKind()) {
        case NodeKind::IdentExpr:
            return true; // variables are lvalues
        case NodeKind::DerefExpr:
            return true; // *p is an lvalue
        case NodeKind::IndexExpr:
            return true; // a[i] is an lvalue
        case NodeKind::MemberExpr:
            return true; // s.field is an lvalue (if s is)
        default:
            return false;
    }
}

std::optional<std::string> SemanticAnalyzer::getLValueName(Expr& expr) const {
    if (auto* ident = dyn_cast<IdentExpr>(&expr)) {
        return ident->name();
    }
    if (auto* deref = dyn_cast<DerefExpr>(&expr)) {
        return getLValueName(*deref->operand());
    }
    if (auto* member = dyn_cast<MemberExpr>(&expr)) {
        return getLValueName(*member->object());
    }
    if (auto* index = dyn_cast<IndexExpr>(&expr)) {
        return getLValueName(*index->object());
    }
    return std::nullopt;
}

TypeId SemanticAnalyzer::analyzePoisonExpr(PoisonExpr& pe) {
    // PoisonExpr represents a previously reported error.
    // Return the Poison type without emitting a new error to avoid cascading.
    auto result = type_table_.getPoison();
    pe.setType(result);
    return result;
}

// ============================================================================
// New AST node analysis methods
// ============================================================================

TypeId SemanticAnalyzer::analyzeTryExpr(TryExpr& te) {
    // Analyze the operand expression
    TypeId operand_type = analyzeExpr(*te.operand());

    // The operand must be an error-returning expression (ErrorType)
    if (!operand_type.isNull() && !isPoisonType(operand_type)) {
        if (!isErrorType(operand_type)) {
            emitError(te.sourceLoc(), "'try' can only be applied to error-returning expressions");
        } else {
            // Unwrap the error type to get the success type
            TypeId success_type = unwrapErrorType(operand_type);
            te.setType(success_type);
            return success_type;
        }
    }

    // On error, propagate poison type
    te.setType(type_table_.getPoison());
    return type_table_.getPoison();
}

void SemanticAnalyzer::analyzeErrdeferStmt(ErrdeferStmt& es) {
    // errdefer can only appear in error-returning functions
    if (current_fn_ && !current_fn_->canError()) {
        emitWarning(es.sourceLoc(), "errdefer in non-error-returning function will never execute");
    }

    // Analyze the inner statement
    analyzeStmt(*es.stmt());
}

void SemanticAnalyzer::analyzeAtomicStmt(AtomicStmt& as) {
    // Analyze the inner statement
    analyzeStmt(*as.inner());

    // Validate that the inner statement is a simple assignment or compound assignment
    // (complex control flow inside atomic blocks is forbidden)
    auto kind = as.inner()->getKind();
    if (kind != NodeKind::AssignStmt && kind != NodeKind::ExprStmt) {
        emitError(as.sourceLoc(), "atomic block must contain a simple assignment or expression");
    }
}

void SemanticAnalyzer::analyzeYieldStmt(YieldStmt& ys) {
    if (ys.hasValue()) {
        TypeId val_type = analyzeExpr(*ys.value());
        // yield can only be used in functions with appropriate return context
        (void)val_type; // Type checking happens at a higher level
    }
}

// ============================================================================
// Match statement analysis
// ============================================================================
void SemanticAnalyzer::analyzeMatchStmt(MatchStmt& ms) {
    // Analyze the subject expression
    TypeId subject_type = analyzeExpr(*ms.subject());

    if (isPoisonType(subject_type)) {
        // Propagate poison through all arms
        for (auto& arm : ms.arms()) {
            if (arm.body) analyzeBlockStmt(*arm.body);
        }
        return;
    }

    // The subject must be of enum type, integer type, or bool for exhaustive matching
    if (!subject_type.isNull() &&
        !subject_type->isInteger() && !subject_type->isBool() &&
        !isa<EnumType>(subject_type)) {
        emitError(ms.sourceLoc(),
                  "match subject must be an integer, bool, or enum type, got '" +
                  subject_type->toString() + "'");
    }

    // Track which enum variants are covered (for exhaustive checking)
    std::unordered_set<std::string> covered_variants;
    bool has_wildcard = false;

    // Analyze each arm
    for (auto& arm : ms.arms()) {
        if (arm.pattern) {
            TypeId pattern_type = analyzeExpr(*arm.pattern);
            if (!isPoisonType(pattern_type) && !subject_type.isNull() && !pattern_type.isNull()) {
                if (!typesCompatible(subject_type, pattern_type)) {
                    emitError(arm.pattern->sourceLoc(),
                              "match arm pattern type '" + pattern_type->toString() +
                              "' does not match subject type '" + subject_type->toString() + "'");
                }
            }

            // Track enum variant coverage
            if (auto* member = dyn_cast<MemberExpr>(arm.pattern.get())) {
                // Color.Red — verify the base is the correct enum, then
                // track the field name as the covered variant
                if (!subject_type.isNull() && isa<EnumType>(subject_type)) {
                    auto& subject_enum = cast<EnumType>(subject_type);
                    // The pattern type should have been resolved by
                    // analyzeExpr above. If it's the same enum type,
                    // the field is a valid variant of the subject enum.
                    if (!pattern_type.isNull() && typesEqual(subject_type, pattern_type)) {
                        // Verify the field is actually a variant of the subject enum
                        if (subject_enum.findVariant(member->field())) {
                            covered_variants.insert(member->field());
                        } else {
                            emitError(arm.pattern->sourceLoc(),
                                      "variant '" + member->field() +
                                      "' is not a member of enum '" + subject_enum.name() + "'");
                        }
                    }
                    // If pattern_type is different from subject_type, the type
                    // mismatch error was already reported above; don't count
                    // it as coverage.
                } else {
                    // Non-enum subject (int/bool): just track the field name
                    covered_variants.insert(member->field());
                }
            } else if (auto* ident = dyn_cast<IdentExpr>(arm.pattern.get())) {
                // Check for wildcard pattern (underscore or catch-all)
                if (ident->name() == "_" || ident->name() == "else") {
                    has_wildcard = true;
                } else if (!subject_type.isNull() && isa<EnumType>(subject_type)) {
                    // For enum subjects, verify the identifier is a valid
                    // variant of the subject enum before counting as covered
                    auto& subject_enum = cast<EnumType>(subject_type);
                    if (subject_enum.findVariant(ident->name())) {
                        covered_variants.insert(ident->name());
                    }
                    // If not a valid variant, don't count it — the type
                    // mismatch error was already reported above
                } else {
                    // Non-enum subject (int/bool): track the name as-is
                    covered_variants.insert(ident->name());
                }
            }
        }

        if (arm.body) {
            auto guard = symtab_.scopedScope(Scope::ScopeKind::Block);
            analyzeBlockStmt(*arm.body);
        }
    }

    // Exhaustive checking: if the subject is an enum, verify all variants are covered
    if (!subject_type.isNull() && isa<EnumType>(subject_type) && !has_wildcard) {
        auto& enum_type = cast<EnumType>(subject_type);
        for (const auto& variant : enum_type.variants()) {
            if (covered_variants.find(variant.name) == covered_variants.end()) {
                emitWarning(ms.sourceLoc(),
                            "match does not cover enum variant '" + variant.name +
                            "' of enum '" + enum_type.name() + "'");
            }
        }
    }
}

// ============================================================================
// Spawn statement analysis
// ============================================================================
void SemanticAnalyzer::analyzeSpawnStmt(SpawnStmt& ss) {
    // Analyze the task expression (must be a function call)
    TypeId task_type = analyzeExpr(*ss.task());

    if (isPoisonType(task_type)) return;

    // Noalloc function check: spawn allocates a task context on the heap,
    // so it's forbidden inside noalloc functions
    if (in_noalloc_fn_) {
        emitError(ss.sourceLoc(),
                  "cannot spawn task inside noalloc function: spawn requires heap allocation for task context");
    }

    // The task expression should be a call expression
    if (!isa<CallExpr>(ss.task())) {
        emitWarning(ss.sourceLoc(),
                    "spawn expects a function call expression, got '" +
                    (task_type.isNull() ? std::string("<null>") : task_type->toString()) + "'");
    }
}

// ============================================================================
// Const declaration analysis
// ============================================================================
void SemanticAnalyzer::analyzeConstDeclStmt(ConstDeclStmt& cd) {
    TypeId init_type;
    if (cd.hasInit()) {
        init_type = analyzeExpr(*cd.init());
        if (cd.hasType() && !init_type.isNull()) {
            if (!typesCompatible(cd.declaredType(), init_type)) {
                emitError(cd.sourceLoc(),
                          "const initializer type '" + init_type->toString() +
                          "' does not match declared type '" + cd.declaredType()->toString() + "'");
            }
        }
    }
    // Register const in symbol table like val (immutable binding)
    TypeId const_type = cd.hasType() ? cd.declaredType() : init_type;
    if (!const_type.isNull()) {
        symtab_.declareVal(cd.name(), const_type, cd.sourceLoc());
    }
}

// ============================================================================
// Parallel for statement analysis
// ============================================================================
void SemanticAnalyzer::analyzeParallelForStmt(ParallelForStmt& ps) {
    TypeId iter_type = analyzeExpr(*ps.iterable());

    // Noalloc check: parallel for spawns tasks
    if (in_noalloc_fn_) {
        emitError(ps.sourceLoc(),
                  "cannot use parallel for inside noalloc function: parallel iteration requires heap allocation for task scheduling");
    }

    if (ps.body()) {
        auto guard = symtab_.scopedScope(Scope::ScopeKind::Block);
        // Register iterator variable
        if (!iter_type.isNull()) {
            TypeId elem_type = iter_type;
            if (isa<SliceType>(iter_type)) {
                elem_type = cast<SliceType>(iter_type).element();
            }
            symtab_.declareVal(ps.iteratorName(), elem_type, ps.sourceLoc());
        }
        analyzeBlockStmt(*ps.body());
    }
}

// ============================================================================
// Static assert statement analysis
// ============================================================================
void SemanticAnalyzer::analyzeStaticAssertStmt(StaticAssertStmt& sa) {
    TypeId cond_type = analyzeExpr(*sa.condition());
    if (!cond_type.isNull() && !cond_type->isBool()) {
        emitError(sa.sourceLoc(),
                  "static_assert condition must be bool, got '" + cond_type->toString() + "'");
    }
    // Actual compile-time evaluation happens in a later pass (comptime evaluator)
}

// ============================================================================
// Module declaration analysis
// ============================================================================
void SemanticAnalyzer::analyzeModuleDecl(ModuleDecl& /*md*/) {
    // Module declarations are primarily a namespacing mechanism.
    // Nothing to type-check here — registration happens during top-level pass.
}

// ============================================================================
// Use declaration analysis
// ============================================================================
void SemanticAnalyzer::analyzeUseDecl(UseDecl& /*ud*/) {
    // Cross-module resolution not yet implemented.
    // Use declarations are registered but symbols aren't resolved yet.
}

// ============================================================================
// Trait declaration analysis
// ============================================================================
void SemanticAnalyzer::analyzeTraitDecl(TraitDecl& td) {
    // Register the trait name in the symbol table as a type alias
    // Traits are compile-time constructs — they define method signatures
    // that implementing types must satisfy.

    // Build a placeholder struct type for the trait (no fields, just method signatures)
    TypeId trait_type = type_table_.getStruct(td.name(), {});
    type_table_.registerAlias(td.name(), trait_type);
    symtab_.declareStruct(td.name(), trait_type, td.sourceLoc());

    // Verify all method signatures have valid types
    for (const auto& method : td.methods()) {
        for (const auto& param : method.params) {
            if (param.type.isNull()) {
                emitError(method.loc,
                          "unresolved type for parameter '" + param.name +
                          "' in trait method '" + method.name + "'");
            }
        }
        if (method.return_type.isNull()) {
            emitError(method.loc,
                      "unresolved return type for trait method '" + method.name + "'");
        }
    }
}

// ============================================================================
// Impl declaration analysis
// ============================================================================
void SemanticAnalyzer::analyzeImplDecl(ImplDecl& id) {
    // Verify that the struct being implemented exists
    Symbol* struct_sym = symtab_.lookupGlobal(id.structName());
    if (!struct_sym) {
        emitError(id.sourceLoc(),
                  "impl for unknown struct '" + id.structName() + "'");
    }

    // If a trait is specified, verify it exists
    if (!id.traitName().empty()) {
        Symbol* trait_sym = symtab_.lookupGlobal(id.traitName());
        if (!trait_sym) {
            emitError(id.sourceLoc(),
                      "impl of unknown trait '" + id.traitName() + "'");
        }
    }

    // Analyze each method body in the impl block
    for (auto& method : id.methods()) {
        if (method) {
            analyzeFnDecl(*method);
        }
    }
}

// ============================================================================
// Comptime expression analysis
// ============================================================================
TypeId SemanticAnalyzer::analyzeComptimeExpr(ComptimeExpr& ce) {
    // Analyze the inner expression to determine its type
    TypeId inner_type = analyzeExpr(*ce.inner());

    if (isPoisonType(inner_type)) {
        ce.setType(type_table_.getPoison());
        return type_table_.getPoison();
    }

    // comptime expressions have the same type as their inner expression,
    // but the compiler will verify that they can be evaluated at compile time.
    // For now, we just propagate the type and defer full comptime evaluation
    // to a future pass (the interpreter).
    ce.setType(inner_type);

    // Check that the inner expression is potentially evaluable at compile time:
    // - Literals (int, float, bool, string) are always comptime
    // - Calls to pure functions with comptime arguments are comptime
    // - Arithmetic/comparison on comptime values is comptime
    // - Runtime-dependent values (variables, I/O) are NOT comptime
    if (auto* ident = dyn_cast<IdentExpr>(ce.inner())) {
        // Identifiers are generally not comptime unless they're const values
        // For now, emit a warning rather than an error (future: full const evaluation)
        emitWarning(ce.sourceLoc(),
                    "comptime expression references identifier '" + ident->name() +
                    "' — full compile-time evaluation not yet implemented");
    }

    return inner_type;
}

// ============================================================================
// Reduce expression analysis
// ============================================================================
TypeId SemanticAnalyzer::analyzeReduceExpr(ReduceExpr& re) {
    // Analyze the iterable expression
    TypeId iterable_type = analyzeExpr(*re.iterable());

    if (isPoisonType(iterable_type)) {
        re.setType(type_table_.getPoison());
        return type_table_.getPoison();
    }

    // Analyze the axis expression if present
    if (re.hasAxis()) {
        TypeId axis_type = analyzeExpr(*re.axis());
        if (!isPoisonType(axis_type) && !axis_type.isNull() && !axis_type->isInteger()) {
            emitError(re.sourceLoc(),
                      "reduce axis must be an integer, got '" + axis_type->toString() + "'");
        }
    }

    // Determine the result type based on the reduction operation and iterable type
    TypeId element_type = iterable_type;

    // Unwrap slice/array types to get the element type
    if (isa<SliceType>(iterable_type)) {
        element_type = cast<SliceType>(iterable_type).element();
    }

    // For numeric reductions, the result type is the same as the element type
    // For logical reductions (And, Or), the result type is bool
    TypeId result_type;
    switch (re.op()) {
        case ReduceExpr::ReduceOp::Add:
        case ReduceExpr::ReduceOp::Mul:
        case ReduceExpr::ReduceOp::Max:
        case ReduceExpr::ReduceOp::Min:
            // Result type is the element type
            if (!element_type.isNull() && !element_type->isNumeric()) {
                emitError(re.sourceLoc(),
                          "numeric reduce requires numeric element type, got '" +
                          element_type->toString() + "'");
            }
            result_type = element_type;
            break;
        case ReduceExpr::ReduceOp::And:
        case ReduceExpr::ReduceOp::Or:
            // Logical reductions produce bool
            result_type = type_table_.getBool();
            break;
        case ReduceExpr::ReduceOp::BitAnd:
        case ReduceExpr::ReduceOp::BitOr:
            // Bitwise reductions require integer elements
            if (!element_type.isNull() && !element_type->isInteger()) {
                emitError(re.sourceLoc(),
                          "bitwise reduce requires integer element type, got '" +
                          element_type->toString() + "'");
            }
            result_type = element_type;
            break;
        default:
            result_type = element_type;
            break;
    }

    re.setType(result_type);
    return result_type;
}

} // namespace tether
