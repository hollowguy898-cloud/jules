#include "opt/ComptimeEvaluator.h"
#include "opt/PreLLVMPipeline.h"

#include <cassert>
#include <cstring>
#include <iostream>
#include <sstream>
#include <cmath>
#include <algorithm>

namespace tether {

// ============================================================================
// ComptimeEvaluator::evaluate — main dispatch
// ============================================================================
ComptimeValue ComptimeEvaluator::evaluate(Expr* expr, TypeTable& type_table,
                                           ComptimeEvalContext& ctx) {
    if (!expr) return ComptimeValue::makePoison();

    switch (expr->getKind()) {
        case NodeKind::IntLiteral: {
            auto& lit = cast<IntLiteral>(*expr);
            return ComptimeValue::makeInt(static_cast<int64_t>(lit.value()),
                                          expr->getType());
        }
        case NodeKind::FloatLiteral: {
            auto& lit = cast<FloatLiteral>(*expr);
            return ComptimeValue::makeFloat(lit.value(), expr->getType());
        }
        case NodeKind::BoolLiteral: {
            auto& lit = cast<BoolLiteral>(*expr);
            return ComptimeValue::makeBool(lit.value(), expr->getType());
        }
        case NodeKind::StringLiteral: {
            auto& lit = cast<StringLiteral>(*expr);
            return ComptimeValue::makeString(lit.value(), expr->getType());
        }
        case NodeKind::IdentExpr: {
            auto& id = cast<IdentExpr>(*expr);
            const ComptimeValue* val = ctx.lookup(id.name());
            if (val) return *val;
            return ComptimeValue::makePoison();
        }
        case NodeKind::BinaryExpr: {
            return evalBinary(&cast<BinaryExpr>(*expr), type_table, ctx);
        }
        case NodeKind::UnaryExpr: {
            return evalUnary(&cast<UnaryExpr>(*expr), type_table, ctx);
        }
        case NodeKind::CallExpr: {
            return evalCall(&cast<CallExpr>(*expr), type_table, ctx);
        }
        case NodeKind::StructInitExpr: {
            return evalStructInit(&cast<StructInitExpr>(*expr), type_table, ctx);
        }
        case NodeKind::ArrayInitExpr: {
            return evalArrayInit(&cast<ArrayInitExpr>(*expr), type_table, ctx);
        }
        case NodeKind::MemberExpr: {
            return evalMember(&cast<MemberExpr>(*expr), type_table, ctx);
        }
        case NodeKind::IndexExpr: {
            return evalIndex(&cast<IndexExpr>(*expr), type_table, ctx);
        }
        case NodeKind::SizeofExpr: {
            auto& sf = cast<SizeofExpr>(*expr);
            if (sf.isTypeOperand()) {
                // We need the type size — use a simplified calculation
                // (full typeSizeBytes is in IRGenerator, but we do a basic version)
                TypeId ty = sf.targetType();
                if (ty) {
                    uint64_t sz = ty->bitWidth() / 8;
                    if (sz == 0) sz = 1; // void/zero-size → 1 byte minimum
                    return ComptimeValue::makeInt(static_cast<int64_t>(sz), type_table.getUSize());
                }
            }
            // For expression operand, try to evaluate the type
            if (sf.isExprOperand() && sf.expr() && sf.expr()->hasType()) {
                TypeId ty = sf.expr()->getType();
                if (ty) {
                    uint64_t sz = ty->bitWidth() / 8;
                    if (sz == 0) sz = 1;
                    return ComptimeValue::makeInt(static_cast<int64_t>(sz), type_table.getUSize());
                }
            }
            return ComptimeValue::makePoison();
        }
        case NodeKind::ComptimeExpr: {
            auto& ce = cast<ComptimeExpr>(*expr);
            // Force evaluation of the inner expression
            ComptimeValue val = evaluate(ce.inner(), type_table, ctx);
            if (val.isPoison()) {
                std::cerr << "[tether] error: comptime expression could not be "
                          << "evaluated at compile time: "
                          << expr->sourceLoc().toString() << std::endl;
            }
            return val;
        }
        case NodeKind::CastExpr: {
            auto& ce = cast<CastExpr>(*expr);
            ComptimeValue inner = evaluate(ce.expr(), type_table, ctx);
            if (inner.isPoison()) return ComptimeValue::makePoison();
            TypeId target = ce.targetType();
            // Int-to-int cast
            if (inner.isInt() && target && target->isInteger()) {
                return ComptimeValue::makeInt(inner.asInt(), target);
            }
            // Int-to-float cast
            if (inner.isInt() && target && target->isFloat()) {
                return ComptimeValue::makeFloat(static_cast<double>(inner.asInt()), target);
            }
            // Float-to-int cast
            if (inner.isFloat() && target && target->isInteger()) {
                return ComptimeValue::makeInt(static_cast<int64_t>(inner.asFloat()), target);
            }
            // Float-to-float cast
            if (inner.isFloat() && target && target->isFloat()) {
                return ComptimeValue::makeFloat(inner.asFloat(), target);
            }
            // Bool-to-int cast
            if (inner.isBool() && target && target->isInteger()) {
                return ComptimeValue::makeInt(inner.asBool() ? 1 : 0, target);
            }
            // Int-to-bool cast
            if (inner.isInt() && target && target->isBool()) {
                return ComptimeValue::makeBool(inner.asInt() != 0, target);
            }
            // Same kind — just update type
            if (inner.isInt()) return ComptimeValue::makeInt(inner.asInt(), target);
            if (inner.isFloat()) return ComptimeValue::makeFloat(inner.asFloat(), target);
            if (inner.isBool()) return ComptimeValue::makeBool(inner.asBool(), target);
            return inner; // passthrough
        }
        default:
            return ComptimeValue::makePoison();
    }
}

// ============================================================================
// evalBinary — evaluate binary expressions at compile time
// ============================================================================
ComptimeValue ComptimeEvaluator::evalBinary(BinaryExpr* bin, TypeTable& tt,
                                             ComptimeEvalContext& ctx) {
    BinaryOp op = bin->op();

    // Short-circuit for logical operators
    if (op == BinaryOp::And) {
        ComptimeValue left = evaluate(bin->left(), tt, ctx);
        if (left.isPoison()) return ComptimeValue::makePoison();
        if (!left.isBool()) return ComptimeValue::makePoison();
        if (!left.asBool()) return ComptimeValue::makeBool(false, bin->getType());
        ComptimeValue right = evaluate(bin->right(), tt, ctx);
        if (right.isPoison()) return ComptimeValue::makePoison();
        if (!right.isBool()) return ComptimeValue::makePoison();
        return ComptimeValue::makeBool(right.asBool(), bin->getType());
    }
    if (op == BinaryOp::Or) {
        ComptimeValue left = evaluate(bin->left(), tt, ctx);
        if (left.isPoison()) return ComptimeValue::makePoison();
        if (!left.isBool()) return ComptimeValue::makePoison();
        if (left.asBool()) return ComptimeValue::makeBool(true, bin->getType());
        ComptimeValue right = evaluate(bin->right(), tt, ctx);
        if (right.isPoison()) return ComptimeValue::makePoison();
        if (!right.isBool()) return ComptimeValue::makePoison();
        return ComptimeValue::makeBool(right.asBool(), bin->getType());
    }

    // For all other operators, evaluate both sides
    ComptimeValue left = evaluate(bin->left(), tt, ctx);
    ComptimeValue right = evaluate(bin->right(), tt, ctx);
    if (left.isPoison() || right.isPoison()) return ComptimeValue::makePoison();

    // String concatenation
    if (op == BinaryOp::Add && left.isString() && right.isString()) {
        return ComptimeValue::makeString(left.asString() + right.asString(),
                                          bin->getType());
    }

    // Integer arithmetic
    if (left.isInt() && right.isInt()) {
        int64_t lv = left.asInt();
        int64_t rv = right.asInt();
        switch (op) {
            case BinaryOp::Add: return ComptimeValue::makeInt(lv + rv, bin->getType());
            case BinaryOp::Sub: return ComptimeValue::makeInt(lv - rv, bin->getType());
            case BinaryOp::Mul: return ComptimeValue::makeInt(lv * rv, bin->getType());
            case BinaryOp::Div:
                if (rv == 0) return ComptimeValue::makePoison();
                // Signed vs unsigned division
                if (bin->left()->getType() && bin->left()->getType()->isSigned()) {
                    return ComptimeValue::makeInt(lv / rv, bin->getType());
                }
                return ComptimeValue::makeInt(
                    static_cast<int64_t>(static_cast<uint64_t>(lv) / static_cast<uint64_t>(rv)),
                    bin->getType());
            case BinaryOp::Mod:
                if (rv == 0) return ComptimeValue::makePoison();
                return ComptimeValue::makeInt(lv % rv, bin->getType());
            case BinaryOp::Eq:  return ComptimeValue::makeBool(lv == rv, tt.getBool());
            case BinaryOp::Ne:  return ComptimeValue::makeBool(lv != rv, tt.getBool());
            case BinaryOp::Lt:  return ComptimeValue::makeBool(lv < rv, tt.getBool());
            case BinaryOp::Le:  return ComptimeValue::makeBool(lv <= rv, tt.getBool());
            case BinaryOp::Gt:  return ComptimeValue::makeBool(lv > rv, tt.getBool());
            case BinaryOp::Ge:  return ComptimeValue::makeBool(lv >= rv, tt.getBool());
            case BinaryOp::BitAnd: return ComptimeValue::makeInt(lv & rv, bin->getType());
            case BinaryOp::BitOr:  return ComptimeValue::makeInt(lv | rv, bin->getType());
            case BinaryOp::BitXor: return ComptimeValue::makeInt(lv ^ rv, bin->getType());
            case BinaryOp::Shl: return ComptimeValue::makeInt(lv << rv, bin->getType());
            case BinaryOp::Shr: return ComptimeValue::makeInt(lv >> rv, bin->getType());
            default: break;
        }
    }

    // Float arithmetic
    if (left.isFloat() && right.isFloat()) {
        double lv = left.asFloat();
        double rv = right.asFloat();
        switch (op) {
            case BinaryOp::Add: return ComptimeValue::makeFloat(lv + rv, bin->getType());
            case BinaryOp::Sub: return ComptimeValue::makeFloat(lv - rv, bin->getType());
            case BinaryOp::Mul: return ComptimeValue::makeFloat(lv * rv, bin->getType());
            case BinaryOp::Div:
                if (rv == 0.0) return ComptimeValue::makePoison();
                return ComptimeValue::makeFloat(lv / rv, bin->getType());
            case BinaryOp::Mod:
                if (rv == 0.0) return ComptimeValue::makePoison();
                return ComptimeValue::makeFloat(std::fmod(lv, rv), bin->getType());
            case BinaryOp::Eq:  return ComptimeValue::makeBool(lv == rv, tt.getBool());
            case BinaryOp::Ne:  return ComptimeValue::makeBool(lv != rv, tt.getBool());
            case BinaryOp::Lt:  return ComptimeValue::makeBool(lv < rv, tt.getBool());
            case BinaryOp::Le:  return ComptimeValue::makeBool(lv <= rv, tt.getBool());
            case BinaryOp::Gt:  return ComptimeValue::makeBool(lv > rv, tt.getBool());
            case BinaryOp::Ge:  return ComptimeValue::makeBool(lv >= rv, tt.getBool());
            default: break;
        }
    }

    // Mixed int/float: promote to float
    if ((left.isInt() && right.isFloat()) || (left.isFloat() && right.isInt())) {
        double lv = left.isFloat() ? left.asFloat() : static_cast<double>(left.asInt());
        double rv = right.isFloat() ? right.asFloat() : static_cast<double>(right.asInt());
        switch (op) {
            case BinaryOp::Add: return ComptimeValue::makeFloat(lv + rv, bin->getType());
            case BinaryOp::Sub: return ComptimeValue::makeFloat(lv - rv, bin->getType());
            case BinaryOp::Mul: return ComptimeValue::makeFloat(lv * rv, bin->getType());
            case BinaryOp::Div:
                if (rv == 0.0) return ComptimeValue::makePoison();
                return ComptimeValue::makeFloat(lv / rv, bin->getType());
            case BinaryOp::Eq:  return ComptimeValue::makeBool(lv == rv, tt.getBool());
            case BinaryOp::Ne:  return ComptimeValue::makeBool(lv != rv, tt.getBool());
            case BinaryOp::Lt:  return ComptimeValue::makeBool(lv < rv, tt.getBool());
            case BinaryOp::Le:  return ComptimeValue::makeBool(lv <= rv, tt.getBool());
            case BinaryOp::Gt:  return ComptimeValue::makeBool(lv > rv, tt.getBool());
            case BinaryOp::Ge:  return ComptimeValue::makeBool(lv >= rv, tt.getBool());
            default: break;
        }
    }

    // Bool comparisons
    if (left.isBool() && right.isBool()) {
        bool lv = left.asBool();
        bool rv = right.asBool();
        switch (op) {
            case BinaryOp::Eq: return ComptimeValue::makeBool(lv == rv, tt.getBool());
            case BinaryOp::Ne: return ComptimeValue::makeBool(lv != rv, tt.getBool());
            default: break;
        }
    }

    // String comparisons
    if (left.isString() && right.isString()) {
        const auto& lv = left.asString();
        const auto& rv = right.asString();
        switch (op) {
            case BinaryOp::Eq: return ComptimeValue::makeBool(lv == rv, tt.getBool());
            case BinaryOp::Ne: return ComptimeValue::makeBool(lv != rv, tt.getBool());
            case BinaryOp::Lt: return ComptimeValue::makeBool(lv < rv, tt.getBool());
            case BinaryOp::Le: return ComptimeValue::makeBool(lv <= rv, tt.getBool());
            case BinaryOp::Gt: return ComptimeValue::makeBool(lv > rv, tt.getBool());
            case BinaryOp::Ge: return ComptimeValue::makeBool(lv >= rv, tt.getBool());
            default: break;
        }
    }

    return ComptimeValue::makePoison();
}

// ============================================================================
// evalUnary — evaluate unary expressions at compile time
// ============================================================================
ComptimeValue ComptimeEvaluator::evalUnary(UnaryExpr* un, TypeTable& tt,
                                            ComptimeEvalContext& ctx) {
    ComptimeValue operand = evaluate(un->operand(), tt, ctx);
    if (operand.isPoison()) return ComptimeValue::makePoison();

    switch (un->op()) {
        case UnaryOp::Neg:
            if (operand.isInt()) return ComptimeValue::makeInt(-operand.asInt(), un->getType());
            if (operand.isFloat()) return ComptimeValue::makeFloat(-operand.asFloat(), un->getType());
            break;
        case UnaryOp::Not:
            if (operand.isBool()) return ComptimeValue::makeBool(!operand.asBool(), tt.getBool());
            if (operand.isInt()) return ComptimeValue::makeBool(operand.asInt() == 0, tt.getBool());
            break;
        case UnaryOp::BitNot:
            if (operand.isInt()) return ComptimeValue::makeInt(~operand.asInt(), un->getType());
            break;
        default:
            break;
    }
    return ComptimeValue::makePoison();
}

// ============================================================================
// evalCall — evaluate function calls at compile time
//
// Only evaluates pure, known-builtin functions. Everything else returns Poison.
// ============================================================================
ComptimeValue ComptimeEvaluator::evalCall(CallExpr* call, TypeTable& tt,
                                           ComptimeEvalContext& ctx) {
    // Only evaluate calls to known pure builtins
    Expr* callee = call->callee();
    if (!callee) return ComptimeValue::makePoison();

    // Check for known comptime-evaluable builtins
    std::string callee_name;
    if (callee->getKind() == NodeKind::IdentExpr) {
        callee_name = cast<IdentExpr>(*callee).name();
    } else if (callee->getKind() == NodeKind::MemberExpr) {
        // Could be something like @typeInfo or std.math.abs — skip for now
        return ComptimeValue::makePoison();
    } else {
        return ComptimeValue::makePoison();
    }

    // Known pure builtins that can be evaluated at compile time
    if (callee_name == "sizeof" && call->argCount() == 1) {
        // sizeof builtin — evaluate the type size
        Expr* arg = call->args()[0].get();
        if (arg->hasType()) {
            TypeId ty = arg->getType();
            if (ty) {
                uint64_t sz = ty->bitWidth() / 8;
                if (sz == 0) sz = 1;
                return ComptimeValue::makeInt(static_cast<int64_t>(sz), tt.getUSize());
            }
        }
    }

    // For all other function calls, return Poison — they might have side effects
    // or depend on runtime state.
    return ComptimeValue::makePoison();
}

// ============================================================================
// evalStructInit — evaluate struct initializers with all-constant fields
// ============================================================================
ComptimeValue ComptimeEvaluator::evalStructInit(StructInitExpr* init, TypeTable& tt,
                                                 ComptimeEvalContext& ctx) {
    ComptimeValue result;
    result.kind = ComptimeValue::Kind::Struct;
    result.type = init->getType();
    result.int_val = 0; // zero the union

    for (const auto& field : init->inits()) {
        ComptimeValue field_val = evaluate(field.value.get(), tt, ctx);
        if (field_val.isPoison()) return ComptimeValue::makePoison();
        result.elements.push_back(std::move(field_val));
        result.field_names.push_back(field.field_name);
    }

    return result;
}

// ============================================================================
// evalArrayInit — evaluate array initializers with all-constant elements
// ============================================================================
ComptimeValue ComptimeEvaluator::evalArrayInit(ArrayInitExpr* init, TypeTable& tt,
                                                ComptimeEvalContext& ctx) {
    ComptimeValue result;
    result.kind = ComptimeValue::Kind::Array;
    result.type = init->getType();
    result.int_val = 0; // zero the union

    for (const auto& elem : init->elements()) {
        ComptimeValue elem_val = evaluate(elem.get(), tt, ctx);
        if (elem_val.isPoison()) return ComptimeValue::makePoison();
        result.elements.push_back(std::move(elem_val));
    }

    return result;
}

// ============================================================================
// evalMember — evaluate member access on comptime-known structs
// ============================================================================
ComptimeValue ComptimeEvaluator::evalMember(MemberExpr* member, TypeTable& tt,
                                             ComptimeEvalContext& ctx) {
    ComptimeValue obj = evaluate(member->object(), tt, ctx);
    if (obj.isPoison()) return ComptimeValue::makePoison();

    if (obj.isStruct()) {
        const std::string& field = member->field();
        for (size_t i = 0; i < obj.field_names.size(); ++i) {
            if (obj.field_names[i] == field) {
                return obj.elements[i];
            }
        }
    }

    return ComptimeValue::makePoison();
}

// ============================================================================
// evalIndex — evaluate index access on comptime-known arrays
// ============================================================================
ComptimeValue ComptimeEvaluator::evalIndex(IndexExpr* index, TypeTable& tt,
                                            ComptimeEvalContext& ctx) {
    ComptimeValue obj = evaluate(index->object(), tt, ctx);
    if (obj.isPoison()) return ComptimeValue::makePoison();

    ComptimeValue idx = evaluate(index->index(), tt, ctx);
    if (idx.isPoison() || !idx.isInt()) return ComptimeValue::makePoison();

    int64_t i = idx.asInt();

    if (obj.isArray()) {
        if (i < 0 || i >= static_cast<int64_t>(obj.elements.size())) {
            return ComptimeValue::makePoison();
        }
        return obj.elements[static_cast<size_t>(i)];
    }

    return ComptimeValue::makePoison();
}

// ============================================================================
// evaluateStmt — evaluate a statement at compile time
// ============================================================================
bool ComptimeEvaluator::evaluateStmt(Stmt* stmt, TypeTable& type_table,
                                      ComptimeEvalContext& ctx) {
    if (!stmt) return false;

    switch (stmt->getKind()) {
        case NodeKind::ConstDeclStmt: {
            auto& decl = cast<ConstDeclStmt>(*stmt);
            if (!decl.hasInit()) return false;
            ComptimeValue val = evaluate(decl.init(), type_table, ctx);
            if (val.isPoison()) return false;
            ctx.bind(decl.name(), val);
            return true;
        }
        case NodeKind::ValDeclStmt: {
            auto& decl = cast<ValDeclStmt>(*stmt);
            if (!decl.hasInit()) return false;
            ComptimeValue val = evaluate(decl.init(), type_table, ctx);
            if (val.isPoison()) return false;
            ctx.bindInScope(decl.name(), val);
            return true;
        }
        case NodeKind::BlockStmt: {
            return evalBlock(&cast<BlockStmt>(*stmt), type_table, ctx);
        }
        case NodeKind::StaticAssertStmt: {
            auto& sa = cast<StaticAssertStmt>(*stmt);
            ComptimeValue cond = evaluate(sa.condition(), type_table, ctx);
            if (cond.isPoison()) {
                std::cerr << "[tether] error: static_assert condition cannot be "
                          << "evaluated at compile time: "
                          << stmt->sourceLoc().toString() << std::endl;
                return false;
            }
            if (!cond.isBool() || !cond.asBool()) {
                std::cerr << "[tether] error: static_assert failed";
                if (!sa.message().empty()) {
                    std::cerr << ": " << sa.message();
                }
                std::cerr << " — " << stmt->sourceLoc().toString() << std::endl;
                return false;
            }
            return true;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            ComptimeValue val = evaluate(es.expr(), type_table, ctx);
            return !val.isPoison();
        }
        default:
            return false;
    }
}

// ============================================================================
// evalBlock — evaluate a block of statements
// ============================================================================
bool ComptimeEvaluator::evalBlock(BlockStmt* block, TypeTable& tt,
                                   ComptimeEvalContext& ctx) {
    if (!block) return false;
    ctx.pushScope();
    bool all_evaluated = true;
    for (const auto& stmt : block->stmts()) {
        if (!evaluateStmt(stmt.get(), tt, ctx)) {
            all_evaluated = false;
        }
    }
    ctx.popScope();
    return all_evaluated;
}

// ============================================================================
// isComptimeEvaluatable — check if an expression can be evaluated at compile time
// ============================================================================
bool ComptimeEvaluator::isComptimeEvaluatable(Expr* expr,
                                                const ComptimeEvalContext& ctx) const {
    if (!expr) return false;

    switch (expr->getKind()) {
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::StringLiteral:
            return true;

        case NodeKind::IdentExpr: {
            auto& id = cast<IdentExpr>(*expr);
            return ctx.lookup(id.name()) != nullptr;
        }

        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            // Short-circuit operators: only need to check left first
            if (bin.op() == BinaryOp::And || bin.op() == BinaryOp::Or) {
                return isComptimeEvaluatable(bin.left(), ctx) &&
                       isComptimeEvaluatable(bin.right(), ctx);
            }
            return isComptimeEvaluatable(bin.left(), ctx) &&
                   isComptimeEvaluatable(bin.right(), ctx);
        }

        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            return isComptimeEvaluatable(un.operand(), ctx);
        }

