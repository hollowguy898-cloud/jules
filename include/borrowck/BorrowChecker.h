#pragma once

#include "cfg/CFG.h"
#include "sema/Type.h"

#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <memory>
#include <cstdint>

namespace jules {

// ============================================================================
// BorrowError - represents a borrow-checking violation
// ============================================================================
struct BorrowError {
    enum class Kind : uint8_t {
        SharedWhileMutBorrowed,  // &T created while &mut T is live
        MutWhileSharedBorrowed,  // &mut T created while &T is live
        MutWhileMutBorrowed,     // &mut T created while another &mut T is live
        MutateWhileSharedBorrow, // variable mutated while &T to it is live
        MoveWhileBorrowed,       // variable moved while any borrow to it is live
    };

    Kind kind;
    SourceLocation loc;             // location of the offending operation
    std::string borrowed_var;       // name of the variable being borrowed
    BorrowKind borrow_kind;         // kind of the conflicting borrow
    SourceLocation borrow_origin;   // where the conflicting borrow was created
    std::string message;

    BorrowError(Kind k, SourceLocation l, std::string var,
                BorrowKind bk, SourceLocation bo, std::string msg)
        : kind(k), loc(std::move(l)), borrowed_var(std::move(var))
        , borrow_kind(bk), borrow_origin(std::move(bo)), message(std::move(msg))
    {}

    std::string toString() const {
        return loc.toString() + ": error: " + message +
               " (conflicting borrow of '" + borrowed_var + "' created at " +
               borrow_origin.toString() + ")";
    }
};

// ============================================================================
// LiveBorrow - represents a borrow that is "live" at some program point
//
// A borrow is live at a point if there exists a path from that point to
// a use of the borrow before the borrow is killed (NLL principle).
// ============================================================================
struct LiveBorrow {
    std::string borrowed_var;  // the variable being borrowed
    BorrowKind kind;           // shared or mutable
    SourceLocation origin;     // where the borrow was created
    std::string borrow_var;    // the variable holding the borrow (if any)

    // Equality for set operations
    bool operator==(const LiveBorrow& other) const {
        return borrowed_var == other.borrowed_var &&
               kind == other.kind &&
               origin.line == other.origin.line &&
               origin.col == other.origin.col;
    }

