#include "sema/SemanticAnalyzer.h"

#include <sstream>
#include <algorithm>

namespace jules {

// ============================================================================
// Constructor
// ============================================================================
SemanticAnalyzer::SemanticAnalyzer(TypeTable& type_table)
    : type_table_(type_table)
    , symtab_()
    , current_fn_(nullptr)
    , in_pure_fn_(false)
    , error_prop_depth_(0)
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
            default:
                break;
        }
    }
}

// ============================================================================
// Diagnostic helpers
// ============================================================================
void SemanticAnalyzer::emitError(const SourceLocation& loc, const std::string& msg) {
    diagnostics_.emplace_back(DiagnosticKind::Error, loc, msg);
}

void SemanticAnalyzer::emitWarning(const SourceLocation& loc, const std::string& msg) {
    diagnostics_.emplace_back(DiagnosticKind::Warning, loc, msg);
}

void SemanticAnalyzer::emitNote(const SourceLocation& loc, const std::string& msg) {
    diagnostics_.emplace_back(DiagnosticKind::Note, loc, msg);
}

bool SemanticAnalyzer::hasErrors() const {
    for (const auto& d : diagnostics_) {
        if (d.isError()) return true;
    }
    return false;
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
    // Check primitives first
    if (name == "u8")    return type_table_.getU8();
    if (name == "u16")   return type_table_.getU16();
    if (name == "u32")   return type_table_.getU32();
    if (name == "u64")   return type_table_.getU64();
    if (name == "usize") return type_table_.getUSize();
    if (name == "i8")    return type_table_.getI8();
    if (name == "i16")   return type_table_.getI16();
    if (name == "i32")   return type_table_.getI32();
    if (name == "i64")   return type_table_.getI64();
    if (name == "isize") return type_table_.getISize();
    if (name == "f32")   return type_table_.getF32();
    if (name == "f64")   return type_table_.getF64();
    if (name == "bool")  return type_table_.getBool();
    if (name == "void")  return type_table_.getVoid();

    // Check the type table for a canonical match
    auto found = type_table_.lookup(name);
    if (found) return *found;

    // Check the type table aliases (e.g., "Vec2" -> struct:Vec2{...})
    auto alias = type_table_.lookupAlias(name);
    if (alias) return *alias;

    // Check symbol table for struct/enum types
    Symbol* sym = symtab_.lookupGlobal(name);
    if (sym && sym->isTypeSymbol()) {
        return sym->type();
    }

    return TypeId(); // null = unresolved
}

