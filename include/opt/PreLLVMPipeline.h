#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "parser/Parser.h"

#include <vector>
#include <memory>
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
// AST Annotation Map
//
// A side-table for storing metadata on AST nodes that the pre-LLVM passes
// compute and the IR generator later consumes. This avoids modifying the
// existing AST node hierarchy.
// ============================================================================
enum class ASTAnnotationKind : uint8_t {
    ColdPath,           // This node is on an error/cold path
    OpaqueBarrier,      // This node crosses an opaque FFI boundary
    PrefetchSite,       // Insert prefetch at this loop boundary
    YieldPoint,         // Insert yield check at this point
    SoATransformed,     // This struct has been SoA-transformed
    AllocatorInlined,   // This allocation call was inlined
    HotField,           // This field access is on the hot path (PGO)
    ColdField,          // This field access is on the cold path (PGO)
    StackAllocated      // This smart pointer allocation can use stack instead of heap
};

struct ASTAnnotation {
    ASTAnnotationKind kind;
    std::string detail;  // Optional detail string (e.g., branch weight, prefetch distance)
};

class ASTAnnotationMap {
public:
    void annotate(const ASTNode* node, ASTAnnotationKind kind, std::string detail = "") {
        annotations_[node].push_back({kind, std::move(detail)});
    }

    bool hasAnnotation(const ASTNode* node, ASTAnnotationKind kind) const {
        auto it = annotations_.find(node);
        if (it == annotations_.end()) return false;
        for (const auto& ann : it->second) {
            if (ann.kind == kind) return true;
        }
        return false;
    }

    const std::vector<ASTAnnotation>* getAnnotations(const ASTNode* node) const {
        auto it = annotations_.find(node);
        if (it == annotations_.end()) return nullptr;
        return &it->second;
    }

    void clear() { annotations_.clear(); }
    size_t size() const { return annotations_.size(); }

private:
    std::unordered_map<const ASTNode*, std::vector<ASTAnnotation>> annotations_;
};

// ============================================================================
// PreLLVMPass - base class for all pre-LLVM optimization passes
//
// These passes run on the AST BEFORE LLVM IR emission. They perform
// transformations that LLVM cannot do because they require semantic
// knowledge that is lost once we lower to LLVM IR.
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

    // Set the annotation map for this pass to write metadata into
    void setAnnotationMap(ASTAnnotationMap* annotations) { annotations_ = annotations; }

protected:
    ASTAnnotationMap* annotations_ = nullptr;
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
// PreLLVMPipeline - orchestrates all pre-LLVM optimization passes
//
// The pipeline runs passes in a specific order:
//   1. Hot/Cold Field Splitting (PGO)   - splits structs before reordering
//   2. Field Reordering                  - reorders for minimal padding
//   3. AoS->SoA Transformation           - splits arrays of structs
//   4. Error Path Separation             - annotates cold paths
//   5. Opaque Type Barrier               - marks FFI boundaries
//   6. Defer Chain Coalescing            - merges defer chains
//   7. Allocator Inline Lowering         - inlines arena bump allocation
//   8. Align-Guided Prefetch Insertion   - inserts prefetch instructions
//   9. Yield Point Insertion             - inserts cooperative yield checks
// ============================================================================
class PreLLVMPipeline {
public:
    PreLLVMPipeline(PreLLVMOptLevel level, TypeTable& type_table);

    PreLLVMPipelineResult run(Program& program);

    void addPass(std::unique_ptr<PreLLVMPass> pass);

    // Access the annotation map for downstream IR generation
    ASTAnnotationMap& annotations() { return annotations_; }
    const ASTAnnotationMap& annotations() const { return annotations_; }

private:
    PreLLVMOptLevel level_;
    TypeTable& type_table_;
    std::vector<std::unique_ptr<PreLLVMPass>> passes_;
    ASTAnnotationMap annotations_;
};

} // namespace tether
