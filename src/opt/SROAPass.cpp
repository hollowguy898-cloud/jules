#include <type_traits>
#include <string>
#include <cassert>

#include "opt/SROAPass.h"

namespace tether {

// ============================================================================
// SROAPass::run
//
// Iterate over every function in the program. For each function, find
// VarDeclStmt/ValDeclStmt with struct types that satisfy the SROA
// eligibility criteria and annotate them in the MetadataMap.
// ============================================================================
bool SROAPass::run(Program& program, TypeTable& type_table) {
    vars_decomposed_ = 0;
    bool any_changed = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (analyzeFn(fn, type_table)) {
            any_changed = true;
        }
    }

    return any_changed;
}

// ============================================================================
// analyzeFn - analyze a single function for SROA opportunities
//
// Algorithm:
//   1. Walk the function body looking for VarDeclStmt/ValDeclStmt with
//      struct types that have ≤4 fields.
//   2. For each candidate, check:
//      a. Not address-taken (no AddrOfExpr referencing it)
//      b. Not passed whole to a function call
//      c. No whole-struct operations (assignment, return)
//      d. At least one MemberExpr accesses its fields (SROA must be useful)
//   3. If eligible, write sroa_eligible + field names/types metadata.
// ============================================================================
bool SROAPass::analyzeFn(FnDecl& fn, TypeTable& /*type_table*/) {
    if (!fn.body()) return false;

    bool any_annotated = false;
    auto& stmts = fn.body()->stmts();

    for (size_t i = 0; i < stmts.size(); ++i) {
        Stmt* stmt = stmts[i].get();
        if (!stmt) continue;

        // Only VarDeclStmt and ValDeclStmt can introduce struct locals
        std::string var_name;
        TypeId var_type;
        ASTNode* decl_node = nullptr;

        if (stmt->getKind() == NodeKind::VarDeclStmt) {
            auto& var = cast<VarDeclStmt>(*stmt);
            var_name = var.name();
            var_type = var.declaredType();
            if (var_type.isNull() && var.hasInit()) var_type = var.init()->getType();
            decl_node = stmt;
        } else if (stmt->getKind() == NodeKind::ValDeclStmt) {
            auto& val = cast<ValDeclStmt>(*stmt);
            var_name = val.name();
            var_type = val.declaredType();
            if (var_type.isNull() && val.hasInit()) var_type = val.init()->getType();
            decl_node = stmt;
        } else {
            continue;
        }

        // Check: is the variable a struct type with ≤4 fields?
        if (!var_type || !isa<StructType>(var_type)) continue;

        auto& st = cast<StructType>(var_type);
        if (st.fieldCount() == 0 || st.fieldCount() > 4) continue;

        // Check: all fields must be scalar (non-aggregate) so that individual
        // SSA values can represent them. Nested structs would require
        // recursive decomposition which we don't support yet.
        bool all_fields_scalar = true;
        for (const auto& field : st.fields()) {
            if (!field.type) { all_fields_scalar = false; break; }
            // We consider a field SROA-compatible if it is a primitive,
            // pointer, reference, enum, or smart pointer (Box only).
            // Aggregate fields (nested struct, slice, error type) disqualify.
            auto fk = field.type->getKind();
            if (fk == TypeKind::Struct || fk == TypeKind::Slice ||
                fk == TypeKind::Error || fk == TypeKind::Tensor) {
                all_fields_scalar = false;
                break;
            }
            // Smart pointers: only Box is scalar (just a ptr); Rc/Arc are aggregate
            if (fk == TypeKind::SmartPointer) {
                auto& sp = cast<SmartPointerType>(field.type);
                if (sp.smartPointerKind() != SmartPointerKind::Box) {
                    all_fields_scalar = false;
                    break;
                }
            }
        }
        if (!all_fields_scalar) continue;

        // Check: not address-taken
        if (isAddressTaken(fn.body(), var_name)) continue;

        // Check: not passed whole to a function call
        if (isPassedWholeToCall(fn.body(), var_name)) continue;

        // Check: no whole-struct operations
        if (hasWholeStructOp(fn.body(), var_name)) continue;

        // Check: at least one field is accessed (SROA must be useful)
        if (!hasFieldAccess(fn.body(), var_name)) continue;

        // Eligible for SROA! Write metadata.
        if (meta_map_) {
            auto& nm = meta_map_->getOrCreate(decl_node);
            nm.sroa_eligible = true;
            nm.sroa_field_names.clear();
            for (const auto& field : st.fields()) {
                nm.sroa_field_names.push_back(var_name + "." + field.name);
            }
        }

        vars_decomposed_++;
        any_annotated = true;
    }

    return any_annotated;
}

