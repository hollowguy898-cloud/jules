#include "opt/SpeculativeOptimizer.h"

#include <sstream>
#include <algorithm>
#include <cmath>

namespace tether {

// ============================================================================
// Assumption kind to string
// ============================================================================
const char* assumptionKindToString(AssumptionKind k) {
    switch (k) {
        case AssumptionKind::NeverNull:        return "NeverNull";
        case AssumptionKind::TypeExact:        return "TypeExact";
        case AssumptionKind::BranchNeverTaken: return "BranchNeverTaken";
        case AssumptionKind::NoOverflow:       return "NoOverflow";
        case AssumptionKind::BoundsInRange:    return "BoundsInRange";
        case AssumptionKind::NoAlias:          return "NoAlias";
        case AssumptionKind::PureCall:         return "PureCall";
    }
    return "Unknown";
}

// ============================================================================
// Run speculative analysis on all functions
// ============================================================================
bool SpeculativeOptimizerPass::run(Program& program, TypeTable& type_table) {
    bool any_changed = false;

    for (auto& tl : program) {
        if (tl->getKind() == NodeKind::FnDecl) {
            auto& fn = cast<FnDecl>(*tl);
            if (!fn.body()) continue;

            auto result = analyzeFunction(fn, type_table);
            if (result.assumptions_made > 0 || result.guards_inserted > 0) {
                any_changed = true;
                total_assumptions_ += result.assumptions_made;
                total_guards_ += result.guards_inserted;
            }
            function_results_[fn.name()] = std::move(result);
        }
    }

    return any_changed;
}

// ============================================================================
// Analyze a single function for speculative optimization opportunities
// ============================================================================
SpeculativeOptResult SpeculativeOptimizerPass::analyzeFunction(FnDecl& fn, TypeTable& type_table) {
    SpeculativeOptResult result;

    // Walk the function body looking for speculation opportunities
    if (fn.body()) {
        walkStmt(fn.body(), result);
    }

    result.assumptions_made = static_cast<int>(result.assumptions.size());
    result.guards_inserted = static_cast<int>(result.guarded_nodes.size());

    return result;
}

// ============================================================================
// Walk a statement recursively looking for speculation opportunities
// ============================================================================
void SpeculativeOptimizerPass::walkStmt(Stmt* stmt, SpeculativeOptResult& result) {
    if (!stmt) return;

    switch (stmt->getKind()) {
        case NodeKind::BlockStmt: {
            auto& block = cast<BlockStmt>(*stmt);
            for (auto& s : block.stmts()) {
                walkStmt(s.get(), result);
            }
            break;
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);

            // Check if this branch is worth speculating on
            if (isWorthSpeculating(is)) {
                // Determine which branch is the unlikely one
                bool then_unlikely = false;
                if (meta_map_) {
                    auto* nm = meta_map_->get(&is);
                    if (nm && nm->branch_prob == BranchProbability::Unlikely) {
                        then_unlikely = true;
                    } else if (nm && nm->branch_prob == BranchProbability::Likely) {
                        then_unlikely = false;
                    } else {
                        // Check if one path is marked cold
                        auto* then_nm = meta_map_->get(is.thenBlock());
                        auto* else_nm = is.hasElse() ? meta_map_->get(is.elseBlock()) : nullptr;
                        if (then_nm && then_nm->llvm_meta.cold_path) {
                            then_unlikely = true;
                        } else if (else_nm && else_nm->llvm_meta.cold_path) {
                            then_unlikely = false;
                        } else if (nm && nm->llvm_meta.cold_path) {
                            // The entire if is on a cold path — not worth speculating
                            then_unlikely = false;
                            break;
                        }
                    }
                }

                // Create the assumption
                SpeculativeAssumption assumption;
                assumption.kind = AssumptionKind::BranchNeverTaken;
                assumption.node = &is;
                assumption.confidence = 0.0f;

                if (meta_map_) {
                    auto* nm = meta_map_->get(&is);
                    if (nm && nm->profile.has_profile) {
                        uint64_t total = nm->profile.branch_taken + nm->profile.branch_not_taken;
                        if (total > 0) {
                            uint64_t likely_count = then_unlikely
                                ? nm->profile.branch_not_taken
                                : nm->profile.branch_taken;
                            assumption.confidence = static_cast<float>(likely_count) / total;
                            assumption.profile_count = total;
                        }
                    }
                }

                // If no PGO data, use heuristic confidence from branch probability
                if (assumption.confidence == 0.0f && meta_map_) {
                    auto* nm = meta_map_->get(&is);
                    if (nm && nm->branch_prob == BranchProbability::Likely) {
                        assumption.confidence = 0.97f;  // Heuristic: "likely" = 97%
                    } else if (nm && nm->branch_prob == BranchProbability::Unlikely) {
                        assumption.confidence = 0.97f;
                    } else {
                        assumption.confidence = 0.90f;  // Default: not confident enough
                    }
                }

                std::string unlikely_branch = then_unlikely ? "then" : "else";
                assumption.description = "Branch " + unlikely_branch +
                    " is never taken (confidence=" +
                    std::to_string(static_cast<int>(assumption.confidence * 100)) + "%)";

                if (assumption.confidence >= confidence_threshold_) {
                    result.assumptions.push_back(assumption);
                    result.guarded_nodes.push_back(&is);
                    node_assumptions_[&is] = assumption;
                    guarded_nodes_.insert(&is);

                    // Also mark in the MetadataMap
                    addAssumptionToMetaMap(&is, assumption);
                }
            }

            // Walk sub-expressions
            walkExpr(is.condition(), result);
            walkStmt(is.thenBlock(), result);
            if (is.hasElse()) {
                walkStmt(is.elseBlock(), result);
            }
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            walkExpr(ws.condition(), result);
            walkStmt(ws.body(), result);
            if (ws.hasIncrement()) {
                walkExpr(ws.increment(), result);
            }
            break;
        }
        case NodeKind::VarDeclStmt: {
            auto& vd = cast<VarDeclStmt>(*stmt);
            if (vd.hasInit()) walkExpr(vd.init(), result);
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& vd = cast<ValDeclStmt>(*stmt);
            if (vd.hasInit()) walkExpr(vd.init(), result);
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            walkExpr(as.target(), result);
            walkExpr(as.value(), result);
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            walkExpr(es.expr(), result);
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& rs = cast<ReturnStmt>(*stmt);
            if (rs.value()) walkExpr(rs.value(), result);
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            walkStmt(ds.stmt(), result);
            break;
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            walkExpr(ms.subject(), result);
            for (auto& arm : ms.arms()) {
                if (arm.body) walkStmt(arm.body.get(), result);
            }
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// Walk an expression recursively looking for speculation opportunities
// ============================================================================
void SpeculativeOptimizerPass::walkExpr(Expr* expr, SpeculativeOptResult& result) {
    if (!expr) return;

    switch (expr->getKind()) {
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);

            // Check for no-overflow speculation on arithmetic
            if (bin.op() == BinaryOp::Add || bin.op() == BinaryOp::Sub ||
                bin.op() == BinaryOp::Mul) {
                if (isWorthSpeculatingNoOverflow(bin)) {
                    SpeculativeAssumption assumption;
                    assumption.kind = AssumptionKind::NoOverflow;
                    assumption.node = &bin;
                    assumption.confidence = 0.96f;  // Heuristic default
                    assumption.description = "Arithmetic operation does not overflow";

                    // Boost confidence if we have PGO data showing no overflow
                    if (meta_map_) {
                        auto* nm = meta_map_->get(&bin);
                        if (nm && nm->profile.has_profile && nm->profile.violation_count == 0) {
                            assumption.confidence = 0.99f;
                            assumption.profile_count = nm->profile.entry_count;
                        }
                    }

                    if (assumption.confidence >= confidence_threshold_) {
                        result.assumptions.push_back(assumption);
                        result.guarded_nodes.push_back(&bin);
                        node_assumptions_[&bin] = assumption;
                        guarded_nodes_.insert(&bin);
                        addAssumptionToMetaMap(&bin, assumption);
                    }
                }
            }

            walkExpr(bin.left(), result);
            walkExpr(bin.right(), result);
            break;
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);

            // Check for pure call speculation
            if (meta_map_) {
                auto* nm = meta_map_->get(&call);
                if (nm && nm->purity == FunctionPurity::Pure) {
                    SpeculativeAssumption assumption;
                    assumption.kind = AssumptionKind::PureCall;
                    assumption.node = &call;
                    assumption.confidence = 1.0f;  // Pure is guaranteed by type system
                    assumption.description = "Function call is pure (no side effects)";
                    result.assumptions.push_back(assumption);
                    result.guarded_nodes.push_back(&call);
                    node_assumptions_[&call] = assumption;
                    guarded_nodes_.insert(&call);
                    addAssumptionToMetaMap(&call, assumption);
                }
            }

            // Walk callee and arguments
            walkExpr(call.callee(), result);
            for (auto& arg : call.args()) {
                walkExpr(arg.get(), result);
            }
            break;
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);

            // Check for bounds speculation
            if (isWorthSpeculatingBounds(idx)) {
                SpeculativeAssumption assumption;
                assumption.kind = AssumptionKind::BoundsInRange;
                assumption.node = &idx;
                assumption.confidence = 0.96f;

                // Boost confidence if we have linear access pattern from L3
                if (meta_map_) {
                    auto* nm = meta_map_->get(&idx);
                    if (nm) {
                        for (const auto& ap : nm->access_patterns) {
                            if (ap.traversal == TraversalKind::Linear && ap.contiguous) {
                                assumption.confidence = 0.99f;
                                break;
                            }
                        }
                    }
                }

                assumption.description = "Array index is within bounds";

                if (assumption.confidence >= confidence_threshold_) {
                    result.assumptions.push_back(assumption);
                    result.guarded_nodes.push_back(&idx);
                    node_assumptions_[&idx] = assumption;
                    guarded_nodes_.insert(&idx);
                    addAssumptionToMetaMap(&idx, assumption);
                }
            }

            walkExpr(idx.object(), result);
            walkExpr(idx.index(), result);
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            walkExpr(un.operand(), result);
            break;
        }
        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            walkExpr(mem.object(), result);
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);

            // Check for non-null speculation on pointer dereference
            if (isWorthSpeculatingNonNull(deref.operand())) {
                SpeculativeAssumption assumption;
                assumption.kind = AssumptionKind::NeverNull;
                assumption.node = &deref;
                assumption.confidence = 0.97f;
                assumption.description = "Dereferenced pointer is never null";

                if (meta_map_) {
                    auto* nm = meta_map_->get(&deref);
                    if (nm && nm->profile.has_profile && nm->profile.violation_count == 0) {
                        assumption.confidence = 0.99f;
                        assumption.profile_count = nm->profile.entry_count;
                    }
                }

                if (assumption.confidence >= confidence_threshold_) {
                    result.assumptions.push_back(assumption);
                    result.guarded_nodes.push_back(&deref);
                    node_assumptions_[&deref] = assumption;
                    guarded_nodes_.insert(&deref);
                    addAssumptionToMetaMap(&deref, assumption);
                }
            }

            walkExpr(deref.operand(), result);
            break;
        }
        default:
            break;
    }
}

