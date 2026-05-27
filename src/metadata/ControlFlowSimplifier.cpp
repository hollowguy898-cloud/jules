#include "metadata/ControlFlowSimplifier.h"
#include "metadata/MetaTypes.h"
#include "ast/AST.h"
#include "sema/Type.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// L2: Control-Flow Simplifier Implementation
// ============================================================================

void ControlFlowSimplifier::simplify(Program& program, TypeTable& type_table, MetadataMap& meta) {
    (void)type_table; // Reserved for future type-aware analysis
    // Walk all top-level declarations, find function bodies, and analyze them
    for (auto& decl : program) {
        if (!decl) continue;

        // Only FnDecl has a body to walk
        if (decl->getKind() == NodeKind::FnDecl) {
            auto& fn = static_cast<FnDecl&>(*decl);
            if (fn.body()) {
                walkStmts(*fn.body(), meta);
            }
        }
    }
}

void ControlFlowSimplifier::walkStmts(BlockStmt& block, MetadataMap& meta) {
    for (auto& stmt : block.stmts()) {
        if (!stmt) continue;

        switch (stmt->getKind()) {
            case NodeKind::IfStmt: {
                auto& if_stmt = static_cast<IfStmt&>(*stmt);
                analyzeIfStmt(if_stmt, meta);
                // Recursively walk the then and else blocks
                if (if_stmt.thenBlock()) {
                    walkStmts(*if_stmt.thenBlock(), meta);
                }
                if (if_stmt.elseBlock()) {
                    walkStmts(*if_stmt.elseBlock(), meta);
                }
                break;
            }
            case NodeKind::WhileStmt: {
                auto& while_stmt = static_cast<WhileStmt&>(*stmt);
                // Walk the while body
                if (while_stmt.body()) {
                    walkStmts(*while_stmt.body(), meta);
                }
                break;
            }
            case NodeKind::BlockStmt: {
                auto& inner_block = static_cast<BlockStmt&>(*stmt);
                walkStmts(inner_block, meta);
                break;
            }
            case NodeKind::VarDeclStmt: {
                // No sub-statements to walk
                break;
            }
            case NodeKind::ValDeclStmt: {
                break;
            }
            case NodeKind::AssignStmt: {
                break;
            }
            case NodeKind::ReturnStmt: {
                break;
            }
            case NodeKind::DeferStmt: {
                auto& defer = static_cast<DeferStmt&>(*stmt);
                if (defer.stmt()) {
                    // Defer wraps a statement which could be a block
                    if (defer.stmt()->getKind() == NodeKind::BlockStmt) {
                        walkStmts(static_cast<BlockStmt&>(*defer.stmt()), meta);
                    }
                }
                break;
            }
            case NodeKind::ExprStmt: {
                break;
            }
            case NodeKind::SwitchStmt: {
                auto& switch_stmt = static_cast<SwitchStmt&>(*stmt);
                for (auto& arm : switch_stmt.arms()) {
                    if (arm.body) {
                        walkStmts(*arm.body, meta);
                    }
                }
                break;
            }
            default:
                break;
        }
    }
}

