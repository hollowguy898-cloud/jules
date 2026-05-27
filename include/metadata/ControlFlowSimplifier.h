#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// L2: Control-Flow Simplifier
//
// Converts if/else to select/masks/predication when profitable.
// Important: only applies transforms when:
//   - Both branches are cheap (simple expressions, no function calls)
//   - Memory behavior remains coherent (no side effects)
//   - Vectorization benefits exist
//
// Avoids "branchless everything syndrome" — branchless code can be
// slower if it computes both sides expensively.
// ============================================================================
class ControlFlowSimplifier {
public:
    // Run the simplifier on the program, updating metadata
    void simplify(Program& program, TypeTable& type_table, MetadataMap& meta);

private:
    // Analyze a single if statement for select profitability
    void analyzeIfStmt(IfStmt& is, MetadataMap& meta);

    // Check if an expression is "cheap" (no side effects, no function calls,
    // no heap access, no more than a few operations)
    bool isCheapExpr(Expr& expr) const;

    // Count the number of operations in an expression tree
    int countOps(Expr& expr) const;

    // Check if an expression has side effects
    bool hasSideEffects(Expr& expr) const;

    // Check if an expression would benefit from vectorization
    bool wouldBenefitFromVectorization(IfStmt& is, MetadataMap& meta) const;

    // Walk all statements recursively
    void walkStmts(BlockStmt& block, MetadataMap& meta);
};

} // namespace tether
