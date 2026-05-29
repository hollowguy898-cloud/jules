#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Allocation Fusion Pass: Detects consecutive Box.new() calls in the same
// block and fuses them into a single batch allocation.
//
// When the compiler encounters multiple Box.new() calls in sequence within
// the same basic block, each one individually hits malloc. This pass detects
// such sequences and annotates them with alloc_fused / alloc_batch_size /
// alloc_batch_offset metadata so that the IR generator can emit a single
// tether_batch_alloc() + tether_batch_carve() pattern instead.
//
// LLVM CANNOT do this: malloc calls are opaque external calls to LLVM.
// Tether knows the Box semantics and can prove that sequential allocations
// of the same lifetime can be fused.
//
// This pass runs in the TetherSpecific phase, after SROA and before
// AllocatorLowerer.
class AllocFusionPass : public PreLLVMPass {
public:
    std::string name() const override { return "AllocFusion"; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }

    int batchesFormed() const { return batches_formed_; }
    int allocationsFused() const { return allocations_fused_; }

private:
    int batches_formed_ = 0;
    int allocations_fused_ = 0;

    // Analyze a single function for allocation fusion opportunities
    bool analyzeFn(FnDecl& fn, TypeTable& type_table);

    // Analyze a block for consecutive Box.new() calls
    bool analyzeBlock(BlockStmt* block, TypeTable& type_table);

    // Check if a CallExpr is Box.new()
    bool isBoxNew(CallExpr& call);

    // Compute the allocation size for a Box.new() call
    int64_t computeBoxAllocSize(CallExpr& call, TypeTable& type_table);

    // Fuse a sequence of Box.new() calls by annotating them in MetadataMap
    void fuseAllocations(const std::vector<CallExpr*>& calls, TypeTable& type_table);
};

} // namespace tether
