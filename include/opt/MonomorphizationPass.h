#pragma once

#include "opt/PreLLVMPassBase.h"
#include "sema/Type.h"
#include "ast/AST.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace tether {

// ============================================================================
// GenericInstance — represents a concrete instantiation of a generic function
//
// When a generic function like `fn max<T>(a: T, b: T) -> T` is called with
// `max(3, 5)`, we create a GenericInstance with concrete_types = [i32] and
// mangled_name = "max_i32". The monomorphized function has all type parameters
// replaced with concrete types, enabling LLVM to generate optimal code without
// vtable indirection or boxing.
// ============================================================================
struct GenericInstance {
    std::string generic_fn_name;       // e.g., "max"
    std::vector<TypeId> concrete_types; // e.g., [i32]
    std::string mangled_name;          // e.g., "max_i32"
    FnDecl* original_fn = nullptr;     // The generic function declaration
};

// ============================================================================
// MonomorphizationPass — replaces generic calls with concrete instantiations
//
// This pass is the Tether equivalent of C++ template instantiation. Without
// monomorphization, generic functions would need runtime dispatch (vtable) or
// boxing — both are 10-100x slower than generating specialized code for each
// concrete type.
//
// The pass operates in three phases:
//   1. Identify all generic function definitions (those with type_params_)
//   2. Walk all call sites and infer concrete type arguments from argument types
//   3. For each (generic_fn, concrete_types) pair, record a GenericInstance
//      and store the mangled name in the MetadataMap for the IRGenerator to use
//
// LLVM CANNOT do this: it has no concept of type-parameterized functions.
// Monomorphization must happen at the AST level before LLVM IR emission.
//
// Future work:
//   - Clone the generic function body and substitute type parameters
//   - Add the monomorphized function to the Program
//   - Replace the original call with a call to the monomorphized version
//   - Support explicit type arguments at call sites (e.g., max<i32>(3, 5))
// ============================================================================
class MonomorphizationPass : public PreLLVMPass {
public:
    MonomorphizationPass() = default;

    std::string name() const override { return "Monomorphization"; }
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }

    // Run monomorphization on all functions
    bool run(Program& program, TypeTable& type_table) override;

    // Get all instantiated functions
    const std::vector<GenericInstance>& instances() const { return instances_; }

    // Get the mangled name for a specific instantiation
    std::string getMangledName(const std::string& fn_name,
                                const std::vector<TypeId>& types) const;

private:
    // Collected instantiations
    std::vector<GenericInstance> instances_;

    // Map from (fn_name;type1;type2;...) -> mangled_name
    std::unordered_map<std::string, std::string> instance_map_;

    // Set of already-instantiated combinations (same key format as instance_map_)
    std::unordered_set<std::string> instantiated_;

    // Generate a mangled name for a generic instantiation
    std::string mangle(const std::string& base_name,
                       const std::vector<TypeId>& types) const;

    // Sanitize a type name for use in mangled identifiers
    std::string sanitizeTypeName(const std::string& type_name) const;

    // Collect all call sites that need monomorphization
    void collectCallSites(Program& program, TypeTable& type_table);

    // Walk an expression tree looking for calls to generic functions
    void walkExpr(Expr* expr,
                  const std::vector<FnDecl*>& generic_fns,
                  TypeTable& type_table);

    // Walk a statement tree looking for calls to generic functions
    void walkStmt(Stmt* stmt,
                  const std::vector<FnDecl*>& generic_fns,
                  TypeTable& type_table);

    // Walk a block looking for calls to generic functions
    void walkBlock(BlockStmt* block,
                   const std::vector<FnDecl*>& generic_fns,
                   TypeTable& type_table);
};

} // namespace tether
