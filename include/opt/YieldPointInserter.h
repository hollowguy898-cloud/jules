#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Yield Point Insertion: For cooperative scheduling (fibers/coroutines),
// inserts yield checks at safe program points. LLVM doesn't understand
// the scheduler contract, so this must be done before IR emission.
//
// A yield check is a conditional call to the scheduler:
//   if (scheduler.should_yield()) { scheduler.yield(); }
//
// Insertion points:
// - Back edges of loops (after N iterations)
// - Before function calls that may take long
// - At explicit `yield` statements (already in AST)
//
// The iteration counter is tracked via a hidden local variable in the
// loop, and the yield check is inserted at the top of the loop body.
//
// This is NOT redundant with LLVM because LLVM has no concept of
// cooperative scheduling or yield points. This is a semantic property
// of the Tether runtime that LLVM cannot infer.
class YieldPointInserterPass : public PreLLVMPass {
public:
    std::string name() const override { return "YieldPointInsertion"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }

    int yieldPointsInserted() const { return yield_points_inserted_; }

private:
    int yield_points_inserted_ = 0;

    // Number of loop iterations between yield checks
    static constexpr int YIELD_INTERVAL = 256;

    bool processWhileLoop(WhileStmt& loop, TypeTable& type_table);
    bool walkStmt(Stmt* stmt, TypeTable& type_table);
    bool walkBlock(BlockStmt* block, TypeTable& type_table);
    bool walkFn(FnDecl& fn, TypeTable& type_table);

    // Check if a function has the @superoptimize directive (no yield insertion)
    bool isSuperOptimized(FnDecl& fn) const;
};

} // namespace tether