        case NodeKind::SizeofExpr:
            return true;

        case NodeKind::ComptimeExpr: {
            auto& ce = cast<ComptimeExpr>(*expr);
            return isComptimeEvaluatable(ce.inner(), ctx);
        }

        case NodeKind::CastExpr: {
            auto& ce = cast<CastExpr>(*expr);
            return isComptimeEvaluatable(ce.expr(), ctx);
        }

        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (const auto& field : init.inits()) {
                if (!isComptimeEvaluatable(field.value.get(), ctx)) return false;
            }
            return true;
        }

        case NodeKind::ArrayInitExpr: {
            auto& init = cast<ArrayInitExpr>(*expr);
            for (const auto& elem : init.elements()) {
                if (!isComptimeEvaluatable(elem.get(), ctx)) return false;
            }
            return true;
        }

        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            return isComptimeEvaluatable(member.object(), ctx);
        }

        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            return isComptimeEvaluatable(index.object(), ctx) &&
                   isComptimeEvaluatable(index.index(), ctx);
        }

        default:
            return false;
    }
}

// ============================================================================
// ComptimeEvalPass — the PreLLVMPass implementation
// ============================================================================

void ComptimeEvalPass::storeComptimeValue(const void* node, const ComptimeValue& val) {
    comptime_values_[node] = val;

    // Also store in the MetadataMap
    if (meta_map_) {
        NodeMeta& nm = meta_map_->getOrCreate(node);
        nm.comptime_evaluated = true;
        switch (val.kind) {
            case ComptimeValue::Kind::Int:
                nm.comptime_int_value = val.asInt();
                break;
            case ComptimeValue::Kind::Float:
                nm.comptime_float_value = val.asFloat();
                break;
            case ComptimeValue::Kind::Bool:
                nm.comptime_bool_value = val.asBool();
                break;
            case ComptimeValue::Kind::String:
                nm.comptime_string_value = val.asString();
                break;
            default:
                break;
        }
    }
}

