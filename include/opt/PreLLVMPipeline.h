#pragma once

#include "opt/PreLLVMPassBase.h"
#include "parser/Parser.h"

#include <vector>
#include <memory>
#include "metadata/SemanticCollector.h"
#include "metadata/ControlFlowSimplifier.h"
#include "metadata/MemoryTopologyAnalyzer.h"
#include "metadata/LayoutTransformer.h"
#include "metadata/LLVMMetadataEmitter.h"
#include "metadata/ProfileGuidedOptimizer.h"
#include "opt/SpeculativeOptimizer.h"
#include "opt/ComptimeEvaluator.h"

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace tether {

// ============================================================================
// ComptimeEvalPass — compile-time evaluation pass
//
// Defined here because it inherits from PreLLVMPass (in PreLLVMPassBase.h)
// and uses ComptimeEvaluator/ComptimeValue (in ComptimeEvaluator.h).
// ============================================================================
class ComptimeEvalPass : public PreLLVMPass {
public:
    std::string name() const override { return "ComptimeEval"; }
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }
    bool run(Program& program, TypeTable& type_table) override;

    // Access the computed comptime values (for IRGenerator to use)
    const std::unordered_map<const void*, ComptimeValue>& comptimeValues() const {
        return comptime_values_;
    }

private:
    std::unordered_map<const void*, ComptimeValue> comptime_values_;
    ComptimeEvaluator evaluator_;

    // Walk all top-level declarations
    void processTopLevel(TopLevel* tl, TypeTable& tt, ComptimeEvalContext& ctx);
    // Walk a function body for comptime expressions
    void processFnBody(FnDecl* fn, TypeTable& tt, ComptimeEvalContext& ctx);
    // Walk statements recursively
    void processStmt(Stmt* stmt, TypeTable& tt, ComptimeEvalContext& ctx);
    // Walk expressions recursively
    void processExpr(Expr* expr, TypeTable& tt, ComptimeEvalContext& ctx);
    // Store a comptime value in both the internal map and the MetadataMap
    void storeComptimeValue(const void* node, const ComptimeValue& val);
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

    // Speculative optimizer (nuclear option #8)
    SpeculativeOptimizerPass speculative_;

    // Compile-time evaluation pass
    std::unique_ptr<ComptimeEvalPass> comptime_eval_;

    // Pre-LLVM transform passes (the "Track 2" passes, now integrated)
    std::vector<std::unique_ptr<PreLLVMPass>> passes_;

    void addPass(std::unique_ptr<PreLLVMPass> pass);
};

} // namespace tether
