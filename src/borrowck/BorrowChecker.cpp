#include "borrowck/BorrowChecker.h"

#include <algorithm>
#include <cassert>
#include <sstream>

namespace jules {

// ============================================================================
// Main entry point: check a single function's CFG
// ============================================================================
void BorrowChecker::check(CFG& cfg) {
    // Phase 1: Compute liveness (live-in / live-out sets for borrows)
    computeLiveness(cfg);

    // Phase 2: Check borrow rules at every node
    checkBorrowRules(cfg);

    // Phase 3: Detect noalias parameters
    detectNoAliasParams(cfg);
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
    //
    // Standard backward dataflow equations:
    //   live_in(n)  = gen(n) ∪ (live_out(n) - kill(n))
    //   live_out(n) = ∪ live_in(s) for s ∈ successors(n)
    //
    // We iterate until no changes occur.

    // Get a reverse postorder for more efficient convergence
    std::vector<CFGNode*> rpo = cfg.reversePostorder();
    // For backward analysis, we want reverse of RPO = postorder
    std::reverse(rpo.begin(), rpo.end());

    bool changed = true;
    int iterations = 0;
    const int MAX_ITERATIONS = 1000; // safety limit

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
        gen.insert(std::move(lb));
    }

    // Kill: borrows whose borrowed variable is redefined in this node.
    // When a variable is redefined, any existing borrows of that variable
    // are no longer valid (they are "killed").
    // Also, if a borrow variable is redefined, the old borrow is killed.
    for (const auto& var : node.definedVars()) {
        // Kill any live borrows of this variable
        for (const auto& bi : node.borrowsCreated()) {
            if (bi.borrow_var == var) {
                LiveBorrow lb;
                lb.borrowed_var = bi.borrowed_var;
                lb.kind = bi.kind;
                lb.origin = bi.origin;
                lb.borrow_var = bi.borrow_var;
                kill.insert(std::move(lb));
            }
        }
    }

    // If a variable that was borrowed gets a new borrow of a different kind,
    // the old borrow is implicitly killed (e.g., &x followed by &mut x kills &x).
    // This is actually a borrow-check error, but for liveness purposes,
    // the new borrow supersedes the old one.
    for (const auto& bi : node.borrowsCreated()) {
        // Kill any previous borrow of the same variable with different kind
        LiveBorrow lb;
        lb.borrowed_var = bi.borrowed_var;
        lb.kind = (bi.kind == BorrowKind::Shared)
            ? BorrowKind::MutExclusive : BorrowKind::Shared;
        lb.origin = bi.origin;
        lb.borrow_var = bi.borrow_var;
        // We don't know the exact origin of the old borrow here,
        // so we can't precisely kill it. The borrow checker will
        // catch the conflict. For liveness, we conservatively
        // keep the old borrow alive.
    }
}

// ============================================================================
// Borrow rule checking
// ============================================================================
void BorrowChecker::checkBorrowRules(CFG& cfg) {
    for (const auto& node_ptr : cfg.nodes()) {
        CFGNode& node = *node_ptr;
        // The set of borrows that are live at the ENTRY of this node
        // is live_in[node]. However, we also need to account for
        // borrows created within this node and check against the
        // borrows that are live just before the creation point.

        // Start with the live-in set
        LiveBorrowSet current_live = live_in_[node.id()];

        // Check each borrow created in this node
        for (const auto& bi : node.borrowsCreated()) {
            // Before creating this borrow, check if it conflicts
            // with currently live borrows
            checkBorrowCreation(node, bi, current_live);

            // Add the new borrow to the current live set
            LiveBorrow lb;
            lb.borrowed_var = bi.borrowed_var;
            lb.kind = bi.kind;
            lb.origin = bi.origin;
            lb.borrow_var = bi.borrow_var;
            current_live.insert(std::move(lb));
        }

        // Check each variable definition (mutation) in this node
        for (const auto& var : node.definedVars()) {
            // Find the source location for this mutation
            // We approximate by using the node's first statement location
            SourceLocation mut_loc = node.stmts().empty()
                ? SourceLocation() : node.stmts().front()->sourceLoc();

            // Check if there's a VarDeclStmt that defines this variable
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

            checkMutation(node, var, mut_loc, current_live);
        }

        // Check for moves (approximately: variable use after borrow)
        for (const auto& var : node.usedVars()) {
            SourceLocation use_loc = node.stmts().empty()
                ? SourceLocation() : node.stmts().front()->sourceLoc();

            // A "move" in this context means the variable is consumed.
            // For now, we approximate: if a variable that is borrowed is
            // used as a function argument or assigned to another variable,
            // that could be a move.
            // We check this more precisely by looking at the actual statements.
            for (auto* stmt : node.stmts()) {
                if (stmt->getKind() == NodeKind::AssignStmt) {
                    auto& as = static_cast<AssignStmt&>(*stmt);
                    // If the RHS is a simple identifier, it might be a move
                    if (as.value()->getKind() == NodeKind::IdentExpr) {
                        auto& ie = static_cast<IdentExpr&>(*as.value());
                        if (ie.name() == var) {
                            use_loc = as.sourceLoc();
                            checkMove(node, var, use_loc, current_live);
                        }
                    }
                }
            }
        }
    }
}