void ComptimeEvalPass::processTopLevel(TopLevel* tl, TypeTable& tt,
                                        ComptimeEvalContext& ctx) {
    if (!tl) return;

    switch (tl->getKind()) {
        case NodeKind::ConstDeclStmt: {
            auto& decl = cast<ConstDeclStmt>(*tl);
            if (decl.hasInit()) {
                ComptimeValue val = evaluator_.evaluate(decl.init(), tt, ctx);
                if (!val.isPoison()) {
                    ctx.bind(decl.name(), val);
                    storeComptimeValue(decl.init(), val);
                    storeComptimeValue(tl, val);
                }
            }
            break;
        }
        case NodeKind::StaticAssertStmt: {
            auto& sa = cast<StaticAssertStmt>(*tl);
            ComptimeValue cond = evaluator_.evaluate(sa.condition(), tt, ctx);
            if (cond.isPoison()) {
                std::cerr << "[tether] error: static_assert condition cannot be "
                          << "evaluated at compile time: "
                          << tl->sourceLoc().toString() << std::endl;
            } else if (!cond.isBool() || !cond.asBool()) {
                std::cerr << "[tether] error: static_assert failed";
                if (!sa.message().empty()) {
                    std::cerr << ": " << sa.message();
                }
                std::cerr << " — " << tl->sourceLoc().toString() << std::endl;
            }
            break;
        }
        case NodeKind::FnDecl: {
            processFnBody(&cast<FnDecl>(*tl), tt, ctx);
            break;
        }
        default:
            break;
    }
}

