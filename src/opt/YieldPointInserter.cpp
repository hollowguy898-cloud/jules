#include "opt/YieldPointInserter.h"

#include <cassert>

namespace tether {

// ============================================================================
// YieldPointInserterPass::run
//
// Walk all functions looking for while loops and function calls where
// yield points should be inserted for cooperative scheduling.
// ============================================================================
bool YieldPointInserterPass::run(Program& program, TypeTable& type_table) {
    yield_points_inserted_ = 0;
    bool any_inserted = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);

        // Skip @superoptimize functions - they don't get yield points
        if (isSuperOptimized(fn)) continue;

        if (walkFn(fn, type_table)) {
            any_inserted = true;
        }
    }

    return any_inserted;
}

// ============================================================================
// walkFn - process a function body
// ============================================================================
bool YieldPointInserterPass::walkFn(FnDecl& fn, TypeTable& type_table) {
    if (!fn.body()) return false;
    return walkBlock(fn.body(), type_table);
}

// ============================================================================
// walkBlock - recursively walk a block looking for while loops
// ============================================================================
bool YieldPointInserterPass::walkBlock(BlockStmt* block, TypeTable& type_table) {
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
// walkStmt - recursively walk statements looking for while loops and calls
// ============================================================================
bool YieldPointInserterPass::walkStmt(Stmt* stmt, TypeTable& type_table) {
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
            // Also recurse into the loop body for nested loops
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

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            // Check if this is a potentially long-running function call
            if (auto* call = dyn_cast<CallExpr>(expr_stmt.expr())) {
                // Annotate the call as a yield point candidate
                if (meta_map_) {
                    meta_map_->getOrCreate(call).yield_point = true;
                }
                yield_points_inserted_++;
                changed = true;
            }
            break;
        }

        default:
            break;
    }

    return changed;
}

// ============================================================================
// processWhileLoop - annotate a while loop for yield point insertion
//
// For cooperative scheduling, we insert a yield check at the top of
// each loop iteration. The check is:
//   if (scheduler.should_yield()) { scheduler.yield(); }
//
// To avoid checking on every iteration, we use a counter that only
// checks every YIELD_INTERVAL iterations:
//   if (++loop_counter % YIELD_INTERVAL == 0) {
//       if (scheduler.should_yield()) { scheduler.yield(); }
//   }
//
// The IR generator will create the hidden counter variable and emit
// the conditional yield check based on the annotation metadata.
// ============================================================================
bool YieldPointInserterPass::processWhileLoop(WhileStmt& loop, TypeTable& /*type_table*/) {
    if (!loop.body()) return false;

    // Check if the loop already has an explicit yield statement
    bool has_explicit_yield = false;
    for (auto& stmt : loop.body()->stmts()) {
        if (stmt->getKind() == NodeKind::YieldStmt) {
            has_explicit_yield = true;
            break;
        }
    }

    // If there's already an explicit yield, don't add automatic yield points
    if (has_explicit_yield) return false;

    // Annotate the while loop for yield point insertion
    // The IR generator will emit a yield check at the top of the loop body
    if (meta_map_) {
        meta_map_->getOrCreate(&loop).yield_point = true;
    }
    yield_points_inserted_++;
    return true;
}

// ============================================================================
// isSuperOptimized - check if a function has the @superoptimize directive
// ============================================================================
bool YieldPointInserterPass::isSuperOptimized(FnDecl& fn) const {
    return fn.hasDirective(CompilerDirective::Superoptimize);
}

} // namespace tether
