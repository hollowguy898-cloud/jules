#include "ast/ASTCloner.h"

#include <cassert>
#include <utility>

namespace tether {

// ============================================================================
// substituteType — apply type parameter substitution
//
// If the type's canonical string representation matches a type parameter name
// (e.g., "T"), replace it with the concrete type from the substitution map.
// For composite types (pointer, reference, etc.), we don't need to recurse
// because the semantic analyzer will have already resolved inner types —
// type parameters appear only as direct named types in parameter/return
// positions, not nested inside pointer/reference types.
// ============================================================================
TypeId ASTCloner::substituteType(TypeId type) const {
    if (type.isNull()) return type;

    // Check if this type's name matches a type parameter
    auto it = type_subst_.find(type->toString());
    if (it != type_subst_.end()) {
        return it->second;
    }
    return type;
}

// ============================================================================
// cloneExpr — deep-clone an expression with type substitution
// ============================================================================
std::unique_ptr<Expr> ASTCloner::cloneExpr(const Expr* expr) {
    if (!expr) return nullptr;

    std::unique_ptr<Expr> result;

    switch (expr->getKind()) {
        case NodeKind::IntLiteral: {
            auto& lit = cast<IntLiteral>(*expr);
            result = std::make_unique<IntLiteral>(
                lit.sourceLoc(), lit.value(), lit.isSigned());
            break;
        }
        case NodeKind::FloatLiteral: {
            auto& lit = cast<FloatLiteral>(*expr);
            result = std::make_unique<FloatLiteral>(
                lit.sourceLoc(), lit.value());
            break;
        }
        case NodeKind::BoolLiteral: {
            auto& lit = cast<BoolLiteral>(*expr);
            result = std::make_unique<BoolLiteral>(
                lit.sourceLoc(), lit.value());
            break;
        }
        case NodeKind::StringLiteral: {
            auto& lit = cast<StringLiteral>(*expr);
            result = std::make_unique<StringLiteral>(
                lit.sourceLoc(), lit.value());
            break;
        }
        case NodeKind::IdentExpr: {
            auto& ident = cast<IdentExpr>(*expr);
            result = std::make_unique<IdentExpr>(
                ident.sourceLoc(), ident.name());
            break;
        }
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            result = std::make_unique<BinaryExpr>(
                bin.sourceLoc(), bin.op(),
                cloneExpr(bin.left()),
                cloneExpr(bin.right()));
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            result = std::make_unique<UnaryExpr>(
                unary.sourceLoc(), unary.op(),
                cloneExpr(unary.operand()));
            break;
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            std::vector<std::unique_ptr<Expr>> cloned_args;
            for (const auto& arg : call.args()) {
                cloned_args.push_back(cloneExpr(arg.get()));
            }
            result = std::make_unique<CallExpr>(
                call.sourceLoc(),
                cloneExpr(call.callee()),
                std::move(cloned_args));
            break;
        }
        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            result = std::make_unique<MemberExpr>(
                member.sourceLoc(),
                cloneExpr(member.object()),
                member.field());
            break;
        }
        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            result = std::make_unique<IndexExpr>(
                index.sourceLoc(),
                cloneExpr(index.object()),
                cloneExpr(index.index()));
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            result = std::make_unique<DerefExpr>(
                deref.sourceLoc(),
                cloneExpr(deref.operand()));
            break;
        }
        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            result = std::make_unique<AddrOfExpr>(
                addr.sourceLoc(),
                cloneExpr(addr.operand()),
                addr.isMutable());
            break;
        }
        case NodeKind::CastExpr: {
            auto& cast_e = cast<CastExpr>(*expr);
            result = std::make_unique<CastExpr>(
                cast_e.sourceLoc(),
                cloneExpr(cast_e.expr()),
                substituteType(cast_e.targetType()));
            break;
        }
        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            std::vector<DesignatedInit> cloned_inits;
            for (const auto& di : init.inits()) {
                cloned_inits.push_back(cloneDesignatedInit(di));
            }
            result = std::make_unique<StructInitExpr>(
                init.sourceLoc(), init.typeName(),
                std::move(cloned_inits));
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& arr = cast<ArrayInitExpr>(*expr);
            std::vector<std::unique_ptr<Expr>> cloned_elems;
            for (const auto& elem : arr.elements()) {
                cloned_elems.push_back(cloneExpr(elem.get()));
            }
            result = std::make_unique<ArrayInitExpr>(
                arr.sourceLoc(), std::move(cloned_elems));
            break;
        }
        case NodeKind::SliceExpr: {
            auto& slice = cast<SliceExpr>(*expr);
            if (slice.hasEnd()) {
                result = std::make_unique<SliceExpr>(
                    slice.sourceLoc(),
                    cloneExpr(slice.object()),
                    cloneExpr(slice.start()),
                    cloneExpr(slice.end()));
            } else {
                result = std::make_unique<SliceExpr>(
                    slice.sourceLoc(),
                    cloneExpr(slice.object()),
                    cloneExpr(slice.start()));
            }
            break;
        }
        case NodeKind::SizeofExpr: {
            auto& sz = cast<SizeofExpr>(*expr);
            if (sz.isTypeOperand()) {
                result = std::make_unique<SizeofExpr>(
                    sz.sourceLoc(), substituteType(sz.targetType()));
            } else {
                result = std::make_unique<SizeofExpr>(
                    sz.sourceLoc(), cloneExpr(sz.expr()));
            }
            break;
        }
        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            result = std::make_unique<UnsafeExpr>(
                unsafe.sourceLoc(),
                cloneExpr(unsafe.inner()));
            break;
        }
        case NodeKind::PoisonExpr: {
            auto& poison = cast<PoisonExpr>(*expr);
            result = std::make_unique<PoisonExpr>(
                poison.sourceLoc(), poison.message());
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_e = cast<TryExpr>(*expr);
            result = std::make_unique<TryExpr>(
                try_e.sourceLoc(),
                cloneExpr(try_e.operand()));
            break;
        }
        case NodeKind::ComptimeExpr: {
            auto& ct = cast<ComptimeExpr>(*expr);
            result = std::make_unique<ComptimeExpr>(
                ct.sourceLoc(),
                cloneExpr(ct.inner()));
            break;
        }
        case NodeKind::ReduceExpr: {
            auto& reduce = cast<ReduceExpr>(*expr);
            result = std::make_unique<ReduceExpr>(
                reduce.sourceLoc(), reduce.op(),
                cloneExpr(reduce.iterable()),
                reduce.hasAxis() ? cloneExpr(reduce.axis()) : nullptr);
            break;
        }
        case NodeKind::TypeofExpr: {
            auto& typeof_e = cast<TypeofExpr>(*expr);
            result = std::make_unique<TypeofExpr>(
                typeof_e.sourceLoc(),
                cloneExpr(typeof_e.operand()));
            break;
        }
        default:
            // Unknown expression type — return a poison expr as a safe fallback
            result = std::make_unique<PoisonExpr>(
                expr->sourceLoc(), "unhandled expr in ASTCloner");
            break;
    }

    // Copy the expression's type annotation (from semantic analysis),
    // applying type substitution if needed
    if (result && expr->hasType()) {
        result->setType(substituteType(expr->getType()));
    }

    return result;
}

