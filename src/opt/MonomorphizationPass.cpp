#include "opt/MonomorphizationPass.h"
#include "metadata/MetaTypes.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <cassert>

namespace tether {

// ============================================================================
// sanitizeTypeName — make a type name safe for use in LLVM identifiers
//
// Replaces characters that are illegal in LLVM identifiers (like ':', '<', '>',
// ',', ' ', '!', '*', '&') with underscores. Ensures the result doesn't start
// with a digit.
// ============================================================================
std::string MonomorphizationPass::sanitizeTypeName(const std::string& name) const {
    std::string result;
    result.reserve(name.size());
    for (char c : name) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') {
            result += c;
        } else {
            result += '_';
        }
    }
    // LLVM identifiers can't start with a digit
    if (!result.empty() && std::isdigit(static_cast<unsigned char>(result[0]))) {
        result = "_" + result;
    }
    if (result.empty()) result = "_";
    return result;
}

// ============================================================================
// mangle — generate a mangled name for a generic instantiation
//
// Example: mangle("max", [i32])  => "max_i32"
// Example: mangle("pair", [i32, f64]) => "pair_i32_f64"
// ============================================================================
std::string MonomorphizationPass::mangle(const std::string& base_name,
                                          const std::vector<TypeId>& types) const {
    std::string result = base_name;
    for (const auto& t : types) {
        result += "_" + sanitizeTypeName(t->toString());
    }
    return result;
}

// ============================================================================
// getMangledName — look up the mangled name for a known instantiation
//
// Returns the original function name if no monomorphized version exists.
// This is used by the IRGenerator to emit calls to the correct function.
// ============================================================================
std::string MonomorphizationPass::getMangledName(const std::string& fn_name,
                                                   const std::vector<TypeId>& types) const {
    std::string key = fn_name;
    for (const auto& t : types) {
        key += ";" + t->toString();
    }
    auto it = instance_map_.find(key);
    if (it != instance_map_.end()) return it->second;
    return fn_name; // Not monomorphized — return original name
}

// ============================================================================
// walkBlock — walk a block statement looking for calls to generic functions
// ============================================================================
void MonomorphizationPass::walkBlock(BlockStmt* block,
                                      const std::vector<FnDecl*>& generic_fns,
                                      TypeTable& type_table) {
    if (!block) return;
    for (auto& stmt : block->stmts()) {
        walkStmt(stmt.get(), generic_fns, type_table);
    }
}

