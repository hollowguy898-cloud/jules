#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Allocator Inline Lowering: Replaces opaque allocator calls with
// inline fast-path code when the allocator type is known.
//
// LLVM sees malloc() as an opaque function call, preventing optimization.
// Tether knows the allocator type at compile time, so it can:
//
// 1. ArenaAllocator: Replace alloc() with a pointer bump (add + align)
//    Old: call @arena_alloc(ptr arena, i64 size)
//    New: %old = load ptr, ptr %arena.offset
//         %aligned = add ptr %old, %aligned_size
//         store %aligned, ptr %arena.offset
//
// 2. StackAllocator: Replace alloc() with alloca (already handled by LLVM)
//
// 3. GeneralAllocator: Keep as malloc/free (LLVM can optimize these with -O2)
//
// This pass MUST run before IR generation because it changes the AST
// to use inline allocation expressions instead of allocator calls.
//
// LLVM CANNOT do this: malloc/free/opaque allocator calls are external
// function calls with unknown side effects at the LLVM level.
class AllocatorLowererPass : public PreLLVMPass {
public:
    std::string name() const override { return "AllocatorLowering"; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }

    int arenasLowered() const { return arenas_lowered_; }
    int stacksLowered() const { return stacks_lowered_; }

private:
    int arenas_lowered_ = 0;
    int stacks_lowered_ = 0;

    bool lowerCallExpr(CallExpr& call, TypeTable& type_table);
    bool walkExpr(Expr* expr, TypeTable& type_table);
    bool walkStmt(Stmt* stmt, TypeTable& type_table);
    bool walkBlock(BlockStmt* block, TypeTable& type_table);
    bool walkFn(FnDecl& fn, TypeTable& type_table);
};

} // namespace tether
