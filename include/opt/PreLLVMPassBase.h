#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "parser/Parser.h"

#include <string>
#include <vector>
#include <memory>
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
// PassCategory — classifies passes into pipeline phases
//
// The unified pipeline runs passes in strict phase order:
//   LayoutTransform  (Phase 3): Change data layout — hot/cold split, reorder, SoA
//   TetherSpecific   (Phase 4): Tether semantics — error paths, barriers, defer, allocator
//   IRHint           (Phase 5): LLVM IR hints — prefetch, yield points
//
// Within each phase, passes run in registration order.
// ============================================================================
enum class PassCategory : uint8_t {
    LayoutTransform,   // Phase 3: Data layout changes
    TetherSpecific,    // Phase 4: Tether-specific semantic transforms
    IRHint,            // Phase 5: LLVM IR annotation hints
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
//
// Each pass declares its category so the pipeline can route it to the
// correct phase. This prevents ordering bugs like running allocator
// lowering before escape analysis.
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

    // Which phase this pass belongs to — determines execution order
    virtual PassCategory category() const = 0;

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

} // namespace tether
