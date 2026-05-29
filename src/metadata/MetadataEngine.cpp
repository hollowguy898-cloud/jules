#include "metadata/MetadataEngine.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// MetadataEngine — constructor
// ============================================================================

MetadataEngine::MetadataEngine() = default;

// ============================================================================
// run — execute the full 6-layer metadata pipeline
//
// The ordering is critical:
//   L1 collects base facts from the source program.
//   L6 enriches those facts with real-world profile data (if available).
//   L3 detects memory access patterns using both L1 and L6 info.
//   L2 simplifies control flow using L3's memory topology info.
//   L4 transforms layouts using everything accumulated so far.
//   L5 translates all collected metadata into LLVM-friendly hints.
//
// The key design principle: memory movement > raw arithmetic.
// Every optimization decision is evaluated against whether it reduces
// stalls, cache misses, and bandwidth waste.
// ============================================================================

void MetadataEngine::run(Program& program, TypeTable& type_table) {
    // L1: Semantic Collector — "understand the program"
    // Collects ownership, aliasing, layout, purity, inline, restrict info
    l1_.collect(program, type_table, meta_);

    // L6: Profile-Guided Optimizer — "learn from reality"
    // Enriches metadata with real execution counts and branch weights
    if (l6_.hasProfile()) {
        l6_.apply(program, meta_);
    }

    // L3: Memory Topology Analyzer — "detect access patterns"
    // Uses L1 semantics and L6 profile data to identify linear traversals,
    // strided access, prefetch opportunities, and vectorization candidates
    l3_.analyze(program, type_table, meta_);

    // L2: Control-Flow Simplifier — "simplify branches"
    // Converts if/else to select/predication when profitable, using
    // L3's memory dependency info to make sound decisions
    l2_.simplify(program, type_table, meta_);

    // L4: Layout Transformer — "packed bitfield only"
    // NOTE: SoA and hot/cold splitting have been moved to dedicated
    // PreLLVMPass implementations (AoSToSoAPass, HotColdSplitterPass)
    // in the unified PreLLVMPipeline. L4 here only handles packed
    // bitfield detection as a complementary lower-priority transform.
    l4_.transform(program, type_table, meta_);

    // L5: LLVM Metadata Emitter — "translate to LLVM hints"
    // Converts all accumulated metadata into LLVM IR annotations
    l5_.emit(program, type_table, meta_);
}

// ============================================================================
// loadProfile — delegate to L6
// ============================================================================

bool MetadataEngine::loadProfile(const std::string& profile_path) {
    return l6_.loadProfile(profile_path);
}

// ============================================================================
// llvmMetadataBlock — return the L5 metadata block
// ============================================================================

std::string MetadataEngine::llvmMetadataBlock() const {
    return l5_.metadataBlock();
}

// ============================================================================
// fnAttributes — delegate to L5 with the engine's metadata map
// ============================================================================

std::string MetadataEngine::fnAttributes(FnDecl& fn) const {
    return l5_.fnAttributes(fn, meta_);
}

// ============================================================================
// paramAttributes — delegate to L5 with the engine's metadata map
// ============================================================================

std::string MetadataEngine::paramAttributes(FnParam& param) const {
    return l5_.paramAttributes(param, meta_);
}

// ============================================================================
// memoryMetadata — delegate to L5 with the engine's metadata map
// ============================================================================

std::string MetadataEngine::memoryMetadata(Expr& expr) const {
    return l5_.memoryMetadata(expr, meta_);
}

// ============================================================================
// branchWeightMetadata — delegate to L5 with the engine's metadata map
// ============================================================================

std::string MetadataEngine::branchWeightMetadata(IfStmt& is) const {
    return l5_.branchWeightMetadata(is, meta_);
}

// ============================================================================
// loopMetadata — delegate to L5 with the engine's metadata map
// ============================================================================

std::string MetadataEngine::loopMetadata(WhileStmt& ws) const {
    return l5_.loopMetadata(ws, meta_);
}

} // namespace tether
