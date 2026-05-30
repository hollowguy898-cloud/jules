#pragma once

#include "ast/AST.h"
#include "sema/Type.h"

#include <unordered_map>
#include <memory>
#include <string>
#include <vector>

namespace tether {

// ============================================================================
// ASTCloner — deep-copy utility for AST nodes
//
// Creates an independent deep copy of any AST subtree. Each unique_ptr child
// is recursively cloned, and TypeId references are preserved (since TypeIds
// are interned flyweight pointers — copying them is safe).
//
// Optionally, a type substitution map can be provided: strings (type param
// names like "T") → TypeId (concrete types like i32). When a VarDeclStmt,
// ValDeclStmt, or CastExpr has a declared type whose toString() matches a
// type parameter name, it is replaced with the concrete type. Expression
// types (filled by semantic analysis) are also substituted.
// ============================================================================
class ASTCloner {
public:
    // Construct with optional type substitution map
    // e.g., subst = { {"T", i32_type_id} }
    explicit ASTCloner(
        const std::unordered_map<std::string, TypeId>& type_subst = {})
        : type_subst_(type_subst) {}

    // Clone an expression (returns a new independent copy)
    std::unique_ptr<Expr> cloneExpr(const Expr* expr);

    // Clone a statement (returns a new independent copy)
    std::unique_ptr<Stmt> cloneStmt(const Stmt* stmt);

    // Clone a block (returns a new independent copy)
    std::unique_ptr<BlockStmt> cloneBlock(const BlockStmt* block);

    // Clone a function declaration (returns a new independent copy)
    std::unique_ptr<FnDecl> cloneFnDecl(const FnDecl* fn);

    // Apply type substitution to a TypeId
    TypeId substituteType(TypeId type) const;

private:
    std::unordered_map<std::string, TypeId> type_subst_;

    // Clone helpers for aggregate sub-types
    DesignatedInit cloneDesignatedInit(const DesignatedInit& init);
    MatchArm cloneMatchArm(const MatchArm& arm);
};

} // namespace tether
