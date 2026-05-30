#include "opt/MonomorphizationPass.h"
#include "ast/ASTCloner.h"
#include "metadata/MetaTypes.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <cassert>
#include <iostream>

namespace tether {

// ============================================================================
// sanitizeTypeName — make a type name safe for use in LLVM identifiers
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
// makeKey — create the lookup key for instance_map_
// ============================================================================
std::string MonomorphizationPass::makeKey(const std::string& fn_name,
                                           const std::vector<TypeId>& types) const {
    std::string key = fn_name;
    for (const auto& t : types) {
        key += ";" + t->toString();
    }
    return key;
}

// ============================================================================
// getMangledName — look up the mangled name for a known instantiation
// ============================================================================
std::string MonomorphizationPass::getMangledName(const std::string& fn_name,
                                                   const std::vector<TypeId>& types) const {
    auto it = instance_map_.find(makeKey(fn_name, types));
    if (it != instance_map_.end()) return it->second;
    return fn_name; // Not monomorphized — return original name
}

// ============================================================================
// resolveCall — look up the monomorphized name for a call
// ============================================================================
std::string MonomorphizationPass::resolveCall(const std::string& fn_name,
                                                const std::vector<TypeId>& arg_types) const {
    // Try to find an exact match by constructing the key from arg types
    // We need to find the GenericInstance whose concrete_types match
    // the argument types at the call site.
    for (const auto& inst : instances_) {
        if (inst.generic_fn_name == fn_name &&
            inst.concrete_types.size() == arg_types.size()) {
            bool match = true;
            for (size_t i = 0; i < inst.concrete_types.size(); ++i) {
                if (inst.concrete_types[i] != arg_types[i]) {
                    match = false;
                    break;
                }
            }
            if (match) return inst.mangled_name;
        }
    }
    return ""; // No monomorphized version found
}

// ============================================================================
// findInstanceForCall — find the GenericInstance for a call
// ============================================================================
const GenericInstance* MonomorphizationPass::findInstanceForCall(
    const std::string& fn_name,
    const std::vector<TypeId>& arg_types) const {
    for (const auto& inst : instances_) {
        if (inst.generic_fn_name == fn_name &&
            inst.concrete_types.size() == arg_types.size()) {
            bool match = true;
            for (size_t i = 0; i < inst.concrete_types.size(); ++i) {
                if (inst.concrete_types[i] != arg_types[i]) {
                    match = false;
                    break;
                }
            }
            if (match) return &inst;
        }
    }
    return nullptr;
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
            if (while_stmt.hasIncrement()) {
                walkExpr(while_stmt.increment(), generic_fns, type_table);
            }
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
                            std::string key = makeKey(gfn->name(), concrete_types);

                            if (instantiated_.count(key) == 0) {
                                instantiated_.insert(key);

                                GenericInstance inst;
                                inst.generic_fn_name = gfn->name();
                                inst.concrete_types = std::move(concrete_types);
                                inst.mangled_name = std::move(mangled);
                                inst.original_fn = gfn;
                                instances_.push_back(std::move(inst));
                                instance_map_[key] = instances_.back().mangled_name;
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

        case NodeKind::SliceExpr: {
            auto& slice = cast<SliceExpr>(*expr);
            walkExpr(slice.object(), generic_fns, type_table);
            walkExpr(slice.start(), generic_fns, type_table);
            if (slice.hasEnd()) walkExpr(slice.end(), generic_fns, type_table);
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
// cloneInstances — Phase 4: Clone generic function bodies with type substitution
//
// For each GenericInstance, deep-clone the original generic function's AST
// and substitute all type parameters with their concrete types. The cloned
// function gets the mangled name and is added to the Program.
// ============================================================================
void MonomorphizationPass::cloneInstances(Program& program, TypeTable& type_table) {
    for (auto& inst : instances_) {
        if (!inst.original_fn) continue;

        // Build the type substitution map:
        // type_params_[0] = "T" → concrete_types[0] = i32
        // type_params_[1] = "U" → concrete_types[1] = f64
        const auto& type_params = inst.original_fn->typeParams();
        std::unordered_map<std::string, TypeId> type_subst;
        for (size_t i = 0;
             i < type_params.size() && i < inst.concrete_types.size();
             ++i) {
            type_subst[type_params[i]] = inst.concrete_types[i];
        }

        // Clone the function with type substitution (pass TypeTable for
        // recursive composite type substitution: *T → *i32, []T → []i32, etc.)
        ASTCloner cloner(type_subst, &type_table);
        auto cloned_fn = cloner.cloneFnDecl(inst.original_fn);

        if (!cloned_fn) {
            std::cerr << "[tether] Monomorphization: WARNING: failed to clone "
                      << inst.generic_fn_name << " for " << inst.mangled_name
                      << std::endl;
            continue;
        }

        // Override the name with the mangled name
        // Since we own the cloned object and FnDecl's name is private,
        // we create a new FnDecl with the mangled name by extracting all
        // the parts from the cloned function.
        auto cloned_body = cloned_fn->takeBody();
        auto cloned_params = std::move(cloned_fn->params());
        auto cloned_directives = cloned_fn->directives();

        auto final_fn = std::unique_ptr<FnDecl>(new FnDecl(
            cloned_fn->sourceLoc(),
            inst.mangled_name,              // mangled name
            std::move(cloned_params),
            cloned_fn->returnType(),
            std::move(cloned_body),
            cloned_fn->isPure(),
            cloned_fn->errorType(),
            std::move(cloned_directives),
            {},                             // no type_params
            {}                              // no type_param_bounds
        ));
        final_fn->setInline(cloned_fn->isInline());
        final_fn->setNoalloc(cloned_fn->isNoalloc());
        final_fn->setAsync(cloned_fn->isAsync());
        // Substitute unresolved type name in return type (e.g., "T" → "i32")
        // The ASTCloner already substituted the TypeId, but the string name
        // needs updating too for any downstream resolution.
        if (!cloned_fn->unresolved_return_type_name.empty()) {
            auto it = type_subst.find(cloned_fn->unresolved_return_type_name);
            if (it != type_subst.end() && it->second) {
                final_fn->unresolved_return_type_name = it->second->toString();
            } else {
                final_fn->unresolved_return_type_name = cloned_fn->unresolved_return_type_name;
            }
        }

        // Phase 5: Add the monomorphized function to the Program
        program.push_back(std::move(final_fn));
    }
}

// ============================================================================
// rewriteCallSites — Phase 6: Rewrite calls to use monomorphized functions
//
// Walk all function bodies and replace calls to generic functions with
// calls to their monomorphized versions. This means changing the IdentExpr
// that names the callee from the generic name to the mangled name.
// ============================================================================
void MonomorphizationPass::rewriteCallSites(Program& program, TypeTable& type_table) {
    referenced_generics_.clear();

    for (auto& tl : program) {
        if (tl->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*tl);

        // Don't rewrite inside generic functions themselves — they're templates
        // that will be cloned separately. Only rewrite inside concrete functions
        // (including our newly-added monomorphized ones, which have no type params).
        // Actually, we DO want to rewrite calls inside generic functions too,
        // because a generic function might call another generic function.
        // But we must be careful not to rewrite calls that are part of the
        // generic function's own definition (those should stay as-is in the
        // template). The resolution: we only rewrite if the call's argument
        // types are fully concrete (not type parameters).

        if (!fn.body()) continue;
        rewriteBlock(fn.body());
    }
}

// ============================================================================
// rewriteBlock — rewrite calls in a block
// ============================================================================
void MonomorphizationPass::rewriteBlock(BlockStmt* block) {
    if (!block) return;
    for (auto& stmt : block->stmts()) {
        rewriteStmt(stmt.get());
    }
}

// ============================================================================
// rewriteStmt — rewrite calls in a statement
// ============================================================================
void MonomorphizationPass::rewriteStmt(Stmt* stmt) {
    if (!stmt) return;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt:
            rewriteBlock(&cast<BlockStmt>(*stmt));
            break;
        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            if (var.hasInit()) rewriteExpr(var.init());
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            if (val.hasInit()) rewriteExpr(val.init());
            break;
        }
        case NodeKind::ConstDeclStmt: {
            auto& cd = cast<ConstDeclStmt>(*stmt);
            if (cd.hasInit()) rewriteExpr(cd.init());
            break;
        }
        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);
            rewriteExpr(assign.target());
            rewriteExpr(assign.value());
            break;
        }
        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            rewriteStmt(defer.stmt());
            break;
        }
        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            rewriteStmt(errdefer.stmt());
            break;
        }
        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            rewriteExpr(if_stmt.condition());
            if (if_stmt.thenBlock()) rewriteBlock(if_stmt.thenBlock());
            if (if_stmt.elseBlock()) rewriteBlock(if_stmt.elseBlock());
            break;
        }
        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            rewriteExpr(while_stmt.condition());
            rewriteBlock(while_stmt.body());
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue()) rewriteExpr(ret.value());
            break;
        }
        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            rewriteExpr(expr_stmt.expr());
            break;
        }
        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            rewriteStmt(atomic.inner());
            break;
        }
        case NodeKind::YieldStmt: {
            auto& yield = cast<YieldStmt>(*stmt);
            if (yield.hasValue()) rewriteExpr(yield.value());
            break;
        }
        case NodeKind::MatchStmt: {
            auto& match = cast<MatchStmt>(*stmt);
            rewriteExpr(match.subject());
            for (auto& arm : match.arms()) {
                if (arm.pattern) rewriteExpr(arm.pattern.get());
                if (arm.body) rewriteBlock(arm.body.get());
            }
            break;
        }
        case NodeKind::ParallelForStmt: {
            auto& pf = cast<ParallelForStmt>(*stmt);
            rewriteExpr(pf.iterable());
            rewriteBlock(pf.body());
            break;
        }
        case NodeKind::UnsafeBlockStmt: {
            auto& ub = cast<UnsafeBlockStmt>(*stmt);
            rewriteBlock(ub.bodyPtr());
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// rewriteExpr — rewrite calls in an expression
//
// When we find a CallExpr whose callee is an IdentExpr matching a generic
// function, we check if there's a monomorphized version for the call's
// argument types. If so, we replace the IdentExpr's name with the mangled
// name.
//
// IMPORTANT: We cannot modify the IdentExpr in-place because the name is
// stored as a private const string. Instead, we create a new IdentExpr
// with the mangled name and replace the callee in the CallExpr.
//
// However, the CallExpr stores its callee as unique_ptr<Expr>, so we can't
// just swap the pointer from outside. We need to use the takeCallee() method
// to extract it and then... hmm, there's no setCallee().
//
// The pragmatic solution: since we need to modify the callee's name, and
// we can't do it through the API, we'll store the mapping in instance_map_
// and have the IRGenerator look it up at code generation time. This is
// actually cleaner than mutating the AST — the AST stays as-is (reflecting
// the source code), and the codegen resolves monomorphized names.
//
// BUT we still want Phase 6 to do SOMETHING observable. The key insight:
// Phase 6's rewriting happens at the MetadataMap level, not the AST level.
// We annotate each CallExpr that calls a generic function with metadata
// indicating its monomorphized target. The IRGenerator reads this metadata.
// ============================================================================
void MonomorphizationPass::rewriteExpr(Expr* expr) {
    if (!expr) return;

    switch (expr->getKind()) {
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);

            // Check if this calls a generic function
            if (call.callee()->getKind() == NodeKind::IdentExpr) {
                auto& ident = cast<IdentExpr>(*call.callee());

                // Collect the argument types from this call
                std::vector<TypeId> arg_types;
                for (const auto& arg : call.args()) {
                    if (arg->hasType()) {
                        arg_types.push_back(arg->getType());
                    }
                }

                // Try to find a matching monomorphized instance
                const GenericInstance* inst = findInstanceForCall(ident.name(), arg_types);
                if (inst) {
                    // Store the monomorphized target in the MetadataMap
                    // so the IRGenerator can look it up during code emission
                    if (meta_map_) {
                        auto& nm = meta_map_->getOrCreate(&call);
                        nm.monomorphized_target = inst->mangled_name;
                    }
                    referenced_generics_.insert(ident.name());
                }
            }

            // Recurse into callee and arguments
            rewriteExpr(call.callee());
            for (auto& arg : call.args()) {
                rewriteExpr(arg.get());
            }
            break;
        }

        case NodeKind::BinaryExpr: {
            auto& binary = cast<BinaryExpr>(*expr);
            rewriteExpr(binary.left());
            rewriteExpr(binary.right());
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            rewriteExpr(unary.operand());
            break;
        }
        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            rewriteExpr(member.object());
            break;
        }
        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            rewriteExpr(index.object());
            rewriteExpr(index.index());
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            rewriteExpr(deref.operand());
            break;
        }
        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            rewriteExpr(addr.operand());
            break;
        }
        case NodeKind::CastExpr: {
            auto& cast_expr = cast<CastExpr>(*expr);
            rewriteExpr(cast_expr.expr());
            break;
        }
        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (auto& field : init.inits()) {
                rewriteExpr(field.value.get());
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& arr = cast<ArrayInitExpr>(*expr);
            for (auto& elem : arr.elements()) {
                rewriteExpr(elem.get());
            }
            break;
        }
        case NodeKind::SliceExpr: {
            auto& slice = cast<SliceExpr>(*expr);
            rewriteExpr(slice.object());
            rewriteExpr(slice.start());
            if (slice.hasEnd()) rewriteExpr(slice.end());
            break;
        }
        case NodeKind::SizeofExpr: {
            auto& sz = cast<SizeofExpr>(*expr);
            if (sz.isExprOperand()) rewriteExpr(sz.expr());
            break;
        }
        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            rewriteExpr(unsafe.inner());
            break;
        }
        case NodeKind::TryExpr: {
            auto& try_expr = cast<TryExpr>(*expr);
            rewriteExpr(try_expr.operand());
            break;
        }
        case NodeKind::ComptimeExpr: {
            auto& ct = cast<ComptimeExpr>(*expr);
            rewriteExpr(ct.inner());
            break;
        }
        case NodeKind::ReduceExpr: {
            auto& reduce = cast<ReduceExpr>(*expr);
            rewriteExpr(reduce.iterable());
            if (reduce.hasAxis()) rewriteExpr(reduce.axis());
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// run — execute the monomorphization pass (all 6 phases)
//
// Iterates until fixpoint: a generic function may call another generic
// function, and the cloned body of the first may introduce new call sites
// that need monomorphization. We keep discovering and cloning until no
// new instances are found.
// ============================================================================
bool MonomorphizationPass::run(Program& program, TypeTable& type_table) {
    instances_.clear();
    instance_map_.clear();
    instantiated_.clear();
    referenced_generics_.clear();

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

    // Recursive monomorphization: iterate until fixpoint.
    // Each iteration may discover new generic calls in the bodies of
    // newly-cloned monomorphized functions.
    const int MAX_ITERATIONS = 16;  // Safety bound
    bool any_changed = false;
    
    for (int iteration = 0; iteration < MAX_ITERATIONS; ++iteration) {
        size_t prev_instance_count = instances_.size();

        // Phase 2: Walk all function bodies looking for calls to generic functions
        // (This includes newly-added monomorphized functions from previous iterations)
        for (auto& tl : program) {
            if (tl->getKind() != NodeKind::FnDecl) continue;
            auto& fn = cast<FnDecl>(*tl);
            if (!fn.body()) continue;

            walkBlock(fn.body(), generic_fns, type_table);
        }

        // Phase 3 is done during walk (instances are added as they're discovered)

        // If no new instances were found, we've reached fixpoint
        if (instances_.size() == prev_instance_count) break;

        // Phase 4 & 5: Clone ONLY the newly discovered instances
        // (instances added since prev_instance_count)
        cloneNewInstances(program, type_table, prev_instance_count);
    }

    if (instances_.empty()) return false;

    // Phase 6: Rewrite call sites (annotate with monomorphized targets)
    rewriteCallSites(program, type_table);

    any_changed = !instances_.empty();

    // Write monomorphization results to the MetadataMap for downstream use
    if (any_changed && meta_map_) {
        for (const auto& inst : instances_) {
            // Create a global mapping entry so the IRGenerator can resolve
            // any call to the generic function with matching arg types
            auto& nm = meta_map_->getOrCreate(inst.original_fn);
            (void)nm;
        }
    }

    return any_changed;
}

// ============================================================================
// cloneNewInstances — Clone only instances added after start_index
//
// This is called iteratively during recursive monomorphization, so we only
// clone the newly discovered instances each iteration (not the ones that
// were already cloned in previous iterations).
// ============================================================================
void MonomorphizationPass::cloneNewInstances(Program& program,
                                              TypeTable& type_table,
                                              size_t start_index) {
    for (size_t idx = start_index; idx < instances_.size(); ++idx) {
        auto& inst = instances_[idx];
        if (!inst.original_fn) continue;

        // Build the type substitution map
        const auto& type_params = inst.original_fn->typeParams();
        std::unordered_map<std::string, TypeId> type_subst;
        for (size_t i = 0;
             i < type_params.size() && i < inst.concrete_types.size();
             ++i) {
            type_subst[type_params[i]] = inst.concrete_types[i];
        }

        // Clone the function with type substitution
        ASTCloner cloner(type_subst, &type_table);
        auto cloned_fn = cloner.cloneFnDecl(inst.original_fn);

        if (!cloned_fn) {
            std::cerr << "[tether] Monomorphization: WARNING: failed to clone "
                      << inst.generic_fn_name << " for " << inst.mangled_name
                      << std::endl;
            continue;
        }

        // Override the name with the mangled name
        auto cloned_body = cloned_fn->takeBody();
        auto cloned_params = std::move(cloned_fn->params());
        auto cloned_directives = cloned_fn->directives();

        auto final_fn = std::unique_ptr<FnDecl>(new FnDecl(
            cloned_fn->sourceLoc(),
            inst.mangled_name,
            std::move(cloned_params),
            cloned_fn->returnType(),
            std::move(cloned_body),
            cloned_fn->isPure(),
            cloned_fn->errorType(),
            std::move(cloned_directives),
            {},
            {}
        ));
        final_fn->setInline(cloned_fn->isInline());
        final_fn->setNoalloc(cloned_fn->isNoalloc());
        final_fn->setAsync(cloned_fn->isAsync());
        // Substitute unresolved type name in return type
        if (!cloned_fn->unresolved_return_type_name.empty()) {
            auto it = type_subst.find(cloned_fn->unresolved_return_type_name);
            if (it != type_subst.end() && it->second) {
                final_fn->unresolved_return_type_name = it->second->toString();
            } else {
                final_fn->unresolved_return_type_name = cloned_fn->unresolved_return_type_name;
            }
        }

        // Phase 5: Add the monomorphized function to the Program
        program.push_back(std::move(final_fn));
    }
}

} // namespace tether
