#include "opt/ErrorPathSeparator.h"

#include <cassert>

namespace tether {

// ============================================================================
// ErrorPathSeparatorPass::run
//
// Walks the entire program AST looking for error-path constructs:
// - TryExpr: the error branch should be cold
// - ErrdeferStmt: the cleanup is only for error paths
// - Functions with error types: catch blocks should be cold
// ============================================================================
bool ErrorPathSeparatorPass::run(Program& program, TypeTable& /*type_table*/) {
    annotated_tries_ = 0;
    annotated_catches_ = 0;
    annotated_errdefers_ = 0;

    bool any_annotated = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        walkFn(fn);
    }

    any_annotated = (annotated_tries_ > 0 || annotated_catches_ > 0 ||
                     annotated_errdefers_ > 0);

    return any_annotated;
}

// ============================================================================
// walkFn - process a function declaration
// ============================================================================
void ErrorPathSeparatorPass::walkFn(FnDecl& fn) {
    if (!fn.body()) return;

    // If this function can error, its error-returning paths are cold
    if (fn.canError()) {
        // The function has an error return type, so implicit error returns
        // from try expressions are cold paths. We annotate the function
        // body so the IR generator knows to lay out the error return block
        // in a cold section.
        if (meta_map_) {
            meta_map_->getOrCreate(fn.body()).llvm_meta.cold_path = true;
        }
    }

    walkBlock(fn.body());
}

// ============================================================================
// walkBlock - recursively walk a block of statements
// ============================================================================
void ErrorPathSeparatorPass::walkBlock(BlockStmt* block) {
    if (!block) return;
    for (auto& stmt : block->stmts()) {
        walkStmt(stmt.get());
    }
}

