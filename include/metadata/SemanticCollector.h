#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// L1: Semantic Collector
//
// Walks the AST after semantic analysis and attaches semantic metadata:
//   - Ownership info from var/val declarations
//   - Aliasing from &mut and & references
//   - Mutability tracking
//   - SoA layout detection from struct declarations
//   - Alignment from struct declarations
//   - Function purity, noalloc, restrict, inline
//
// This is the "understand the program" layer.
// ============================================================================
class SemanticCollector {
public:
    // Run the collector on the program, populating the metadata map
    void collect(Program& program, TypeTable& type_table, MetadataMap& meta);

private:
    // Collect from top-level declarations
    void collectFnDecl(FnDecl& fn, MetadataMap& meta);
    void collectStructDecl(StructDecl& sd, TypeTable& type_table, MetadataMap& meta);
    void collectEnumDecl(EnumDecl& ed, MetadataMap& meta);

    // Collect from statements
    void collectStmt(Stmt& stmt, MetadataMap& meta);
    void collectBlockStmt(BlockStmt& block, MetadataMap& meta);
    void collectVarDeclStmt(VarDeclStmt& vd, MetadataMap& meta);
    void collectValDeclStmt(ValDeclStmt& vd, MetadataMap& meta);
    void collectAssignStmt(AssignStmt& as, MetadataMap& meta);
    void collectIfStmt(IfStmt& is, MetadataMap& meta);
    void collectWhileStmt(WhileStmt& ws, MetadataMap& meta);

    // Collect from expressions (for aliasing info)
    void collectExpr(Expr& expr, MetadataMap& meta);

    // Classify a type's aliasing properties
    AliasingKind classifyAliasing(TypeId type) const;

    // Determine if a struct has bool fields that could be packed
    bool hasPackedBitfieldCandidate(StructDecl& sd) const;

    // Count consecutive bool fields
    int countConsecutiveBools(const std::vector<StructFieldDecl>& fields, int start) const;
};

} // namespace tether
