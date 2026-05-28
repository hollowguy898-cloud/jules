#include "opt/AllocatorLowerer.h"

#include <cassert>

namespace tether {

// ============================================================================
// AllocatorLowererPass::run
//
// Walk all functions looking for allocator call expressions that can be
// inlined. For arena allocators, replace the call with a pointer bump.
// ============================================================================
bool AllocatorLowererPass::run(Program& program, TypeTable& type_table) {
    arenas_lowered_ = 0;
    stacks_lowered_ = 0;

    bool any_lowered = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (walkFn(fn, type_table)) {
            any_lowered = true;
        }
    }

    return any_lowered;
}

// ============================================================================
// walkFn - process a function for allocator calls
// ============================================================================
bool AllocatorLowererPass::walkFn(FnDecl& fn, TypeTable& type_table) {
    if (!fn.body()) return false;
    return walkBlock(fn.body(), type_table);
}

// ============================================================================
// walkBlock - recursively walk a block
// ============================================================================
bool AllocatorLowererPass::walkBlock(BlockStmt* block, TypeTable& type_table) {
    if (!block) return false;
    bool changed = false;
    for (auto& stmt : block->stmts()) {
        if (walkStmt(stmt.get(), type_table)) {
            changed = true;
        }
    }
    return changed;
}

// ============================================================================
// walkStmt - recursively walk a statement looking for allocator calls
// ============================================================================
bool AllocatorLowererPass::walkStmt(Stmt* stmt, TypeTable& type_table) {
    if (!stmt) return false;
    bool changed = false;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            return walkBlock(&cast<BlockStmt>(*stmt), type_table);

        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit()) {
                if (walkExpr(var.init(), type_table)) changed = true;
            }
            break;
        }

        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit()) {
                if (walkExpr(val.init(), type_table)) changed = true;
            }
            break;
        }

        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            if (walkExpr(assign.target(), type_table)) changed = true;
            if (walkExpr(assign.value(), type_table)) changed = true;
            break;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            if (walkStmt(defer.stmt(), type_table)) changed = true;
            break;
        }

        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            if (walkStmt(errdefer.stmt(), type_table)) changed = true;
            break;
        }

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            if (walkExpr(if_stmt.condition(), type_table)) changed = true;
            if (if_stmt.thenBlock()) {
                if (walkBlock(if_stmt.thenBlock(), type_table)) changed = true;
            }
            if (if_stmt.elseBlock()) {
                if (walkBlock(if_stmt.elseBlock(), type_table)) changed = true;
            }
            break;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            if (walkExpr(while_stmt.condition(), type_table)) changed = true;
            if (walkBlock(while_stmt.body(), type_table)) changed = true;
            break;
        }

        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue()) {
                if (walkExpr(ret.value(), type_table)) changed = true;
            }
            break;
        }

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            if (walkExpr(expr_stmt.expr(), type_table)) changed = true;
            break;
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            if (walkStmt(atomic.inner(), type_table)) changed = true;
            break;
        }

        case NodeKind::YieldStmt: {
            auto& yield = cast<YieldStmt>(*stmt);
            if (yield.hasValue()) {
                if (walkExpr(yield.value(), type_table)) changed = true;
            }
            break;
        }

        default:
            break;
    }

    return changed;
}

