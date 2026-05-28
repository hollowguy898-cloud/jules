#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "parser/Parser.h"

#include <vector>
#include <memory>
#include "metadata/MetaTypes.h"
#include "metadata/SemanticCollector.h"
#include "metadata/ControlFlowSimplifier.h"
#include "metadata/MemoryTopologyAnalyzer.h"
#include "metadata/LayoutTransformer.h"
#include "metadata/LLVMMetadataEmitter.h"
#include "metadata/ProfileGuidedOptimizer.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace tether {

// ============================================================================
// Pre-LLVM Optimization Level
// ============================================================================
enum class PreLLVMOptLevel : uint8_t {
    None = 0,       // No pre-LLVM optimizations
    Basic = 1,      // Field reorder, defer coalesce, cold path annotation, opaque barrier
    Aggressive = 2  // All passes including AoS->SoA, allocator lowering, prefetch, yield, hot/cold split
};

// ============================================================================
// PreLLVMPass - base class for all pre-LLVM optimization passes
//
// These passes run on the AST BEFORE LLVM IR emission. They perform
// transformations that LLVM cannot do because they require semantic
// knowledge that is lost once we lower to LLVM IR.
//
// UNIFIED PIPELINE: All passes write to MetadataMap only.
// The old ASTAnnotationMap dual-channel has been eliminated.
// ============================================================================
class PreLLVMPass {
public:
    virtual ~PreLLVMPass() = default;

    // Human-readable name for logging
    virtual std::string name() const = 0;

    // Run the pass on the program. Returns true if any transformation was made.
    virtual bool run(Program& program, TypeTable& type_table) = 0;

    // Returns false by default - most pre-LLVM passes are explicitly NOT
    // redundant with LLVM's own optimizations. Override and return true
    // only if a pass does something LLVM already handles.
    virtual bool isRedundantWithLLVM() const { return false; }

    // Set the metadata map for this pass to write typed metadata into
    void setMetadataMap(MetadataMap* meta_map) { meta_map_ = meta_map; }

protected:
    MetadataMap* meta_map_ = nullptr;
};

// ============================================================================
// PreLLVMPipelineResult - result of running the full pipeline
// ============================================================================
struct PreLLVMPipelineResult {
    int passes_run = 0;
    int transformations_made = 0;
    std::vector<std::string> pass_log;
};

// ============================================================================
// PreLLVMPipeline - UNIFIED pre-LLVM optimization pipeline
//
// This is a single coherent pipeline that runs both the MetadataEngine
// analysis layers AND the pre-LLVM transform passes in the correct order.
//
// The old two-track system (MetadataEngine + PreLLVMPipeline running
// separately with shared MetadataMap) caused clobbering, stale metadata,
// and duplicated analysis. This unified pipeline fixes all of that.
//
// Pipeline ordering principle:
//   1. Collect facts (semantic analysis, PGO, escape analysis)
//   2. Refine facts (access patterns, control flow, error paths)
//   3. Transform (layout changes: hot/cold split, reorder, SoA, bitfield)
//   4. Tether-specific transforms (opaque barrier, defer, allocator)
//   5. IR hints (prefetch, yield points)
//   6. Emit LLVM metadata (MUST be last — sees final state)
//
// Every pass writes to a single MetadataMap. No dual-channel output.
// L5 (LLVMMetadataEmitter) runs LAST so it sees all accumulated data.
// ============================================================================
class PreLLVMPipeline {
public:
    PreLLVMPipeline(PreLLVMOptLevel level, TypeTable& type_table);

    PreLLVMPipelineResult run(Program& program);

    // Access the unified metadata map
    MetadataMap& metadata() { return metadata_; }
    const MetadataMap& metadata() const { return metadata_; }

    // Load profile data (call before run() for PGO)
    bool loadProfile(const std::string& profile_path);
    bool hasProfile() const;

    // LLVM metadata emission queries (delegated to L5, which runs during pipeline)
    std::string llvmMetadataBlock() const;
    std::string fnAttributes(FnDecl& fn) const;
    std::string paramAttributes(FnParam& param) const;
    std::string memoryMetadata(Expr& expr) const;
    std::string branchWeightMetadata(IfStmt& is) const;
    std::string loopMetadata(WhileStmt& ws) const;

    // Dump all metadata for inspection
    std::string dump() const { return metadata_.dump(); }

private:
    PreLLVMOptLevel level_;
    TypeTable& type_table_;
    MetadataMap metadata_;

    // Metadata engine layers (used as sub-passes in the unified pipeline)
    SemanticCollector l1_;
    ControlFlowSimplifier l2_;
    MemoryTopologyAnalyzer l3_;
    LayoutTransformer l4_;       // Now only does packed bitfield
    LLVMMetadataEmitter l5_;
    ProfileGuidedOptimizer l6_;

    // Pre-LLVM transform passes (the "Track 2" passes, now integrated)
    std::vector<std::unique_ptr<PreLLVMPass>> passes_;

    void addPass(std::unique_ptr<PreLLVMPass> pass);
};

} // namespace tether
