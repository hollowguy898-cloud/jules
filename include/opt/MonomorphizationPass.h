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
// The pass operates in six phases:
//   Phase 1: Identify all generic function definitions (those with type_params_)
//   Phase 2: Walk all call sites and infer concrete type arguments from arg types
//   Phase 3: For each (generic_fn, concrete_types) pair, record a GenericInstance
//   Phase 4: Clone the generic function body with type parameter substitution
//   Phase 5: Add monomorphized functions to the Program
//   Phase 6: Rewrite call sites to call the monomorphized function
//
// Phases 4-6 are the "keystone" optimization: they unlock ALL other
// optimizations (inlining, SROA, TBAA, auto-vectorization, escape analysis,
// AoS→SoA, etc.) for generic code, which was previously type-erased and
// opaque to LLVM.
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

    // Get the monomorphization map: original_name -> mangled_name
    // Used by the IRGenerator to resolve call targets
    const std::unordered_map<std::string, std::string>& instanceMap() const {
        return instance_map_;
    }

    // Look up the monomorphized name for a call to fn_name with the given
    // argument types. Returns empty string if no monomorphized version exists.
    std::string resolveCall(const std::string& fn_name,
                            const std::vector<TypeId>& arg_types) const;

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

    // Make the lookup key for instance_map_: fn_name;type1;type2;...
    std::string makeKey(const std::string& fn_name,
                        const std::vector<TypeId>& types) const;

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

    // Phase 4: Clone generic function bodies with type substitution
    void cloneInstances(Program& program, TypeTable& type_table);

    // Clone only instances added after start_index (for recursive monomorphization)
    void cloneNewInstances(Program& program, TypeTable& type_table, size_t start_index);

    // Phase 6: Rewrite call sites to call monomorphized functions
    void rewriteCallSites(Program& program, TypeTable& type_table);

    // Rewrite calls in an expression tree
    void rewriteExpr(Expr* expr);

    // Rewrite calls in a statement tree
    void rewriteStmt(Stmt* stmt);

    // Rewrite calls in a block
    void rewriteBlock(BlockStmt* block);

    // Find the GenericInstance matching a call to fn_name with the given arg types
    const GenericInstance* findInstanceForCall(
        const std::string& fn_name,
        const std::vector<TypeId>& arg_types) const;

    // Track the set of generic functions that are still referenced
    // (to decide whether we can remove the original generic fn later)
    std::unordered_set<std::string> referenced_generics_;
};

} // namespace tether