void ComptimeEvalPass::processFnBody(FnDecl* fn, TypeTable& tt,
                                      ComptimeEvalContext& ctx) {
    if (!fn || !fn->body()) return;

    // Create a new scope for the function
    ctx.pushScope();

    // Bind function parameters — they are runtime values, so don't bind them.
    // Only comptime-known values within the function body can be evaluated.

    // Walk the function body looking for comptime expressions
    for (const auto& stmt : fn->body()->stmts()) {
        processStmt(stmt.get(), tt, ctx);
    }

    ctx.popScope();
}

void ComptimeEvalPass::processStmt(Stmt* stmt, TypeTable& tt,
                                    ComptimeEvalContext& ctx) {
    if (!stmt) return;

    switch (stmt->getKind()) {
        case NodeKind::ConstDeclStmt: {
            auto& decl = cast<ConstDeclStmt>(*stmt);
            if (decl.hasInit()) {
                ComptimeValue val = evaluator_.evaluate(decl.init(), tt, ctx);
                if (!val.isPoison()) {
                    ctx.bindInScope(decl.name(), val);
                    storeComptimeValue(decl.init(), val);
                    // Also mark the ConstDeclStmt itself
                    if (meta_map_) {
                        meta_map_->getOrCreate(stmt).comptime_evaluated = true;
                    }
                }
            }
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& decl = cast<ValDeclStmt>(*stmt);
            if (decl.hasInit()) {
                ComptimeValue val = evaluator_.evaluate(decl.init(), tt, ctx);
                if (!val.isPoison()) {
                    ctx.bindInScope(decl.name(), val);
                    storeComptimeValue(decl.init(), val);
                }
            }
            break;
        }
        case NodeKind::VarDeclStmt: {
            auto& decl = cast<VarDeclStmt>(*stmt);
            if (decl.hasInit()) {
                ComptimeValue val = evaluator_.evaluate(decl.init(), tt, ctx);
                if (!val.isPoison()) {
                    ctx.bindInScope(decl.name(), val);
                    storeComptimeValue(decl.init(), val);
                }
            }
            break;
        }
        case NodeKind::StaticAssertStmt: {
            auto& sa = cast<StaticAssertStmt>(*stmt);
            ComptimeValue cond = evaluator_.evaluate(sa.condition(), tt, ctx);
            if (cond.isPoison()) {
                std::cerr << "[tether] error: static_assert condition cannot be "
                          << "evaluated at compile time: "
                          << stmt->sourceLoc().toString() << std::endl;
            } else if (!cond.isBool() || !cond.asBool()) {
                std::cerr << "[tether] error: static_assert failed";
                if (!sa.message().empty()) {
                    std::cerr << ": " << sa.message();
                }
                std::cerr << " — " << stmt->sourceLoc().toString() << std::endl;
            }
            // Mark the condition as comptime-evaluated
            if (!cond.isPoison()) {
                storeComptimeValue(sa.condition(), cond);
            }
            break;
        }
        case NodeKind::BlockStmt: {
            ctx.pushScope();
            for (const auto& s : cast<BlockStmt>(*stmt).stmts()) {
                processStmt(s.get(), tt, ctx);
            }
            ctx.popScope();
            break;
        }
        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            processExpr(if_stmt.condition(), tt, ctx);
            if (if_stmt.thenBlock()) {
                ctx.pushScope();
                for (const auto& s : if_stmt.thenBlock()->stmts()) {
                    processStmt(s.get(), tt, ctx);
                }
                ctx.popScope();
            }
            if (if_stmt.elseBlock()) {
                ctx.pushScope();
                for (const auto& s : if_stmt.elseBlock()->stmts()) {
                    processStmt(s.get(), tt, ctx);
                }
                ctx.popScope();
            }
            break;
        }
        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            processExpr(while_stmt.condition(), tt, ctx);
            if (while_stmt.body()) {
                ctx.pushScope();
                for (const auto& s : while_stmt.body()->stmts()) {
                    processStmt(s.get(), tt, ctx);
                }
                ctx.popScope();
            }
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            processExpr(es.expr(), tt, ctx);
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            processStmt(ds.stmt(), tt, ctx);
            break;
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            processExpr(ms.subject(), tt, ctx);
            for (const auto& arm : ms.arms()) {
                if (arm.body) {
                    ctx.pushScope();
                    for (const auto& s : arm.body->stmts()) {
                        processStmt(s.get(), tt, ctx);
                    }
                    ctx.popScope();
                }
            }
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            processExpr(as.value(), tt, ctx);
            processExpr(as.target(), tt, ctx);
            break;
        }
        default:
            break;
    }
}

