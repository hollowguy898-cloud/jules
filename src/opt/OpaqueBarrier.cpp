#include "opt/OpaqueBarrier.h"

#include <cassert>

namespace tether {

// ============================================================================
// OpaqueBarrierPass::run
//
// Walk all function declarations looking for functions that interact
// with opaque types (FFI boundaries). When found, annotate them so
// the IR generator emits appropriate barriers and attributes.
// ============================================================================
bool OpaqueBarrierPass::run(Program& program, TypeTable& type_table) {
    barriers_inserted_ = 0;
    bool any_barriers = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (processFnDecl(fn, type_table)) {
            any_barriers = true;
        }
    }

    return any_barriers;
}

// ============================================================================
// processFnDecl - check a function for opaque type interactions
// ============================================================================
bool OpaqueBarrierPass::processFnDecl(FnDecl& fn, TypeTable& type_table) {
    if (!fn.body()) return false;

    bool needs_barrier = false;

    // Check if the function takes opaque parameters
    if (takesOpaqueParams(fn, type_table)) {
        needs_barrier = true;
    }

    // Check if the function returns an opaque type
    if (returnsOpaque(fn, type_table)) {
        needs_barrier = true;
    }

    // Walk the function body looking for expressions that interact with
    // opaque types through pointer casts, member accesses on opaque ptrs, etc.
    walkBlock(fn.body(), type_table);

    if (needs_barrier) {
        // Annotate the function declaration as needing an opaque barrier.
        // The IR generator will:
        // 1. Add `inaccessiblememonly` attribute to the function
        // 2. Insert memory fences around calls to this function
        // 3. Prevent store-to-load forwarding across this function's calls
        if (annotations_) {
            annotations_->annotate(&fn,
                ASTAnnotationKind::OpaqueBarrier,
                "opaque_function:" + fn.name());
        }
        if (meta_map_) {
            meta_map_->getOrCreate(&fn).opaque_barrier = true;
        }
        barriers_inserted_++;
    }

    return needs_barrier || barriers_inserted_ > 0;
}

// ============================================================================
// walkBlock - recursively walk a block looking for opaque type access
// ============================================================================
bool OpaqueBarrierPass::walkBlock(BlockStmt* block, TypeTable& type_table) {
    if (!block) return false;
    bool found = false;
    for (auto& stmt : block->stmts()) {
        if (walkStmt(stmt.get(), type_table)) {
            found = true;
        }
    }
    return found;
}

// ============================================================================
// walkStmt - recursively walk statements
// ============================================================================
bool OpaqueBarrierPass::walkStmt(Stmt* stmt, TypeTable& type_table) {
    if (!stmt) return false;
    bool found = false;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            return walkBlock(&cast<BlockStmt>(*stmt), type_table);

        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            // Check if the variable's type is opaque
            if (var.hasType() && isOpaqueType(var.declaredType())) {
                // Insert a barrier annotation around this variable
                if (annotations_) {
                    annotations_->annotate(&var,
                        ASTAnnotationKind::OpaqueBarrier,
                        "opaque_var:" + var.name());
                }
                if (meta_map_) {
                    meta_map_->getOrCreate(&var).opaque_barrier = true;
                }
                barriers_inserted_++;
                found = true;
            }
            if (var.hasInit()) {
                if (walkExpr(var.init(), type_table)) found = true;
            }
            break;
        }

        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasType() && isOpaqueType(val.declaredType())) {
                if (annotations_) {
                    annotations_->annotate(&val,
                        ASTAnnotationKind::OpaqueBarrier,
                        "opaque_val:" + val.name());
                }
                if (meta_map_) {
                    meta_map_->getOrCreate(&val).opaque_barrier = true;
                }
                barriers_inserted_++;
                found = true;
            }
            if (val.hasInit()) {
                if (walkExpr(val.init(), type_table)) found = true;
            }
            break;
        }

        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            if (walkExpr(assign.target(), type_table)) found = true;
            if (walkExpr(assign.value(), type_table)) found = true;
            break;
        }

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            if (walkExpr(if_stmt.condition(), type_table)) found = true;
            if (if_stmt.thenBlock()) {
                if (walkBlock(if_stmt.thenBlock(), type_table)) found = true;
            }
            if (if_stmt.elseBlock()) {
                if (walkBlock(if_stmt.elseBlock(), type_table)) found = true;
            }
            break;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            if (walkExpr(while_stmt.condition(), type_table)) found = true;
            if (walkBlock(while_stmt.body(), type_table)) found = true;
            break;
        }

        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue()) {
                if (walkExpr(ret.value(), type_table)) found = true;
            }
            break;
        }

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            if (walkExpr(expr_stmt.expr(), type_table)) found = true;
            break;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            if (walkStmt(defer.stmt(), type_table)) found = true;
            break;
        }

        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            if (walkStmt(errdefer.stmt(), type_table)) found = true;
            break;
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            if (walkStmt(atomic.inner(), type_table)) found = true;
            break;
        }

        default:
            break;
    }

    return found;
}