// ============================================================================
// Determine if a branch is worth speculating on
// ============================================================================
bool SpeculativeOptimizerPass::isWorthSpeculating(IfStmt& is) {
    if (!meta_map_) return false;

    auto* nm = meta_map_->get(&is);
    if (!nm) return false;

    // In an AOT compiler, deoptimization guards are dangerous because there's
    // no JIT to fall back to when the assumption is violated. Without PGO data,
    // we should NOT insert deopt guards — only emit branch probability hints
    // (!prof metadata) so LLVM's basic block layout is improved.
    //
    // Heuristic-based branch probabilities (Likely/Unlikely) are NOT reliable
    // enough for deopt guards. Only PGO data with high confidence is safe.

    // Only worth speculating with deopt guard if we have PGO data
    if (nm->profile.has_profile) {
        uint64_t total = nm->profile.branch_taken + nm->profile.branch_not_taken;
        if (total > 100) {  // Need enough samples
            uint64_t max_branch = std::max(nm->profile.branch_taken,
                                            nm->profile.branch_not_taken);
            float ratio = static_cast<float>(max_branch) / total;
            if (ratio >= confidence_threshold_) {
                return true;
            }
        }
    }

    // Branch probability hints (Likely/Unlikely) and cold path markings
    // are used for LLVM metadata emission only, NOT for deopt guards.
    // The codegen will emit !prof metadata for branch hints, but will
    // keep the original branch logic intact.

    return false;
}

