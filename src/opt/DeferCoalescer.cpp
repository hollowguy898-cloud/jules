#include "opt/DeferCoalescer.h"

#include <cassert>
#include <vector>

namespace tether {

// ============================================================================
// DeferCoalescerPass::run
//
// Walk all functions looking for consecutive defer statements that can
// be coalesced into a single defer block.
// ============================================================================
bool DeferCoalescerPass::run(Program& program, TypeTable& /*type_table*/) {
    coalesced_groups_ = 0;
    bool any_coalesced = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (walkFn(fn)) {
            any_coalesced = true;
        }
    }

    return any_coalesced;
}

// ============================================================================
// walkFn - process a function body
// ============================================================================
bool DeferCoalescerPass::walkFn(FnDecl& fn) {
    if (!fn.body()) return false;
    return walkBlock(*fn.body());
}

// ============================================================================
// walkBlock - recursively walk a block, looking for consecutive defers
// ============================================================================
bool DeferCoalescerPass::walkBlock(BlockStmt& block) {
    bool changed = false;

    // First, coalesce consecutive defers in this block
    if (coalesceBlock(block)) {
        changed = true;
    }

    // Then recurse into child blocks
    for (auto& stmt : block.stmts()) {
        if (walkStmt(*stmt)) {
            changed = true;
        }
    }

    return changed;
}

// ============================================================================
// walkStmt - recursively walk a statement
// ============================================================================
bool DeferCoalescerPass::walkStmt(Stmt& stmt) {
    switch (stmt.getKind()) {
        case NodeKind::BlockStmt:
            return walkBlock(cast<BlockStmt>(stmt));

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(stmt);
            bool changed = false;
            if (if_stmt.thenBlock()) {
                if (walkBlock(*if_stmt.thenBlock())) changed = true;
            }
            if (if_stmt.elseBlock()) {
                if (walkBlock(*if_stmt.elseBlock())) changed = true;
            }
            return changed;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(stmt);
            if (while_stmt.body()) {
                return walkBlock(*while_stmt.body());
            }
            return false;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(stmt);
            // The inner statement might contain blocks with defers
            return walkStmt(*defer.stmt());
        }

        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(stmt);
            return walkStmt(*errdefer.stmt());
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(stmt);
            return walkStmt(*atomic.inner());
        }

        default:
            return false;
    }
}

// ============================================================================
// coalesceBlock - find and merge consecutive defer statements
//
// When we find two or more consecutive DeferStmts, we merge them into
// a single DeferStmt containing a BlockStmt with all the inner statements.
// The order is reversed because defers execute in reverse order.
//
// Example:
//   defer free(a);  // stmt[0]
//   defer free(b);  // stmt[1]
//   defer free(c);  // stmt[2]
//
// Defers execute in reverse order: free(c), free(b), free(a)
// So the coalesced block should contain: free(c), free(b), free(a)
// Which is exactly the reverse of the declaration order.
// ============================================================================
bool DeferCoalescerPass::coalesceBlock(BlockStmt& block) {
    auto& stmts = block.stmts();
    if (stmts.size() < 2) return false;

    bool changed = false;
    size_t i = 0;

    while (i < stmts.size()) {
        // Find the start of a consecutive run of DeferStmts
        if (stmts[i]->getKind() != NodeKind::DeferStmt) {
            i++;
            continue;
        }

        // We found the start of a defer run. Find how many consecutive
        // defers there are.
        size_t run_start = i;
        size_t run_end = i + 1;

        while (run_end < stmts.size() &&
               stmts[run_end]->getKind() == NodeKind::DeferStmt) {
            run_end++;
        }

        size_t run_length = run_end - run_start;

        // Only coalesce if there are 2+ consecutive defers
        if (run_length >= 2) {
            // Collect all the inner statements from the defers.
            // Defers execute in reverse order, so we reverse the list
            // to get the correct execution order in the coalesced block.
            std::vector<std::unique_ptr<Stmt>> inner_stmts;
            inner_stmts.reserve(run_length);

            // Collect in reverse order (last defer first)
            for (size_t j = run_end; j > run_start; --j) {
                auto& defer_stmt = cast<DeferStmt>(*stmts[j - 1]);
                inner_stmts.push_back(defer_stmt.takeStmt());
            }

            // Create a new BlockStmt containing all the inner statements
            auto coalesced_block = std::make_unique<BlockStmt>(
                stmts[run_start]->sourceLoc(),
                std::move(inner_stmts));

            // Replace the first defer with a new defer wrapping the block
            auto coalesced_defer = std::make_unique<DeferStmt>(
                stmts[run_start]->sourceLoc(),
                std::move(coalesced_block));

            // Annotate the coalesced defer so the IR generator knows
            // this is a coalesced cleanup block
            if (annotations_) {
                annotations_->annotate(coalesced_defer.get(),
                    ASTAnnotationKind::ColdPath,
                    "coalesced_defer:" + std::to_string(run_length));
            }

            // Replace the first statement with the coalesced defer
            stmts[run_start] = std::move(coalesced_defer);

            // Remove the remaining defers from the run (they've been merged)
            // We need to be careful about indices after removal
            for (size_t j = run_end - 1; j > run_start; --j) {
                stmts.erase(stmts.begin() + static_cast<ptrdiff_t>(j));
            }

            coalesced_groups_++;
            changed = true;

            // Advance past the coalesced defer
            i = run_start + 1;
        } else {
            i = run_end;
        }
    }

    return changed;
}

} // namespace tether