    bool operator!=(const LiveBorrow& other) const {
        return !(*this == other);
    }
};

// Hash functor for LiveBorrow
struct LiveBorrowHash {
    size_t operator()(const LiveBorrow& lb) const noexcept {
        size_t h = std::hash<std::string>()(lb.borrowed_var);
        h ^= std::hash<uint32_t>()(lb.origin.line) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<uint32_t>()(lb.origin.col) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= static_cast<size_t>(lb.kind) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

using LiveBorrowSet = std::unordered_set<LiveBorrow, LiveBorrowHash>;

// ============================================================================
// NoAliasInfo - tracks which parameters are noalias for codegen
// ============================================================================
struct NoAliasInfo {
    std::string param_name;
    TypeId param_type;

    NoAliasInfo(std::string name, TypeId type)
        : param_name(std::move(name)), param_type(type) {}
};

// ============================================================================
// BorrowChecker - implements Non-Lexical Lifetimes (NLL) checking
//
// The algorithm:
//   1. For each function, build the CFG (already done by CFGBuilder).
//   2. Perform liveness analysis on the CFG to compute, for each node,
//      the set of borrows that are live at the EXIT of that node.
//      - A borrow is live at a point if it might be used on some path
//        from that point to the exit.
//   3. For each node, check borrow rules at the ENTRY (using the live-out
//      set of the predecessor) and at each statement within the node.
//   4. Report errors for any violations.
//
// Liveness analysis is a backward dataflow analysis:
//   live_in(n)  = gen(n) ∪ (live_out(n) - kill(n))
//   live_out(n) = ∪ live_in(s) for all successors s of n
//
// Where:
//   gen(n)  = borrows created in node n
//   kill(n) = borrows whose last use is in node n
// ============================================================================
class BorrowChecker {
public:
    BorrowChecker() = default;

    // -----------------------------------------------------------------------
    // Main entry point: check a single function's CFG
    // -----------------------------------------------------------------------
    void check(CFG& cfg);

    // -----------------------------------------------------------------------
    // Check all functions (convenience for multiple CFGs)
    // -----------------------------------------------------------------------
    void checkAll(std::vector<std::unique_ptr<CFG>>& cfgs);

    // -----------------------------------------------------------------------
    // Query results
    // -----------------------------------------------------------------------
    bool hasErrors() const { return !errors_.empty(); }
    const std::vector<BorrowError>& errors() const { return errors_; }

    // -----------------------------------------------------------------------
    // No-alias information for codegen
    // -----------------------------------------------------------------------
    const std::vector<NoAliasInfo>& noAliasParams() const { return noalias_params_; }

    // -----------------------------------------------------------------------
    // Get the live-borrow set at the exit of a specific node (for debugging)
    // -----------------------------------------------------------------------
    const LiveBorrowSet& liveOut(CFGNode::NodeId id) const;

    // -----------------------------------------------------------------------
    // Get all computed liveness data (for downstream passes)
    // -----------------------------------------------------------------------
    const std::unordered_map<CFGNode::NodeId, LiveBorrowSet>& liveOutSets() const {
        return live_out_;
    }
    const std::unordered_map<CFGNode::NodeId, LiveBorrowSet>& liveInSets() const {
        return live_in_;
    }

private:
    // -----------------------------------------------------------------------
    // Liveness analysis
    // -----------------------------------------------------------------------

    // Compute live-in and live-out sets for all nodes in the CFG.
    // Uses iterative backward dataflow analysis.
    void computeLiveness(CFG& cfg);

    // Compute the gen set (borrows created) and kill set (borrows killed)
    // for a single node.
    void computeGenKill(CFGNode& node,
                        LiveBorrowSet& gen,
                        LiveBorrowSet& kill);

    // -----------------------------------------------------------------------
    // Borrow rule checking
    // -----------------------------------------------------------------------

    // Check borrow rules at every node, using the computed liveness data.
    void checkBorrowRules(CFG& cfg);

    // Check whether creating a new borrow at a node would violate any rules,
    // given the set of currently live borrows.
    void checkBorrowCreation(CFGNode& node,
                             const BorrowInfo& new_borrow,
                             const LiveBorrowSet& live_borrows);

    // Check whether a mutation (assignment) at a node would violate any rules,
    // given the set of currently live borrows.
    void checkMutation(CFGNode& node,
                       const std::string& var_name,
                       const SourceLocation& loc,
                       const LiveBorrowSet& live_borrows);

    // Check whether a move of a variable would violate any rules.
    void checkMove(CFGNode& node,
                   const std::string& var_name,
                   const SourceLocation& loc,
                   const LiveBorrowSet& live_borrows);

    // -----------------------------------------------------------------------
    // No-alias parameter detection
    // -----------------------------------------------------------------------

    // Identify &mut parameters and mark them as noalias.
    void detectNoAliasParams(CFG& cfg);

    // -----------------------------------------------------------------------
    // Error reporting
    // -----------------------------------------------------------------------

    void reportError(BorrowError::Kind kind,
                     const SourceLocation& loc,
                     const std::string& borrowed_var,
                     BorrowKind borrow_kind,
                     const SourceLocation& borrow_origin,
                     const std::string& message);

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    // Borrow-checking errors
    std::vector<BorrowError> errors_;

    // No-alias parameter information
    std::vector<NoAliasInfo> noalias_params_;

    // Liveness analysis results: live_in[n] and live_out[n]
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> live_in_;
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> live_out_;

    // Gen and Kill sets per node (computed once)
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> gen_sets_;
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> kill_sets_;
};

} // namespace jules