// ============================================================================
// Determine if a pointer dereference is worth speculating non-null
// ============================================================================
bool SpeculativeOptimizerPass::isWorthSpeculatingNonNull(Expr* expr) {
    if (!expr || !meta_map_) return false;

    // References (&T, &mut T) are guaranteed non-null by the type system
    if (expr->hasType()) {
        TypeId t = expr->getType();
        if (t && (isa<ReferenceType>(t) || isa<MutReferenceType>(t))) {
            return false;  // No need to speculate — it's guaranteed by type system
        }
    }

    // Raw pointers: worth speculating if we have PGO data showing no nulls
    auto* nm = meta_map_->get(expr);
    if (!nm) return false;

    if (nm->profile.has_profile && nm->profile.violation_count == 0 && nm->profile.entry_count > 100) {
        return true;
    }

    return false;
}

// ============================================================================
// Determine if an array access is worth speculating bounds on
// ============================================================================
bool SpeculativeOptimizerPass::isWorthSpeculatingBounds(IndexExpr& idx) {
    if (!meta_map_) return false;

    auto* nm = meta_map_->get(&idx);
    if (!nm) return false;

    // If the borrow checker already proved bounds are safe, no need to speculate
    if (nm->bounds_proven_safe) return false;

    // Worth speculating if we have linear access pattern from L3
    for (const auto& ap : nm->access_patterns) {
        if (ap.traversal == TraversalKind::Linear && ap.contiguous) {
            return true;
        }
    }

    // Worth speculating if we have PGO data showing no out-of-bounds
    if (nm->profile.has_profile && nm->profile.violation_count == 0 && nm->profile.entry_count > 100) {
        return true;
    }

    return false;
}

