#include "cfg/CFG.h"

#include <algorithm>
#include <cassert>

namespace tether {

// ============================================================================
// CFGBuilder::build - main entry point
// ============================================================================
std::unique_ptr<CFG> CFGBuilder::build(FnDecl& fn) {
    auto cfg = std::make_unique<CFG>(fn.name());
    cfg_ = cfg.get();

    // Reset state
    break_targets_.clear();
    continue_targets_.clear();
    deferred_stmts_.clear();

    // Create entry and exit nodes
    CFGNode* entry = newBlock("entry");
    CFGNode* exit = newBlock("exit");
    cfg->setEntry(entry);
    cfg->setExit(exit);

    // Build the function body
    if (fn.body()) {
        CFGNode* body_exit = buildBlock(*fn.body(), entry);

        // Connect body exit to the function exit
        if (body_exit && !body_exit->isTerminal()) {
            // Emit deferred statements before exit
            for (auto it = deferred_stmts_.rbegin(); it != deferred_stmts_.rend(); ++it) {
                CFGNode* defer_node = newBlock("defer");
                defer_node->addStmt(*it);
                trackStatement(**it, defer_node);
                body_exit->addSuccessor(defer_node);
                body_exit = defer_node;
            }
            body_exit->addSuccessor(exit);
        }
    } else {
        entry->addSuccessor(exit);
    }

    // Build the O(1) node index for nodeById lookups
    cfg->rebuildNodeIndex();

    return cfg;
}

// ============================================================================
// buildBlock - process a block of statements
// ============================================================================
CFGNode* CFGBuilder::buildBlock(BlockStmt& block, CFGNode* current) {
    for (auto& stmt : block.stmts()) {
        if (!stmt) continue;
        current = buildStmt(*stmt, current);
        if (!current) {
            // Unreachable code after return/break/continue
            // Create a new unreachable block for remaining statements
            current = newBlock("unreachable");
        }
    }
    return current;
}

// ============================================================================
// buildStmt - dispatch to specific statement builders
// ============================================================================
CFGNode* CFGBuilder::buildStmt(Stmt& stmt, CFGNode* current) {
    switch (stmt.getKind()) {
        case NodeKind::VarDeclStmt:
            return buildVarDeclStmt(static_cast<VarDeclStmt&>(stmt), current);
        case NodeKind::ValDeclStmt:
            return buildValDeclStmt(static_cast<ValDeclStmt&>(stmt), current);
        case NodeKind::AssignStmt:
            return buildAssignStmt(static_cast<AssignStmt&>(stmt), current);
        case NodeKind::DeferStmt:
            return buildDeferStmt(static_cast<DeferStmt&>(stmt), current);
        // BUG FIX: Add explicit handling for ErrdeferStmt, AtomicStmt, YieldStmt
        // instead of falling through to the default case.
        case NodeKind::ErrdeferStmt: {
            auto& es = static_cast<ErrdeferStmt&>(stmt);
            current->addStmt(&es);
            trackStatement(es, current);
            // Errdefer stmts are collected for error-path handling (similar to defer)
            deferred_stmts_.push_back(es.stmt());
            return current;
        }
        case NodeKind::AtomicStmt: {
            auto& as = static_cast<AtomicStmt&>(stmt);
            current->addStmt(&as);
            trackStatement(as, current);
            // Build the inner statement for variable tracking
            if (as.inner()) {
                current = buildStmt(*as.inner(), current);
            }
            return current;
        }
        case NodeKind::YieldStmt: {
            auto& ys = static_cast<YieldStmt&>(stmt);
            current->addStmt(&ys);
            trackStatement(ys, current);
            // Yield creates a suspend point in the CFG — model as a
            // split into a yield-resume path
            return current;
        }
        case NodeKind::IfStmt:
            return buildIfStmt(static_cast<IfStmt&>(stmt), current);
        case NodeKind::WhileStmt:
            return buildWhileStmt(static_cast<WhileStmt&>(stmt), current);
        case NodeKind::ReturnStmt:
            return buildReturnStmt(static_cast<ReturnStmt&>(stmt), current);
        case NodeKind::BreakStmt:
            return buildBreakStmt(static_cast<BreakStmt&>(stmt), current);
        case NodeKind::ContinueStmt:
            return buildContinueStmt(static_cast<ContinueStmt&>(stmt), current);
        case NodeKind::ExprStmt:
            return buildExprStmt(static_cast<ExprStmt&>(stmt), current);
        case NodeKind::BlockStmt:
            return buildBlock(static_cast<BlockStmt&>(stmt), current);
        default:
            // Unknown statement type — just add it and continue
            current->addStmt(&stmt);
            trackStatement(stmt, current);
            return current;
    }
}

// ============================================================================
// Statement-specific builders
// ============================================================================

CFGNode* CFGBuilder::buildVarDeclStmt(VarDeclStmt& vd, CFGNode* current) {
    current->addStmt(&vd);
    trackStatement(vd, current);
    current->defineVar(vd.name());

    // Track borrow if the initializer is an AddrOfExpr
    if (vd.hasInit()) {
        std::vector<BorrowInfo> borrows;
        collectBorrows(*vd.init(), borrows);
        for (auto& b : borrows) {
            // The borrow variable is the declared variable
            BorrowInfo info(b.borrowed_var, b.kind, b.origin, vd.name(), b.borrowed_type);
            current->addBorrowCreated(std::move(info));
        }
    }

    return current;
}

CFGNode* CFGBuilder::buildValDeclStmt(ValDeclStmt& vd, CFGNode* current) {
    current->addStmt(&vd);
    trackStatement(vd, current);
    current->defineVar(vd.name());

    // Track borrow if the initializer is an AddrOfExpr
    if (vd.hasInit()) {
        std::vector<BorrowInfo> borrows;
        collectBorrows(*vd.init(), borrows);
        for (auto& b : borrows) {
            BorrowInfo info(b.borrowed_var, b.kind, b.origin, vd.name(), b.borrowed_type);
            current->addBorrowCreated(std::move(info));
        }
    }

    return current;
}

CFGNode* CFGBuilder::buildAssignStmt(AssignStmt& as, CFGNode* current) {
    current->addStmt(&as);
    trackStatement(as, current);

    // Track the definition of the target variable
    std::unordered_set<std::string> defined;
    collectDefinedVars(*as.target(), defined);
    for (const auto& var : defined) {
        current->defineVar(var);
    }

    return current;
}

CFGNode* CFGBuilder::buildDeferStmt(DeferStmt& ds, CFGNode* current) {
    // Defer statements are not emitted immediately;
    // they are collected and emitted at function exit.
    // We still add the statement to the current node for tracking.
    current->addStmt(&ds);
    trackStatement(ds, current);
    deferred_stmts_.push_back(ds.stmt());
    return current;
}

CFGNode* CFGBuilder::buildIfStmt(IfStmt& is, CFGNode* current) {
    // Add the condition to the current node
    current->addStmt(&is);
    trackStatement(is, current);

    // Create merge node
    CFGNode* merge = newBlock("if_merge");

    // Build the then-branch
    CFGNode* then_entry = newBlock("if_then");
    current->addSuccessor(then_entry);
    CFGNode* then_exit = buildBlock(*is.thenBlock(), then_entry);
    if (then_exit && !then_exit->isTerminal()) {
        then_exit->addSuccessor(merge);
    }

    // Build the else-branch (if present)
    if (is.hasElse()) {
        CFGNode* else_entry = newBlock("if_else");
        current->addSuccessor(else_entry);
        CFGNode* else_exit = buildBlock(*is.elseBlock(), else_entry);
        if (else_exit && !else_exit->isTerminal()) {
            else_exit->addSuccessor(merge);
        }
    } else {
        // No else: the false branch goes directly to merge
        current->addSuccessor(merge);
    }

    return merge;
}

CFGNode* CFGBuilder::buildWhileStmt(WhileStmt& ws, CFGNode* current) {
    // Create the loop header (condition evaluation)
    CFGNode* header = newBlock("while_cond");
    current->addSuccessor(header);

    // Create the loop body
    CFGNode* body_entry = newBlock("while_body");
    header->addSuccessor(body_entry);

    // Create the loop exit (after loop)
    CFGNode* loop_exit = newBlock("while_exit");
    header->addSuccessor(loop_exit);

    // Add the condition to the header node
    header->addStmt(&ws);
    trackStatement(ws, header);

    // Push break/continue targets
    break_targets_.push_back(loop_exit);
    continue_targets_.push_back(header);

    // Build the body
    CFGNode* body_exit = buildBlock(*ws.body(), body_entry);

    // Pop break/continue targets
    break_targets_.pop_back();
    continue_targets_.pop_back();

    // Loop back edge: body exit -> header
    if (body_exit && !body_exit->isTerminal()) {
        // If there's an increment expression, add it on the back edge
        if (ws.hasIncrement()) {
            CFGNode* incr_node = newBlock("while_incr");
            incr_node->addStmt(&ws);
            trackStatement(ws, incr_node);
            body_exit->addSuccessor(incr_node);
            incr_node->addSuccessor(header);
        } else {
            body_exit->addSuccessor(header);
        }
    }

    return loop_exit;
}

CFGNode* CFGBuilder::buildReturnStmt(ReturnStmt& rs, CFGNode* current) {
    current->addStmt(&rs);
    trackStatement(rs, current);

    // A return statement connects to the exit node (through deferred stmts)
    CFGNode* exit = cfg_->exit();

    // Create a path through deferred statements
    CFGNode* ret_path = current;
    for (auto it = deferred_stmts_.rbegin(); it != deferred_stmts_.rend(); ++it) {
        CFGNode* defer_node = newBlock("defer_ret");
        defer_node->addStmt(*it);
        trackStatement(**it, defer_node);
        ret_path->addSuccessor(defer_node);
        ret_path = defer_node;
    }
    ret_path->addSuccessor(exit);

    // Return nullptr to signal that subsequent code is unreachable
    return nullptr;
}

CFGNode* CFGBuilder::buildBreakStmt(BreakStmt& bs, CFGNode* current) {
    current->addStmt(&bs);
    trackStatement(bs, current);

    // Connect to the break target (loop exit)
    if (!break_targets_.empty()) {
        current->addSuccessor(break_targets_.back());
    }

    // Return nullptr — code after break is unreachable
    return nullptr;
}

CFGNode* CFGBuilder::buildContinueStmt(ContinueStmt& cs, CFGNode* current) {
    current->addStmt(&cs);
    trackStatement(cs, current);

    // Connect to the continue target (loop header/condition)
    if (!continue_targets_.empty()) {
        current->addSuccessor(continue_targets_.back());
    }

    // Return nullptr — code after continue is unreachable
    return nullptr;
}

CFGNode* CFGBuilder::buildExprStmt(ExprStmt& es, CFGNode* current) {
    current->addStmt(&es);
    trackStatement(es, current);
    return current;
}

// ============================================================================
// Expression analysis for variable/borrow tracking
// ============================================================================

void CFGBuilder::collectUsedVars(Expr& expr, std::unordered_set<std::string>& used) {
    switch (expr.getKind()) {
        case NodeKind::IdentExpr: {
            auto& ie = static_cast<IdentExpr&>(expr);
            used.insert(ie.name());
            break;
        }
        case NodeKind::BinaryExpr: {
            auto& be = static_cast<BinaryExpr&>(expr);
            // For assignments, the LHS is a def, not a use
            if (be.op() == BinaryOp::Assign) {
                collectUsedVars(*be.right(), used);
                // The LHS may still use variables (e.g., a[i] = x uses i)
                // but the target variable itself is defined, not used
                std::unordered_set<std::string> lhs_used;
                collectUsedVars(*be.left(), lhs_used);
                // For simple ident on LHS, it's a def, not a use
                if (be.left()->getKind() != NodeKind::IdentExpr) {
                    used.insert(lhs_used.begin(), lhs_used.end());
                }
            } else {
                collectUsedVars(*be.left(), used);
                collectUsedVars(*be.right(), used);
            }
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& ue = static_cast<UnaryExpr&>(expr);
            collectUsedVars(*ue.operand(), used);
            break;
        }
        case NodeKind::CallExpr: {
            auto& ce = static_cast<CallExpr&>(expr);
            collectUsedVars(*ce.callee(), used);
            for (auto& arg : ce.args()) {
                collectUsedVars(*arg, used);
            }
            break;
        }
        case NodeKind::MemberExpr: {
            auto& me = static_cast<MemberExpr&>(expr);
            collectUsedVars(*me.object(), used);
            break;
        }
        case NodeKind::IndexExpr: {
            auto& ie = static_cast<IndexExpr&>(expr);
            collectUsedVars(*ie.object(), used);
            collectUsedVars(*ie.index(), used);
            break;
        }
        case NodeKind::DerefExpr: {
            auto& de = static_cast<DerefExpr&>(expr);
            collectUsedVars(*de.operand(), used);
            break;
        }
        case NodeKind::AddrOfExpr: {
            auto& ae = static_cast<AddrOfExpr&>(expr);
            // &x creates a borrow of x, but x is not "used" in the traditional
            // sense — however, for liveness analysis, the borrow of x means
            // x must be live. We record it as a use.
            collectUsedVars(*ae.operand(), used);
            break;
        }
        case NodeKind::CastExpr: {
            auto& ce = static_cast<CastExpr&>(expr);
            collectUsedVars(*ce.expr(), used);
            break;
        }
        case NodeKind::SelectExpr: {
            auto& se = static_cast<SelectExpr&>(expr);
            collectUsedVars(*se.condition(), used);
            collectUsedVars(*se.trueExpr(), used);
            collectUsedVars(*se.falseExpr(), used);
            break;
        }
        case NodeKind::StructInitExpr: {
            auto& sie = static_cast<StructInitExpr&>(expr);
            for (auto& init : sie.inits()) {
                if (init.value) {
                    collectUsedVars(*init.value, used);
                }
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& aie = static_cast<ArrayInitExpr&>(expr);
            for (auto& elem : aie.elements()) {
                collectUsedVars(*elem, used);
            }
            break;
        }
        case NodeKind::SizeofExpr: {
            auto& se = static_cast<SizeofExpr&>(expr);
            if (se.isExprOperand() && se.expr()) {
                collectUsedVars(*se.expr(), used);
            }
            break;
        }
        case NodeKind::UnsafeExpr: {
            auto& ue = static_cast<UnsafeExpr&>(expr);
            collectUsedVars(*ue.inner(), used);
            break;
        }
        default:
            // Literals don't use any variables
            break;
    }
}

void CFGBuilder::collectDefinedVars(Expr& expr, std::unordered_set<std::string>& defined) {
    if (expr.getKind() == NodeKind::IdentExpr) {
        auto& ie = static_cast<IdentExpr&>(expr);
        defined.insert(ie.name());
    } else if (expr.getKind() == NodeKind::MemberExpr) {
        auto& me = static_cast<MemberExpr&>(expr);
        collectDefinedVars(*me.object(), defined);
    } else if (expr.getKind() == NodeKind::IndexExpr) {
        auto& ie = static_cast<IndexExpr&>(expr);
        collectDefinedVars(*ie.object(), defined);
    } else if (expr.getKind() == NodeKind::DerefExpr) {
        // *p = ... defines through the pointer, but we can't statically
        // determine the target variable. For now, we don't track this.
    }
}

void CFGBuilder::collectBorrows(Expr& expr, std::vector<BorrowInfo>& borrows) {
    switch (expr.getKind()) {
        case NodeKind::AddrOfExpr: {
            auto& ae = static_cast<AddrOfExpr&>(expr);
            // &x or &mut x creates a borrow
            if (ae.operand()->getKind() == NodeKind::IdentExpr) {
                auto& ie = static_cast<IdentExpr&>(*ae.operand());
                BorrowKind kind = ae.isMutable()
                    ? BorrowKind::MutExclusive
                    : BorrowKind::Shared;
                TypeId borrow_type = ae.getType();
                borrows.emplace_back(
                    ie.name(), kind, ae.sourceLoc(), "", borrow_type);
            } else {
                // Borrowing through a member access or index
                // e.g., &s.field or &a[i]
                std::unordered_set<std::string> used;
                collectUsedVars(*ae.operand(), used);
                BorrowKind kind = ae.isMutable()
                    ? BorrowKind::MutExclusive
                    : BorrowKind::Shared;
                TypeId borrow_type = ae.getType();
                for (const auto& var : used) {
                    borrows.emplace_back(
                        var, kind, ae.sourceLoc(), "", borrow_type);
                }
            }
            break;
        }
        case NodeKind::BinaryExpr: {
            auto& be = static_cast<BinaryExpr&>(expr);
            collectBorrows(*be.left(), borrows);
            collectBorrows(*be.right(), borrows);
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& ue = static_cast<UnaryExpr&>(expr);
            collectBorrows(*ue.operand(), borrows);
            break;
        }
        case NodeKind::CallExpr: {
            auto& ce = static_cast<CallExpr&>(expr);
            collectBorrows(*ce.callee(), borrows);
            for (auto& arg : ce.args()) {
                collectBorrows(*arg, borrows);
            }
            break;
        }
        case NodeKind::SelectExpr: {
            auto& se = static_cast<SelectExpr&>(expr);
            collectBorrows(*se.condition(), borrows);
            collectBorrows(*se.trueExpr(), borrows);
            collectBorrows(*se.falseExpr(), borrows);
            break;
        }
        default:
            // Most expressions don't create borrows
            break;
    }
}

void CFGBuilder::trackStatement(Stmt& stmt, CFGNode* node) {
    // Collect variable uses and definitions from the statement
    switch (stmt.getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& vd = static_cast<VarDeclStmt&>(stmt);
            if (vd.hasInit()) {
                std::unordered_set<std::string> used;
                collectUsedVars(*vd.init(), used);
                for (const auto& var : used) {
                    node->useVar(var);
                }

                std::vector<BorrowInfo> borrows;
                collectBorrows(*vd.init(), borrows);
                for (auto& b : borrows) {
                    BorrowInfo info(b.borrowed_var, b.kind, b.origin, vd.name(), b.borrowed_type);
                    node->addBorrowCreated(std::move(info));
                }
            }
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& vd = static_cast<ValDeclStmt&>(stmt);
            if (vd.hasInit()) {
                std::unordered_set<std::string> used;
                collectUsedVars(*vd.init(), used);
                for (const auto& var : used) {
                    node->useVar(var);
                }

                std::vector<BorrowInfo> borrows;
                collectBorrows(*vd.init(), borrows);
                for (auto& b : borrows) {
                    BorrowInfo info(b.borrowed_var, b.kind, b.origin, vd.name(), b.borrowed_type);
                    node->addBorrowCreated(std::move(info));
                }
            }
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = static_cast<AssignStmt&>(stmt);
            {
                std::unordered_set<std::string> used;
                collectUsedVars(*as.value(), used);
                // Also collect uses from the LHS (e.g., index in a[i])
                if (as.target()->getKind() != NodeKind::IdentExpr) {
                    collectUsedVars(*as.target(), used);
                }
                for (const auto& var : used) {
                    node->useVar(var);
                }
            }
            {
                std::unordered_set<std::string> defined;
                collectDefinedVars(*as.target(), defined);
                for (const auto& var : defined) {
                    node->defineVar(var);
                }
            }
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = static_cast<ExprStmt&>(stmt);
            std::unordered_set<std::string> used;
            collectUsedVars(*es.expr(), used);
            for (const auto& var : used) {
                node->useVar(var);
            }

            std::vector<BorrowInfo> borrows;
            collectBorrows(*es.expr(), borrows);
            for (auto& b : borrows) {
                node->addBorrowCreated(std::move(b));
            }
            break;
        }
        case NodeKind::IfStmt: {
            auto& is = static_cast<IfStmt&>(stmt);
            std::unordered_set<std::string> used;
            collectUsedVars(*is.condition(), used);
            for (const auto& var : used) {
                node->useVar(var);
            }
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = static_cast<WhileStmt&>(stmt);
            std::unordered_set<std::string> used;
            collectUsedVars(*ws.condition(), used);
            for (const auto& var : used) {
                node->useVar(var);
            }
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& rs = static_cast<ReturnStmt&>(stmt);
            if (rs.hasValue()) {
                std::unordered_set<std::string> used;
                collectUsedVars(*rs.value(), used);
                for (const auto& var : used) {
                    node->useVar(var);
                }
            }
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = static_cast<DeferStmt&>(stmt);
            // Track the inner statement's variable usage
            trackStatement(*ds.stmt(), node);
            break;
        }
        default:
            break;
    }
}

} // namespace tether