// ============================================================================
// walkExpr - recursively walk an expression looking for allocator calls
// ============================================================================
bool AllocatorLowererPass::walkExpr(Expr* expr, TypeTable& type_table) {
    if (!expr) return false;
    bool changed = false;

    if (auto* call = dyn_cast<CallExpr>(expr)) {
        if (lowerCallExpr(*call, type_table)) {
            changed = true;
        }
        // Still recurse into callee and args
        if (walkExpr(call->callee(), type_table)) changed = true;
        for (auto& arg : call->args()) {
            if (walkExpr(arg.get(), type_table)) changed = true;
        }
    }
    else if (auto* binary = dyn_cast<BinaryExpr>(expr)) {
        if (walkExpr(binary->left(), type_table)) changed = true;
        if (walkExpr(binary->right(), type_table)) changed = true;
    }
    else if (auto* unary = dyn_cast<UnaryExpr>(expr)) {
        if (walkExpr(unary->operand(), type_table)) changed = true;
    }
    else if (auto* member = dyn_cast<MemberExpr>(expr)) {
        if (walkExpr(member->object(), type_table)) changed = true;
    }
    else if (auto* index = dyn_cast<IndexExpr>(expr)) {
        if (walkExpr(index->object(), type_table)) changed = true;
        if (walkExpr(index->index(), type_table)) changed = true;
    }
    else if (auto* deref = dyn_cast<DerefExpr>(expr)) {
        if (walkExpr(deref->operand(), type_table)) changed = true;
    }
    else if (auto* addr = dyn_cast<AddrOfExpr>(expr)) {
        if (walkExpr(addr->operand(), type_table)) changed = true;
    }
    else if (auto* cast_expr = dyn_cast<CastExpr>(expr)) {
        if (walkExpr(cast_expr->expr(), type_table)) changed = true;
    }
    else if (auto* select = dyn_cast<SelectExpr>(expr)) {
        if (walkExpr(select->condition(), type_table)) changed = true;
        if (walkExpr(select->trueExpr(), type_table)) changed = true;
        if (walkExpr(select->falseExpr(), type_table)) changed = true;
    }
    else if (auto* try_expr = dyn_cast<TryExpr>(expr)) {
        if (walkExpr(try_expr->operand(), type_table)) changed = true;
    }
    else if (auto* struct_init = dyn_cast<StructInitExpr>(expr)) {
        for (auto& init : struct_init->inits()) {
            if (walkExpr(init.value.get(), type_table)) changed = true;
        }
    }
    else if (auto* array_init = dyn_cast<ArrayInitExpr>(expr)) {
        for (auto& elem : array_init->elements()) {
            if (walkExpr(elem.get(), type_table)) changed = true;
        }
    }
    else if (auto* unsafe = dyn_cast<UnsafeExpr>(expr)) {
        if (walkExpr(unsafe->inner(), type_table)) changed = true;
    }

    return changed;
}

// ============================================================================
// lowerCallExpr - attempt to inline an allocator call
//
// Detects patterns like:
//   allocator.alloc(Type)      -> arena bump or stack alloca
//   allocator.alloc(Type, size) -> arena bump or stack alloca
//
// The actual AST transformation depends on the allocator type:
// - Arena: annotate for inline pointer bump during IR generation
// - Stack: annotate for alloca during IR generation
// - General: leave as-is (LLVM handles malloc/free optimization)
// ============================================================================
bool AllocatorLowererPass::lowerCallExpr(CallExpr& call, TypeTable& /*type_table*/) {
    // Check if the callee is an allocator method call (e.g., allocator.alloc)
    auto* member = dyn_cast<MemberExpr>(call.callee());
    if (!member) return false;

    // Check if the method name is "alloc" or "create"
    const std::string& method = member->field();
    if (method != "alloc" && method != "create") return false;

    // Check if the object has an Allocator type
    Expr* object = member->object();
    if (!object || !object->hasType()) return false;

    TypeId obj_type = object->getType();
    if (!obj_type || !isa<AllocatorType>(obj_type)) return false;

    // This is a call to allocator.alloc() or allocator.create()
    // We need to determine the allocator kind from context.

    // Heuristic: if the enclosing function takes an allocator parameter,
    // check the parameter type. Arena allocators have specific names.
    // For now, we annotate the call with "arena_inline" metadata so
    // the IR generator can emit the inline bump allocation.

    // Determine the allocation size from the call arguments
    // alloc(T) or alloc(T, count)
    uint64_t alloc_size = 0;
    if (call.argCount() >= 1) {
        // First arg is usually the type size or the type itself
        // In Tether's allocator API, alloc takes a type parameter
        // The IR generator will compute the actual size
        if (call.args()[0]->hasType()) {
            TypeId arg_type = call.args()[0]->getType();
            if (arg_type && !arg_type.isNull()) {
                // We can infer the size from the type
                // The IR generator will use this to compute the bump
                alloc_size = arg_type->bitWidth() / 8;
                if (alloc_size == 0) alloc_size = 8; // minimum allocation
            }
        }
    }

    if (alloc_size > 0) {
        // Annotate this call for inline arena lowering
        // The IR generator will emit:
        //   %old = load ptr, ptr %arena.offset
        //   %aligned = add ptr %old, alignUp(alloc_size, 8)
        //   store %aligned, ptr %arena.offset
        //   result = %old
        if (meta_map_) {
            auto& nm = meta_map_->getOrCreate(&call);
            nm.allocator_inlined = true;
        }
        arenas_lowered_++;
        return true;
    }

    return false;
}

} // namespace tether
