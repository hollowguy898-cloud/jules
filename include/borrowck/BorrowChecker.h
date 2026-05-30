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

namespace tether {

// ============================================================================
// Path expression for precise borrow tracking
// e.g., "point.x" vs "point.y" — these are different borrows
// ============================================================================
struct PathExpr {
    std::vector<std::string> segments;  // e.g., ["point", "x"]

    PathExpr() = default;
    PathExpr(std::initializer_list<std::string> segs) : segments(segs) {}
    explicit PathExpr(std::vector<std::string> segs) : segments(std::move(segs)) {}

    // Construct from a dotted string like "point.x"
    static PathExpr fromString(const std::string& s) {
        PathExpr p;
        std::string seg;
        for (char c : s) {
            if (c == '.') {
                if (!seg.empty()) {
                    p.segments.push_back(std::move(seg));
                    seg.clear();
                }
            } else {
                seg += c;
            }
        }
        if (!seg.empty()) {
            p.segments.push_back(std::move(seg));
        }
        return p;
    }

    std::string toString() const {
        std::string result;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (i > 0) result += ".";
            result += segments[i];
        }
        return result;
    }

    bool operator==(const PathExpr& o) const { return segments == o.segments; }
    bool operator!=(const PathExpr& o) const { return segments != o.segments; }

    // Is this path a prefix of another? (point is prefix of point.x)
    bool isPrefixOf(const PathExpr& other) const {
        if (segments.size() > other.segments.size()) return false;
        for (size_t i = 0; i < segments.size(); ++i) {
            if (segments[i] != other.segments[i]) return false;
        }
        return true;
    }

    // Do these paths overlap? (either is prefix of the other, or they are equal)
    bool overlaps(const PathExpr& other) const {
        return isPrefixOf(other) || other.isPrefixOf(*this);
    }

    bool empty() const { return segments.empty(); }
    size_t size() const { return segments.size(); }
};

struct PathExprHash {
    size_t operator()(const PathExpr& p) const noexcept {
        size_t h = 0;
        for (const auto& s : p.segments) {
            h ^= std::hash<std::string>()(s) + 0x9e3779b9 + (h << 6) + (h >> 2);
        }
        return h;
    }
};

// ============================================================================
// Lifetime identifier
// ============================================================================
using LifetimeId = uint32_t;
static constexpr LifetimeId INVALID_LIFETIME = static_cast<LifetimeId>(-1);

// ============================================================================
// Lifetime constraint: 'a must outlive 'b
// ============================================================================
struct LifetimeConstraint {
    LifetimeId longer;  // 'a
    LifetimeId shorter; // 'b

    bool operator==(const LifetimeConstraint& o) const {
        return longer == o.longer && shorter == o.shorter;
    }
};

// ============================================================================
// Reborrow edge: child reborrows parent
// If parent is invalidated, child is recursively invalidated
// ============================================================================
struct ReborrowEdge {
    PathExpr parent_path;
    PathExpr child_path;
    BorrowKind kind;
};

// ============================================================================
// Function signature for interprocedural analysis
// ============================================================================
struct FnBorrowSignature {
    std::string fn_name;
    std::vector<LifetimeId> param_lifetimes;  // One lifetime per parameter
    LifetimeId return_lifetime;                 // Lifetime of the return value
    std::vector<LifetimeConstraint> constraints; // Lifetime constraints
    std::vector<bool> params_moved;            // Whether each parameter is moved (consumed)
    std::vector<bool> params_noalias;          // Whether each parameter is noalias
    bool is_unsafe = false;                     // Whether this function is in an unsafe block
};

// ============================================================================
// BorrowError - represents a borrow-checking violation (legacy, kept for compat)
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
// Enhanced error kinds for the strict checker
// ============================================================================
enum class StrictBorrowErrorKind : uint8_t {
    MutWhileSharedBorrowed,
    MutWhileMutBorrowed,
    SharedWhileMutBorrowed,
    MutateWhileSharedBorrow,
    MoveWhileBorrowed,
    UseAfterMove,           // Variable used after being moved
    LifetimeOutOfBounds,    // Reference outlives its owner
    ReborrowConflict,       // Reborrow violates parent's borrow rules
    UnsafeRequired,         // Operation requires unsafe block
    BoundsCheckFailed,      // Can't prove array bounds at compile time
};