// ============================================================================
// walkStmt - recursively walk a statement
// ============================================================================
void ErrorPathSeparatorPass::walkStmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            walkBlock(&cast<BlockStmt>(*stmt));
            break;

        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit()) walkExpr(var.init());
            break;
        }

        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit()) walkExpr(val.init());
            break;
        }

        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            walkExpr(assign.target());
            walkExpr(assign.value());
            break;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            walkStmt(defer.stmt());
            break;
        }

        case NodeKind::ErrdeferStmt: {
            // Errdefer statements only execute on error paths
            annotateErrdefer(cast<ErrdeferStmt>(*stmt));
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            walkStmt(errdefer.stmt());
            break;
        }

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            walkExpr(if_stmt.condition());
            if (if_stmt.thenBlock()) {
                walkBlock(if_stmt.thenBlock());
            }
            if (if_stmt.elseBlock()) {
                // Check if the else block is an error path (catch block)
                // In Tether, an else after a try expression is the error/catch path
                annotateBlock(if_stmt.elseBlock());
                walkBlock(if_stmt.elseBlock());
            }
            break;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            walkExpr(while_stmt.condition());
            walkBlock(while_stmt.body());
            break;
        }

        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue()) walkExpr(ret.value());
            break;
        }

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            walkExpr(expr_stmt.expr());
            break;
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            walkStmt(atomic.inner());
            break;
        }

        case NodeKind::YieldStmt: {
            auto& yield = cast<YieldStmt>(*stmt);
            if (yield.hasValue()) walkExpr(yield.value());
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// walkExpr - recursively walk an expression looking for TryExpr
// ============================================================================
void ErrorPathSeparatorPass::walkExpr(Expr* expr) {
    if (!expr) return;

    switch (expr->getKind()) {
        case NodeKind::TryExpr: {
            // Try expressions have an implicit error path: if the operand
            // evaluates to an error, the function immediately returns that error.
            // This error path should be marked as cold (unlikely).
            annotateTryExpr(cast<TryExpr>(*expr));
            auto& try_expr = cast<TryExpr>(*expr);
            walkExpr(try_expr.operand());
            break;
        }

        case NodeKind::BinaryExpr: {
            auto& binary = cast<BinaryExpr>(*expr);
            walkExpr(binary.left());
            walkExpr(binary.right());
            break;
        }

        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            walkExpr(unary.operand());
            break;
        }

        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            walkExpr(call.callee());
            for (auto& arg : call.args()) {
                walkExpr(arg.get());
            }
            break;
        }

        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            walkExpr(member.object());
            break;
        }

        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            walkExpr(index.object());
            walkExpr(index.index());
            break;
        }

        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            walkExpr(deref.operand());
            break;
        }

        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            walkExpr(addr.operand());
            break;
        }

        case NodeKind::CastExpr: {
            auto& cast_expr = cast<CastExpr>(*expr);
            walkExpr(cast_expr.expr());
            break;
        }

        case NodeKind::SelectExpr: {
            auto& select = cast<SelectExpr>(*expr);
            walkExpr(select.condition());
            walkExpr(select.trueExpr());
            walkExpr(select.falseExpr());
            break;
        }

        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (auto& field : init.inits()) {
                walkExpr(field.value.get());
            }
            break;
        }

        case NodeKind::ArrayInitExpr: {
            auto& init = cast<ArrayInitExpr>(*expr);
            for (auto& elem : init.elements()) {
                walkExpr(elem.get());
            }
            break;
        }

        case NodeKind::SizeofExpr: {
            auto& sizeof_expr = cast<SizeofExpr>(*expr);
            if (sizeof_expr.isExprOperand()) {
                walkExpr(sizeof_expr.expr());
            }
            break;
        }

        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            walkExpr(unsafe.inner());
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// annotateTryExpr - mark the implicit error path of a try expression as cold
// ============================================================================
void ErrorPathSeparatorPass::annotateTryExpr(TryExpr& expr) {
    // The try expression creates an implicit branch:
    //   - Success path: continue with the unwrapped value (likely/hot)
    //   - Error path: immediately return the error (unlikely/cold)
    //
    // We annotate the TryExpr node with ColdPath + branch weight metadata.
    // The IR generator will emit:
    //   - br i1 %is_error, label %error_return, label %success, !prof !{!"branch_weights", i32 1, i32 1000}
    //   - The error_return block gets the `cold` attribute
    if (meta_map_) {
        auto& nm = meta_map_->getOrCreate(&expr);
        nm.llvm_meta.cold_path = true;
        nm.branch_prob = BranchProbability::Unlikely;
    }
    annotated_tries_++;
}

// ============================================================================
// annotateBlock - mark a block as a cold path (catch block)
// ============================================================================
void ErrorPathSeparatorPass::annotateBlock(BlockStmt* block) {
    if (!block) return;

    // Check if this block contains error-handling code by looking at its
    // contents for patterns like error checks, error returns, etc.
    bool is_error_block = false;

    for (auto& stmt : block->stmts()) {
        // If the block contains a return with an error value, it's a catch block
        if (stmt->getKind() == NodeKind::ReturnStmt) {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && ret.value()->hasType()) {
                TypeId ret_type = ret.value()->getType();
                if (ret_type && ret_type->isError()) {
                    is_error_block = true;
                    break;
                }
            }
        }
        // If the block contains an errdefer, it's error-handling code
        if (stmt->getKind() == NodeKind::ErrdeferStmt) {
            is_error_block = true;
            break;
        }
    }

    if (is_error_block) {
        if (meta_map_) {
            meta_map_->getOrCreate(block).llvm_meta.cold_path = true;
        }
        annotated_catches_++;
    }
}

// ============================================================================
// annotateErrdefer - mark an errdefer statement's body as a cold path
// ============================================================================
void ErrorPathSeparatorPass::annotateErrdefer(ErrdeferStmt& stmt) {
    // Errdefer blocks only execute on error paths, so they are inherently cold
    if (meta_map_) {
        meta_map_->getOrCreate(&stmt).llvm_meta.cold_path = true;
    }
    annotated_errdefers_++;
}

} // namespace tether
