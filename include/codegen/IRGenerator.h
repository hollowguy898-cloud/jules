#pragma once

#include "ast/AST.h"
#include "sema/Type.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <sstream>
#include <cstdint>

namespace tether {

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
    // -----------------------------------------------------------------------
    IRGenerator(const std::vector<std::unique_ptr<TopLevel>>& program,
                TypeTable& type_table);

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

    // =======================================================================
    // Output streams
    //   module_out_ – the final module-level output
    //   alloca_ss_  – current-function alloca instructions  (entry block)
    //   body_ss_    – current-function body instructions
    // =======================================================================
    std::ostringstream module_out_;
    std::ostringstream alloca_ss_;
    std::ostringstream body_ss_;

    // =======================================================================
    // Code-generation state
    // =======================================================================
    int reg_counter_   = 0;
    int label_counter_ = 0;

    // Map source variable names → LLVM alloca register names
    std::unordered_map<std::string, std::string> var_allocas_;
    std::unordered_set<std::string>               used_alloca_names_;

    // Defer stack (raw Stmt pointers – owned by the AST, not by us)
    std::vector<Stmt*> defer_stack_;

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
    bool        current_can_error_   = false;
    bool        current_fn_has_simd_ = false;
    std::string current_fn_name_;
    std::string current_ret_alloca_; // alloca for the aggregate return value

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
};

} // namespace tether