// ============================================================================
// Enhanced borrow error with path-based tracking
// ============================================================================
struct StrictBorrowError {
    StrictBorrowErrorKind kind;
    SourceLocation loc;
    PathExpr path;               // Path expression of the offending variable
    BorrowKind borrow_kind;
    SourceLocation borrow_origin;
    PathExpr conflict_path;      // Path of the conflicting borrow
    std::string message;
    std::string help_text;       // Suggested fix
    bool is_warning = false;     // If true, this is a warning (in unsafe block)

    std::string toString() const {
        std::string prefix = is_warning ? "warning" : "error";
        std::string result = loc.toString() + ": " + prefix + ": " + message;
        if (!path.empty()) {
            result += " (path: '" + path.toString() + "')";
        }
        if (!conflict_path.empty()) {
            result += " (conflicting path: '" + conflict_path.toString() +
                      "' created at " + borrow_origin.toString() + ")";
        }
        if (!help_text.empty()) {
            result += "\n  help: " + help_text;
        }
        return result;
    }
};

// ============================================================================
// LiveBorrow - represents a borrow that is "live" at some program point
// ============================================================================
struct LiveBorrow {
    std::string borrowed_var;  // the variable being borrowed
    BorrowKind kind;           // shared or mutable
    SourceLocation origin;     // where the borrow was created
    std::string borrow_var;    // the variable holding the borrow (if any)
    PathExpr path;             // path expression (e.g., point.x)

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
// BorrowChecker - implements strict affine type system with NLL checking
//
// The algorithm runs in 7 phases:
//   Phase 1: Collect function signatures (FnBorrowSignature)
//   Phase 2: Compute lifetimes (assign lifetime IDs, build constraints)
//   Phase 3: Check path-based borrow rules (use PathExpr instead of var names)
//   Phase 4: Check reborrows (invalidate child borrows when parent is mutated)
//   Phase 5: Check moves (strictly enforce moved-out vars cannot be used)
//   Phase 6: Solve lifetime constraints (verify no ref outlives owner)
//   Phase 7: Detect noalias params (enhanced with interprocedural info)
//
// Additionally:
//   - Array bounds elimination via checkBounds()
//   - unsafe block support via enterUnsafe()/leaveUnsafe()
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
    // Query results (legacy)
    // -----------------------------------------------------------------------
    bool hasErrors() const { return !errors_.empty() || hasStrictErrors(); }
    const std::vector<BorrowError>& errors() const { return errors_; }

    // -----------------------------------------------------------------------
    // Query results (strict)
    // -----------------------------------------------------------------------
    bool hasStrictErrors() const;
    const std::vector<StrictBorrowError>& strictErrors() const { return strict_errors_; }

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

    // -----------------------------------------------------------------------
    // Function signatures (interprocedural)
    // -----------------------------------------------------------------------
    const std::unordered_map<std::string, FnBorrowSignature>& fnSignatures() const {
        return fn_signatures_;
    }

    // -----------------------------------------------------------------------
    // Bounds proven safe (for IRGenerator to skip bounds checks)
    // -----------------------------------------------------------------------
    const std::unordered_set<const void*>& boundsProvenSafe() const {
        return bounds_proven_safe_;
    }

    // -----------------------------------------------------------------------
    // Nodes proven borrow-safe (for MetadataMap)
    // -----------------------------------------------------------------------
    const std::unordered_set<const void*>& borrowProvenSafe() const {
        return borrow_proven_safe_;
    }

    // -----------------------------------------------------------------------
    // Nodes in unsafe blocks
    // -----------------------------------------------------------------------
    const std::unordered_set<const void*>& unsafeBlockNodes() const {
        return unsafe_block_nodes_;
    }