// ============================================================================
// isAddressTaken - check if a variable has its address taken anywhere
// ============================================================================
bool SROAPass::isAddressTaken(BlockStmt* block, const std::string& var_name) {
    if (!block) return false;
    for (auto& stmt : block->stmts()) {
        if (isAddressTakenStmt(stmt.get(), var_name)) return true;
    }
    return false;
}

bool SROAPass::isAddressTakenStmt(Stmt* stmt, const std::string& var_name) {
    if (!stmt) return false;
    switch (stmt->getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit() && isAddressTakenExpr(var.init(), var_name)) return true;
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit() && isAddressTakenExpr(val.init(), var_name)) return true;
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            // If the target is our variable and it's being assigned via deref,
            // that means someone has the address. But more directly, if the
            // target IS an AddrOfExpr of our variable, it's address-taken.
            // Actually, we just need to check if &var_name appears anywhere.
            if (isAddressTakenExpr(as.target(), var_name)) return true;
            if (isAddressTakenExpr(as.value(), var_name)) return true;
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            if (isAddressTakenExpr(es.expr(), var_name)) return true;
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && isAddressTakenExpr(ret.value(), var_name)) return true;
            break;
        }
        case NodeKind::BlockStmt: {
            return isAddressTaken(&cast<BlockStmt>(*stmt), var_name);
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);
            if (is.condition() && isAddressTakenExpr(is.condition(), var_name)) return true;
            if (is.thenBlock() && isAddressTaken(is.thenBlock(), var_name)) return true;
            if (is.hasElse() && is.elseBlock() && isAddressTaken(is.elseBlock(), var_name)) return true;
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            if (ws.condition() && isAddressTakenExpr(ws.condition(), var_name)) return true;
            if (ws.body() && isAddressTaken(ws.body(), var_name)) return true;
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            return isAddressTakenStmt(ds.stmt(), var_name);
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = cast<ErrdeferStmt>(*stmt);
            return isAddressTakenStmt(es.stmt(), var_name);
        }
        case NodeKind::AtomicStmt: {
            auto& as = cast<AtomicStmt>(*stmt);
            return isAddressTakenStmt(as.inner(), var_name);
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            if (ms.subject() && isAddressTakenExpr(ms.subject(), var_name)) return true;
            for (const auto& arm : ms.arms()) {
                if (arm.body && isAddressTaken(arm.body.get(), var_name)) return true;
            }
            break;
        }
        default: break;
    }
    return false;
}