void ControlFlowSimplifier::analyzeIfStmt(IfStmt& is, MetadataMap& meta) {
    // Get or create metadata for this if statement node
    NodeMeta& node_meta = meta.getOrCreate(&is);

    // ---- Determine branch probability based on condition analysis ----
    Expr* cond = is.condition();
    if (cond) {
        // Check for !expr — the else branch is Likely
        if (cond->getKind() == NodeKind::UnaryExpr) {
            auto& unary = static_cast<UnaryExpr&>(*cond);
            if (unary.op() == UnaryOp::Not) {
                // !expr means the else branch is Likely (i.e., the condition
                // is usually false, so we usually go to else)
                node_meta.branch_prob = BranchProbability::Likely;
            }
        }
        // Check for expr == true or expr != false — then branch is Likely
        else if (cond->getKind() == NodeKind::BinaryExpr) {
            auto& binary = static_cast<BinaryExpr&>(*cond);
            BinaryOp op = binary.op();

            if (op == BinaryOp::Eq) {
                // Check if comparing to true literal
                if (binary.right() && binary.right()->getKind() == NodeKind::BoolLiteral) {
                    auto& lit = static_cast<BoolLiteral&>(*binary.right());
                    if (lit.value()) {
                        // expr == true -> then branch is Likely
                        node_meta.branch_prob = BranchProbability::Likely;
                    } else {
                        // expr == false -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
                // Also check left side for true literal
                else if (binary.left() && binary.left()->getKind() == NodeKind::BoolLiteral) {
                    auto& lit = static_cast<BoolLiteral&>(*binary.left());
                    if (lit.value()) {
                        // true == expr -> then branch is Likely
                        node_meta.branch_prob = BranchProbability::Likely;
                    } else {
                        // false == expr -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
            }
            else if (op == BinaryOp::Ne) {
                // Check if comparing != false
                if (binary.right() && binary.right()->getKind() == NodeKind::BoolLiteral) {
                    auto& lit = static_cast<BoolLiteral&>(*binary.right());
                    if (!lit.value()) {
                        // expr != false -> then branch is Likely
                        node_meta.branch_prob = BranchProbability::Likely;
                    } else {
                        // expr != true -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
                else if (binary.left() && binary.left()->getKind() == NodeKind::BoolLiteral) {
                    auto& lit = static_cast<BoolLiteral&>(*binary.left());
                    if (!lit.value()) {
                        // false != expr -> then branch is Likely
                        node_meta.branch_prob = BranchProbability::Likely;
                    } else {
                        // true != expr -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
            }
            // Check for expr == false or expr == 0 -> then branch Unlikely
            if (op == BinaryOp::Eq && node_meta.branch_prob == BranchProbability::Unknown) {
                // Check for expr == 0 (IntLiteral with value 0)
                if (binary.right() && binary.right()->getKind() == NodeKind::IntLiteral) {
                    auto& lit = static_cast<IntLiteral&>(*binary.right());
                    if (lit.value() == 0) {
                        // expr == 0 -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
                else if (binary.left() && binary.left()->getKind() == NodeKind::IntLiteral) {
                    auto& lit = static_cast<IntLiteral&>(*binary.left());
                    if (lit.value() == 0) {
                        // 0 == expr -> then branch is Unlikely
                        node_meta.branch_prob = BranchProbability::Unlikely;
                    }
                }
            }
        }
        // Default: Unknown (already the default)
    }

    // ---- Determine select profitability ----
    // Both branches must exist for select to make sense
    if (!is.thenBlock() || !is.hasElse() || !is.elseBlock()) {
        // Without an else branch, select conversion is not applicable
        node_meta.select_profit = SelectProfitability::Neutral;
        return;
    }

    // Analyze the then and else branches
    // Both branches must be "pure expressions" — i.e., each branch should
    // be a single ExprStmt, not a VarDecl, Return, or other statement type.
    BlockStmt* then_block = is.thenBlock();
    BlockStmt* else_block = is.elseBlock();

    // Each branch must contain exactly one statement and it must be an ExprStmt
    if (then_block->stmtCount() != 1 || else_block->stmtCount() != 1) {
        node_meta.select_profit = SelectProfitability::Neutral;
        return;
    }

    Stmt* then_stmt = then_block->stmts()[0].get();
    Stmt* else_stmt = else_block->stmts()[0].get();

    if (!then_stmt || !else_stmt) {
        node_meta.select_profit = SelectProfitability::Neutral;
        return;
    }

    // Check that both are ExprStmts (pure expressions, not var decls/returns)
    if (then_stmt->getKind() != NodeKind::ExprStmt ||
        else_stmt->getKind() != NodeKind::ExprStmt) {
        node_meta.select_profit = SelectProfitability::Neutral;
        return;
    }

    auto& then_expr_stmt = static_cast<ExprStmt&>(*then_stmt);
    auto& else_expr_stmt = static_cast<ExprStmt&>(*else_stmt);

    if (!then_expr_stmt.expr() || !else_expr_stmt.expr()) {
        node_meta.select_profit = SelectProfitability::Neutral;
        return;
    }

    Expr& then_expr = *then_expr_stmt.expr();
    Expr& else_expr = *else_expr_stmt.expr();

    // Check for side effects in both branches
    bool then_has_side_effects = hasSideEffects(then_expr);
    bool else_has_side_effects = hasSideEffects(else_expr);

    if (then_has_side_effects || else_has_side_effects) {
        // Side effects mean select is unprofitable (would execute both sides)
        node_meta.select_profit = SelectProfitability::Unprofitable;
        return;
    }

    // Check if both branches are cheap expressions
    bool then_cheap = isCheapExpr(then_expr);
    bool else_cheap = isCheapExpr(else_expr);

    if (!then_cheap || !else_cheap) {
        // At least one branch is expensive — select is unprofitable
        node_meta.select_profit = SelectProfitability::Unprofitable;
        return;
    }

    // Count operations in both branches
    int then_ops = countOps(then_expr);
    int else_ops = countOps(else_expr);

    if (then_ops >= 5 || else_ops >= 5) {
        // Too many operations for select to be worthwhile
        node_meta.select_profit = SelectProfitability::Unprofitable;
        return;
    }

    // Check for function calls in both branches (even cheap ones should not have calls)
    // Already covered by isCheapExpr returning false for CallExpr, but double-check
    // since hasSideEffects catches CallExpr too

    // Check vectorization benefit
    bool vectorization_benefit = wouldBenefitFromVectorization(is, meta);

    if (vectorization_benefit) {
        // Both branches are cheap, no side effects, inside a vectorizable context
        node_meta.select_profit = SelectProfitability::Profitable;
        // Also set vectorization-safe on the node
        node_meta.llvm_meta.vectorization_safe = true;
    } else {
        // Cheap branches, no side effects, but no vectorization benefit
        node_meta.select_profit = SelectProfitability::Neutral;
    }
}

bool ControlFlowSimplifier::isCheapExpr(Expr& expr) const {
    switch (expr.getKind()) {
        // Literals are trivially cheap
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::StringLiteral:
            return true;

        // Variable reads are cheap
        case NodeKind::IdentExpr:
            return true;

        // Member access is cheap if the object is cheap
        case NodeKind::MemberExpr: {
            auto& member = static_cast<MemberExpr&>(expr);
            if (member.object()) {
                return isCheapExpr(*member.object());
            }
            return false;
        }

        // Binary expression is cheap if both operands are cheap and it's
        // an arithmetic/comparison operator (not assignment)
        case NodeKind::BinaryExpr: {
            auto& binary = static_cast<BinaryExpr&>(expr);
            BinaryOp op = binary.op();

            // Assignment operators are not cheap (they write memory)
            if (op == BinaryOp::Assign || op == BinaryOp::AddAssign ||
                op == BinaryOp::SubAssign || op == BinaryOp::MulAssign ||
                op == BinaryOp::DivAssign || op == BinaryOp::ModAssign ||
                op == BinaryOp::AndAssign || op == BinaryOp::OrAssign ||
                op == BinaryOp::XorAssign || op == BinaryOp::ShlAssign ||
                op == BinaryOp::ShrAssign) {
                return false;
            }

            // Arithmetic and comparison operators are cheap if both operands are cheap
            bool left_cheap = binary.left() ? isCheapExpr(*binary.left()) : false;
            bool right_cheap = binary.right() ? isCheapExpr(*binary.right()) : false;
            return left_cheap && right_cheap;
        }

        // Cast expression is cheap if the operand is cheap
        case NodeKind::CastExpr: {
            auto& cast = static_cast<CastExpr&>(expr);
            if (cast.expr()) {
                return isCheapExpr(*cast.expr());
            }
            return false;
        }

        // Unary expressions — check the operand
        case NodeKind::UnaryExpr: {
            auto& unary = static_cast<UnaryExpr&>(expr);
            // Neg, Not, BitNot are cheap if operand is cheap
            if (unary.op() == UnaryOp::Neg || unary.op() == UnaryOp::Not ||
                unary.op() == UnaryOp::BitNot) {
                if (unary.operand()) {
                    return isCheapExpr(*unary.operand());
                }
            }
            // Deref and Addr are not cheap in general (memory access)
            return false;
        }

        // CallExpr — function calls may have side effects
        case NodeKind::CallExpr:
            return false;

        // StructInitExpr — involves memory writes
        case NodeKind::StructInitExpr:
            return false;

        // UnsafeExpr — unknown semantics
        case NodeKind::UnsafeExpr:
            return false;

        // IndexExpr — array access, not trivially cheap
        case NodeKind::IndexExpr:
            return false;

        // DerefExpr — pointer dereference, not cheap
        case NodeKind::DerefExpr:
            return false;

        // AddrOfExpr — address-of, generally cheap but we're conservative
        case NodeKind::AddrOfExpr:
            return false;

        // SelectExpr — already a select, could be cheap but conservative
        case NodeKind::SelectExpr:
            return false;

        // SizeofExpr — compile-time constant, cheap
        case NodeKind::SizeofExpr:
            return true;

        // ArrayInitExpr — involves memory
        case NodeKind::ArrayInitExpr:
            return false;

        // TryExpr — may involve error handling
        case NodeKind::TryExpr:
            return false;

        // ComptimeExpr — compile-time
        case NodeKind::ComptimeExpr:
            return true;

        // ReduceExpr — involves loops/parallelism
        case NodeKind::ReduceExpr:
            return false;

        // PoisonExpr — error node
        case NodeKind::PoisonExpr:
            return false;

        default:
            return false;
    }
}

int ControlFlowSimplifier::countOps(Expr& expr) const {
    switch (expr.getKind()) {
        // Literals have zero operations
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::StringLiteral:
            return 0;

        // IdentExpr is a variable read — zero ops
        case NodeKind::IdentExpr:
            return 0;

        // MemberExpr — 0 ops (just a field access)
        case NodeKind::MemberExpr: {
            auto& member = static_cast<MemberExpr&>(expr);
            int obj_ops = member.object() ? countOps(*member.object()) : 0;
            return obj_ops; // member access itself is 0 ops
        }

        // BinaryExpr — 1 op + left + right
        case NodeKind::BinaryExpr: {
            auto& binary = static_cast<BinaryExpr&>(expr);
            int left_ops = binary.left() ? countOps(*binary.left()) : 0;
            int right_ops = binary.right() ? countOps(*binary.right()) : 0;
            return 1 + left_ops + right_ops;
        }

        // UnaryExpr — 1 op + operand
        case NodeKind::UnaryExpr: {
            auto& unary = static_cast<UnaryExpr&>(expr);
            int operand_ops = unary.operand() ? countOps(*unary.operand()) : 0;
            return 1 + operand_ops;
        }

        // CastExpr — 1 op + operand
        case NodeKind::CastExpr: {
            auto& cast = static_cast<CastExpr&>(expr);
            int expr_ops = cast.expr() ? countOps(*cast.expr()) : 0;
            return 1 + expr_ops;
        }

        // CallExpr — count as expensive (5 ops per arg + the call itself)
        case NodeKind::CallExpr: {
            auto& call = static_cast<CallExpr&>(expr);
            int total = 5; // base cost for the call
            for (auto& arg : call.args()) {
                if (arg) total += countOps(*arg);
            }
            return total;
        }

        // StructInitExpr — count as expensive
        case NodeKind::StructInitExpr:
            return 5;

        // IndexExpr — 1 op + object + index
        case NodeKind::IndexExpr: {
            auto& index = static_cast<IndexExpr&>(expr);
            int obj_ops = index.object() ? countOps(*index.object()) : 0;
            int idx_ops = index.index() ? countOps(*index.index()) : 0;
            return 1 + obj_ops + idx_ops;
        }

        // DerefExpr — 1 op + operand
        case NodeKind::DerefExpr: {
            auto& deref = static_cast<DerefExpr&>(expr);
            int operand_ops = deref.operand() ? countOps(*deref.operand()) : 0;
            return 1 + operand_ops;
        }

        // AddrOfExpr — 0 ops (address is computed at compile time)
        case NodeKind::AddrOfExpr:
            return 0;

        // SelectExpr — 1 op + condition + true_expr + false_expr
        case NodeKind::SelectExpr: {
            auto& sel = static_cast<SelectExpr&>(expr);
            int cond_ops = sel.condition() ? countOps(*sel.condition()) : 0;
            int true_ops = sel.trueExpr() ? countOps(*sel.trueExpr()) : 0;
            int false_ops = sel.falseExpr() ? countOps(*sel.falseExpr()) : 0;
            return 1 + cond_ops + true_ops + false_ops;
        }

        // SizeofExpr — 0 ops (compile-time constant)
        case NodeKind::SizeofExpr:
            return 0;

        // UnsafeExpr — unknown, count inner + 1
        case NodeKind::UnsafeExpr: {
            auto& unsafe = static_cast<UnsafeExpr&>(expr);
            int inner_ops = unsafe.inner() ? countOps(*unsafe.inner()) : 0;
            return 1 + inner_ops;
        }

        // ArrayInitExpr — sum of all elements + 1
        case NodeKind::ArrayInitExpr: {
            auto& arr = static_cast<ArrayInitExpr&>(expr);
            int total = 1;
            for (auto& elem : arr.elements()) {
                if (elem) total += countOps(*elem);
            }
            return total;
        }

        default:
            return 1;
    }
}

bool ControlFlowSimplifier::hasSideEffects(Expr& expr) const {
    switch (expr.getKind()) {
        // CallExpr — function calls may have side effects
        case NodeKind::CallExpr:
            return true;

        // StructInitExpr — involves memory writes
        case NodeKind::StructInitExpr:
            return true;

        // UnsafeExpr — unknown semantics
        case NodeKind::UnsafeExpr:
            return true;

        // DerefExpr on mutable references has side effects
        case NodeKind::DerefExpr: {
            auto& deref = static_cast<DerefExpr&>(expr);
            if (deref.operand()) {
                // Check if the operand's type is a mutable reference
                TypeId op_type = deref.operand()->getType();
                if (op_type && op_type->getKind() == TypeKind::MutReference) {
                    return true;
                }
            }
            // DerefExpr on non-mutable pointers is still a read, but we
            // conservatively check the operand for further side effects
            return deref.operand() ? hasSideEffects(*deref.operand()) : false;
        }

        // BinaryExpr — check operands, and assignment operators are side effects
        case NodeKind::BinaryExpr: {
            auto& binary = static_cast<BinaryExpr&>(expr);
            BinaryOp op = binary.op();
            // Assignment operators write to the target
            if (op == BinaryOp::Assign || op == BinaryOp::AddAssign ||
                op == BinaryOp::SubAssign || op == BinaryOp::MulAssign ||
                op == BinaryOp::DivAssign || op == BinaryOp::ModAssign ||
                op == BinaryOp::AndAssign || op == BinaryOp::OrAssign ||
                op == BinaryOp::XorAssign || op == BinaryOp::ShlAssign ||
                op == BinaryOp::ShrAssign) {
                return true;
            }
            // Check operands recursively
            bool left_se = binary.left() ? hasSideEffects(*binary.left()) : false;
            bool right_se = binary.right() ? hasSideEffects(*binary.right()) : false;
            return left_se || right_se;
        }

        // UnaryExpr — check operand
        case NodeKind::UnaryExpr: {
            auto& unary = static_cast<UnaryExpr&>(expr);
            return unary.operand() ? hasSideEffects(*unary.operand()) : false;
        }

        // MemberExpr — check object
        case NodeKind::MemberExpr: {
            auto& member = static_cast<MemberExpr&>(expr);
            return member.object() ? hasSideEffects(*member.object()) : false;
        }

        // IndexExpr — check object and index
        case NodeKind::IndexExpr: {
            auto& index = static_cast<IndexExpr&>(expr);
            bool obj_se = index.object() ? hasSideEffects(*index.object()) : false;
            bool idx_se = index.index() ? hasSideEffects(*index.index()) : false;
            return obj_se || idx_se;
        }

        // CastExpr — check inner
        case NodeKind::CastExpr: {
            auto& cast = static_cast<CastExpr&>(expr);
            return cast.expr() ? hasSideEffects(*cast.expr()) : false;
        }

        // SelectExpr — check all three sub-expressions
        case NodeKind::SelectExpr: {
            auto& sel = static_cast<SelectExpr&>(expr);
            bool cond_se = sel.condition() ? hasSideEffects(*sel.condition()) : false;
            bool true_se = sel.trueExpr() ? hasSideEffects(*sel.trueExpr()) : false;
            bool false_se = sel.falseExpr() ? hasSideEffects(*sel.falseExpr()) : false;
            return cond_se || true_se || false_se;
        }

        // ArrayInitExpr — check elements
        case NodeKind::ArrayInitExpr: {
            auto& arr = static_cast<ArrayInitExpr&>(expr);
            for (auto& elem : arr.elements()) {
                if (elem && hasSideEffects(*elem)) return true;
            }
            return false;
        }

        // Literals, identifiers, sizeof — no side effects
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::StringLiteral:
        case NodeKind::IdentExpr:
        case NodeKind::SizeofExpr:
            return false;

        // AddrOfExpr — taking an address is not a side effect per se
        case NodeKind::AddrOfExpr: {
            auto& addr = static_cast<AddrOfExpr&>(expr);
            return addr.operand() ? hasSideEffects(*addr.operand()) : false;
        }

        // TryExpr — error propagation is a side effect
        case NodeKind::TryExpr:
            return true;

        // ComptimeExpr — compile-time, no runtime side effects
        case NodeKind::ComptimeExpr: {
            auto& ct = static_cast<ComptimeExpr&>(expr);
            return ct.inner() ? hasSideEffects(*ct.inner()) : false;
        }

        // ReduceExpr — parallel reduction, may have side effects
        case NodeKind::ReduceExpr:
            return true;

        // PoisonExpr — error node
        case NodeKind::PoisonExpr:
            return false;

        default:
            // Conservative: assume side effects for unknown expression types
            return true;
    }
}

bool ControlFlowSimplifier::wouldBenefitFromVectorization(IfStmt& is, MetadataMap& meta) const {
    // For now, return true if any access pattern metadata exists for the function.
    // In a full implementation, we would track whether this if statement is
    // inside a while loop by maintaining a context stack during walkStmts.
    // The simplest heuristic: check if the IfStmt's node already has
    // loop-related metadata, or if any metadata in the map has
    // linear access patterns (indicating hot loops).

    // Check if this if statement has any access patterns associated with it
    const NodeMeta* nm = meta.get(&is);
    if (nm && nm->has_loop_with_linear_access) {
        return true;
    }

    // Check if any node in the metadata map has loop-with-linear-access set.
    // This indicates the function contains hot loops, and if statements
    // inside those loops benefit from vectorization.
    // Since we don't have direct parent tracking, we check the global state.
    // A simpler approach: check if any metadata exists at all with
    // access patterns or vectorization info.
    // For practical purposes, if there are access patterns in the metadata,
    // the L3 layer has identified loops, and this if might be inside one.
    //
    // We iterate through the structs metadata since that's where L3 writes
    // traversal information. If any struct has linear traversal metadata,
    // loops exist in this program.
    for (const auto& [name, struct_meta] : meta.structs()) {
        (void)name; // suppress unused warning
        if (struct_meta.layout != LayoutKind::AoS ||
            struct_meta.transform.kind != TransformKind::None) {
            // Some layout analysis has been done, indicating loops were found
            return true;
        }
    }

    return false;
}

} // namespace tether
