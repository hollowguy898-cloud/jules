#pragma once

#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"

namespace tether {

// ============================================================================
// L3: Memory Topology Analyzer
//
// This is the real heart of post-Moore optimization. Detects:
//   - Linear traversal (sequential index access)
//   - Strided traversal (index += constant > 1)
//   - Sparse access (conditional access in loops)
//   - Gather/scatter patterns
//   - Cache-line waste
//   - Pointer chasing
//
// Attaches metadata like:
//   streaming_access=true
//   prefetch_distance=2_cachelines
//   likely_sequential=true
// ============================================================================
class MemoryTopologyAnalyzer {
public:
    // Run the analyzer on the program
    void analyze(Program& program, TypeTable& type_table, MetadataMap& meta);

    // Index access info — public so file-scope helpers can use it
    struct IndexAccessInfo {
        std::string variable_name;  // The array/slice being indexed
        Expr* index_expr;           // The index expression
        AccessKind access_kind;     // Read or write
        bool is_assignment_target;  // Is this on the left side of = ?
    };

private:
    // Analyze a function body
    void analyzeFn(FnDecl& fn, TypeTable& type_table, MetadataMap& meta);

    // Analyze a while loop for access patterns
    void analyzeWhileLoop(WhileStmt& ws, TypeTable& type_table, MetadataMap& meta);

    // Collect all index expressions within a loop body
    std::vector<IndexAccessInfo> collectIndexAccesses(BlockStmt& body);

    // Determine the traversal kind from the index expressions
    TraversalKind determineTraversal(
        const std::vector<IndexAccessInfo>& accesses,
        WhileStmt& ws) const;

    // Check if the loop variable increments by 1 (linear traversal)
    bool isLinearIncrement(WhileStmt& ws) const;

    // Check if the loop variable increments by a constant K (strided)
    bool isStridedIncrement(WhileStmt& ws, int64_t& stride) const;

    // Get the loop variable name from a while loop
    std::string getLoopVarName(WhileStmt& ws) const;

    // Compute prefetch distance based on access pattern and type size
    int64_t computePrefetchDistance(
        TraversalKind traversal,
        uint64_t element_size_bytes) const;

    // Check if accesses are to contiguous memory
    bool isContiguousAccess(
        const std::vector<IndexAccessInfo>& accesses,
        const std::string& loop_var) const;

    // Check if the loop body has dependencies that prevent vectorization
    bool hasLoopCarriedDependency(
        const std::vector<IndexAccessInfo>& reads,
        const std::vector<IndexAccessInfo>& writes,
        const std::string& loop_var) const;

    // Analyze struct types for cache-line waste
    void analyzeStructWaste(StructDecl& sd, TypeTable& type_table, MetadataMap& meta);

    // Compute cache-line utilization for a struct
    // Returns the number of wasted bytes per cache line
    uint64_t computeCacheLineWaste(StructDecl& sd, TypeTable& type_table) const;

    // Walk statements recursively
    void walkStmts(BlockStmt& block, TypeTable& type_table, MetadataMap& meta);
};

} // namespace tether