bool SROAPass::isAddressTakenExpr(Expr* expr, const std::string& var_name) {
    if (!expr) return false;
    switch (expr->getKind()) {
        case NodeKind::AddrOfExpr: {
            // Direct address-of: &x
            auto& addr = cast<AddrOfExpr>(*expr);
            if (addr.operand()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*addr.operand());
                if (ident.name() == var_name) return true;
            }
            // Also check nested: &x.field — still takes address of x
            if (addr.operand()->getKind() == NodeKind::MemberExpr) {
                auto& mem = cast<MemberExpr>(*addr.operand());
                if (mem.object()->getKind() == NodeKind::IdentExpr) {
                    auto& ident = cast<IdentExpr>(*mem.object());
                    if (ident.name() == var_name) return true;
                }
            }
            // Recurse into the operand for deeper nesting
            return isAddressTakenExpr(addr.operand(), var_name);
        }
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            if (isAddressTakenExpr(bin.left(), var_name)) return true;
            if (isAddressTakenExpr(bin.right(), var_name)) return true;
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            return isAddressTakenExpr(un.operand(), var_name);
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            if (isAddressTakenExpr(call.callee(), var_name)) return true;
            for (auto& arg : call.args()) {
                if (isAddressTakenExpr(arg.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            return isAddressTakenExpr(mem.object(), var_name);
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            if (isAddressTakenExpr(idx.object(), var_name)) return true;
            if (isAddressTakenExpr(idx.index(), var_name)) return true;
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return isAddressTakenExpr(deref.operand(), var_name);
        }
        case NodeKind::CastExpr: {
            auto& cast_e = cast<CastExpr>(*expr);
            return isAddressTakenExpr(cast_e.expr(), var_name);
        }
        case NodeKind::StructInitExpr: {
            auto& si = cast<StructInitExpr>(*expr);
            for (auto& init : si.inits()) {
                if (isAddressTakenExpr(init.value.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& ai = cast<ArrayInitExpr>(*expr);
            for (auto& elem : ai.elements()) {
                if (isAddressTakenExpr(elem.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_e = cast<TryExpr>(*expr);
            return isAddressTakenExpr(try_e.operand(), var_name);
        }
        case NodeKind::UnsafeExpr: {
            auto& unsafe_e = cast<UnsafeExpr>(*expr);
            return isAddressTakenExpr(unsafe_e.inner(), var_name);
        }
        default: break;
    }
    return false;
}

// ============================================================================
// isPassedWholeToCall - check if a variable is passed as a whole struct
// to a function call (not just individual fields)
// ============================================================================
bool SROAPass::isPassedWholeToCall(BlockStmt* block, const std::string& var_name) {
    if (!block) return false;
    for (auto& stmt : block->stmts()) {
        if (isPassedWholeToCallStmt(stmt.get(), var_name)) return true;
    }
    return false;
}

bool SROAPass::isPassedWholeToCallStmt(Stmt* stmt, const std::string& var_name) {
    if (!stmt) return false;
    switch (stmt->getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit() && isPassedWholeToCallExpr(var.init(), var_name)) return true;
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit() && isPassedWholeToCallExpr(val.init(), var_name)) return true;
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            if (isPassedWholeToCallExpr(as.value(), var_name)) return true;
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            if (isPassedWholeToCallExpr(es.expr(), var_name)) return true;
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && isPassedWholeToCallExpr(ret.value(), var_name)) return true;
            break;
        }
        case NodeKind::BlockStmt: {
            return isPassedWholeToCall(&cast<BlockStmt>(*stmt), var_name);
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);
            if (is.thenBlock() && isPassedWholeToCall(is.thenBlock(), var_name)) return true;
            if (is.hasElse() && is.elseBlock() && isPassedWholeToCall(is.elseBlock(), var_name)) return true;
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            if (ws.body() && isPassedWholeToCall(ws.body(), var_name)) return true;
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            return isPassedWholeToCallStmt(ds.stmt(), var_name);
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = cast<ErrdeferStmt>(*stmt);
            return isPassedWholeToCallStmt(es.stmt(), var_name);
        }
        case NodeKind::AtomicStmt: {
            auto& as = cast<AtomicStmt>(*stmt);
            return isPassedWholeToCallStmt(as.inner(), var_name);
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            for (const auto& arm : ms.arms()) {
                if (arm.body && isPassedWholeToCall(arm.body.get(), var_name)) return true;
            }
            break;
        }
        default: break;
    }
    return false;
}

bool SROAPass::isPassedWholeToCallExpr(Expr* expr, const std::string& var_name) {
    if (!expr) return false;
    switch (expr->getKind()) {
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            // Check if any argument is a direct IdentExpr referencing our variable
            for (auto& arg : call.args()) {
                if (arg->getKind() == NodeKind::IdentExpr) {
                    auto& ident = cast<IdentExpr>(*arg);
                    if (ident.name() == var_name) return true;
                }
            }
            // Recurse into callee and arguments
            if (isPassedWholeToCallExpr(call.callee(), var_name)) return true;
            for (auto& arg : call.args()) {
                if (isPassedWholeToCallExpr(arg.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            if (isPassedWholeToCallExpr(bin.left(), var_name)) return true;
            if (isPassedWholeToCallExpr(bin.right(), var_name)) return true;
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            return isPassedWholeToCallExpr(un.operand(), var_name);
        }
        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            return isPassedWholeToCallExpr(mem.object(), var_name);
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            if (isPassedWholeToCallExpr(idx.object(), var_name)) return true;
            if (isPassedWholeToCallExpr(idx.index(), var_name)) return true;
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return isPassedWholeToCallExpr(deref.operand(), var_name);
        }
        case NodeKind::CastExpr: {
            auto& cast_e = cast<CastExpr>(*expr);
            return isPassedWholeToCallExpr(cast_e.expr(), var_name);
        }
        case NodeKind::StructInitExpr: {
            auto& si = cast<StructInitExpr>(*expr);
            for (auto& init : si.inits()) {
                if (isPassedWholeToCallExpr(init.value.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_e = cast<TryExpr>(*expr);
            return isPassedWholeToCallExpr(try_e.operand(), var_name);
        }
        default: break;
    }
    return false;
}

// ============================================================================
// hasWholeStructOp - check if a variable is used in a whole-struct operation
// (e.g., assignment of the whole struct, return of the whole struct,
//  use in a binary expression, or initialization of another struct)
// ============================================================================
bool SROAPass::hasWholeStructOp(BlockStmt* block, const std::string& var_name) {
    if (!block) return false;
    for (auto& stmt : block->stmts()) {
        if (hasWholeStructOpStmt(stmt.get(), var_name)) return true;
    }
    return false;
}

bool SROAPass::hasWholeStructOpStmt(Stmt* stmt, const std::string& var_name) {
    if (!stmt) return false;
    switch (stmt->getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            // If the init expression is the whole variable, it's a whole-struct op
            // e.g., val y = x  (where x is our struct variable)
            if (var.hasInit() && hasWholeStructOpExpr(var.init(), var_name)) return true;
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit() && hasWholeStructOpExpr(val.init(), var_name)) return true;
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            // If the value is the whole variable
            if (hasWholeStructOpExpr(as.value(), var_name)) return true;
            // If the target IS the whole variable (x = something), that's a whole-struct assign
            if (as.target()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*as.target());
                if (ident.name() == var_name) return true;
            }
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && hasWholeStructOpExpr(ret.value(), var_name)) return true;
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            if (hasWholeStructOpExpr(es.expr(), var_name)) return true;
            break;
        }
        case NodeKind::BlockStmt: {
            return hasWholeStructOp(&cast<BlockStmt>(*stmt), var_name);
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);
            if (is.thenBlock() && hasWholeStructOp(is.thenBlock(), var_name)) return true;
            if (is.hasElse() && is.elseBlock() && hasWholeStructOp(is.elseBlock(), var_name)) return true;
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            if (ws.body() && hasWholeStructOp(ws.body(), var_name)) return true;
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            return hasWholeStructOpStmt(ds.stmt(), var_name);
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = cast<ErrdeferStmt>(*stmt);
            return hasWholeStructOpStmt(es.stmt(), var_name);
        }
        case NodeKind::AtomicStmt: {
            auto& as = cast<AtomicStmt>(*stmt);
            return hasWholeStructOpStmt(as.inner(), var_name);
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            for (const auto& arm : ms.arms()) {
                if (arm.body && hasWholeStructOp(arm.body.get(), var_name)) return true;
            }
            break;
        }
        default: break;
    }
    return false;
}

bool SROAPass::hasWholeStructOpExpr(Expr* expr, const std::string& var_name) {
    if (!expr) return false;
    // If the expression is a direct IdentExpr referencing our variable,
    // it's a whole-struct use (as opposed to a MemberExpr which accesses
    // a specific field).
    if (expr->getKind() == NodeKind::IdentExpr) {
        auto& ident = cast<IdentExpr>(*expr);
        if (ident.name() == var_name) return true;
    }
    // Don't recurse into MemberExpr — x.field is NOT a whole-struct use
    // of x (it's a field access, which is exactly what SROA promotes).
    // But DO recurse into other expression types to catch deeper uses.
    switch (expr->getKind()) {
        case NodeKind::IdentExpr:
            // Already handled above
            break;
        case NodeKind::MemberExpr:
            // DON'T recurse into the object of a MemberExpr if the object
            // is our variable — that's a field access, not whole-struct use.
            // But DO recurse if the object is something else (could contain
            // a whole-struct use of our variable deeper inside).
            {
                auto& mem = cast<MemberExpr>(*expr);
                if (mem.object()->getKind() == NodeKind::IdentExpr) {
                    auto& ident = cast<IdentExpr>(*mem.object());
                    if (ident.name() == var_name) {
                        // This is x.field — NOT a whole-struct use. Skip.
                        break;
                    }
                }
                // Object is something else — recurse
                if (hasWholeStructOpExpr(mem.object(), var_name)) return true;
            }
            break;
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            if (hasWholeStructOpExpr(bin.left(), var_name)) return true;
            if (hasWholeStructOpExpr(bin.right(), var_name)) return true;
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            return hasWholeStructOpExpr(un.operand(), var_name);
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            // Whole-struct uses in call args are caught by isPassedWholeToCall
            // but we also check here for consistency
            if (hasWholeStructOpExpr(call.callee(), var_name)) return true;
            for (auto& arg : call.args()) {
                if (hasWholeStructOpExpr(arg.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            if (hasWholeStructOpExpr(idx.object(), var_name)) return true;
            if (hasWholeStructOpExpr(idx.index(), var_name)) return true;
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return hasWholeStructOpExpr(deref.operand(), var_name);
        }
        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            // &x is caught by isAddressTaken, but &x.field is not a whole-struct op
            if (addr.operand()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*addr.operand());
                if (ident.name() == var_name) return true;
            }
            if (hasWholeStructOpExpr(addr.operand(), var_name)) return true;
            break;
        }
        case NodeKind::CastExpr: {
            auto& cast_e = cast<CastExpr>(*expr);
            return hasWholeStructOpExpr(cast_e.expr(), var_name);
        }
        case NodeKind::StructInitExpr: {
            auto& si = cast<StructInitExpr>(*expr);
            for (auto& init : si.inits()) {
                if (hasWholeStructOpExpr(init.value.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& ai = cast<ArrayInitExpr>(*expr);
            for (auto& elem : ai.elements()) {
                if (hasWholeStructOpExpr(elem.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_e = cast<TryExpr>(*expr);
            return hasWholeStructOpExpr(try_e.operand(), var_name);
        }
        case NodeKind::UnsafeExpr: {
            auto& unsafe_e = cast<UnsafeExpr>(*expr);
            return hasWholeStructOpExpr(unsafe_e.inner(), var_name);
        }
        default: break;
    }
    return false;
}

// ============================================================================
// hasFieldAccess - check if any MemberExpr accesses the variable's fields
// ============================================================================
bool SROAPass::hasFieldAccess(BlockStmt* block, const std::string& var_name) {
    if (!block) return false;
    for (auto& stmt : block->stmts()) {
        if (hasFieldAccessStmt(stmt.get(), var_name)) return true;
    }
    return false;
}

bool SROAPass::hasFieldAccessStmt(Stmt* stmt, const std::string& var_name) {
    if (!stmt) return false;
    switch (stmt->getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit() && hasFieldAccessExpr(var.init(), var_name)) return true;
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit() && hasFieldAccessExpr(val.init(), var_name)) return true;
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            if (hasFieldAccessExpr(as.target(), var_name)) return true;
            if (hasFieldAccessExpr(as.value(), var_name)) return true;
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            if (hasFieldAccessExpr(es.expr(), var_name)) return true;
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && hasFieldAccessExpr(ret.value(), var_name)) return true;
            break;
        }
        case NodeKind::BlockStmt: {
            return hasFieldAccess(&cast<BlockStmt>(*stmt), var_name);
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);
            if (is.condition() && hasFieldAccessExpr(is.condition(), var_name)) return true;
            if (is.thenBlock() && hasFieldAccess(is.thenBlock(), var_name)) return true;
            if (is.hasElse() && is.elseBlock() && hasFieldAccess(is.elseBlock(), var_name)) return true;
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            if (ws.condition() && hasFieldAccessExpr(ws.condition(), var_name)) return true;
            if (ws.body() && hasFieldAccess(ws.body(), var_name)) return true;
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            return hasFieldAccessStmt(ds.stmt(), var_name);
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = cast<ErrdeferStmt>(*stmt);
            return hasFieldAccessStmt(es.stmt(), var_name);
        }
        case NodeKind::AtomicStmt: {
            auto& as = cast<AtomicStmt>(*stmt);
            return hasFieldAccessStmt(as.inner(), var_name);
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            if (ms.subject() && hasFieldAccessExpr(ms.subject(), var_name)) return true;
            for (const auto& arm : ms.arms()) {
                if (arm.body && hasFieldAccess(arm.body.get(), var_name)) return true;
            }
            break;
        }
        default: break;
    }
    return false;
}

bool SROAPass::hasFieldAccessExpr(Expr* expr, const std::string& var_name) {
    if (!expr) return false;
    switch (expr->getKind()) {
        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            // Check if the object is our variable: x.field
            if (mem.object()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*mem.object());
                if (ident.name() == var_name) return true;
            }
            // Recurse into the object (could be a nested member access)
            if (hasFieldAccessExpr(mem.object(), var_name)) return true;
            break;
        }
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            if (hasFieldAccessExpr(bin.left(), var_name)) return true;
            if (hasFieldAccessExpr(bin.right(), var_name)) return true;
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            return hasFieldAccessExpr(un.operand(), var_name);
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            if (hasFieldAccessExpr(call.callee(), var_name)) return true;
            for (auto& arg : call.args()) {
                if (hasFieldAccessExpr(arg.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            if (hasFieldAccessExpr(idx.object(), var_name)) return true;
            if (hasFieldAccessExpr(idx.index(), var_name)) return true;
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return hasFieldAccessExpr(deref.operand(), var_name);
        }
        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            return hasFieldAccessExpr(addr.operand(), var_name);
        }
        case NodeKind::CastExpr: {
            auto& cast_e = cast<CastExpr>(*expr);
            return hasFieldAccessExpr(cast_e.expr(), var_name);
        }
        case NodeKind::StructInitExpr: {
            auto& si = cast<StructInitExpr>(*expr);
            for (auto& init : si.inits()) {
                if (hasFieldAccessExpr(init.value.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& ai = cast<ArrayInitExpr>(*expr);
            for (auto& elem : ai.elements()) {
                if (hasFieldAccessExpr(elem.get(), var_name)) return true;
            }
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_e = cast<TryExpr>(*expr);
            return hasFieldAccessExpr(try_e.operand(), var_name);
        }
        case NodeKind::UnsafeExpr: {
            auto& unsafe_e = cast<UnsafeExpr>(*expr);
            return hasFieldAccessExpr(unsafe_e.inner(), var_name);
        }
        default: break;
    }
    return false;
}

} // namespace tether
