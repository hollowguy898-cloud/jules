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
// Optionally, a type substitution map and TypeTable can be provided for
// monomorphization: strings (type param names like "T") → TypeId (concrete
// types like i32). When any type's toString() matches a type parameter name,
// it is replaced with the concrete type. For composite types (*T, &T, Box<T>,
// etc.), the substitution recurses into inner types and the TypeTable is used
// to intern the resulting substituted composite type.
// ============================================================================
class ASTCloner {
public:
    // Construct with optional type substitution map
    // e.g., subst = { {"T", i32_type_id} }
    // type_table: required for recursive composite type substitution
    //   (creating *i32 from *T requires TypeTable::getPointer(i32))
    explicit ASTCloner(
        const std::unordered_map<std::string, TypeId>& type_subst = {},
        TypeTable* type_table = nullptr)
        : type_subst_(type_subst), type_table_(type_table) {}

    // Clone an expression (returns a new independent copy)
    std::unique_ptr<Expr> cloneExpr(const Expr* expr);

    // Clone a statement (returns a new independent copy)
    std::unique_ptr<Stmt> cloneStmt(const Stmt* stmt);

    // Clone a block (returns a new independent copy)
    std::unique_ptr<BlockStmt> cloneBlock(const BlockStmt* block);

    // Clone a function declaration (returns a new independent copy)
    std::unique_ptr<FnDecl> cloneFnDecl(const FnDecl* fn);

    // Apply type substitution to a TypeId.
    // For simple types where toString() matches a type param name, returns
    // the concrete type directly. For composite types (*T, &T, Box<T>, etc.),
    // recursively substitutes inner types and creates the new composite type
    // via TypeTable (if provided).
    TypeId substituteType(TypeId type) const;

    // Substitute a type param name string with its concrete equivalent.
    // Used for FnParam::unresolved_type_name substitution.
    std::string substituteTypeName(const std::string& name) const;

private:
    std::unordered_map<std::string, TypeId> type_subst_;
    TypeTable* type_table_;  // May be nullptr — needed for composite type subst

    // Clone helpers for aggregate sub-types
    DesignatedInit cloneDesignatedInit(const DesignatedInit& init);
    MatchArm cloneMatchArm(const MatchArm& arm);
};

} // namespace tether
