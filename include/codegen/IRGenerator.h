#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "opt/PreLLVMPipeline.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

namespace tether {

// ============================================================================
// FastBuf — append-only string buffer, avoids ostringstream overhead
// ============================================================================
class FastBuf {
    std::string buf_;
public:
    FastBuf() { buf_.reserve(4096); }
    void clear() { buf_.clear(); }
    std::string str() const { return buf_; }
    FastBuf& operator<<(char c) { buf_ += c; return *this; }
    FastBuf& operator<<(const char* s) { buf_.append(s); return *this; }
    FastBuf& operator<<(const std::string& s) { buf_.append(s); return *this; }
    FastBuf& operator<<(int v) { buf_ += std::to_string(v); return *this; }
    FastBuf& operator<<(unsigned int v) { buf_ += std::to_string(v); return *this; }
    FastBuf& operator<<(long v) { buf_ += std::to_string(v); return *this; }
    FastBuf& operator<<(unsigned long v) { buf_ += std::to_string(v); return *this; }
    FastBuf& operator<<(long long v) { buf_ += std::to_string(v); return *this; }
    FastBuf& operator<<(unsigned long long v) { buf_ += std::to_string(v); return *this; }
};

// ============================================================================
// IRGenerator - walks the typed AST and emits LLVM IR as text (.ll format)
//
// Produces valid LLVM IR (LLVM 15+ opaque-pointer syntax) that can be
// compiled by llc or clang.  All local variables use alloca at function
// entry; the LLVM mem2reg pass promotes them to SSA values.
// ============================================================================
class IRGenerator {
public:
    // -----------------------------------------------------------------------
    // Construction
    //
    // annotations: optional annotation map from the PreLLVM optimization
    // pipeline. When provided, the IR generator consumes optimization
    // metadata (cold paths, prefetch sites, yield points, SoA transforms,
    // allocator lowering, opaque barriers) and emits the corresponding
    // LLVM IR constructs.
    // -----------------------------------------------------------------------
    IRGenerator(const std::vector<std::unique_ptr<TopLevel>>& program,
                TypeTable& type_table,
                ASTAnnotationMap* annotations = nullptr);

    // -----------------------------------------------------------------------
    // Main entry point – returns the full LLVM IR module as a string
    // -----------------------------------------------------------------------
    std::string generate();

private:
    // =======================================================================
    // Type helpers
    // =======================================================================
    std::string llvmType(TypeId type) const;
    std::string llvmReturnType(TypeId type, bool can_error) const;
    std::string llvmParamType(TypeId type) const;
    std::string sanitizeName(const std::string& name) const;
    bool        isAggregateType(TypeId type) const;
    uint64_t    typeSizeBytes(TypeId type) const;
    uint64_t    typeAlignmentBytes(TypeId type) const;
    void        collectNeededTypes(TypeId type);

    // =======================================================================
    // Register / label generation
    // =======================================================================
    std::string nextReg();                               // %0, %1, ...
    std::string nextLabel(const std::string& hint = ""); // then.0, else.1, ...
    std::string makeAllocaName(const std::string& source_name);

    // =======================================================================
    // Module-level emission
    // =======================================================================
    void emitModuleHeader();
    void emitTypeDefinitions();
    void emitRuntimeDecls();
    void emitStringConstants();
    void emitMetadata();

    // =======================================================================
    // Top-level emission
    // =======================================================================
    void emitTopLevel(TopLevel* tl);
    void emitFnDecl(FnDecl* fn);
    void emitStructDecl(StructDecl* sd);
    void emitEnumDecl(EnumDecl* ed);

    // =======================================================================
    // Expression emission
    //   emitExpr  – returns the SSA value (for aggregates, a ptr to memory)
    //   emitLValue – returns a pointer (alloca / GEP result)
    // =======================================================================
    std::string emitExpr(Expr* expr);
    std::string emitLValue(Expr* expr);
    void        emitBinaryOp(const std::string& result_reg,
                             const std::string& ll_type,
                             const std::string& lhs,
                             BinaryOp op,
                             const std::string& rhs,
                             TypeId result_type);