    // -----------------------------------------------------------------------
    // Moved-out variables
    // -----------------------------------------------------------------------
    const std::unordered_set<std::string>& movedOutVars() const {
        return moved_out_vars_;
    }

private:
    // -----------------------------------------------------------------------
    // Phase 1: Collect function signatures
    // -----------------------------------------------------------------------
    void collectFnSignatures(CFG& cfg);

    // -----------------------------------------------------------------------
    // Phase 2: Compute lifetimes
    // -----------------------------------------------------------------------
    void computeLifetimes(CFG& cfg);

    // -----------------------------------------------------------------------
    // Liveness analysis (backward dataflow)
    // -----------------------------------------------------------------------
    void computeLiveness(CFG& cfg);
    void computeGenKill(CFGNode& node,
                        LiveBorrowSet& gen,
                        LiveBorrowSet& kill);

    // -----------------------------------------------------------------------
    // Phase 3: Check path-based borrow rules
    // -----------------------------------------------------------------------
    void checkBorrowRules(CFG& cfg);

    // Check whether creating a new borrow at a node would violate any rules,
    // given the set of currently live borrows. Path-aware.
    void checkBorrowCreation(CFGNode& node,
                             const BorrowInfo& new_borrow,
                             const LiveBorrowSet& live_borrows);

    // Check whether a mutation (assignment) at a node would violate any rules,
    // given the set of currently live borrows. Path-aware.
    void checkMutation(CFGNode& node,
                       const std::string& var_name,
                       const PathExpr& mut_path,
                       const SourceLocation& loc,
                       const LiveBorrowSet& live_borrows);

    // -----------------------------------------------------------------------
    // Phase 4: Check reborrows
    // -----------------------------------------------------------------------
    void checkReborrows(CFG& cfg);

    // Record a reborrow edge: child reborrows parent
    void recordReborrow(const PathExpr& parent, const PathExpr& child, BorrowKind kind);

    // Invalidate all reborrows rooted at parent_path, recursively
    void invalidateReborrows(const PathExpr& parent_path,
                             std::vector<PathExpr>& invalidated);

    // -----------------------------------------------------------------------
    // Phase 5: Check moves (strict enforcement)
    // -----------------------------------------------------------------------
    void checkMoves(CFG& cfg);

    // Check whether a variable use is valid (not after a move)
    void checkUseAfterMove(const std::string& var_name,
                           const PathExpr& path,
                           const SourceLocation& loc);

    // Mark a variable as moved out (unless its type is Copy)
    // Copy types (primitives, enums) are implicitly copied on assignment,
    // so the original variable remains valid — no move semantics needed.
    void markMoved(const std::string& var_name, TypeId type = TypeId());

    // Check whether a type implements Copy semantics.
    // Copy types are trivially copyable (primitives, enums) and do not
    // have move semantics — assignment copies the value, leaving the
    // source valid.
    static bool isCopyType(TypeId type);

    // -----------------------------------------------------------------------
    // Phase 6: Solve lifetime constraints
    // -----------------------------------------------------------------------
    void solveLifetimeConstraints();

    // Check that no reference outlives its owner
    void checkLifetimes(CFG& cfg);

    // -----------------------------------------------------------------------
    // Phase 7: Detect noalias params
    // -----------------------------------------------------------------------
    void detectNoAliasParams(CFG& cfg);

    // -----------------------------------------------------------------------
    // Array bounds elimination
    // -----------------------------------------------------------------------
    void checkBounds(CFG& cfg);

    // Try to prove that an IndexExpr's index is in bounds
    // Returns true if bounds can be proven at compile time
    bool tryProveBounds(const Expr& object, const Expr& index, CFGNode& node);

    // -----------------------------------------------------------------------
    // Interprocedural analysis
    // -----------------------------------------------------------------------
    void checkInterprocedural(CFG& cfg);

    // At a call site, check the callee's signature
    void checkCallSite(const std::string& fn_name,
                       const std::vector<std::string>& arg_names,
                       const SourceLocation& loc);

    // -----------------------------------------------------------------------
    // Unsafe block support
    // -----------------------------------------------------------------------
    void enterUnsafe();
    void leaveUnsafe();
    bool inUnsafeContext() const { return in_unsafe_context_; }

