#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Opaque Type Barrier: Ensures LLVM does not make optimization assumptions
// across FFI boundaries for `opaque` types. Inserts:
// 1. `inaccessiblememonly` attribute on functions that take opaque params
// 2. Memory barriers around opaque type accesses
// 3. Prevents store-to-load forwarding across opaque boundaries
//
// This is NOT redundant with LLVM because LLVM's `extern` doesn't prevent
// all aliasing assumptions. Tether's `opaque` is stronger than C's
// incomplete type - it explicitly says "you know NOTHING about this memory."
//
// Without this pass, LLVM might assume that a store to a regular variable
// cannot alias a load from an opaque pointer, and could reorder memory
// operations incorrectly across FFI calls.
class OpaqueBarrierPass : public PreLLVMPass {
public:
    std::string name() const override { return "OpaqueBarrier"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }

    int barriersInserted() const { return barriers_inserted_; }

private:
    int barriers_inserted_ = 0;

    bool processFnDecl(FnDecl& fn, TypeTable& type_table);
    bool walkExpr(Expr* expr, TypeTable& type_table);
    bool walkStmt(Stmt* stmt, TypeTable& type_table);
    bool walkBlock(BlockStmt* block, TypeTable& type_table);

    // Check if a function signature involves opaque types
    bool takesOpaqueParams(FnDecl& fn, TypeTable& type_table) const;
    bool returnsOpaque(FnDecl& fn, TypeTable& type_table) const;
    bool isOpaqueType(TypeId type) const;
};

} // namespace tether
