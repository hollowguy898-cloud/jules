#pragma once

#include "opt/PreLLVMPassBase.h"
#include "metadata/MetaTypes.h"
#include "ast/AST.h"
#include "sema/Type.h"

#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace tether {

// AssumptionKind is defined in MetaTypes.h — it's shared between
// the speculative optimizer and the MetadataMap.
// Forward declaration for the toString helper in this module.
const char* assumptionKindToString(AssumptionKind k);

// ============================================================================
// A single speculative assumption
// ============================================================================
struct SpeculativeAssumption {
    AssumptionKind kind;
    const ASTNode* node = nullptr;     // The node this assumption applies to
    std::string description;           // Human-readable description for diagnostics
    float confidence = 0.0f;           // 0.0-1.0, how confident we are
    uint64_t profile_count = 0;        // How many times observed (from PGO)
    uint64_t violation_count = 0;      // How many times assumption was violated
};

// ============================================================================
// Result of speculative optimization on a function
// ============================================================================
struct SpeculativeOptResult {
    std::vector<SpeculativeAssumption> assumptions;
    std::vector<const ASTNode*> guarded_nodes;  // Nodes that got deopt guards
    int assumptions_made = 0;
    int guards_inserted = 0;
};

// ============================================================================
// SpeculativeOptimizerPass - Identifies opportunities for speculative
// optimization and marks them in the MetadataMap for the IRGenerator
//
// The IRGenerator can then emit deoptimization guards for these assumptions,
// allowing the fast path to run without runtime checks.
//
// This pass runs in Phase 2 of the unified pipeline (after L1-L3 analysis,
// before transforms), so it can use PGO data, branch probabilities, and
// access pattern analysis.
// ============================================================================
class SpeculativeOptimizerPass : public PreLLVMPass {
public:
    SpeculativeOptimizerPass() = default;

    std::string name() const override { return "SpeculativeOptimizer"; }
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }

    // Run speculative analysis on all functions
    bool run(Program& program, TypeTable& type_table) override;

    // Get speculative assumptions for a node
    const SpeculativeAssumption* getAssumption(const ASTNode* node) const;

    // Get all assumptions for a function
    const SpeculativeOptResult* getFunctionResult(const std::string& fn_name) const;

    // Check if a node has a deoptimization guard
    bool hasDeoptGuard(const ASTNode* node) const;

    // Get/set the confidence threshold (assumptions below this are not made)
    float confidenceThreshold() const { return confidence_threshold_; }
    void setConfidenceThreshold(float t) { confidence_threshold_ = t; }

    bool hasResults() const { return !function_results_.empty(); }

    // Total statistics
    int totalAssumptions() const { return total_assumptions_; }
    int totalGuards() const { return total_guards_; }

private:
    float confidence_threshold_ = 0.95f;  // Only speculate if >95% confident
    int total_assumptions_ = 0;
    int total_guards_ = 0;

    // Analyze a single function
    SpeculativeOptResult analyzeFunction(FnDecl& fn, TypeTable& type_table);

    // Determine if a branch is worth speculating on
    bool isWorthSpeculating(IfStmt& is);

    // Determine if a pointer dereference is worth speculating non-null
    bool isWorthSpeculatingNonNull(Expr* expr);

    // Determine if an array access is worth speculating bounds on
    bool isWorthSpeculatingBounds(IndexExpr& idx);

    // Determine if arithmetic is worth speculating no-overflow on
    bool isWorthSpeculatingNoOverflow(BinaryExpr& bin);

    // Recursively walk an expression to find speculation opportunities
    void walkExpr(Expr* expr, SpeculativeOptResult& result);
    void walkStmt(Stmt* stmt, SpeculativeOptResult& result);

    // Add an assumption to the MetadataMap
    void addAssumptionToMetaMap(const ASTNode* node, const SpeculativeAssumption& assumption);

    std::unordered_map<const ASTNode*, SpeculativeAssumption> node_assumptions_;
    std::unordered_map<std::string, SpeculativeOptResult> function_results_;
    std::unordered_set<const ASTNode*> guarded_nodes_;
};

} // namespace tether
