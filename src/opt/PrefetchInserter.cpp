#include "opt/PrefetchInserter.h"

#include <cassert>
#include <functional>

namespace tether {

// ============================================================================
// PrefetchInserterPass::run
//
// Walk all functions looking for while loops that access aligned data.
// When we find a loop accessing data with align(64) or higher, we
// annotate the loop for prefetch insertion during IR generation.
// ============================================================================
bool PrefetchInserterPass::run(Program& program, TypeTable& type_table) {
    prefetches_inserted_ = 0;
    bool any_inserted = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (walkFn(fn, type_table)) {
            any_inserted = true;
        }
    }

    return any_inserted;
}

// ============================================================================
// walkFn - process a function body
// ============================================================================
bool PrefetchInserterPass::walkFn(FnDecl& fn, TypeTable& type_table) {
    if (!fn.body()) return false;
    return walkBlock(fn.body(), type_table);
}

// ============================================================================
// walkBlock - recursively walk a block looking for while loops
// ============================================================================
bool PrefetchInserterPass::walkBlock(BlockStmt* block, TypeTable& type_table) {
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
// walkStmt - recursively walk statements looking for while loops
// ============================================================================
bool PrefetchInserterPass::walkStmt(Stmt* stmt, TypeTable& type_table) {
    if (!stmt) return false;
    bool changed = false;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            return walkBlock(&cast<BlockStmt>(*stmt), type_table);

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
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
            if (processWhileLoop(while_stmt, type_table)) {
                changed = true;
            }
            // Also recurse into the loop body
            if (walkBlock(while_stmt.body(), type_table)) {
                changed = true;
            }
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

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            if (walkStmt(atomic.inner(), type_table)) changed = true;
            break;
        }

        default:
            break;
    }

    return changed;
}

// ============================================================================
// processWhileLoop - check if a while loop accesses aligned data
//
// We look for patterns where the loop body accesses data[i] where data
// has align(64) or higher. If found, we annotate the while loop for
// prefetch insertion.
// ============================================================================
bool PrefetchInserterPass::processWhileLoop(WhileStmt& loop, TypeTable& /*type_table*/) {
    if (!loop.body()) return false;

    // Walk the loop body looking for index expressions on aligned data
    bool found_aligned_access = false;
    std::string prefetch_detail;

    std::function<void(Expr*)> walk_expr = [&](Expr* expr) {
        if (!expr) return;

        if (auto* index = dyn_cast<IndexExpr>(expr)) {
            // Check if the indexed object has an aligned type
            if (index->object()->hasType()) {
                TypeId obj_type = index->object()->getType();
                if (hasHighAlignment(obj_type)) {
                    uint32_t align = getAlignmentValue(obj_type);
                    found_aligned_access = true;
                    prefetch_detail = "align:" + std::to_string(align) +
                                     ":distance:" + std::to_string(DEFAULT_PREFETCH_DISTANCE);
                }
            }
            walk_expr(index->object());
            walk_expr(index->index());
        }
        else if (auto* member = dyn_cast<MemberExpr>(expr)) {
            walk_expr(member->object());
        }
        else if (auto* binary = dyn_cast<BinaryExpr>(expr)) {
            walk_expr(binary->left());
            walk_expr(binary->right());
        }
        else if (auto* call = dyn_cast<CallExpr>(expr)) {
            walk_expr(call->callee());
            for (auto& arg : call->args()) {
                walk_expr(arg.get());
            }
        }
        else if (auto* unary = dyn_cast<UnaryExpr>(expr)) {
            walk_expr(unary->operand());
        }
        else if (auto* deref = dyn_cast<DerefExpr>(expr)) {
            walk_expr(deref->operand());
        }
    };

    for (auto& stmt : loop.body()->stmts()) {
        if (auto* expr_stmt = dyn_cast<ExprStmt>(stmt.get())) {
            walk_expr(expr_stmt->expr());
        }
        else if (auto* var = dyn_cast<VarDeclStmt>(stmt.get())) {
            if (var->hasInit()) walk_expr(var->init());
        }
        else if (auto* val = dyn_cast<ValDeclStmt>(stmt.get())) {
            if (val->hasInit()) walk_expr(val->init());
        }
        else if (auto* assign = dyn_cast<AssignStmt>(stmt.get())) {
            walk_expr(assign->target());
            walk_expr(assign->value());
        }
    }

    if (found_aligned_access && !prefetch_detail.empty()) {
        // Annotate the while loop for prefetch insertion
        // The IR generator will emit:
        //   call void @llvm.prefetch(ptr %next_addr, i32 0, i32 3, i32 1)
        // at the start of each loop iteration
        if (meta_map_) {
            auto& nm = meta_map_->getOrCreate(&loop);
            if (nm.llvm_meta.prefetch_distance <= 0) {
                nm.llvm_meta.prefetch_distance = DEFAULT_PREFETCH_DISTANCE;
            }
        }
        prefetches_inserted_++;
        return true;
    }

    return false;
}

// ============================================================================
// hasHighAlignment - check if a type has explicit alignment >= 64 bytes
// ============================================================================
bool PrefetchInserterPass::hasHighAlignment(TypeId type) const {
    if (type.isNull()) return false;

    // Direct aligned type
    if (isa<AlignedType>(type)) {
        const auto& aligned = cast<AlignedType>(type);
        return aligned.alignment() >= 64;
    }

    // Pointer to aligned type
    if (isa<PointerType>(type)) {
        const auto& ptr = cast<PointerType>(type);
        return hasHighAlignment(ptr.pointee());
    }

    // Struct type with alignment annotation
    // Check if this is a struct that was declared with align(N)
    // The StructDecl stores alignment info; we check the type's
    // canonical string for the alignment marker
    if (isa<StructType>(type)) {
        // The struct may have been registered with alignment via
        // the StructDecl's alignment() method, but the TypeTable
        // doesn't store alignment on the StructType directly.
        // We rely on the AST-level alignment annotation.
        (void)cast<StructType>(type);  // suppress unused warning
        return false;
    }

    return false;
}

// ============================================================================
// getAlignmentValue - get the alignment of an aligned type
// ============================================================================
uint32_t PrefetchInserterPass::getAlignmentValue(TypeId type) const {
    if (type.isNull()) return 0;

    if (isa<AlignedType>(type)) {
        const auto& aligned = cast<AlignedType>(type);
        return aligned.alignment();
    }

    if (isa<PointerType>(type)) {
        const auto& ptr = cast<PointerType>(type);
        return getAlignmentValue(ptr.pointee());
    }

    return 0;
}

} // namespace tether