void BorrowChecker::checkBorrowCreation(CFGNode& /*node*/,
                                         const BorrowInfo& new_borrow,
                                         const LiveBorrowSet& live_borrows) {
    for (const auto& lb : live_borrows) {
        // Only check borrows of the same variable
        if (lb.borrowed_var != new_borrow.borrowed_var) continue;

        // Rule 1: Cannot create &mut T while any &T or &mut T to same
        // variable is live
        if (new_borrow.kind == BorrowKind::MutExclusive) {
            if (lb.kind == BorrowKind::Shared) {
                reportError(
                    BorrowError::Kind::MutWhileSharedBorrowed,
                    new_borrow.origin,
                    new_borrow.borrowed_var,
                    lb.kind,
                    lb.origin,
                    "cannot create '&mut " + new_borrow.borrowed_var +
                    "' because it is already borrowed as & (shared)");
            } else if (lb.kind == BorrowKind::MutExclusive) {
                reportError(
                    BorrowError::Kind::MutWhileMutBorrowed,
                    new_borrow.origin,
                    new_borrow.borrowed_var,
                    lb.kind,
                    lb.origin,
                    "cannot create '&mut " + new_borrow.borrowed_var +
                    "' because it is already exclusively borrowed as &mut");
            }
        }

        // Rule 2: Cannot create &T while an &mut T to same variable is live
        if (new_borrow.kind == BorrowKind::Shared &&
            lb.kind == BorrowKind::MutExclusive) {
            reportError(
                BorrowError::Kind::SharedWhileMutBorrowed,
                new_borrow.origin,
                new_borrow.borrowed_var,
                lb.kind,
                lb.origin,
                "cannot create '&" + new_borrow.borrowed_var +
                "' because it is exclusively borrowed as &mut");
        }

        // Note: Multiple &T borrows to the same variable are allowed
        // (shared borrows can coexist)
    }
}

void BorrowChecker::checkMutation(CFGNode& /*node*/,
                                   const std::string& var_name,
                                   const SourceLocation& loc,
                                   const LiveBorrowSet& live_borrows) {
    for (const auto& lb : live_borrows) {
        // Only check borrows of the same variable
        if (lb.borrowed_var != var_name) continue;

        // Rule 3: Cannot mutate a variable while a &T to it is live
        if (lb.kind == BorrowKind::Shared) {
            reportError(
                BorrowError::Kind::MutateWhileSharedBorrow,
                loc,
                var_name,
                lb.kind,
                lb.origin,
                "cannot mutate '" + var_name +
                "' because it is borrowed as & (shared)");
        }

        // Mutating while &mut is live is also an error (double mutable borrow)
        if (lb.kind == BorrowKind::MutExclusive) {
            reportError(
                BorrowError::Kind::MutWhileMutBorrowed,
                loc,
                var_name,
                lb.kind,
                lb.origin,
                "cannot mutate '" + var_name +
                "' because it is exclusively borrowed as &mut");
        }
    }
}

void BorrowChecker::checkMove(CFGNode& /*node*/,
                               const std::string& var_name,
                               const SourceLocation& loc,
                               const LiveBorrowSet& live_borrows) {
    for (const auto& lb : live_borrows) {
        // Only check borrows of the same variable
        if (lb.borrowed_var != var_name) continue;

        // Rule 4: Cannot move a variable while any borrow to it is live
        reportError(
            BorrowError::Kind::MoveWhileBorrowed,
            loc,
            var_name,
            lb.kind,
            lb.origin,
            "cannot move '" + var_name +
            "' because it is borrowed" +
            (lb.kind == BorrowKind::Shared ? " as & (shared)" : " as &mut (exclusive)"));
    }
}

// ============================================================================
// No-alias parameter detection
// ============================================================================
void BorrowChecker::detectNoAliasParams(CFG& cfg) {
    // Parameters of type &mut T can be marked noalias for the codegen,
    // provided they don't alias with any other &mut parameter or get
    // reborrowed in conflicting ways.
    //
    // Simple heuristic: any &mut parameter that passes borrow checking
    // without errors can be marked noalias.
    //
    // We check if there were any borrow errors involving this parameter.

    // Collect the set of variables involved in borrow errors
    std::unordered_set<std::string> error_vars;
    for (const auto& err : errors_) {
        error_vars.insert(err.borrowed_var);
    }

    // Walk through the entry node to find parameter borrows
    CFGNode* entry = cfg.entry();
    if (!entry) return;

    // The entry node's statements should include parameter declarations.
    // We also look at the live_in set for the entry to find what's borrowed.
    // For now, we scan the first few nodes for &mut parameter usage.

    for (const auto& node : cfg.nodes()) {
        for (const auto& bi : node->borrowsCreated()) {
            // If a parameter is borrowed as &mut and no errors involve it,
            // mark it as noalias
            if (bi.kind == BorrowKind::MutExclusive) {
                // Check if the borrowed variable is a parameter
                // (Heuristic: if it appears in the entry node's uses
                //  and is defined in the entry, it's likely a parameter)
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

// ============================================================================
// Live-out set accessor
// ============================================================================
const LiveBorrowSet& BorrowChecker::liveOut(CFGNode::NodeId id) const {
    static const LiveBorrowSet empty_set;
    auto it = live_out_.find(id);
    if (it != live_out_.end()) return it->second;
    return empty_set;
}

} // namespace jules
