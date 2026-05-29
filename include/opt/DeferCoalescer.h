#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Defer Chain Coalescing: Merges multiple defer statements in the same
// scope into a single cleanup block. LLVM sees each defer as a separate
// call; by coalescing them, we reduce code size and improve branch prediction.
//
// Example:
//   defer free(a);
//   defer free(b);
//   defer free(c);
// Becomes:
//   defer { free(c); free(b); free(a); }  // Single cleanup point
//
// This is NOT redundant with LLVM because LLVM doesn't know the semantic
// relationship between defer statements - they are executed in reverse
// order at scope exit, and coalescing them into a single cleanup block
// reduces the number of implicit branches and cleanup points.
class DeferCoalescerPass : public PreLLVMPass {
public:
    std::string name() const override { return "DeferCoalescing"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }

    int coalescedGroups() const { return coalesced_groups_; }

private:
    int coalesced_groups_ = 0;

    bool coalesceBlock(BlockStmt& block);
    bool walkStmt(Stmt& stmt);
    bool walkBlock(BlockStmt& block);
    bool walkFn(FnDecl& fn);
};

} // namespace tether