void ComptimeEvalPass::processExpr(Expr* expr, TypeTable& tt,
                                    ComptimeEvalContext& ctx) {
    if (!expr) return;

    // Try to evaluate this expression
    ComptimeValue val = evaluator_.evaluate(expr, tt, ctx);
    if (!val.isPoison()) {
        storeComptimeValue(expr, val);
    }

    // Recurse into sub-expressions to find more comptime opportunities
    switch (expr->getKind()) {
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            processExpr(bin.left(), tt, ctx);
            processExpr(bin.right(), tt, ctx);
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            processExpr(un.operand(), tt, ctx);
            break;
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            processExpr(call.callee(), tt, ctx);
            for (const auto& arg : call.args()) {
                processExpr(arg.get(), tt, ctx);
            }
            break;
        }
        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            processExpr(member.object(), tt, ctx);
            break;
        }
        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            processExpr(index.object(), tt, ctx);
            processExpr(index.index(), tt, ctx);
            break;
        }
        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (const auto& field : init.inits()) {
                processExpr(field.value.get(), tt, ctx);
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& init = cast<ArrayInitExpr>(*expr);
            for (const auto& elem : init.elements()) {
                processExpr(elem.get(), tt, ctx);
            }
            break;
        }
        case NodeKind::ComptimeExpr: {
            auto& ce = cast<ComptimeExpr>(*expr);
            // Force evaluation of the inner expression
            ComptimeValue inner_val = evaluator_.evaluate(ce.inner(), tt, ctx);
            if (!inner_val.isPoison()) {
                storeComptimeValue(ce.inner(), inner_val);
            }
            processExpr(ce.inner(), tt, ctx);
            break;
        }
        case NodeKind::CastExpr: {
            auto& ce = cast<CastExpr>(*expr);
            processExpr(ce.expr(), tt, ctx);
            break;
        }
        case NodeKind::SizeofExpr:
            // Already handled above
            break;
        default:
            break;
    }
}