// ============================================================================
// Determine if arithmetic is worth speculating no-overflow on
// ============================================================================
bool SpeculativeOptimizerPass::isWorthSpeculatingNoOverflow(BinaryExpr& bin) {
    // Only speculate on integer arithmetic (floats don't overflow the same way)
    if (!bin.left()->hasType() || !bin.left()->getType()) return false;
    TypeId lt = bin.left()->getType();
    if (!lt->isInteger()) return false;

    // If both operands are small constants, overflow is impossible
    if (bin.left()->getKind() == NodeKind::IntLiteral &&
        bin.right()->getKind() == NodeKind::IntLiteral) {
        return false;  // No need to speculate — compiler can verify
    }

    // If we have PGO data showing no overflow, speculate
    if (meta_map_) {
        auto* nm = meta_map_->get(&bin);
        if (nm && nm->profile.has_profile && nm->profile.violation_count == 0 && nm->profile.entry_count > 100) {
            return true;
        }
    }

    return false;
}

// ============================================================================
// Get speculative assumptions for a node
// ============================================================================
const SpeculativeAssumption* SpeculativeOptimizerPass::getAssumption(const ASTNode* node) const {
    auto it = node_assumptions_.find(node);
    if (it != node_assumptions_.end()) return &it->second;
    return nullptr;
}

// ============================================================================
// Get all assumptions for a function
// ============================================================================
const SpeculativeOptResult* SpeculativeOptimizerPass::getFunctionResult(const std::string& fn_name) const {
    auto it = function_results_.find(fn_name);
    if (it != function_results_.end()) return &it->second;
    return nullptr;
}

// ============================================================================
// Check if a node has a deoptimization guard
// ============================================================================
bool SpeculativeOptimizerPass::hasDeoptGuard(const ASTNode* node) const {
    return guarded_nodes_.count(node) > 0;
}

// ============================================================================
// Add an assumption to the MetadataMap
// ============================================================================
void SpeculativeOptimizerPass::addAssumptionToMetaMap(const ASTNode* node,
                                                       const SpeculativeAssumption& assumption) {
    if (!meta_map_) return;

    auto& nm = meta_map_->getOrCreate(node);
    nm.has_speculative_assumption = true;
    nm.speculative_kind = assumption.kind;
    nm.speculative_confidence = assumption.confidence;
}

} // namespace tether