// ============================================================================
// cloneStmt — deep-clone a statement with type substitution
// ============================================================================
std::unique_ptr<Stmt> ASTCloner::cloneStmt(const Stmt* stmt) {
    if (!stmt) return nullptr;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt: {
            return cloneBlock(&cast<BlockStmt>(*stmt));
        }
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            auto result = std::make_unique<VarDeclStmt>(
                var.sourceLoc(), var.name(),
                substituteType(var.declaredType()),
                cloneExpr(var.init()));
            return result;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            auto result = std::make_unique<ValDeclStmt>(
                val.sourceLoc(), val.name(),
                substituteType(val.declaredType()),
                cloneExpr(val.init()));
            return result;
        }
        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            return std::make_unique<AssignStmt>(
                assign.sourceLoc(),
                cloneExpr(assign.target()),
                cloneExpr(assign.value()));
        }
        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            return std::make_unique<DeferStmt>(
                defer.sourceLoc(),
                cloneStmt(defer.stmt()));
        }
        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            return std::make_unique<ErrdeferStmt>(
                errdefer.sourceLoc(),
                cloneStmt(errdefer.stmt()));
        }
        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            return std::make_unique<IfStmt>(
                if_stmt.sourceLoc(),
                cloneExpr(if_stmt.condition()),
                cloneBlock(if_stmt.thenBlock()),
                if_stmt.hasElse() ? cloneBlock(if_stmt.elseBlock()) : nullptr);
        }
        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            return std::make_unique<WhileStmt>(
                while_stmt.sourceLoc(),
                cloneExpr(while_stmt.condition()),
                cloneBlock(while_stmt.body()),
                while_stmt.hasIncrement() ? cloneExpr(while_stmt.increment()) : nullptr);
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            return std::make_unique<ReturnStmt>(
                ret.sourceLoc(),
                ret.hasValue() ? cloneExpr(ret.value()) : nullptr);
        }
        case NodeKind::BreakStmt: {
            auto& brk = cast<BreakStmt>(*stmt);
            return std::make_unique<BreakStmt>(brk.sourceLoc());
        }
        case NodeKind::ContinueStmt: {
            auto& cont = cast<ContinueStmt>(*stmt);
            return std::make_unique<ContinueStmt>(cont.sourceLoc());
        }
        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            return std::make_unique<ExprStmt>(
                expr_stmt.sourceLoc(),
                cloneExpr(expr_stmt.expr()));
        }
        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            return std::make_unique<AtomicStmt>(
                atomic.sourceLoc(),
                cloneStmt(atomic.inner()),
                atomic.ordering());
        }
        case NodeKind::YieldStmt: {
            auto& yield = cast<YieldStmt>(*stmt);
            return std::make_unique<YieldStmt>(
                yield.sourceLoc(),
                yield.hasValue() ? cloneExpr(yield.value()) : nullptr);
        }
        case NodeKind::MatchStmt: {
            auto& match = cast<MatchStmt>(*stmt);
            std::vector<MatchArm> cloned_arms;
            for (const auto& arm : match.arms()) {
                cloned_arms.push_back(cloneMatchArm(arm));
            }
            return std::make_unique<MatchStmt>(
                match.sourceLoc(),
                cloneExpr(match.subject()),
                std::move(cloned_arms));
        }
        case NodeKind::ConstDeclStmt: {
            auto& cd = cast<ConstDeclStmt>(*stmt);
            return std::make_unique<ConstDeclStmt>(
                cd.sourceLoc(), cd.name(),
                substituteType(cd.declaredType()),
                cloneExpr(cd.init()));
        }
        case NodeKind::ParallelForStmt: {
            auto& pf = cast<ParallelForStmt>(*stmt);
            return std::make_unique<ParallelForStmt>(
                pf.sourceLoc(), pf.iteratorName(),
                cloneExpr(pf.iterable()),
                cloneBlock(pf.body()));
        }
        case NodeKind::UnsafeBlockStmt: {
            auto& ub = cast<UnsafeBlockStmt>(*stmt);
            return std::make_unique<UnsafeBlockStmt>(
                ub.sourceLoc(),
                cloneBlock(ub.bodyPtr()));
        }
        default:
            // Fallback: wrap a poison expr in an ExprStmt
            return std::make_unique<ExprStmt>(
                stmt->sourceLoc(),
                std::make_unique<PoisonExpr>(
                    stmt->sourceLoc(), "unhandled stmt in ASTCloner"));
    }
}

