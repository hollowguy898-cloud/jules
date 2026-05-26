#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Align-Guided Prefetch Insertion: When `align(64)` is used on struct
// fields or arrays, inserts prefetch instructions at strategic loop
// boundaries. LLVM's prefetching is generic and doesn't understand
// Tether's alignment hints.
//
// For a loop accessing data[i] where data has align(64):
// - Insert prefetch for data[i+PREFETCH_DISTANCE] at the start of
//   each loop iteration
//
// The prefetch distance is determined by the alignment and cache line size:
// - align(64) -> 64-byte cache line -> prefetch 8 iterations ahead
// - align(128) -> 128-byte -> prefetch 4 iterations ahead
//
// This is NOT redundant with LLVM because LLVM doesn't know about
// Tether's alignment guarantees and cannot infer that align(64) means
// "this data is cache-line aligned and prefetching will be beneficial."
class PrefetchInserterPass : public PreLLVMPass {
public:
    std::string name() const override { return "AlignPrefetch"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }

    int prefetchesInserted() const { return prefetches_inserted_; }

private:
    int prefetches_inserted_ = 0;

    // Default prefetch distance (iterations ahead to prefetch)
    static constexpr int DEFAULT_PREFETCH_DISTANCE = 8;

    bool processWhileLoop(WhileStmt& loop, TypeTable& type_table);
    bool walkStmt(Stmt* stmt, TypeTable& type_table);
    bool walkBlock(BlockStmt* block, TypeTable& type_table);
    bool walkFn(FnDecl& fn, TypeTable& type_table);

    // Check if a type has explicit alignment >= 64 bytes
    bool hasHighAlignment(TypeId type) const;
    // Get the alignment value if the type is aligned
    uint32_t getAlignmentValue(TypeId type) const;
};

} // namespace tether