    // =======================================================================
    // Statement emission
    // =======================================================================
    void emitStmt(Stmt* stmt);
    void emitBlockStmt(BlockStmt* block);
    void emitDeferBlocks();
    void emitErrdeferBlocks();
    std::string emitAtomicRMW(const std::string& result_reg,
                               const std::string& ll_type,
                               const std::string& ptr_reg,
                               const std::string& val_reg,
                               BinaryOp op,
                               AtomicStmt::Ordering ordering);

    // =======================================================================
    // Smart-pointer emission helpers
    // =======================================================================
    std::string emitBoxNew(Expr* value, TypeId pointee_type);
    void        emitBoxDrop(const std::string& ptr_val);
    std::string emitRcNew(Expr* value, TypeId pointee_type);
    std::string emitRcClone(const std::string& rc_struct_ptr, TypeId pointee_type);
    void        emitRcDrop(const std::string& rc_struct_ptr, TypeId pointee_type);
    std::string emitArcNew(Expr* value, TypeId pointee_type);
    std::string emitArcClone(const std::string& arc_struct_ptr, TypeId pointee_type);
    void        emitArcDrop(const std::string& arc_struct_ptr, TypeId pointee_type);

    // =======================================================================
    // Error-handling helpers
    // =======================================================================
    // Given a call result of ErrorType, extract the value and error-flag,
    // check the flag, and if set emit an early return.  Returns the
    // extracted (non-error) value register.
    std::string emitErrorCheck(const std::string& result_reg, TypeId error_type);

    // =======================================================================
    // Allocator helpers
    // =======================================================================
    std::string emitAllocatorCall(Expr* allocator_expr, TypeId alloc_type,
                                  const std::string& count_reg);

    // =======================================================================
    // Input references
    // =======================================================================
    const std::vector<std::unique_ptr<TopLevel>>& program_;
    TypeTable& type_table_;
    ASTAnnotationMap* annotations_;  // May be nullptr if no pre-LLVM opts ran

    // =======================================================================
    // Output streams
    //   module_out_ – the final module-level output
    //   alloca_ss_  – current-function alloca instructions  (entry block)
    //   body_ss_    – current-function body instructions
    // =======================================================================
    FastBuf module_out_;
    FastBuf alloca_ss_;
    FastBuf body_ss_;

    // =======================================================================
    // Code-generation state
    // =======================================================================
    int reg_counter_   = 0;
    int label_counter_ = 0;

    // Scoped variable allocation map — each scope level has its own map
    // of variable names → LLVM alloca register names. On block exit, the
    // scope is popped, restoring access to outer variables.
    std::vector<std::unordered_map<std::string, std::string>> scope_stack_;

    // Push a new variable scope (call on block entry)
    void pushScope() { scope_stack_.emplace_back(); }

    // Pop a variable scope (call on block exit)
    void popScope() { if (!scope_stack_.empty()) scope_stack_.pop_back(); }

    // Look up a variable name in the scope stack (innermost first)
    std::string* lookupVarAlloca(const std::string& name) {
        for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        // Fallback: check the flat map for function parameters (pre-scope)
        auto found = var_allocas_.find(name);
        if (found != var_allocas_.end()) return &found->second;
        return nullptr;
    }

    // Register a variable in the current (innermost) scope
    void registerVarAlloca(const std::string& name, const std::string& alloca) {
        if (!scope_stack_.empty()) {
            scope_stack_.back()[name] = alloca;
        } else {
            var_allocas_[name] = alloca;
        }
    }

    std::unordered_set<std::string>               used_alloca_names_;

    // Function-level variable allocas (for parameters, which are registered
    // before any scope is pushed). Block-scoped variables go into scope_stack_.
    std::unordered_map<std::string, std::string> var_allocas_;

    // Defer stack (raw Stmt pointers – owned by the AST, not by us)
    std::vector<Stmt*> defer_stack_;

    // Errdefer stack (similar to defer_stack_ but only runs on error paths)
    std::vector<Stmt*> errdefer_stack_;

    // Stack-allocated Box pointers (from EscapeAnalysis StackAllocated annotation).
    // Tracks which Box alloca names were stack-allocated so emitBoxDrop can
    // skip the free() call. Cleared per-function.
    std::unordered_set<std::string> stack_box_allocas_;