    // -----------------------------------------------------------------------
    // Error reporting
    // -----------------------------------------------------------------------

    // Legacy error (kept for backward compatibility)
    void reportError(BorrowError::Kind kind,
                     const SourceLocation& loc,
                     const std::string& borrowed_var,
                     BorrowKind borrow_kind,
                     const SourceLocation& borrow_origin,
                     const std::string& message);

    // Strict error (path-based)
    void reportStrictError(StrictBorrowErrorKind kind,
                           const SourceLocation& loc,
                           const PathExpr& path,
                           BorrowKind borrow_kind,
                           const SourceLocation& borrow_origin,
                           const PathExpr& conflict_path,
                           const std::string& message,
                           const std::string& help_text = "");

    // -----------------------------------------------------------------------
    // Helper: extract PathExpr from BorrowInfo
    // -----------------------------------------------------------------------
    PathExpr pathFromBorrowInfo(const BorrowInfo& bi) const;

    // Helper: extract PathExpr from a variable name (may contain dots)
    PathExpr pathFromVarName(const std::string& name) const;

    // Helper: check if two paths conflict for borrow purposes
    // Returns true if the paths overlap (one is prefix of the other)
    bool pathsConflict(const PathExpr& a, const PathExpr& b) const;

    // Helper: check if a type is trivially copyable (Copy semantics).
    // Trivially-copyable types use copy semantics on assignment — the
    // source variable remains valid. This includes:
    //   - Primitive types (i32, f64, bool, etc.)
    //   - Enums (discriminator is just an integer)
    //   - Pointers and references (just an address)
    //   - Slices (ptr + len — both are trivially copyable)
    //   - Arrays of Copy types
    //   - Structs where ALL fields are Copy (no ownership types)
    //
    // NOT trivially copyable (affine / move semantics):
    //   - Box<T> (owns heap allocation)
    //   - Rc<T> (owns refcount)
    //   - Arc<T> (owns atomic refcount)
    //   - Structs containing any non-Copy field
    bool isTriviallyCopyable(TypeId type) const;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------

    // Legacy borrow-checking errors
    std::vector<BorrowError> errors_;

    // Strict borrow-checking errors
    std::vector<StrictBorrowError> strict_errors_;

    // No-alias parameter information
    std::vector<NoAliasInfo> noalias_params_;

    // Liveness analysis results: live_in[n] and live_out[n]
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> live_in_;
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> live_out_;

    // Gen and Kill sets per node (computed once)
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> gen_sets_;
    std::unordered_map<CFGNode::NodeId, LiveBorrowSet> kill_sets_;

    // --- Path-based borrow tracking ---
    using PathBorrowSet = std::unordered_set<PathExpr, PathExprHash>;

    // Track which paths are currently borrowed
    struct ActiveBorrow {
        PathExpr path;
        BorrowKind kind;
        LifetimeId lifetime;
        SourceLocation origin;
        std::string borrow_var;  // Variable holding this borrow
    };

    // Track moved-out variables
    std::unordered_set<std::string> moved_out_vars_;

    // Track reborrows (child → parent edges)
    std::vector<ReborrowEdge> reborrow_edges_;

    // --- Lifetime inference ---
    std::vector<LifetimeConstraint> lifetime_constraints_;
    std::unordered_map<std::string, LifetimeId> var_lifetimes_;  // var → lifetime
    LifetimeId next_lifetime_id_ = 1;  // 0 is 'static

    // --- Interprocedural signatures ---
    std::unordered_map<std::string, FnBorrowSignature> fn_signatures_;

    // --- Unsafe context ---
    bool in_unsafe_context_ = false;
    int unsafe_depth_ = 0;

    // --- Strict mode ---
    bool strict_mode_ = true;  // Default: strict (no guards needed)

    // --- Bounds proven safe (set of AST node pointers) ---
    std::unordered_set<const void*> bounds_proven_safe_;

    // --- Borrow proven safe (set of AST node pointers) ---
    std::unordered_set<const void*> borrow_proven_safe_;

    // --- Nodes in unsafe blocks ---
    std::unordered_set<const void*> unsafe_block_nodes_;
};

} // namespace tether