// ============================================================================
// walkStmt — walk a statement looking for calls to generic functions
// ============================================================================
void MonomorphizationPass::walkStmt(Stmt* stmt,
                                     const std::vector<FnDecl*>& generic_fns,
                                     TypeTable& type_table) {
    if (!stmt) return;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            walkBlock(&cast<BlockStmt>(*stmt), generic_fns, type_table);
            break;

        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit()) walkExpr(var.init(), generic_fns, type_table);
            break;
        }

        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit()) walkExpr(val.init(), generic_fns, type_table);
            break;
        }

        case NodeKind::ConstDeclStmt: {
            auto& cd = cast<ConstDeclStmt>(*stmt);
            if (cd.hasInit()) walkExpr(cd.init(), generic_fns, type_table);
            break;
        }

        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            walkExpr(assign.target(), generic_fns, type_table);
            walkExpr(assign.value(), generic_fns, type_table);
            break;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            walkStmt(defer.stmt(), generic_fns, type_table);
            break;
        }

        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            walkStmt(errdefer.stmt(), generic_fns, type_table);
            break;
        }

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            walkExpr(if_stmt.condition(), generic_fns, type_table);
            if (if_stmt.thenBlock()) {
                walkBlock(if_stmt.thenBlock(), generic_fns, type_table);
            }
            if (if_stmt.elseBlock()) {
                walkBlock(if_stmt.elseBlock(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            walkExpr(while_stmt.condition(), generic_fns, type_table);
            walkBlock(while_stmt.body(), generic_fns, type_table);
            break;
        }

        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue()) walkExpr(ret.value(), generic_fns, type_table);
            break;
        }

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            walkExpr(expr_stmt.expr(), generic_fns, type_table);
            break;
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            walkStmt(atomic.inner(), generic_fns, type_table);
            break;
        }

        case NodeKind::YieldStmt: {
            auto& yield = cast<YieldStmt>(*stmt);
            if (yield.hasValue()) walkExpr(yield.value(), generic_fns, type_table);
            break;
        }

        case NodeKind::MatchStmt: {
            auto& match = cast<MatchStmt>(*stmt);
            walkExpr(match.subject(), generic_fns, type_table);
            for (auto& arm : match.arms()) {
                if (arm.pattern) walkExpr(arm.pattern.get(), generic_fns, type_table);
                if (arm.body) walkBlock(arm.body.get(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::ParallelForStmt: {
            auto& pf = cast<ParallelForStmt>(*stmt);
            walkExpr(pf.iterable(), generic_fns, type_table);
            walkBlock(pf.body(), generic_fns, type_table);
            break;
        }

        case NodeKind::UnsafeBlockStmt: {
            auto& ub = cast<UnsafeBlockStmt>(*stmt);
            walkBlock(ub.bodyPtr(), generic_fns, type_table);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// walkExpr — walk an expression looking for calls to generic functions
//
// When a CallExpr is found whose callee is an IdentExpr matching a known
// generic function, we infer the concrete type arguments from the types of
// the call arguments. For example:
//
//   fn max<T>(a: T, b: T) -> T    // generic function
//   max(3, 5)                       // T = i32 (inferred from arg types)
//   max(1.0, 2.0)                   // T = f64 (inferred from arg types)
// ============================================================================
void MonomorphizationPass::walkExpr(Expr* expr,
                                     const std::vector<FnDecl*>& generic_fns,
                                     TypeTable& type_table) {
    if (!expr) return;

    switch (expr->getKind()) {
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);

            // Check if this is a call to a generic function
            if (call.callee()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*call.callee());
                for (auto* gfn : generic_fns) {
                    if (gfn->name() == ident.name()) {
                        // Infer concrete type arguments from call argument types.
                        //
                        // For a generic function fn foo<T, U>(a: T, b: U),
                        // we look at the types of the actual arguments at this
                        // call site to determine T and U.
                        //
                        // Current limitation: we only infer types from positional
                        // arguments. Explicit type arguments (e.g., foo<i32, f64>)
                        // are not yet supported by the parser.
                        std::vector<TypeId> concrete_types;
                        for (size_t i = 0; i < call.args().size() && i < gfn->params().size(); ++i) {
                            TypeId arg_type = call.args()[i]->hasType()
                                ? call.args()[i]->getType() : TypeId();
                            if (arg_type) {
                                concrete_types.push_back(arg_type);
                            }
                        }

                        // Only record if we inferred all type parameters
                        if (concrete_types.size() >= gfn->typeParamCount()) {
                            std::string mangled = mangle(gfn->name(), concrete_types);
                            std::string key = gfn->name();
                            for (const auto& t : concrete_types) {
                                key += ";" + t->toString();
                            }

                            if (instantiated_.count(key) == 0) {
                                instantiated_.insert(key);

                                GenericInstance inst;
                                inst.generic_fn_name = gfn->name();
                                inst.concrete_types = std::move(concrete_types);
                                inst.mangled_name = std::move(mangled);
                                inst.original_fn = gfn;
                                instances_.push_back(std::move(inst));
                                instance_map_[key] = instances_.back().mangled_name;

                                // Write monomorphization info to the MetadataMap
                                // so the IRGenerator can look up the mangled name
                                if (meta_map_) {
                                    auto& nm = meta_map_->getOrCreate(&call);
                                    (void)nm; // Suppress unused warning; full integration pending
                                    // Store the mangled name as a custom field
                                    // (IRGenerator will check for this)
                                    // For now we just mark that this call was
                                    // monomorphized — the full integration
                                    // requires plumbing the pass results through.
                                }
                            }
                        }
                        break; // Found matching generic fn, stop searching
                    }
                }
            }

            // Walk into nested expressions (callee and arguments)
            walkExpr(call.callee(), generic_fns, type_table);
            for (auto& arg : call.args()) {
                walkExpr(arg.get(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::BinaryExpr: {
            auto& binary = cast<BinaryExpr>(*expr);
            walkExpr(binary.left(), generic_fns, type_table);
            walkExpr(binary.right(), generic_fns, type_table);
            break;
        }

        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            walkExpr(unary.operand(), generic_fns, type_table);
            break;
        }

        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            walkExpr(member.object(), generic_fns, type_table);
            break;
        }

        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            walkExpr(index.object(), generic_fns, type_table);
            walkExpr(index.index(), generic_fns, type_table);
            break;
        }

        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            walkExpr(deref.operand(), generic_fns, type_table);
            break;
        }

        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            walkExpr(addr.operand(), generic_fns, type_table);
            break;
        }

        case NodeKind::CastExpr: {
            auto& cast_expr = cast<CastExpr>(*expr);
            walkExpr(cast_expr.expr(), generic_fns, type_table);
            break;
        }

        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (auto& field : init.inits()) {
                walkExpr(field.value.get(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::ArrayInitExpr: {
            auto& arr_init = cast<ArrayInitExpr>(*expr);
            for (auto& elem : arr_init.elements()) {
                walkExpr(elem.get(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::SizeofExpr: {
            auto& sizeof_expr = cast<SizeofExpr>(*expr);
            if (sizeof_expr.isExprOperand()) {
                walkExpr(sizeof_expr.expr(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            walkExpr(unsafe.inner(), generic_fns, type_table);
            break;
        }

        case NodeKind::TryExpr: {
            auto& try_expr = cast<TryExpr>(*expr);
            walkExpr(try_expr.operand(), generic_fns, type_table);
            break;
        }

        case NodeKind::ComptimeExpr: {
            auto& ct = cast<ComptimeExpr>(*expr);
            walkExpr(ct.inner(), generic_fns, type_table);
            break;
        }

        case NodeKind::ReduceExpr: {
            auto& reduce = cast<ReduceExpr>(*expr);
            walkExpr(reduce.iterable(), generic_fns, type_table);
            if (reduce.hasAxis()) {
                walkExpr(reduce.axis(), generic_fns, type_table);
            }
            break;
        }

        case NodeKind::TypeofExpr: {
            auto& typeof_expr = cast<TypeofExpr>(*expr);
            walkExpr(typeof_expr.operand(), generic_fns, type_table);
            break;
        }

        default:
            break;
    }
}

// ============================================================================
// run — execute the monomorphization pass
//
// Phase 1: Find all generic functions (those with type_params_)
// Phase 2: Walk all call sites and infer concrete types from argument types
// Phase 3: Record GenericInstances for each unique (fn, types) pair
//
// Future phases (not yet implemented):
//   Phase 4: Clone generic function bodies with type substitution
//   Phase 5: Add monomorphized functions to the Program
//   Phase 6: Replace original calls with calls to monomorphized versions
// ============================================================================
bool MonomorphizationPass::run(Program& program, TypeTable& type_table) {
    instances_.clear();
    instance_map_.clear();
    instantiated_.clear();

    // Phase 1: Find all generic functions
    std::vector<FnDecl*> generic_fns;
    for (auto& tl : program) {
        if (tl->getKind() == NodeKind::FnDecl) {
            auto& fn = cast<FnDecl>(*tl);
            if (fn.isGeneric()) {
                generic_fns.push_back(&fn);
            }
        }
    }

    if (generic_fns.empty()) return false;

    // Phase 2: Walk all function bodies looking for calls to generic functions
    for (auto& tl : program) {
        if (tl->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*tl);
        if (!fn.body()) continue;

        walkBlock(fn.body(), generic_fns, type_table);
    }

    // Phase 3 is done during walk (instances are added as they're discovered)

    bool any_changed = !instances_.empty();

    // Write monomorphization results to the MetadataMap for downstream use
    if (any_changed && meta_map_) {
        for (const auto& inst : instances_) {
            // Store the mapping from original name to mangled name
            // so the IRGenerator can look it up
            auto& nm = meta_map_->getOrCreate(inst.original_fn);
            (void)nm; // Suppress unused warning; full integration pending
            // The IRGenerator will need to know that calls to inst.generic_fn_name
            // with these concrete types should use inst.mangled_name instead.
            // This is stored in instance_map_ for now; the IRGenerator
            // integration requires plumbing the pass results through the pipeline.
        }
    }

    return any_changed;
}

} // namespace tether