    // Track whether current function can error (for errdefer codegen)
    bool current_fn_can_error_ = false;

    // Runtime functions the generated code references
    std::unordered_set<std::string> needed_runtime_;

    // String constants:  source string → global-constant index
    std::unordered_map<std::string, int> string_constants_;
    int string_counter_ = 0;

    // Struct / composite type definitions we need to emit
    struct TypeInfo {
        std::string llvm_name;   // e.g. "%struct.Point"
        std::string body;        // e.g. "{ i32, i32 }"
    };
    std::unordered_map<std::string, TypeInfo> needed_types_;
    std::vector<std::string>                   type_emit_order_; // preserve insertion order

    // =======================================================================
    // Current-function context
    // =======================================================================
    TypeId      current_return_type_;
    TypeId      current_fn_error_type_;  // BUG FIX: stored for correct default terminator
    bool        current_can_error_   = false;
    bool        current_fn_has_simd_ = false;
    std::string current_fn_name_;
    std::string current_ret_alloca_; // alloca for the aggregate return value
    std::string current_err_slot_;   // callee-side: name of the ptr %err_slot parameter
    std::string caller_err_slot_;    // caller-side: alloca name of the i32 err_slot for the most recent error-returning call

    // =======================================================================
    // Loop context (for break / continue)
    // =======================================================================
    struct LoopContext {
        std::string break_label;
        std::string continue_label;
    };
    std::vector<LoopContext> loop_stack_;

    // =======================================================================
    // Basic-block state
    // =======================================================================
    bool terminated_ = false;
    bool isTerminated() const { return terminated_; }
    void setTerminated(bool t) { terminated_ = t; }

    // =======================================================================
    // Metadata (for @superoptimize / @polly directives)
    // =======================================================================
    struct MetadataEntry {
        int         id;       // !0, !1, …
        std::string content;  // e.g. !{!"jules.superoptimize"}
    };
    std::vector<MetadataEntry>           metadata_entries_;
    int                                   metadata_counter_ = 0;
    std::unordered_map<std::string, int>  metadata_map_;    // key → id

    // Returns the metadata-id for the given key, allocating one if needed.
    int getMetadataId(const std::string& key);

    // Emits @simd loop vectorization metadata and returns the metadata ID
    // for the loop annotation node (e.g. !llvm.loop !N).
    int emitSimdLoopMetadata();

    // =======================================================================
    // Annotation-driven emission helpers
    //
    // These methods check the ASTAnnotationMap for optimization metadata
    // computed by the pre-LLVM pipeline and emit the appropriate LLVM IR.
    // =======================================================================

    // Check if a node has a ColdPath annotation; if so, emit branch weight
    // metadata on the enclosing branch instruction.
    // Returns the profile metadata string (e.g., "!prof !5") or empty string.
    std::string emitColdPathMetadata(const ASTNode* node);

    // Emit a prefetch intrinsic if the WhileStmt has a PrefetchSite annotation.
    void emitPrefetchIfAnnotated(WhileStmt* loop);

    // Emit a yield check at the top of a loop body if the WhileStmt has
    // a YieldPoint annotation. Uses a counter to only check every N iterations.
    void emitYieldCheckIfAnnotated(WhileStmt* loop);

    // Check if a FnDecl has an OpaqueBarrier annotation; if so, add
    // the appropriate function attributes and emit fences around calls.
    bool hasOpaqueBarrierAnnotation(FnDecl* fn) const;

    // Emit an inline arena bump allocation if a CallExpr has an
    // AllocatorInlined annotation. Returns the result register.
    std::string emitInlineAllocatorIfAnnotated(CallExpr* call, TypeId ret_type);

    // Emit SoA-transformed array access if a MemberExpr/IndexExpr has
    // a SoATransformed annotation. Returns the result register.
    std::string emitSoAAccessIfAnnotated(Expr* expr);

    // Check if an expression has a SoATransformed annotation.
    bool hasSoAAnnotation(Expr* expr) const;

    // Get the branch weight metadata ID for cold/hot branch hints.
    // Creates metadata entries like: !{!"branch_weights", i32 1, i32 1000}
    int getBranchWeightMetadataId(uint32_t cold_weight, uint32_t hot_weight);
};

} // namespace tether
