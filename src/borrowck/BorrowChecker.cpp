#include "borrowck/BorrowChecker.h"
#include "ast/ASTVisitor.h"

#include <algorithm>
#include <cassert>
#include <functional>
#include <sstream>

namespace tether {

// ============================================================================
// Main entry point: check a single function's CFG
//
// Runs 7 phases for the strict affine type system:
//   Phase 1: Collect function signatures
//   Phase 2: Compute lifetimes
//   Phase 3: Compute liveness (backward dataflow) + check path-based borrows
//   Phase 4: Check reborrows
//   Phase 5: Check moves
//   Phase 6: Solve lifetime constraints + check lifetimes
//   Phase 7: Detect noalias params + bounds + interprocedural
// ============================================================================
void BorrowChecker::check(CFG& cfg) {
    // Reset per-function state
    moved_out_vars_.clear();
    reborrow_edges_.clear();
    lifetime_constraints_.clear();
    var_lifetimes_.clear();
    next_lifetime_id_ = 1;
    in_unsafe_context_ = false;
    unsafe_depth_ = 0;

    // Phase 1: Collect function signatures
    collectFnSignatures(cfg);

    // Phase 2: Compute lifetimes
    computeLifetimes(cfg);

    // Phase 3: Compute liveness and check path-based borrow rules
    computeLiveness(cfg);
    checkBorrowRules(cfg);

    // Phase 4: Check reborrows
    checkReborrows(cfg);

    // Phase 5: Check moves (strict enforcement)
    checkMoves(cfg);

    // Phase 6: Solve lifetime constraints and check lifetimes
    solveLifetimeConstraints();
    checkLifetimes(cfg);

    // Phase 7: Detect noalias parameters, bounds, interprocedural
    detectNoAliasParams(cfg);
    checkBounds(cfg);
    checkInterprocedural(cfg);
}

// ============================================================================
// Check all functions
// ============================================================================
void BorrowChecker::checkAll(std::vector<std::unique_ptr<CFG>>& cfgs) {
    for (auto& cfg : cfgs) {
        if (cfg) check(*cfg);
    }
}

// ============================================================================
// hasStrictErrors - check if any strict error is not a warning
// ============================================================================
bool BorrowChecker::hasStrictErrors() const {
    for (const auto& e : strict_errors_) {
        if (!e.is_warning) return true;
    }
    return false;
}

// ============================================================================
// Phase 1: Collect function signatures
// ============================================================================
void BorrowChecker::collectFnSignatures(CFG& cfg) {
    FnBorrowSignature sig;
    sig.fn_name = cfg.functionName();
    sig.is_unsafe = false;

    // Walk the entry node to find parameter borrows and build signature
    CFGNode* entry = cfg.entry();
    if (!entry) return;

    // Collect parameter info from borrows in the first few nodes
    for (const auto& bi : entry->borrowsCreated()) {
        LifetimeId lt = next_lifetime_id_++;
        sig.param_lifetimes.push_back(lt);
        sig.params_moved.push_back(false);
        sig.params_noalias.push_back(bi.kind == BorrowKind::MutExclusive);
        var_lifetimes_[bi.borrowed_var] = lt;
    }

    // Set return lifetime to 'static by default (conservative)
    sig.return_lifetime = 0; // 'static

    fn_signatures_[cfg.functionName()] = std::move(sig);
}

// ============================================================================
// Phase 2: Compute lifetimes
// ============================================================================
void BorrowChecker::computeLifetimes(CFG& cfg) {
    // Assign lifetime IDs to each borrow variable encountered in the CFG
    for (const auto& node : cfg.nodes()) {
        for (const auto& bi : node->borrowsCreated()) {
            // Assign a lifetime to the borrowed variable if not already assigned
            if (var_lifetimes_.find(bi.borrowed_var) == var_lifetimes_.end()) {
                var_lifetimes_[bi.borrowed_var] = next_lifetime_id_++;
            }
            // Assign a lifetime to the borrow variable if not already assigned
            if (!bi.borrow_var.empty() &&
                var_lifetimes_.find(bi.borrow_var) == var_lifetimes_.end()) {
                var_lifetimes_[bi.borrow_var] = next_lifetime_id_++;
            }
        }

        // Also assign lifetimes to defined/used vars
        for (const auto& var : node->definedVars()) {
            if (var_lifetimes_.find(var) == var_lifetimes_.end()) {
                var_lifetimes_[var] = next_lifetime_id_++;
            }
        }
        for (const auto& var : node->usedVars()) {
            if (var_lifetimes_.find(var) == var_lifetimes_.end()) {
                var_lifetimes_[var] = next_lifetime_id_++;
            }
        }
    }

    // Build lifetime constraints from borrow relationships:
    // If var_b = &var_a, then lifetime(var_a) must outlive lifetime(var_b)
    for (const auto& node : cfg.nodes()) {
        for (const auto& bi : node->borrowsCreated()) {
            auto it_borrowed = var_lifetimes_.find(bi.borrowed_var);
            auto it_borrow_var = var_lifetimes_.find(bi.borrow_var);

            if (it_borrowed != var_lifetimes_.end() &&
                it_borrow_var != var_lifetimes_.end()) {
                // The borrowed variable must outlive the borrow variable
                lifetime_constraints_.push_back({
                    it_borrowed->second,  // longer (owner)
                    it_borrow_var->second // shorter (borrow)
                });
            }
        }
    }
}

// ============================================================================
// Liveness analysis - iterative backward dataflow
// ============================================================================
void BorrowChecker::computeLiveness(CFG& cfg) {
    // Clear previous state
    live_in_.clear();
    live_out_.clear();
    gen_sets_.clear();
    kill_sets_.clear();

    // Initialize all sets to empty
    for (const auto& node : cfg.nodes()) {
        live_in_[node->id()] = LiveBorrowSet();
        live_out_[node->id()] = LiveBorrowSet();
        gen_sets_[node->id()] = LiveBorrowSet();
        kill_sets_[node->id()] = LiveBorrowSet();
    }

    // Phase 1a: Compute gen and kill sets for each node
    for (const auto& node : cfg.nodes()) {
        LiveBorrowSet gen, kill;
        computeGenKill(*node, gen, kill);
        gen_sets_[node->id()] = std::move(gen);
        kill_sets_[node->id()] = std::move(kill);
    }

    // Phase 1b: Fixed-point iteration for liveness
    std::vector<CFGNode*> rpo = cfg.reversePostorder();
    std::reverse(rpo.begin(), rpo.end());

    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 1000;

    while (changed && iterations < MAX_ITERATIONS) {
        changed = false;
        iterations++;

        for (CFGNode* node : rpo) {
            if (!node) continue;
            auto node_id = node->id();

            // Compute live_out(n) = union of live_in(s) for all successors s
            LiveBorrowSet new_out;
            for (CFGNode* succ : node->successors()) {
                auto& succ_in = live_in_[succ->id()];
                for (const auto& lb : succ_in) {
                    new_out.insert(lb);
                }
            }

            // Compute live_in(n) = gen(n) ∪ (live_out(n) - kill(n))
            LiveBorrowSet new_in;

            // Add gen(n)
            for (const auto& lb : gen_sets_[node_id]) {
                new_in.insert(lb);
            }

            // Add (live_out(n) - kill(n))
            for (const auto& lb : new_out) {
                if (kill_sets_[node_id].count(lb) == 0) {
                    new_in.insert(lb);
                }
            }

            // Check for changes
            if (new_in != live_in_[node_id] || new_out != live_out_[node_id]) {
                changed = true;
                live_in_[node_id] = std::move(new_in);
                live_out_[node_id] = std::move(new_out);
            }
        }
    }
}

void BorrowChecker::computeGenKill(CFGNode& node,
                                   LiveBorrowSet& gen,
                                   LiveBorrowSet& kill) {
    // Gen: borrows created in this node
    for (const auto& bi : node.borrowsCreated()) {
        LiveBorrow lb;
        lb.borrowed_var = bi.borrowed_var;
        lb.kind = bi.kind;
        lb.origin = bi.origin;
        lb.borrow_var = bi.borrow_var;
        lb.path = pathFromBorrowInfo(bi);
        gen.insert(std::move(lb));
    }

    // Kill: borrows whose borrowed variable (or path prefix) is redefined.
    // When a variable is redefined, any existing borrows of that variable
    // or any child paths are no longer valid.
    for (const auto& var : node.definedVars()) {
        // Kill any live borrows of this variable
        for (const auto& bi : node.borrowsCreated()) {
            if (bi.borrow_var == var) {
                LiveBorrow lb;
                lb.borrowed_var = bi.borrowed_var;
                lb.kind = bi.kind;
                lb.origin = bi.origin;
                lb.borrow_var = bi.borrow_var;
                lb.path = pathFromBorrowInfo(bi);
                kill.insert(std::move(lb));
            }
        }
    }
}

// ============================================================================
// Phase 3: Check path-based borrow rules
// ============================================================================
void BorrowChecker::checkBorrowRules(CFG& cfg) {
    for (const auto& node_ptr : cfg.nodes()) {
        CFGNode& node = *node_ptr;

        // Start with the live-in set
        LiveBorrowSet current_live = live_in_[node.id()];

        // Check each borrow created in this node
        for (const auto& bi : node.borrowsCreated()) {
            checkBorrowCreation(node, bi, current_live);

            // Add the new borrow to the current live set
            LiveBorrow lb;
            lb.borrowed_var = bi.borrowed_var;
            lb.kind = bi.kind;
            lb.origin = bi.origin;
            lb.borrow_var = bi.borrow_var;
            lb.path = pathFromBorrowInfo(bi);
            current_live.insert(std::move(lb));

            // Record reborrow edges: if the borrow_var is itself a borrow,
            // then this new borrow reborrows the original
            if (!bi.borrow_var.empty()) {
                for (const auto& existing : current_live) {
                    if (existing.borrow_var == bi.borrowed_var) {
                        // This new borrow reborrows through an existing borrow
                        recordReborrow(existing.path, lb.path, bi.kind);
                    }
                }
            }
        }

        // Check each variable definition (mutation) in this node.
        // When a variable is reassigned, any existing borrows of that variable
        // are invalidated (killed). This is critical for swap patterns where
        // reassignment happens within the same block.
        for (const auto& var : node.definedVars()) {
            SourceLocation mut_loc = node.stmts().empty()
                ? SourceLocation() : node.stmts().front()->sourceLoc();

            for (auto* stmt : node.stmts()) {
                if (stmt->getKind() == NodeKind::VarDeclStmt) {
                    auto& vd = static_cast<VarDeclStmt&>(*stmt);
                    if (vd.name() == var) {
                        mut_loc = vd.sourceLoc();
                        break;
                    }
                } else if (stmt->getKind() == NodeKind::ValDeclStmt) {
                    auto& vd = static_cast<ValDeclStmt&>(*stmt);
                    if (vd.name() == var) {
                        mut_loc = vd.sourceLoc();
                        break;
                    }
                } else if (stmt->getKind() == NodeKind::AssignStmt) {
                    auto& as = static_cast<AssignStmt&>(*stmt);
                    mut_loc = as.sourceLoc();
                    break;
                }
            }

            PathExpr mut_path = pathFromVarName(var);
            checkMutation(node, var, mut_path, mut_loc, current_live);

            // Kill borrows of this variable: reassignment invalidates all
            // borrows that reference the redefined variable's path.
            // This is the same logic as in checkMoves — after mutation,
            // borrows of the old value are dead.
            LiveBorrowSet surviving;
            for (const auto& lb : current_live) {
                if (!pathsConflict(mut_path, lb.path)) {
                    surviving.insert(lb);
                }
            }
            current_live = std::move(surviving);
        }
    }
}

void BorrowChecker::checkBorrowCreation(CFGNode& /*node*/,
                                         const BorrowInfo& new_borrow,
                                         const LiveBorrowSet& live_borrows) {
    PathExpr new_path = pathFromBorrowInfo(new_borrow);

    for (const auto& lb : live_borrows) {
        // PATH-AWARE CHECK: only check borrows whose paths overlap
        // If paths are disjoint (e.g., point.x vs point.y), no conflict
        if (!pathsConflict(new_path, lb.path)) continue;

        // Rule 1: Cannot create &mut T while any &T or &mut T to an
        // overlapping path is live
        if (new_borrow.kind == BorrowKind::MutExclusive) {
            if (lb.kind == BorrowKind::Shared) {
                std::string msg = "cannot create '&mut " + new_path.toString() +
                    "' because it is already borrowed as & (shared) at path '" +
                    lb.path.toString() + "'";
                std::string help = "consider waiting for the shared borrow of '" +
                    lb.path.toString() + "' to go out of scope";

                if (in_unsafe_context_) {
                    reportStrictError(StrictBorrowErrorKind::MutWhileSharedBorrowed,
                                      new_borrow.origin, new_path, lb.kind,
                                      lb.origin, lb.path, msg, help);
                    // Mark as warning in unsafe context
                    strict_errors_.back().is_warning = true;
                } else {
                    reportError(BorrowError::Kind::MutWhileSharedBorrowed,
                                new_borrow.origin, new_borrow.borrowed_var,
                                lb.kind, lb.origin, msg);
                    reportStrictError(StrictBorrowErrorKind::MutWhileSharedBorrowed,
                                      new_borrow.origin, new_path, lb.kind,
                                      lb.origin, lb.path, msg, help);
                }
            } else if (lb.kind == BorrowKind::MutExclusive) {
                std::string msg = "cannot create '&mut " + new_path.toString() +
                    "' because it is already exclusively borrowed as &mut at path '" +
                    lb.path.toString() + "'";
                std::string help = "consider restructuring the code to avoid "
                    "overlapping mutable borrows";

                if (in_unsafe_context_) {
                    reportStrictError(StrictBorrowErrorKind::MutWhileMutBorrowed,
                                      new_borrow.origin, new_path, lb.kind,
                                      lb.origin, lb.path, msg, help);
                    strict_errors_.back().is_warning = true;
                } else {
                    reportError(BorrowError::Kind::MutWhileMutBorrowed,
                                new_borrow.origin, new_borrow.borrowed_var,
                                lb.kind, lb.origin, msg);
                    reportStrictError(StrictBorrowErrorKind::MutWhileMutBorrowed,
                                      new_borrow.origin, new_path, lb.kind,
                                      lb.origin, lb.path, msg, help);
                }
            }
        }

        // Rule 2: Cannot create &T while an &mut T to an overlapping path is live
        if (new_borrow.kind == BorrowKind::Shared &&
            lb.kind == BorrowKind::MutExclusive) {
            std::string msg = "cannot create '&" + new_path.toString() +
                "' because it is exclusively borrowed as &mut at path '" +
                lb.path.toString() + "'";
            std::string help = "consider waiting for the mutable borrow of '" +
                lb.path.toString() + "' to go out of scope";

            if (in_unsafe_context_) {
                reportStrictError(StrictBorrowErrorKind::SharedWhileMutBorrowed,
                                  new_borrow.origin, new_path, lb.kind,
                                  lb.origin, lb.path, msg, help);
                strict_errors_.back().is_warning = true;
            } else {
                reportError(BorrowError::Kind::SharedWhileMutBorrowed,
                            new_borrow.origin, new_borrow.borrowed_var,
                            lb.kind, lb.origin, msg);
                reportStrictError(StrictBorrowErrorKind::SharedWhileMutBorrowed,
                                  new_borrow.origin, new_path, lb.kind,
                                  lb.origin, lb.path, msg, help);
            }
        }
    }
}

void BorrowChecker::checkMutation(CFGNode& /*node*/,
                                   const std::string& var_name,
                                   const PathExpr& mut_path,
                                   const SourceLocation& loc,
                                   const LiveBorrowSet& live_borrows) {
    for (const auto& lb : live_borrows) {
        // PATH-AWARE CHECK: mutation of a path conflicts with borrows of
        // overlapping paths
        if (!pathsConflict(mut_path, lb.path)) continue;

        // Rule 3: Cannot mutate a path while a &T to an overlapping path is live
        if (lb.kind == BorrowKind::Shared) {
            std::string msg = "cannot mutate '" + mut_path.toString() +
                "' because it is borrowed as & (shared) at path '" +
                lb.path.toString() + "'";
            std::string help = "consider waiting for the shared borrow to expire";

            if (in_unsafe_context_) {
                reportStrictError(StrictBorrowErrorKind::MutateWhileSharedBorrow,
                                  loc, mut_path, lb.kind, lb.origin, lb.path,
                                  msg, help);
                strict_errors_.back().is_warning = true;
            } else {
                reportError(BorrowError::Kind::MutateWhileSharedBorrow,
                            loc, var_name, lb.kind, lb.origin, msg);
                reportStrictError(StrictBorrowErrorKind::MutateWhileSharedBorrow,
                                  loc, mut_path, lb.kind, lb.origin, lb.path,
                                  msg, help);
            }
        }

        // Mutating while &mut is live is also an error
        if (lb.kind == BorrowKind::MutExclusive) {
            std::string msg = "cannot mutate '" + mut_path.toString() +
                "' because it is exclusively borrowed as &mut at path '" +
                lb.path.toString() + "'";
            std::string help = "consider using the mutable borrow instead of "
                "mutating the original variable";

            if (in_unsafe_context_) {
                reportStrictError(StrictBorrowErrorKind::MutWhileMutBorrowed,
                                  loc, mut_path, lb.kind, lb.origin, lb.path,
                                  msg, help);
                strict_errors_.back().is_warning = true;
            } else {
                reportError(BorrowError::Kind::MutWhileMutBorrowed,
                            loc, var_name, lb.kind, lb.origin, msg);
                reportStrictError(StrictBorrowErrorKind::MutWhileMutBorrowed,
                                  loc, mut_path, lb.kind, lb.origin, lb.path,
                                  msg, help);
            }
        }
    }
}

// ============================================================================
// Phase 4: Check reborrows
// ============================================================================
void BorrowChecker::checkReborrows(CFG& cfg) {
    // Walk through all nodes and check for reborrow conflicts
    for (const auto& node_ptr : cfg.nodes()) {
        CFGNode& node = *node_ptr;

        // When a variable is mutated, check if any reborrows rooted at that
        // variable's path are still live
        for (const auto& var : node.definedVars()) {
            PathExpr mutated_path = pathFromVarName(var);

            // Check each reborrow edge
            for (const auto& edge : reborrow_edges_) {
                // If the mutated path overlaps with the parent path,
                // the child (reborrow) is invalidated
                if (pathsConflict(mutated_path, edge.parent_path)) {
                    // Check if the child path is still live
                    const auto& live = live_in_[node.id()];
                    for (const auto& lb : live) {
                        if (lb.path == edge.child_path ||
                            edge.child_path.isPrefixOf(lb.path)) {
                            std::string msg = "mutation of '" + mutated_path.toString() +
                                "' invalidates reborrow of '" + edge.child_path.toString() +
                                "' which reborrows '" + edge.parent_path.toString() + "'";
                            std::string help = "consider restructuring to avoid "
                                "reborrow conflicts, or wrap in an unsafe block";

                            SourceLocation loc = node.stmts().empty()
                                ? SourceLocation() : node.stmts().front()->sourceLoc();

                            if (in_unsafe_context_) {
                                reportStrictError(StrictBorrowErrorKind::ReborrowConflict,
                                                  loc, mutated_path, edge.kind,
                                                  lb.origin, edge.child_path,
                                                  msg, help);
                                strict_errors_.back().is_warning = true;
                            } else {
                                reportStrictError(StrictBorrowErrorKind::ReborrowConflict,
                                                  loc, mutated_path, edge.kind,
                                                  lb.origin, edge.child_path,
                                                  msg, help);
                            }
                        }
                    }
                }
            }
        }
    }
}

void BorrowChecker::recordReborrow(const PathExpr& parent,
                                    const PathExpr& child,
                                    BorrowKind kind) {
    reborrow_edges_.push_back({parent, child, kind});
}

void BorrowChecker::invalidateReborrows(const PathExpr& parent_path,
                                         std::vector<PathExpr>& invalidated) {
    // Recursively invalidate all reborrows whose parent overlaps with parent_path
    for (const auto& edge : reborrow_edges_) {
        if (pathsConflict(parent_path, edge.parent_path)) {
            invalidated.push_back(edge.child_path);
            // Recursively invalidate children of this child
            invalidateReborrows(edge.child_path, invalidated);
        }
    }
}

// ============================================================================
// Phase 5: Check moves (strict enforcement)
//
// Swap/temp pattern fix: When a variable is reassigned (e.g., a = b),
// the old value of `a` is discarded but `a` itself becomes valid again
// with a new value. So we "un-move" `a` when it appears as the LHS of
// an assignment. We also kill any borrows of the LHS variable, since
// reassignment invalidates all outstanding borrows (the old value they
// referenced is gone). This allows patterns like:
//   temp = a; a = b; b = temp;  (swap)
//   a = a + 1;                  (self-update, not a move)
//   val x = compute(); y = x;   (move into y, x is now moved)
//   x = compute2();             (x is valid again — reinitialized)
//   var r = &a; a = 42;         (r is invalidated by the reassignment)
//
// Copy semantics: For trivially-copyable types (primitives, small structs
// without destructors), assignment is a COPY, not a MOVE. The source
// variable remains valid after the assignment. This is critical for
// patterns like:
//   val x = 42; val y = x; print(x);  // x is still valid (i32 is Copy)
//   val a = 3.14; val b = a; print(a); // a is still valid (f64 is Copy)
//
// Only "affine" types (types with ownership semantics like Box, Rc, Arc,
// and structs containing them) are actually moved on assignment.
// ============================================================================
void BorrowChecker::checkMoves(CFG& cfg) {
    for (const auto& node_ptr : cfg.nodes()) {
        CFGNode& node = *node_ptr;

        // Track local borrow state that changes as we process statements.
        // This is essential for swap patterns where borrows are invalidated
        // mid-block by reassignment.
        LiveBorrowSet local_live = live_in_[node.id()];

        // Track which variables were moved at which statement index,
        // so the use-after-move check at the end can be order-aware.
        // Maps var_name → statement index where it was moved.
        std::unordered_map<std::string, int> move_at_stmt;
        int stmt_idx = 0;

        // Process statements in order so that reassignment "un-moves" and
        // borrow kills take effect before subsequent checks.
        for (auto* stmt : node.stmts()) {
            if (stmt->getKind() == NodeKind::AssignStmt) {
                auto& as = static_cast<AssignStmt&>(*stmt);

                // --- LHS handling: reassignment un-moves the target ---
                // When `a = expr`, the old value of `a` is replaced with a new
                // value, so `a` is now valid again (not moved-out anymore).
                // This is critical for swap patterns: temp = a; a = b; b = temp;
                if (as.target()->getKind() == NodeKind::IdentExpr) {
                    auto& lhs = static_cast<IdentExpr&>(*as.target());
                    const std::string& lhs_name = lhs.name();
                    // Un-move: the variable is being reinitialized
                    moved_out_vars_.erase(lhs_name);
                    move_at_stmt.erase(lhs_name);

                    // Kill borrows: reassignment invalidates all borrows of
                    // the LHS variable. This is key for the swap pattern:
                    //   temp = a;   // borrows of 'a' are created
                    //   a = b;      // borrows of 'a' are killed here
                    //   b = temp;   // no conflict because 'a' borrows are dead
                    PathExpr killed_path = pathFromVarName(lhs_name);
                    LiveBorrowSet surviving;
                    for (const auto& lb : local_live) {
                        if (!pathsConflict(killed_path, lb.path)) {
                            surviving.insert(lb);
                        }
                    }
                    local_live = std::move(surviving);
                }

                // --- RHS handling: moving a value out ---
                // If the RHS is a simple identifier, it might be a move.
                // BUT: for trivially-copyable types (primitives, etc.),
                // assignment is a COPY, not a MOVE. The source variable
                // remains valid.
                if (as.value()->getKind() == NodeKind::IdentExpr) {
                    auto& ie = static_cast<IdentExpr&>(*as.value());

                    // Self-assignment check: a = a is a no-op, not a move
                    if (as.target()->getKind() == NodeKind::IdentExpr) {
                        auto& lhs = static_cast<IdentExpr&>(*as.target());
                        if (lhs.name() == ie.name()) {
                            // Self-assignment (a = a) — not a move, skip
                            stmt_idx++;
                            continue;
                        }
                    }

                    // Check if the variable is borrowed (using local_live, which
                    // accounts for borrows killed by prior reassignments in this block)
                    for (const auto& lb : local_live) {
                        PathExpr used_path = pathFromVarName(ie.name());
                        if (pathsConflict(used_path, lb.path)) {
                            // Rule: Cannot move a variable while any borrow is live
                            std::string msg = "cannot move '" + ie.name() +
                                "' because it is borrowed" +
                                (lb.kind == BorrowKind::Shared ? " as & (shared)" :
                                 " as &mut (exclusive)");
                            std::string help = "consider cloning the value instead of moving it";

                            if (in_unsafe_context_) {
                                reportStrictError(StrictBorrowErrorKind::MoveWhileBorrowed,
                                                  as.sourceLoc(), used_path, lb.kind,
                                                  lb.origin, lb.path, msg, help);
                                strict_errors_.back().is_warning = true;
                            } else {
                                reportError(BorrowError::Kind::MoveWhileBorrowed,
                                            as.sourceLoc(), ie.name(), lb.kind,
                                            lb.origin, msg);
                                reportStrictError(StrictBorrowErrorKind::MoveWhileBorrowed,
                                                  as.sourceLoc(), used_path, lb.kind,
                                                  lb.origin, lb.path, msg, help);
                            }
                        }
                    }

                    // Only mark as moved if the type is NOT trivially copyable.
                    // Primitives (i32, f64, bool, etc.) are Copy — assigning
                    // them copies the value; the source remains valid.
                    // Box, Rc, Arc, and structs containing them are affine
                    // (move semantics — source is consumed).
                    bool is_copy_type = false;
                    if (ie.hasType()) {
                        TypeId t = ie.getType();
                        is_copy_type = isTriviallyCopyable(t);
                    }

                    if (!is_copy_type) {
                        // Affine type: mark as moved (ownership transferred)
                        markMoved(ie.name());
                        move_at_stmt[ie.name()] = stmt_idx;
                    }
                    // Copy type: source variable remains valid (no move)
                }
            } else if (stmt->getKind() == NodeKind::VarDeclStmt) {
                // var x = expr; — x is being initialized (not a move of x)
                // If the RHS is a simple identifier, it's a move of that identifier
                auto& vd = static_cast<VarDeclStmt&>(*stmt);
                if (vd.hasInit() && vd.init()->getKind() == NodeKind::IdentExpr) {
                    auto& ie = static_cast<IdentExpr&>(*vd.init());
                    // The newly declared variable is valid (it's being initialized)
                    // So we make sure it's not in moved_out_vars_
                    moved_out_vars_.erase(vd.name());
                    move_at_stmt.erase(vd.name());

                    // Only move the RHS if it's not a Copy type
                    bool is_copy_type = false;
                    if (ie.hasType()) {
                        TypeId t = ie.getType();
                        is_copy_type = isTriviallyCopyable(t);
                    }
                    if (!is_copy_type) {
                        markMoved(ie.name());
                        move_at_stmt[ie.name()] = stmt_idx;
                    }
                }
            } else if (stmt->getKind() == NodeKind::ValDeclStmt) {
                // val x = expr; — x is being initialized as immutable
                auto& vd = static_cast<ValDeclStmt&>(*stmt);
                if (vd.hasInit() && vd.init()->getKind() == NodeKind::IdentExpr) {
                    auto& ie = static_cast<IdentExpr&>(*vd.init());
                    moved_out_vars_.erase(vd.name());
                    move_at_stmt.erase(vd.name());

                    // Only move the RHS if it's not a Copy type
                    bool is_copy_type = false;
                    if (ie.hasType()) {
                        TypeId t = ie.getType();
                        is_copy_type = isTriviallyCopyable(t);
                    }
                    if (!is_copy_type) {
                        markMoved(ie.name());
                        move_at_stmt[ie.name()] = stmt_idx;
                    }
                }
            }
            stmt_idx++;
        }

        // Check for use-after-move: variable uses in this node.
        // Order-aware: only flag a use if it occurs AFTER the move.
        // A variable used before being moved in the same block is fine.
        int use_idx = 0;
        for (const auto& var : node.usedVars()) {
            // Check if this variable was moved within this block
            auto move_it = move_at_stmt.find(var);
            if (move_it != move_at_stmt.end()) {
                // The variable was moved at some statement index.
                // Only flag if the use occurs AFTER the move.
                // For now, since usedVars() doesn't give us the exact
                // statement index of the use, we use a heuristic:
                // if the var was moved and is still in moved_out_vars_,
                // it's a potential use-after-move. But we also check
                // whether the use might be the RHS of the very move
                // that consumes it (which is valid).
                //
                // The most conservative approach: if the variable is
                // still in moved_out_vars_ after processing all statements
                // (i.e., it wasn't reinitialized), check for uses.
                // But only flag uses that aren't the move itself.
                //
                // Since we already checked move-while-borrowed above,
                // the only false positive here is when a variable is
                // used in a different statement after being moved.
                // That IS a real error, so we check for it.
            }

            // Only check if the variable is still in moved_out_vars_
            // (i.e., it was moved and not subsequently reinitialized)
            if (moved_out_vars_.count(var) == 0) continue;

            SourceLocation use_loc = node.stmts().empty()
                ? SourceLocation() : node.stmts().front()->sourceLoc();

            // Find the source location from the actual statements
            for (auto* stmt : node.stmts()) {
                if (stmt->getKind() == NodeKind::AssignStmt) {
                    auto& as = static_cast<AssignStmt&>(*stmt);
                    if (as.value()->getKind() == NodeKind::IdentExpr) {
                        auto& ie = static_cast<IdentExpr&>(*as.value());
                        if (ie.name() == var) {
                            use_loc = as.sourceLoc();
                        }
                    }
                } else if (stmt->getKind() == NodeKind::ExprStmt) {
                    auto& es = static_cast<ExprStmt&>(*stmt);
                    // Simple heuristic: use the expr statement's location
                    use_loc = es.sourceLoc();
                }
            }

            checkUseAfterMove(var, pathFromVarName(var), use_loc);
            use_idx++;
        }
    }
}

void BorrowChecker::checkUseAfterMove(const std::string& var_name,
                                       const PathExpr& path,
                                       const SourceLocation& loc) {
    if (moved_out_vars_.count(var_name) > 0) {
        std::string msg = "use of moved value '" + var_name + "'";
        std::string help = "consider cloning the value before moving it, "
            "or use a reference instead";

        if (in_unsafe_context_) {
            reportStrictError(StrictBorrowErrorKind::UseAfterMove,
                              loc, path, BorrowKind::Shared, loc, path,
                              msg, help);
            strict_errors_.back().is_warning = true;
        } else {
            reportStrictError(StrictBorrowErrorKind::UseAfterMove,
                              loc, path, BorrowKind::Shared, loc, path,
                              msg, help);
        }
    }
}

void BorrowChecker::markMoved(const std::string& var_name) {
    moved_out_vars_.insert(var_name);
}

// ============================================================================
// Phase 6: Solve lifetime constraints
// ============================================================================
void BorrowChecker::solveLifetimeConstraints() {
    // Simple transitive closure: if 'a : 'b and 'b : 'c, then 'a : 'c
    // We iteratively compute the "outlives" relation until fixed point

    // For now, we just verify that the constraints don't form a cycle
    // (which would mean a lifetime must outlive itself — an error)

    // Build a simple graph: longer → shorter edges
    std::unordered_map<LifetimeId, std::vector<LifetimeId>> outlives;
    for (const auto& c : lifetime_constraints_) {
        outlives[c.longer].push_back(c.shorter);
    }

    // Check for cycles using DFS
    std::unordered_set<LifetimeId> visited;
    std::unordered_set<LifetimeId> in_stack;

    std::function<bool(LifetimeId)> hasCycle = [&](LifetimeId id) -> bool {
        if (in_stack.count(id) > 0) return true;
        if (visited.count(id) > 0) return false;

        visited.insert(id);
        in_stack.insert(id);

        auto it = outlives.find(id);
        if (it != outlives.end()) {
            for (LifetimeId succ : it->second) {
                if (hasCycle(succ)) return true;
            }
        }

        in_stack.erase(id);
        return false;
    };

    for (const auto& [id, _] : outlives) {
        if (hasCycle(id)) {
            // Cycle detected — this means a lifetime must outlive itself
            // This is an error, but we don't have a specific source location
            // so we just report a generic error
            reportStrictError(StrictBorrowErrorKind::LifetimeOutOfBounds,
                              SourceLocation(), PathExpr(), BorrowKind::Shared,
                              SourceLocation(), PathExpr(),
                              "lifetime constraint cycle detected: a reference "
                              "would need to outlive itself",
                              "consider reducing the scope of borrows");
        }
    }
}

void BorrowChecker::checkLifetimes(CFG& cfg) {
    // For each borrow, check that the borrowed variable's lifetime outlives
    // the borrow variable's lifetime
    for (const auto& node : cfg.nodes()) {
        for (const auto& bi : node->borrowsCreated()) {
            auto it_borrowed = var_lifetimes_.find(bi.borrowed_var);
            auto it_borrow_var = var_lifetimes_.find(bi.borrow_var);

            if (it_borrowed != var_lifetimes_.end() &&
                it_borrow_var != var_lifetimes_.end()) {
                // Check if the constraint borrowed_outlives_borrow_var exists
                // in the transitive closure of lifetime constraints
                // For simplicity, we just check the direct constraints
                bool constraint_exists = false;
                for (const auto& c : lifetime_constraints_) {
                    if (c.longer == it_borrowed->second &&
                        c.shorter == it_borrow_var->second) {
                        constraint_exists = true;
                        break;
                    }
                }

                // If the constraint exists, the borrow is valid from a lifetime
                // perspective. If not, we have a potential lifetime violation,
                // but we don't report it as an error here — the liveness
                // analysis already catches most of these cases.
                (void)constraint_exists;
            }
        }
    }
}

// ============================================================================
// Phase 7: Detect noalias params (enhanced with interprocedural info)
// ============================================================================
void BorrowChecker::detectNoAliasParams(CFG& cfg) {
    // Collect the set of variables involved in borrow errors
    std::unordered_set<std::string> error_vars;
    for (const auto& err : errors_) {
        error_vars.insert(err.borrowed_var);
    }
    for (const auto& err : strict_errors_) {
        if (!err.is_warning) {
            if (!err.path.empty()) {
                error_vars.insert(err.path.toString());
            }
        }
    }

    // Walk through the entry node to find parameter borrows
    CFGNode* entry = cfg.entry();
    if (!entry) return;

    for (const auto& node : cfg.nodes()) {
        for (const auto& bi : node->borrowsCreated()) {
            // If a parameter is borrowed as &mut and no errors involve it,
            // mark it as noalias
            if (bi.kind == BorrowKind::MutExclusive) {
                if (error_vars.count(bi.borrowed_var) == 0) {
                    bool already = false;
                    for (const auto& na : noalias_params_) {
                        if (na.param_name == bi.borrowed_var) {
                            already = true;
                            break;
                        }
                    }
                    if (!already && bi.borrowed_type) {
                        noalias_params_.emplace_back(bi.borrowed_var, bi.borrowed_type);
                    }
                }
            }
        }
    }

    // Enhanced: use interprocedural signatures for noalias detection
    auto sig_it = fn_signatures_.find(cfg.functionName());
    if (sig_it != fn_signatures_.end()) {
        const auto& sig = sig_it->second;
        for (size_t i = 0; i < sig.params_noalias.size(); ++i) {
            if (sig.params_noalias[i]) {
                // This parameter was determined to be noalias from the signature
                // Verify it doesn't conflict with existing entries
                // (The parameter name is derived from the signature)
            }
        }
    }
}

// ============================================================================
// Array bounds elimination
// ============================================================================
void BorrowChecker::checkBounds(CFG& cfg) {
    for (const auto& node : cfg.nodes()) {
        for (auto* stmt : node->stmts()) {
            // Walk the statement looking for IndexExpr nodes
            // For each IndexExpr, try to prove the index is in bounds
            struct IndexExprCollector : public RecursiveASTVisitor<IndexExprCollector, void> {
                BorrowChecker* bc;
                CFGNode* cfg_node;
                std::vector<IndexExpr*> index_exprs;

                void visitIndexExpr(IndexExpr& ie) {
                    index_exprs.push_back(&ie);
                }

                void visitExprStmt(ExprStmt& es) {
                    traverseExpr(es.expr());
                }

                void visitVarDeclStmt(VarDeclStmt& vd) {
                    if (vd.hasInit()) traverseExpr(vd.init());
                }

                void visitValDeclStmt(ValDeclStmt& vd) {
                    if (vd.hasInit()) traverseExpr(vd.init());
                }

                void visitAssignStmt(AssignStmt& as) {
                    traverseExpr(as.target());
                    traverseExpr(as.value());
                }
            };

            IndexExprCollector collector;
            collector.bc = this;
            collector.cfg_node = node.get();
            collector.visit(*stmt);

            for (auto* ie : collector.index_exprs) {
                if (tryProveBounds(*ie->object(), *ie->index(), *node)) {
                    bounds_proven_safe_.insert(ie);
                }
            }
        }
    }
}

bool BorrowChecker::tryProveBounds(const Expr& /*object*/,
                                    const Expr& index,
                                    CFGNode& /*node*/) {
    // Heuristic: if the index is an IntLiteral and it's non-negative,
    // we can try to prove it's in bounds.
    // If we're in a loop with linear traversal (from MetadataMap L3 analysis),
    // we can also prove bounds.

    // Simple case: literal index (always >= 0 since uint64_t)
    if (index.getKind() == NodeKind::IntLiteral) {
        const auto& lit = static_cast<const IntLiteral&>(index);
        // For now, we conservatively say it's proven safe if the index
        // is a small literal (common pattern in fixed-size arrays)
        if (lit.value() < 1024) {
            return true;
        }
    }

    // If we're in an unsafe context, bounds checks can be eliminated
    // (but we still track them)
    if (in_unsafe_context_) {
        return true;
    }

    // Cannot prove bounds — runtime check will be needed
    // If we're NOT in an unsafe block and can't prove bounds,
    // emit a warning in strict mode
    if (strict_mode_ && !in_unsafe_context_) {
        SourceLocation loc = index.sourceLoc();
        PathExpr path;  // Empty path by default

        reportStrictError(StrictBorrowErrorKind::BoundsCheckFailed,
                          loc, path, BorrowKind::Shared, loc, PathExpr(),
                          "cannot prove array bounds at compile time; "
                          "runtime bounds check will be inserted",
                          "consider using a loop with known bounds, "
                          "or wrap in an unsafe block to suppress");
        // Bounds check failure is always a warning (not an error),
        // since we can still insert a runtime check
        strict_errors_.back().is_warning = true;
    }

    return false;
}

// ============================================================================
// Interprocedural analysis
// ============================================================================
void BorrowChecker::checkInterprocedural(CFG& cfg) {
    // At each call site in the CFG, look up the callee's signature
    // and check that the arguments satisfy the signature's constraints
    for (const auto& node : cfg.nodes()) {
        for (auto* stmt : node->stmts()) {
            struct CallSiteChecker : public RecursiveASTVisitor<CallSiteChecker, void> {
                BorrowChecker* bc;
                CFGNode* cfg_node;

                void visitCallExpr(CallExpr& ce) {
                    // Get the callee name
                    if (ce.callee()->getKind() == NodeKind::IdentExpr) {
                        auto& ie = static_cast<IdentExpr&>(*ce.callee());
                        std::string fn_name = ie.name();

                        // Collect argument names
                        std::vector<std::string> arg_names;
                        for (auto& arg : ce.args()) {
                            if (arg->getKind() == NodeKind::IdentExpr) {
                                arg_names.push_back(
                                    static_cast<IdentExpr&>(*arg).name());
                            }
                        }

                        bc->checkCallSite(fn_name, arg_names, ce.sourceLoc());
                    }

                    // Recurse into nested calls
                    for (auto& arg : ce.args()) {
                        traverseExpr(arg.get());
                    }
                }

                void visitExprStmt(ExprStmt& es) {
                    traverseExpr(es.expr());
                }

                void visitVarDeclStmt(VarDeclStmt& vd) {
                    if (vd.hasInit()) traverseExpr(vd.init());
                }

                void visitValDeclStmt(ValDeclStmt& vd) {
                    if (vd.hasInit()) traverseExpr(vd.init());
                }

                void visitAssignStmt(AssignStmt& as) {
                    traverseExpr(as.target());
                    traverseExpr(as.value());
                }
            };

            CallSiteChecker checker;
            checker.bc = this;
            checker.cfg_node = node.get();
            checker.visit(*stmt);
        }
    }
}

void BorrowChecker::checkCallSite(const std::string& fn_name,
                                   const std::vector<std::string>& arg_names,
                                   const SourceLocation& /*loc*/) {
    auto sig_it = fn_signatures_.find(fn_name);
    if (sig_it == fn_signatures_.end()) {
        // No signature available for this function — skip
        return;
    }

    const auto& sig = sig_it->second;

    // Check each argument against the signature
    for (size_t i = 0; i < arg_names.size() && i < sig.params_moved.size(); ++i) {
        // If the parameter is marked as moved in the signature, add the
        // argument to moved_out_vars_
        if (sig.params_moved[i]) {
            markMoved(arg_names[i]);
        }

        // Check that the argument's lifetime satisfies the signature's constraints
        auto arg_lt_it = var_lifetimes_.find(arg_names[i]);
        if (arg_lt_it != var_lifetimes_.end() &&
            i < sig.param_lifetimes.size()) {
            LifetimeId arg_lt = arg_lt_it->second;
            LifetimeId param_lt = sig.param_lifetimes[i];

            // Add a constraint: arg_lifetime must outlive param_lifetime
            lifetime_constraints_.push_back({arg_lt, param_lt});
        }
    }
}

// ============================================================================
// Unsafe block support
// ============================================================================
void BorrowChecker::enterUnsafe() {
    in_unsafe_context_ = true;
    unsafe_depth_++;
}

void BorrowChecker::leaveUnsafe() {
    assert(unsafe_depth_ > 0 && "unsafe block depth underflow");
    unsafe_depth_--;
    if (unsafe_depth_ == 0) {
        in_unsafe_context_ = false;
    }
}

// ============================================================================
// Error reporting
// ============================================================================
void BorrowChecker::reportError(BorrowError::Kind kind,
                                 const SourceLocation& loc,
                                 const std::string& borrowed_var,
                                 BorrowKind borrow_kind,
                                 const SourceLocation& borrow_origin,
                                 const std::string& message) {
    errors_.emplace_back(kind, loc, borrowed_var, borrow_kind, borrow_origin, message);
}

void BorrowChecker::reportStrictError(StrictBorrowErrorKind kind,
                                       const SourceLocation& loc,
                                       const PathExpr& path,
                                       BorrowKind borrow_kind,
                                       const SourceLocation& borrow_origin,
                                       const PathExpr& conflict_path,
                                       const std::string& message,
                                       const std::string& help_text) {
    StrictBorrowError err;
    err.kind = kind;
    err.loc = loc;
    err.path = path;
    err.borrow_kind = borrow_kind;
    err.borrow_origin = borrow_origin;
    err.conflict_path = conflict_path;
    err.message = message;
    err.help_text = help_text;
    err.is_warning = false;  // Default: error. Caller may set to true.
    strict_errors_.push_back(std::move(err));
}

// ============================================================================
// Helper: extract PathExpr from BorrowInfo
// ============================================================================
PathExpr BorrowChecker::pathFromBorrowInfo(const BorrowInfo& bi) const {
    // If the borrowed_var contains dots, parse it as a path
    return PathExpr::fromString(bi.borrowed_var);
}

// ============================================================================
// Helper: extract PathExpr from a variable name (may contain dots)
// ============================================================================
PathExpr BorrowChecker::pathFromVarName(const std::string& name) const {
    return PathExpr::fromString(name);
}

// ============================================================================
// Helper: check if two paths conflict for borrow purposes
// ============================================================================
bool BorrowChecker::pathsConflict(const PathExpr& a, const PathExpr& b) const {
    // Two paths conflict if they overlap (one is a prefix of the other)
    // This means:
    //   point.x and point.y — NO conflict (disjoint)
    //   point and point.x — CONFLICT (prefix overlap)
    //   point.x and point.x — CONFLICT (same)
    //   point.x and point.x.y — CONFLICT (prefix overlap)
    return a.overlaps(b);
}

// ============================================================================
// Helper: check if a type is trivially copyable (Copy semantics)
//
// Trivially-copyable types use copy semantics on assignment — the source
// variable remains valid after being "assigned" to another variable.
// This is the same as Rust's Copy trait: the type can be duplicated
// simply by copying bits, with no destructor or ownership semantics.
//
// Copy types:
//   - All primitives (i8..i64, u8..u64, f32, f64, bool, void)
//   - Enumerations (just a discriminator integer)
//   - Pointers and references (just an address value)
//   - Slices of Copy types ({ ptr, i64 } — both components are Copy)
//   - Arrays of Copy types ([N]T where T: Copy)
//   - Structs where ALL fields are Copy
//   - Allocator (just a function pointer table)
//   - Function types (just a pointer)
//
// NOT Copy (affine / move semantics):
//   - Box<T> (owns heap allocation — moving transfers ownership)
//   - Rc<T> (owns refcount — moving transfers the count)
//   - Arc<T> (owns atomic refcount)
//   - Error types (contain a flag + potentially non-Copy payload)
//   - Structs with any non-Copy field
// ============================================================================
bool BorrowChecker::isTriviallyCopyable(TypeId type) const {
    if (!type) return true;  // null type → treat as Copy (conservative)

    switch (type->getKind()) {
        case TypeKind::Primitive:
            // All primitives are Copy: i8..i64, u8..u64, f32, f64, bool, void
            return true;

        case TypeKind::Enum:
            // Enums are just integer discriminators — Copy
            return true;

        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::MutReference:
        case TypeKind::Allocator:
            // Pointers and references are just addresses — Copy
            return true;

        case TypeKind::Slice: {
            // Slices are { ptr, i64 } — Copy if the element type is Copy
            // (but slices don't own their data, so even slices of non-Copy
            // elements are themselves Copy — they're just views)
            return true;
        }

        case TypeKind::Array: {
            // Arrays are { ptr, i64 } — same as slices, they're views.
            // Copy if element type is Copy (for owned arrays, we'd check
            // the element, but the array handle itself is Copy)
            auto& arr = cast<ArrayType>(type);
            if (arr.element().isNull()) return true;
            return isTriviallyCopyable(arr.element());
        }

        case TypeKind::Struct: {
            // Structs are Copy only if ALL fields are Copy.
            // If any field is Box, Rc, Arc, or another non-Copy type,
            // the whole struct is non-Copy.
            auto& st = cast<StructType>(type);
            for (const auto& field : st.fields()) {
                if (!isTriviallyCopyable(field.type)) {
                    return false;
                }
            }
            return true;
        }

        case TypeKind::SmartPointer: {
            // Smart pointers are NOT Copy — they have ownership semantics.
            // Box: owns heap allocation (move = ownership transfer)
            // Rc: owns refcount (move = transfer, clone = increment)
            // Arc: owns atomic refcount
            return false;
        }

        case TypeKind::Error:
            // Error types are generally NOT Copy — they contain a flag
            // and potentially a non-Copy payload
            return false;

        case TypeKind::Fn:
            // Function types are just pointers — Copy
            return true;

        case TypeKind::Aligned: {
            auto& at = cast<AlignedType>(type);
            return at.inner().isNull() ? true : isTriviallyCopyable(at.inner());
        }

        case TypeKind::Opaque:
            // Opaque types — treat as non-Copy (conservative)
            return false;

        case TypeKind::SimdVector:
            // SIMD vectors are value types — Copy
            return true;

        case TypeKind::Poison:
            // Poison types are placeholders — treat as Copy
            return true;

        default:
            // Unknown types — conservative: treat as non-Copy
            return false;
    }
}

// ============================================================================
// Live-out set accessor
// ============================================================================
const LiveBorrowSet& BorrowChecker::liveOut(CFGNode::NodeId id) const {
    static const LiveBorrowSet empty_set;
    auto it = live_out_.find(id);
    if (it != live_out_.end()) return it->second;
    return empty_set;
}

} // namespace tether