// ============================================================================
// First pass: register all top-level declarations
// ============================================================================
void SemanticAnalyzer::registerTopLevelDecls(Program& program) {
    for (auto& decl : program) {
        if (!decl) continue;

        switch (decl->getKind()) {
            case NodeKind::FnDecl: {
                auto& fn = static_cast<FnDecl&>(*decl);
                // Build the function type, resolving any null param types
                std::vector<FnParam> params = fn.params();
                // Resolve null parameter types using param_type_annotations_
                for (size_t i = 0; i < params.size(); i++) {
                    if (params[i].type.isNull()) {
                        std::string key = fn.name() + ":" + std::to_string(i);
                        auto it = param_type_annotations_.find(key);
                        if (it != param_type_annotations_.end()) {
                            params[i].type = resolveTypeName(it->second, fn.sourceLoc());
                        }
                        // If still null, use a placeholder that will be fixed later
                        if (params[i].type.isNull()) {
                            params[i].type = type_table_.getU8();
                        }
                    }
                }
                TypeId ret_type = fn.returnType();
                if (ret_type.isNull()) {
                    // Try to resolve return type too
                    // For now, default to void
                    ret_type = type_table_.getVoid();
                }
                bool is_pure = fn.isPure();
                TypeId err_type = fn.errorType();

                TypeId fn_type = type_table_.getFn(
                    std::move(params), ret_type, is_pure, err_type);

                // Register in the global scope
                Symbol* sym = symtab_.declareFn(fn.name(), fn_type, fn.sourceLoc());
                if (!sym) {
                    emitError(fn.sourceLoc(),
                              "duplicate function declaration '" + fn.name() + "'");
                }

                // Track pure functions
                if (is_pure) {
                    pure_functions_.insert(fn.name());
                }

                // Track function types
                function_types_[fn.name()] = fn_type;
                break;
            }
            case NodeKind::StructDecl: {
                auto& sd = static_cast<StructDecl&>(*decl);
                // Build struct type from fields
                // Note: field types from the parser should already be resolved for primitives
                std::vector<StructField> fields;
                for (const auto& f : sd.fields()) {
                    fields.push_back({f.name, f.type});
                }
                TypeId struct_type = type_table_.getStruct(sd.name(), std::move(fields));
                type_table_.registerAlias(sd.name(), struct_type);
                Symbol* sym = symtab_.declareStruct(sd.name(), struct_type, sd.sourceLoc());
                if (!sym) {
                    emitError(sd.sourceLoc(),
                              "duplicate struct declaration '" + sd.name() + "'");
                }
                break;
            }
            case NodeKind::EnumDecl: {
                auto& ed = static_cast<EnumDecl&>(*decl);
                // Build enum type from variants
                std::vector<EnumVariant> variants;
                for (const auto& v : ed.variants()) {
                    variants.push_back({v.name, v.value});
                }
                TypeId enum_type = type_table_.getEnum(ed.name(), std::move(variants));
                type_table_.registerAlias(ed.name(), enum_type);
                Symbol* sym = symtab_.declareEnum(ed.name(), enum_type, ed.sourceLoc());
                if (!sym) {
                    emitError(ed.sourceLoc(),
                              "duplicate enum declaration '" + ed.name() + "'");
                }
                break;
            }
            case NodeKind::ImportDecl: {
                auto& id = static_cast<ImportDecl&>(*decl);
                // For now, just register as a symbol
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
    current_fn_ = &fn;
    in_pure_fn_ = fn.isPure();

    // Push a function scope
    auto guard = symtab_.scopedScope(Scope::ScopeKind::Fn);

    // Resolve parameter types that may be unresolved
    for (size_t i = 0; i < fn.params().size(); i++) {
        if (fn.params()[i].type.isNull()) {
            // Try the param_type_annotations_ from the parser
            std::string key = fn.name() + ":" + std::to_string(i);
            auto it = param_type_annotations_.find(key);
            if (it != param_type_annotations_.end()) {
                TypeId resolved = resolveTypeName(it->second, fn.sourceLoc());
                if (!resolved.isNull()) {
                    fn.setParamType(i, resolved);
                } else {
                    emitError(fn.sourceLoc(),
                              "cannot resolve type for parameter '" + fn.params()[i].name + "'");
                    fn.setParamType(i, type_table_.getU8());
                }
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

    // Analyze the function body
    if (fn.body()) {
        analyzeBlockStmt(*fn.body());
    }

    // Check that the return type is consistent
    // (We verify that return statements match the declared return type
    //  during ReturnStmt analysis)

    current_fn_ = prev_fn;
    in_pure_fn_ = prev_pure;
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
            emitError(as.sourceLoc(),
                      "cannot assign to immutable variable '" + *lval_name + "'");
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
        case NodeKind::SelectExpr:
            return analyzeSelectExpr(static_cast<SelectExpr&>(expr));
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
        ie.setType(TypeId());
        return TypeId();
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
            result_type = operand_type;
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

TypeId SemanticAnalyzer::analyzeSelectExpr(SelectExpr& se) {
    TypeId cond_type = analyzeExpr(*se.condition());
    TypeId true_type = analyzeExpr(*se.trueExpr());
    TypeId false_type = analyzeExpr(*se.falseExpr());

    if (cond_type.isNull()) {
        se.setType(TypeId());
        return TypeId();
    }

    // Condition must be bool
    if (!cond_type->isBool()) {
        emitError(se.sourceLoc(),
                  "select condition must be 'bool', got '" +
                  cond_type->toString() + "'");
    }

    // Both branches must have compatible types
    if (!true_type.isNull() && !false_type.isNull()) {
        if (!typesCompatible(true_type, false_type)) {
            emitError(se.sourceLoc(),
                      "select branches have incompatible types: '" +
                      true_type->toString() + "' and '" + false_type->toString() + "'");
        }
    }

    TypeId result_type = true_type.isNull() ? false_type : true_type;
    se.setType(result_type);
    return result_type;
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
            if (!init_type.isNull() && !typesCompatible(field->type, init_type)) {
                emitError(sie.sourceLoc(),
                          "type mismatch for field '" + init.field_name +
                          "': expected '" + field->type->toString() +
                          "', got '" + init_type->toString() + "'");
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

} // namespace jules
