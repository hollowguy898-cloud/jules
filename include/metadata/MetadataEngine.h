#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "metadata/SemanticCollector.h"
#include "metadata/ControlFlowSimplifier.h"
#include "metadata/MemoryTopologyAnalyzer.h"
#include "metadata/LayoutTransformer.h"
#include "metadata/LLVMMetadataEmitter.h"
#include "metadata/ProfileGuidedOptimizer.h"

#include <string>
#include <memory>

namespace tether {

// ============================================================================
// MetadataEngine — DEPRECATED: superseded by PreLLVMPipeline
//
// This standalone engine is NO LONGER USED by the compiler driver.
// The unified PreLLVMPipeline now orchestrates both the MetadataEngine
// analysis layers (L1-L6) AND the pre-LLVM transform passes in a
// single correct ordering. See opt/PreLLVMPipeline.h for details.
//
// This class is retained ONLY for unit testing individual analysis layers
// in isolation. Do NOT call MetadataEngine::run() in production code —
// it omits escape analysis, allocator lowering, SoA transforms, and
// hot/cold splitting, producing incomplete optimization.
//
// The layers still run in the documented order:
//   L1: Semantic Collector    — "understand the program"
//   L6: Profile-Guided Opt   — "learn from reality" (if profile available)
//   L3: Memory Topology       — "detect access patterns"
//   L2: Control-Flow Simplify — "simplify branches"
//   L4: Layout Transform      — "packed bitfield only" (SoA/hot-cold moved to passes)
//   L5: LLVM Metadata Emit    — "translate to LLVM hints"
//
// The key design principle: memory movement > raw arithmetic.
// Every optimization decision is evaluated against whether it reduces
// stalls, cache misses, and bandwidth waste.
// ============================================================================
class [[deprecated("Use PreLLVMPipeline instead — MetadataEngine omits escape analysis, "
                        "allocator lowering, SoA, and hot/cold splitting")]] MetadataEngine {
public:
    MetadataEngine();

    // Run the metadata analysis layers ONLY (no transform passes).
    // DEPRECATED: Use PreLLVMPipeline::run() for the full pipeline.
    // This method is retained for isolated layer testing only.
    void run(Program& program, TypeTable& type_table);

    // Load profile data (call before run() for PGO)
    bool loadProfile(const std::string& profile_path);

    // Access the collected metadata
    const MetadataMap& metadata() const { return meta_; }
    MetadataMap& metadata() { return meta_; }

    // Get the LLVM metadata block (from L5)
    std::string llvmMetadataBlock() const;

    // Get function-level LLVM attributes (from L5)
    std::string fnAttributes(FnDecl& fn) const;

    // Get parameter-level LLVM attributes (from L5)
    std::string paramAttributes(FnParam& param) const;

    // Get memory access LLVM metadata (from L5)
    std::string memoryMetadata(Expr& expr) const;

    // Get branch weight LLVM metadata (from L5)
    std::string branchWeightMetadata(IfStmt& is) const;

    // Get loop LLVM metadata (from L5)
    std::string loopMetadata(WhileStmt& ws) const;

    // Dump all metadata for inspection ("show me transformed layout")
    std::string dump() const { return meta_.dump(); }

    // Check if profile data is loaded
    bool hasProfile() const { return l6_.hasProfile(); }

private:
    MetadataMap meta_;
    SemanticCollector l1_;
    ControlFlowSimplifier l2_;
    MemoryTopologyAnalyzer l3_;
    LayoutTransformer l4_;
    LLVMMetadataEmitter l5_;
    ProfileGuidedOptimizer l6_;
};

} // namespace tether