// ============================================================================
// walkExpr - recursively walk expressions looking for opaque type access
// ============================================================================
bool OpaqueBarrierPass::walkExpr(Expr* expr, TypeTable& type_table) {
    if (!expr) return false;
    bool found = false;

    // Check if this expression's type is opaque
    if (expr->hasType() && isOpaqueType(expr->getType())) {
        // Found an expression that produces/uses an opaque value
        if (annotations_) {
            annotations_->annotate(expr,
                ASTAnnotationKind::OpaqueBarrier,
                "opaque_expr");
        }
        if (meta_map_) {
            meta_map_->getOrCreate(expr).opaque_barrier = true;
        }
        barriers_inserted_++;
        found = true;
    }

    // Recurse into sub-expressions
    if (auto* call = dyn_cast<CallExpr>(expr)) {
        // Check if the callee is an extern/FFI function
        // Calls to FFI functions that take/return opaque types need barriers
        if (call->callee()->hasType()) {
            TypeId callee_type = call->callee()->getType();
            if (callee_type && isa<FnType>(callee_type)) {
                const auto& fn_type = cast<FnType>(callee_type);
                // Check if any parameter or return type is opaque
                bool has_opaque_param = false;
                for (const auto& param : fn_type.params()) {
                    if (isOpaqueType(param.type)) {
                        has_opaque_param = true;
                        break;
                    }
                }
                if (has_opaque_param || isOpaqueType(fn_type.returnType())) {
                    if (annotations_) {
                        annotations_->annotate(call,
                            ASTAnnotationKind::OpaqueBarrier,
                            "ffi_call");
                    }
                    if (meta_map_) {
                        meta_map_->getOrCreate(call).opaque_barrier = true;
                    }
                    barriers_inserted_++;
                    found = true;
                }
            }
        }

        if (walkExpr(call->callee(), type_table)) found = true;
        for (auto& arg : call->args()) {
            if (walkExpr(arg.get(), type_table)) found = true;
        }
    }
    else if (auto* binary = dyn_cast<BinaryExpr>(expr)) {
        if (walkExpr(binary->left(), type_table)) found = true;
        if (walkExpr(binary->right(), type_table)) found = true;
    }
    else if (auto* unary = dyn_cast<UnaryExpr>(expr)) {
        if (walkExpr(unary->operand(), type_table)) found = true;
    }
    else if (auto* member = dyn_cast<MemberExpr>(expr)) {
        if (walkExpr(member->object(), type_table)) found = true;
    }
    else if (auto* index = dyn_cast<IndexExpr>(expr)) {
        if (walkExpr(index->object(), type_table)) found = true;
        if (walkExpr(index->index(), type_table)) found = true;
    }
    else if (auto* deref = dyn_cast<DerefExpr>(expr)) {
        if (walkExpr(deref->operand(), type_table)) found = true;
    }
    else if (auto* addr = dyn_cast<AddrOfExpr>(expr)) {
        if (walkExpr(addr->operand(), type_table)) found = true;
    }
    else if (auto* cast_expr = dyn_cast<CastExpr>(expr)) {
        // Casts to/from opaque types need barriers
        if (isOpaqueType(cast_expr->targetType())) {
            if (annotations_) {
                annotations_->annotate(cast_expr,
                    ASTAnnotationKind::OpaqueBarrier,
                    "opaque_cast");
            }
            if (meta_map_) {
                meta_map_->getOrCreate(cast_expr).opaque_barrier = true;
            }
            barriers_inserted_++;
            found = true;
        }
        if (walkExpr(cast_expr->expr(), type_table)) found = true;
    }
    else if (auto* select = dyn_cast<SelectExpr>(expr)) {
        if (walkExpr(select->condition(), type_table)) found = true;
        if (walkExpr(select->trueExpr(), type_table)) found = true;
        if (walkExpr(select->falseExpr(), type_table)) found = true;
    }
    else if (auto* try_expr = dyn_cast<TryExpr>(expr)) {
        if (walkExpr(try_expr->operand(), type_table)) found = true;
    }
    else if (auto* struct_init = dyn_cast<StructInitExpr>(expr)) {
        for (auto& init : struct_init->inits()) {
            if (walkExpr(init.value.get(), type_table)) found = true;
        }
    }
    else if (auto* unsafe = dyn_cast<UnsafeExpr>(expr)) {
        if (walkExpr(unsafe->inner(), type_table)) found = true;
    }

    return found;
}

// ============================================================================
// takesOpaqueParams - check if a function takes opaque type parameters
// ============================================================================
bool OpaqueBarrierPass::takesOpaqueParams(FnDecl& fn, TypeTable& /*type_table*/) const {
    for (const auto& param : fn.params()) {
        if (isOpaqueType(param.type)) return true;
        // Also check pointer to opaque (very common FFI pattern)
        if (param.type && isa<PointerType>(param.type)) {
            const auto& ptr = cast<PointerType>(param.type);
            if (isOpaqueType(ptr.pointee())) return true;
        }
    }
    return false;
}

// ============================================================================
// returnsOpaque - check if a function returns an opaque type
// ============================================================================
bool OpaqueBarrierPass::returnsOpaque(FnDecl& fn, TypeTable& /*type_table*/) const {
    if (isOpaqueType(fn.returnType())) return true;
    // Check pointer to opaque
    if (fn.returnType() && isa<PointerType>(fn.returnType())) {
        const auto& ptr = cast<PointerType>(fn.returnType());
        if (isOpaqueType(ptr.pointee())) return true;
    }
    return false;
}

// ============================================================================
// isOpaqueType - check if a type is an opaque type
// ============================================================================
bool OpaqueBarrierPass::isOpaqueType(TypeId type) const {
    if (type.isNull()) return false;
    return isa<OpaqueType>(type);
}

} // namespace tether
