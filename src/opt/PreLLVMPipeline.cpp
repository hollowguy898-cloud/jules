#include "opt/PreLLVMPipeline.h"
#include "opt/FieldReorderer.h"
#include "opt/AoSToSoA.h"
#include "opt/ErrorPathSeparator.h"
#include "opt/AllocatorLowerer.h"
#include "opt/DeferCoalescer.h"
#include "opt/SROAPass.h"
#include "opt/PrefetchInserter.h"
#include "opt/YieldPointInserter.h"
#include "opt/OpaqueBarrier.h"
#include "opt/HotColdSplitter.h"
#include "opt/EscapeAnalysis.h"
#include "opt/NichedErrorPass.h"
#include "opt/AllocFusionPass.h"

#include <sstream>
#include <iostream>

namespace tether {

// ============================================================================
// FieldReordererAdapter - wraps the existing FieldReorderer as a PreLLVMPass
// ============================================================================
class FieldReordererAdapterPass : public PreLLVMPass {
public:
    std::string name() const override { return "FieldReordering"; }
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::LayoutTransform; }

    bool run(Program& program, TypeTable& type_table) override {
        FieldReorderer reorderer(type_table);
        bool any_changed = false;
        for (auto& top_level : program) {
            if (top_level->getKind() == NodeKind::StructDecl) {
                auto& decl = cast<StructDecl>(*top_level);
                auto result = reorderer.analyze(decl);
                if (result.was_improved) {
                    reorderer.apply(decl, result);
                    any_changed = true;

                    // Write to MetadataMap using merge (no clobbering)
                    if (meta_map_) {
                        auto& nm = meta_map_->getOrCreate(&decl);
                        nm.layout_transform.kind = TransformKind::FieldReorder;
                        nm.layout_transform.struct_name = decl.name();
                        nm.layout_transform.reorder_map = result.reordered_order;
                        nm.layout_transform.detail = "field_reorder:original_size=" +
                            std::to_string(result.original_size) +
                            ":reordered_size=" + std::to_string(result.reordered_size);

                        // Merge struct-level metadata (not clobber)
                        MetadataMap::StructMeta smeta;
                        smeta.name = decl.name();
                        smeta.transform.kind = TransformKind::FieldReorder;
                        smeta.transform.struct_name = decl.name();
                        smeta.transform.reorder_map = result.reordered_order;
                        smeta.transform.detail = nm.layout_transform.detail;
                        meta_map_->mergeStructMeta(decl.name(), std::move(smeta));
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
// Sets up pre-LLVM transform passes based on optimization level.
// The MetadataEngine layers (L1-L6) are run directly in run() in the
// correct interleaved order — they are NOT added as passes here because
// they have a different calling convention (take MetadataMap& param).
// ============================================================================
PreLLVMPipeline::PreLLVMPipeline(PreLLVMOptLevel level, TypeTable& type_table)
    : level_(level), type_table_(type_table)
{
    if (level_ == PreLLVMOptLevel::None) {
        return;
    }

    // ========================================================================
    // Layout transform passes (run after analysis, before code-specific passes)
    //
    // These are the ONLY passes that do layout transforms. The old L4
    // LayoutTransformer no longer does SoA or hot/cold split — only
    // packed bitfield — so there is no duplication.
    // ========================================================================
    if (level_ == PreLLVMOptLevel::Aggressive) {
        // Hot/cold splitting — must run BEFORE field reordering
        addPass(std::make_unique<HotColdSplitterPass>());
    }

    // Field reordering — after split, before SoA
    addPass(std::make_unique<FieldReordererAdapterPass>());

    if (level_ == PreLLVMOptLevel::Aggressive) {
        // AoS→SoA — after reordering
        addPass(std::make_unique<AoSToSoAPass>());
    }

    // ========================================================================
    // Tether-specific transforms
    // ========================================================================

    // Error path separation — annotates cold paths
    addPass(std::make_unique<ErrorPathSeparatorPass>());

    // Opaque type barrier — prevents LLVM aliasing assumptions at FFI
    addPass(std::make_unique<OpaqueBarrierPass>());

    // Defer coalescing — merges consecutive defer statements
    addPass(std::make_unique<DeferCoalescerPass>());

    // SROA — scalar replacement of aggregates for small structs
    // Must run AFTER DeferCoalescer and BEFORE AllocatorLowerer
    addPass(std::make_unique<SROAPass>());

    // Niched error type optimization — use pointer/integer niche encoding
    // instead of { T, i1 } for error types. Must run BEFORE codegen so
    // the IRGenerator knows which representation to use.
    addPass(std::make_unique<NichedErrorPass>());

    if (level_ == PreLLVMOptLevel::Aggressive) {
        // Allocation fusion — batch consecutive Box allocations
        // Must run BEFORE AllocatorLowerer (fusion takes priority over arena)
        addPass(std::make_unique<AllocFusionPass>());

        // Allocator lowering — inlines arena bump allocation
        // Must run AFTER EscapeAnalysis (needs escape analysis results)
        addPass(std::make_unique<AllocatorLowererPass>());

        // Prefetch insertion — uses L3's computed prefetch distance
        // Must run AFTER ErrorPathSeparator (needs cold path annotations)
        // NOTE: The PrefetchInserter now reads L3's computed
        // prefetch_distance from the MetadataMap instead of using a
        // fixed default, so it benefits from the analysis layer.
        addPass(std::make_unique<PrefetchInserterPass>());

        // Yield point insertion — for cooperative scheduling
        // Must be LAST transform — after all other transforms are complete
        addPass(std::make_unique<YieldPointInserterPass>());
    }
}

// ============================================================================
// addPass — register a pass with category ordering validation
//
// Validates that passes are registered in non-decreasing category order:
//   LayoutTransform (Phase 3) → TetherSpecific (Phase 4) → IRHint (Phase 5)
//
// This prevents subtle ordering bugs where a Phase 4 pass is accidentally
// registered before a Phase 3 pass, which would break dependencies.
// ============================================================================
void PreLLVMPipeline::addPass(std::unique_ptr<PreLLVMPass> pass) {
    // Validate category ordering: each new pass's category must be >= the
    // previous pass's category. This ensures passes are registered in a
    // valid phase order.
    if (!passes_.empty()) {
        auto prev_cat = static_cast<uint8_t>(passes_.back()->category());
        auto new_cat = static_cast<uint8_t>(pass->category());
        if (new_cat < prev_cat) {
            // Log a warning but don't crash — the phase-based execution
            // in run() will route the pass to the correct phase anyway.
            // This just means the registration order is misleading.
            std::cerr << "[tether] WARNING: Pass '" << pass->name()
                      << "' (category " << static_cast<int>(new_cat)
                      << ") registered after '" << passes_.back()->name()
                      << "' (category " << static_cast<int>(prev_cat)
                      << ") — passes should be registered in phase order"
                      << std::endl;
        }
    }

    pass->setMetadataMap(&metadata_);
    passes_.push_back(std::move(pass));
}

// ============================================================================
// run — execute the UNIFIED pipeline
//
// The pipeline runs in a single pass with correct ordering:
//
//   Phase 1: ANALYSIS — collect semantic facts
//     L1: SemanticCollector — ownership, aliasing, purity, layout
//     L6: ProfileGuidedOptimizer — PGO data (if available)
//     EscapeAnalysis — Box/Rc/Arc escape detection
//
//   Phase 2: ANALYSIS REFINEMENT — derive deeper facts
//     L3: MemoryTopologyAnalyzer — access patterns, prefetch, vectorization
//     L2: ControlFlowSimplifier — branch probability, select profitability
//
//   Phase 3: LAYOUT TRANSFORMS — use analysis to optimize data layout
//     HotColdSplitter — hot/cold field splitting
//     FieldReorderer — field reordering for padding
//     AoSToSoA — AoS→SoA transformation
//     L4: LayoutTransformer — packed bitfield only (no more SoA/hot-cold dup)
//
//   Phase 4: TETHER-SPECIFIC TRANSFORMS
//     ErrorPathSeparator — cold path annotation
//     OpaqueBarrier — FFI aliasing barrier
//     DeferCoalescer — consecutive defer merging
//     AllocatorLowerer — arena bump inline lowering
//
//   Phase 5: IR HINTS
//     PrefetchInserter — L3-driven prefetch insertion
//     YieldPointInserter — cooperative yield checks
//
//   Phase 6: LLVM METADATA EMISSION (MUST be last)
//     L5: LLVMMetadataEmitter — translate all metadata to LLVM IR hints
//
// This ordering fixes the old problems:
//   - L5 now runs AFTER all transforms (no stale metadata)
//   - Escape analysis runs BEFORE allocator lowering
//   - L4 no longer duplicates SoA/hot-cold (only packed bitfield)
//   - All metadata writes use mergeStructMeta (no clobbering)
//   - Single MetadataMap, no dual-channel ASTAnnotationMap
// ============================================================================
PreLLVMPipelineResult PreLLVMPipeline::run(Program& program) {
    PreLLVMPipelineResult result;

    if (level_ == PreLLVMOptLevel::None) {
        result.pass_log.push_back("[PreLLVM] Optimization level: None - skipping all passes");
        return result;
    }

    std::string level_str = (level_ == PreLLVMOptLevel::Basic) ? "Basic" : "Aggressive";
    result.pass_log.push_back("[PreLLVM] Running unified " + level_str + " pipeline");

    // ========================================================================
    // Phase 1: ANALYSIS — collect semantic facts
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 1: Semantic analysis (L1)");
    l1_.collect(program, type_table_, metadata_);
    result.passes_run++;
    result.pass_log.push_back("[PreLLVM]   -> L1 SemanticCollector: " +
                              std::to_string(metadata_.size()) + " nodes annotated");

    if (l6_.hasProfile()) {
        result.pass_log.push_back("[PreLLVM] Phase 1b: Profile-guided optimization (L6)");
        l6_.apply(program, metadata_);
        result.passes_run++;
        result.pass_log.push_back("[PreLLVM]   -> L6 ProfileGuidedOptimizer applied");
    }

    if (level_ == PreLLVMOptLevel::Aggressive) {
        result.pass_log.push_back("[PreLLVM] Phase 1c: Escape analysis");
        EscapeAnalysisPass escape;
        escape.setMetadataMap(&metadata_);
        bool changed = escape.run(program, type_table_);
        result.passes_run++;
        if (changed) {
            result.transformations_made++;
            result.pass_log.push_back("[PreLLVM]   -> EscapeAnalysis: " +
                                      std::to_string(escape.boxesStackAllocated() +
                                      escape.rcsStackAllocated() +
                                      escape.arcsStackAllocated()) +
                                      " smart pointers stack-allocated");
        } else {
            result.pass_log.push_back("[PreLLVM]   -> EscapeAnalysis: no changes");
        }
    }

    // ========================================================================
    // Phase 2: ANALYSIS REFINEMENT — derive deeper facts from Phase 1
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 2: Memory topology analysis (L3)");
    l3_.analyze(program, type_table_, metadata_);
    result.passes_run++;
    result.pass_log.push_back("[PreLLVM]   -> L3 MemoryTopologyAnalyzer applied");

    result.pass_log.push_back("[PreLLVM] Phase 2b: Control-flow simplification (L2)");
    l2_.simplify(program, type_table_, metadata_);
    result.passes_run++;
    result.pass_log.push_back("[PreLLVM]   -> L2 ControlFlowSimplifier applied");

    // ========================================================================
    // Phase 2c: Speculative optimization (Nuclear #8)
    //
    // Analyzes the program for speculative optimization opportunities:
    //   - Branches that are almost never taken (deopt guard the cold path)
    //   - Pointers that are never null (deopt guard the null check)
    //   - Array indices that are always in bounds (skip bounds check)
    //   - Arithmetic that never overflows (use unchecked math)
    //
    // Must run after L1-L3 (needs branch probabilities and access patterns)
    // and before transforms (so deopt guards are in place before codegen).
    // ========================================================================

    if (level_ == PreLLVMOptLevel::Aggressive) {
        result.pass_log.push_back("[PreLLVM] Phase 2c: Speculative optimization (Nuclear #8)");
        speculative_.setMetadataMap(&metadata_);
        bool spec_changed = speculative_.run(program, type_table_);
        result.passes_run++;
        if (spec_changed) {
            result.transformations_made++;
            result.pass_log.push_back("[PreLLVM]   -> SpeculativeOptimizer: " +
                                      std::to_string(speculative_.totalAssumptions()) +
                                      " assumptions, " +
                                      std::to_string(speculative_.totalGuards()) +
                                      " deopt guards");
        } else {
            result.pass_log.push_back("[PreLLVM]   -> SpeculativeOptimizer: no speculative opportunities found");
        }
    }

    // ========================================================================
    // Phase 3: LAYOUT TRANSFORMS — use analysis to optimize data layout
    //
    // Only LayoutTransform-category passes run here. These change the
    // physical memory layout of data structures.
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 3: Layout transforms");
    for (auto& pass : passes_) {
        if (pass->category() != PassCategory::LayoutTransform) continue;

        std::string note;
        bool changed = pass->run(program, type_table_);
        result.passes_run++;

        if (changed) {
            result.transformations_made++;
            note = " -> made transformations";
        } else {
            note = " -> no changes";
        }
        result.pass_log.push_back("[PreLLVM]   " + pass->name() + note);
    }

    // L4: Packed bitfield (the only remaining L4 responsibility)
    // This runs after the other layout transforms because packed bitfield
    // is a lower-priority, complementary transform
    if (level_ == PreLLVMOptLevel::Aggressive) {
        result.pass_log.push_back("[PreLLVM]   L4 LayoutTransformer (packed bitfield only)");
        l4_.transform(program, type_table_, metadata_);
        result.passes_run++;
    }

    // ========================================================================
    // Phase 4: TETHER-SPECIFIC TRANSFORMS — semantic optimizations
    //
    // Only TetherSpecific-category passes run here. These rely on the
    // analysis from Phase 1-2 and the layout decisions from Phase 3.
    //
    // CRITICAL ORDERING: AllocatorLowerer runs AFTER EscapeAnalysis
    // (which ran in Phase 1c) because it may consume stack_allocated
    // annotations when deciding allocation strategy.
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 4: Tether-specific transforms");
    for (auto& pass : passes_) {
        if (pass->category() != PassCategory::TetherSpecific) continue;

        std::string note;
        bool changed = pass->run(program, type_table_);
        result.passes_run++;

        if (changed) {
            result.transformations_made++;
            note = " -> made transformations";
        } else {
            note = " -> no changes";
        }
        result.pass_log.push_back("[PreLLVM]   " + pass->name() + note);
    }

    // ========================================================================
    // Phase 5: IR HINTS — annotate for LLVM optimization
    //
    // Only IRHint-category passes run here. These add metadata that
    // helps LLVM generate better code but don't change semantics.
    // They must run AFTER all transforms so the hints are accurate.
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 5: IR hints");
    for (auto& pass : passes_) {
        if (pass->category() != PassCategory::IRHint) continue;

        std::string note;
        bool changed = pass->run(program, type_table_);
        result.passes_run++;

        if (changed) {
            result.transformations_made++;
            note = " -> made transformations";
        } else {
            note = " -> no changes";
        }
        result.pass_log.push_back("[PreLLVM]   " + pass->name() + note);
    }

    // ========================================================================
    // Phase 6: LLVM METADATA EMISSION (MUST be last!)
    //
    // L5 translates ALL accumulated metadata into LLVM IR annotations.
    // It MUST run after all transforms so it sees the final state.
    // The old pipeline ran L5 before Track 2 transforms — that was a bug.
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Phase 6: LLVM metadata emission (L5)");
    l5_.emit(program, type_table_, metadata_);
    result.passes_run++;

    // ========================================================================
    // Pipeline summary
    // ========================================================================

    result.pass_log.push_back("[PreLLVM] Pipeline complete: " +
                              std::to_string(result.passes_run) + " passes run, " +
                              std::to_string(result.transformations_made) + " transformations made, " +
                              std::to_string(metadata_.size()) + " metadata nodes, " +
                              std::to_string(metadata_.structs().size()) + " structs analyzed");

    return result;
}

// ============================================================================
// loadProfile — delegate to L6
// ============================================================================
bool PreLLVMPipeline::loadProfile(const std::string& profile_path) {
    return l6_.loadProfile(profile_path);
}

bool PreLLVMPipeline::hasProfile() const {
    return l6_.hasProfile();
}

// ============================================================================
// LLVM metadata queries — delegate to L5
// ============================================================================
std::string PreLLVMPipeline::llvmMetadataBlock() const {
    return l5_.metadataBlock();
}

std::string PreLLVMPipeline::fnAttributes(FnDecl& fn) const {
    return l5_.fnAttributes(fn, metadata_);
}

std::string PreLLVMPipeline::paramAttributes(FnParam& param) const {
    return l5_.paramAttributes(param, metadata_);
}

std::string PreLLVMPipeline::memoryMetadata(Expr& expr) const {
    return l5_.memoryMetadata(expr, metadata_);
}

std::string PreLLVMPipeline::branchWeightMetadata(IfStmt& is) const {
    return l5_.branchWeightMetadata(is, metadata_);
}

std::string PreLLVMPipeline::loopMetadata(WhileStmt& ws) const {
    return l5_.loopMetadata(ws, metadata_);
}

} // namespace tether