// ============================================================================
// ComptimeEvalPass::run — main entry point
// ============================================================================
bool ComptimeEvalPass::run(Program& program, TypeTable& type_table) {
    ComptimeEvalContext ctx;
    bool any_evaluated = false;

    // First pass: evaluate top-level const declarations and static_asserts
    for (auto& tl : program) {
        if (tl->getKind() == NodeKind::ConstDeclStmt) {
            auto& decl = cast<ConstDeclStmt>(*tl);
            if (decl.hasInit()) {
                ComptimeValue val = evaluator_.evaluate(decl.init(), type_table, ctx);
                if (!val.isPoison()) {
                    ctx.bind(decl.name(), val);
                    storeComptimeValue(decl.init(), val);
                    storeComptimeValue(tl.get(), val);
                    any_evaluated = true;
                }
            }
        } else if (tl->getKind() == NodeKind::StaticAssertStmt) {
            auto& sa = cast<StaticAssertStmt>(*tl);
            ComptimeValue cond = evaluator_.evaluate(sa.condition(), type_table, ctx);
            if (cond.isPoison()) {
                std::cerr << "[tether] error: static_assert condition cannot be "
                          << "evaluated at compile time: "
                          << tl->sourceLoc().toString() << std::endl;
            } else if (!cond.isBool() || !cond.asBool()) {
                std::cerr << "[tether] error: static_assert failed";
                if (!sa.message().empty()) {
                    std::cerr << ": " << sa.message();
                }
                std::cerr << " — " << tl->sourceLoc().toString() << std::endl;
            } else {
                any_evaluated = true;
            }
            if (!cond.isPoison()) {
                storeComptimeValue(sa.condition(), cond);
            }
        }
    }

    // Second pass: walk function bodies for comptime expressions
    for (auto& tl : program) {
        if (tl->getKind() == NodeKind::FnDecl) {
            processFnBody(&cast<FnDecl>(*tl), type_table, ctx);
        }
    }

    // Count how many values we computed
    any_evaluated = any_evaluated || !comptime_values_.empty();

    return any_evaluated;
}

} // namespace tether
