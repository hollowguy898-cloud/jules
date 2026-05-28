#include "opt/PreLLVMPipeline.h"
#include "opt/FieldReorderer.h"
#include "opt/AoSToSoA.h"
#include "opt/ErrorPathSeparator.h"
#include "opt/AllocatorLowerer.h"
#include "opt/DeferCoalescer.h"
#include "opt/PrefetchInserter.h"
#include "opt/YieldPointInserter.h"
#include "opt/OpaqueBarrier.h"
#include "opt/HotColdSplitter.h"
#include "opt/EscapeAnalysis.h"

#include <sstream>

namespace tether {

// ============================================================================
// FieldReordererAdapter - wraps the existing FieldReorderer as a PreLLVMPass
// ============================================================================
class FieldReordererAdapterPass : public PreLLVMPass {
public:
    std::string name() const override { return "FieldReordering"; }
    bool isRedundantWithLLVM() const override { return false; }

    bool run(Program& program, TypeTable& type_table) override {
        FieldReorderer reorderer(type_table);
        auto results = reorderer.analyzeAll(program);
        bool any_changed = false;
        for (auto& top_level : program) {
            if (top_level->getKind() == NodeKind::StructDecl) {
                auto& decl = cast<StructDecl>(*top_level);
                auto result = reorderer.analyze(decl);
                if (result.was_improved) {
                    reorderer.apply(decl, result);
                    any_changed = true;

                    // Write to MetadataMap: record the field reorder transform
                    if (meta_map_) {
                        auto& nm = meta_map_->getOrCreate(&decl);
                        nm.layout_transform.kind = TransformKind::FieldReorder;
                        nm.layout_transform.struct_name = decl.name();
                        nm.layout_transform.reorder_map = result.reordered_order;
                        nm.layout_transform.detail = "field_reorder:original_size=" +
                            std::to_string(result.original_size) +
                            ":reordered_size=" + std::to_string(result.reordered_size);
                    }
                }
            }
        }
        return any_changed;
    }
};

// ============================================================================
// PreLLVMPipeline constructor
//
// Sets up the appropriate passes based on optimization level.
//
// CORRECT pass ordering:
//   1. EscapeAnalysis       — must run BEFORE AllocatorLowerer (need escape info)
//   2. HotColdSplitter      — splits before reordering
//   3. FieldReorderer       — reorder after split
//   4. AoSToSoA             — transform after reorder
//   5. ErrorPathSeparator   — annotate cold paths (used by prefetch decisions)
//   6. OpaqueBarrier        — mark FFI boundaries
//   7. DeferCoalescer       — merge defer chains
//   8. AllocatorLowerer     — inline allocators (needs escape analysis results)
//   9. PrefetchInserter     — insert prefetches (needs cold path + access pattern info)
//  10. YieldPointInserter   — insert yields (must be last, after all other transforms)
// ============================================================================
PreLLVMPipeline::PreLLVMPipeline(PreLLVMOptLevel level, TypeTable& type_table)
    : level_(level), type_table_(type_table)
{
    if (level_ == PreLLVMOptLevel::None) {
        return;
    }

    // ========================================================================
    // Pass 1: Escape Analysis (must run before AllocatorLowerer)
    //
    // Aggressive-only: needs full analysis of smart pointer lifetimes
    // ========================================================================
    if (level_ == PreLLVMOptLevel::Aggressive) {
        addPass(std::make_unique<EscapeAnalysisPass>());
    }

    // ========================================================================
    // Pass 2: Hot/Cold splitting (must run BEFORE field reordering)
    //
    // Only at Aggressive level since it requires PGO data or heuristics.
    // ========================================================================
    if (level_ == PreLLVMOptLevel::Aggressive) {
        addPass(std::make_unique<HotColdSplitterPass>());
    }

    // ========================================================================
    // Pass 3: Field reordering for padding minimization
    // ========================================================================
    addPass(std::make_unique<FieldReordererAdapterPass>());

    // ========================================================================
    // Pass 4: AoS->SoA transforms (must come after reorder)
    // ========================================================================
    if (level_ == PreLLVMOptLevel::Aggressive) {
        addPass(std::make_unique<AoSToSoAPass>());
    }

    // ========================================================================
    // Pass 5: Error path separation - annotates cold paths
    // ========================================================================
    addPass(std::make_unique<ErrorPathSeparatorPass>());

    // ========================================================================
    // Pass 6: Opaque type barrier - prevents LLVM aliasing assumptions at FFI
    // ========================================================================
    addPass(std::make_unique<OpaqueBarrierPass>());

    // ========================================================================
    // Pass 7: Defer coalescing - merges consecutive defer statements
    // ========================================================================
    addPass(std::make_unique<DeferCoalescerPass>());

    // ========================================================================
    // Aggressive-only passes below
    // ========================================================================
    if (level_ == PreLLVMOptLevel::Aggressive) {
        // Pass 8: Allocator lowering - inlines arena bump allocation
        // Must run AFTER EscapeAnalysis (needs escape analysis results)
        addPass(std::make_unique<AllocatorLowererPass>());

        // Pass 9: Prefetch insertion - uses alignment hints + cold path info
        // Must run AFTER ErrorPathSeparator (needs cold path annotations)
        addPass(std::make_unique<PrefetchInserterPass>());

        // Pass 10: Yield point insertion - for cooperative scheduling
        // Must be LAST — after all other transformations are complete
        addPass(std::make_unique<YieldPointInserterPass>());
    }
}

// ============================================================================
// addPass
// ============================================================================
void PreLLVMPipeline::addPass(std::unique_ptr<PreLLVMPass> pass) {
    pass->setAnnotationMap(&annotations_);
    if (metadata_map_) {
        pass->setMetadataMap(metadata_map_);
    }
    passes_.push_back(std::move(pass));
}

// ============================================================================
// run - execute all passes in order
// ============================================================================
PreLLVMPipelineResult PreLLVMPipeline::run(Program& program) {
    PreLLVMPipelineResult result;

    if (level_ == PreLLVMOptLevel::None) {
        result.pass_log.push_back("[PreLLVM] Optimization level: None - skipping all passes");
        return result;
    }

    std::string level_str = (level_ == PreLLVMOptLevel::Basic) ? "Basic" : "Aggressive";
    result.pass_log.push_back("[PreLLVM] Running " + level_str + " pipeline with " +
                              std::to_string(passes_.size()) + " passes");

    for (auto& pass : passes_) {
        bool redundant = pass->isRedundantWithLLVM();
        std::string note = redundant ? " [WARNING: redundant with LLVM]" : "";
        result.pass_log.push_back("[PreLLVM] Running pass: " + pass->name() + note);

        bool changed = pass->run(program, type_table_);
        result.passes_run++;

        if (changed) {
            result.transformations_made++;
            result.pass_log.push_back("[PreLLVM]   -> " + pass->name() + " made transformations");
        } else {
            result.pass_log.push_back("[PreLLVM]   -> " + pass->name() + " made no changes");
        }
    }

    result.pass_log.push_back("[PreLLVM] Pipeline complete: " +
                              std::to_string(result.passes_run) + " passes run, " +
                              std::to_string(result.transformations_made) + " made transformations, " +
                              std::to_string(annotations_.size()) + " AST annotations, " +
                              std::to_string(metadata_map_ ? metadata_map_->size() : 0) + " metadata nodes");

    return result;
}

} // namespace tether