// ============================================================================
// cloneBlock — deep-clone a block statement
// ============================================================================
std::unique_ptr<BlockStmt> ASTCloner::cloneBlock(const BlockStmt* block) {
    if (!block) return nullptr;

    std::vector<std::unique_ptr<Stmt>> cloned_stmts;
    for (const auto& stmt : block->stmts()) {
        cloned_stmts.push_back(cloneStmt(stmt.get()));
    }
    return std::make_unique<BlockStmt>(
        block->sourceLoc(), std::move(cloned_stmts));
}

// ============================================================================
// cloneFnDecl — deep-clone a function declaration with type substitution
//
// Creates a complete independent copy of a function. Type parameters are
// substituted in: parameter types, return type, error type, and all
// types within the body. The cloned function has NO type_params_ (it's
// fully concrete after monomorphization).
// ============================================================================
std::unique_ptr<FnDecl> ASTCloner::cloneFnDecl(const FnDecl* fn) {
    if (!fn) return nullptr;

    // Clone parameter types with substitution
    std::vector<FnParam> cloned_params;
    for (const auto& param : fn->params()) {
        FnParam cloned;
        cloned.name = param.name;
        cloned.type = substituteType(param.type);
        cloned.is_mutable = param.is_mutable;
        cloned.is_restrict = param.is_restrict;
        cloned.unresolved_type_name = param.unresolved_type_name;
        cloned_params.push_back(std::move(cloned));
    }

    // Clone the body
    auto cloned_body = cloneBlock(fn->body());

    // Create the new function — NO type_params (it's concrete now)
    // Use direct new instead of make_unique because FnDecl's constructor
    // takes move-only unique_ptr arguments which make_unique can't handle.
    auto result = std::unique_ptr<FnDecl>(new FnDecl(
        fn->sourceLoc(),
        fn->name(),                    // name will be overwritten by caller
        std::move(cloned_params),
        substituteType(fn->returnType()),
        std::move(cloned_body),
        fn->isPure(),
        substituteType(fn->errorType()),
        fn->directives(),
        {},                            // no type_params — fully concrete
        {}                             // no type_param_bounds
    ));

    // Copy additional flags
    result->setInline(fn->isInline());
    result->setNoalloc(fn->isNoalloc());
    result->setAsync(fn->isAsync());

    return result;
}

// ============================================================================
// cloneDesignatedInit — clone a .field = value initializer
// ============================================================================
DesignatedInit ASTCloner::cloneDesignatedInit(const DesignatedInit& init) {
    DesignatedInit result;
    result.field_name = init.field_name;
    result.value = cloneExpr(init.value.get());
    return result;
}

// ============================================================================
// cloneMatchArm — clone a match arm (pattern + body)
// ============================================================================
MatchArm ASTCloner::cloneMatchArm(const MatchArm& arm) {
    MatchArm result;
    result.pattern = cloneExpr(arm.pattern.get());
    result.body = cloneBlock(arm.body.get());
    return result;
}

} // namespace tether
