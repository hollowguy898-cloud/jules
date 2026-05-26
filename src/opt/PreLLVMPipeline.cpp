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
                }
            }
        }
        return any_changed;
    }
};

// ============================================================================
// PreLLVMPipeline constructor
//
// Sets up the appropriate passes based on optimization level:
//   None:       No passes
//   Basic:      FieldReorderer, DeferCoalescer, ErrorPathSeparator, OpaqueBarrier
//   Aggressive: All passes including AoS->SoA, AllocatorLowerer,
//               PrefetchInserter, YieldPointInserter, HotColdSplitter
// ============================================================================
PreLLVMPipeline::PreLLVMPipeline(PreLLVMOptLevel level, TypeTable& type_table)
    : level_(level), type_table_(type_table)
{
    if (level_ == PreLLVMOptLevel::None) {
        return;
    }

    // ========================================================================
    // Both Basic and Aggressive levels get these passes
    // ========================================================================

    // Hot/Cold splitting must run BEFORE field reordering, because it
    // changes the set of fields in the struct (splits off cold fields).
    // Only at Aggressive level since it requires PGO data or heuristics.
    if (level_ == PreLLVMOptLevel::Aggressive) {
        addPass(std::make_unique<HotColdSplitterPass>());
    }

    // Field reordering for padding minimization
    addPass(std::make_unique<FieldReordererAdapterPass>());

    // AoS->SoA transforms struct layout (must come before access rewriting)
    if (level_ == PreLLVMOptLevel::Aggressive) {
        addPass(std::make_unique<AoSToSoAPass>());
    }

    // Error path separation - annotates cold paths for LLVM
    addPass(std::make_unique<ErrorPathSeparatorPass>());

    // Opaque type barrier - prevents LLVM aliasing assumptions at FFI boundaries
    addPass(std::make_unique<OpaqueBarrierPass>());

    // Defer coalescing - merges consecutive defer statements
    addPass(std::make_unique<DeferCoalescerPass>());

    // Aggressive-only passes below
    if (level_ == PreLLVMOptLevel::Aggressive) {
        // Allocator lowering - inlines arena bump allocation
        addPass(std::make_unique<AllocatorLowererPass>());

        // Prefetch insertion - uses alignment hints
        addPass(std::make_unique<PrefetchInserterPass>());

        // Yield point insertion - for cooperative scheduling
        addPass(std::make_unique<YieldPointInserterPass>());
    }
}

// ============================================================================
// addPass
// ============================================================================
void PreLLVMPipeline::addPass(std::unique_ptr<PreLLVMPass> pass) {
    pass->setAnnotationMap(&annotations_);
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
                              std::to_string(annotations_.size()) + " AST annotations");

    return result;
}

} // namespace tether
