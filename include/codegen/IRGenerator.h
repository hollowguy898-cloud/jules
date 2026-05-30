#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"

#include <string>
#include <vector>
#include <memory>
#include <set>
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
// compiled by llc or clang.  Scalar non-address-taken variables are emitted
// as SSA values directly (no alloca/load/store).  Aggregate and address-taken
// variables use alloca at function entry; the LLVM mem2reg pass promotes the
// remaining simple allocas.  Phi nodes are emitted at control-flow join points
// (if/else merge, loop headers) for SSA-tracked variables.
// ============================================================================
class IRGenerator {
public:
    // -----------------------------------------------------------------------
    // Construction
    //
    // meta_map: optional MetadataMap from the unified metadata pipeline
    // (MetadataEngine + PreLLVMPipeline). When provided, the IR generator
    // consumes typed optimization metadata (cold paths, prefetch sites,
    // yield points, SoA transforms, allocator lowering, opaque barriers)
    // and emits the corresponding LLVM IR constructs.
    // -----------------------------------------------------------------------
    IRGenerator(const std::vector<std::unique_ptr<TopLevel>>& program,
                TypeTable& type_table,
                MetadataMap* meta_map = nullptr,
                int opt_level = 2);

    // -----------------------------------------------------------------------
    // Main entry point – returns the full LLVM IR module as a string
    // -----------------------------------------------------------------------
    std::string generate();

private:
    // =======================================================================
    // Type helpers
    // =======================================================================
    std::string llvmType(TypeId type);
    std::string llvmReturnType(TypeId type, bool can_error);
    std::string llvmParamType(TypeId type);
    std::string sanitizeName(const std::string& name) const;
    std::string zeroConstant(const std::string& llvm_type) const;
    bool        isAggregateType(TypeId type) const;
    uint64_t    typeSizeBytes(TypeId type) const;
    uint64_t    typeAlignmentBytes(TypeId type) const;
    void        collectNeededTypes(TypeId type);
    std::string emitTypeCast(const std::string& val, TypeId from_type, TypeId to_type);

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

    // Emit TBAA type descriptors and alias scopes from the metadata engine
    void emitMetaMapTBAA();

    // Pre-register TBAA metadata IDs (called before code emission)
    void preRegisterTBAAMetadata();

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
    std::string emitBinaryOp(const std::string& result_reg,
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
    MetadataMap* meta_map_;          // May be nullptr if metadata engine not run

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
    int phi_patch_counter_ = 0;  // Global counter for unique phi placeholder names
                                  // (prevents collision in nested loops)

    // =======================================================================
    // SSA Variable Tracking
    //
    // Instead of alloca+load+store for every variable, we track the current
    // SSA value for each variable. For scalar non-address-taken variables,
    // we emit the value directly and update it on assignment. For aggregate
    // types or address-taken variables, we fall back to alloca.
    //
    // At control-flow join points (if/else merge, loop headers), we emit
    // phi nodes for variables that were assigned in multiple branches.
    // =======================================================================

    // Information about an SSA-tracked variable
    struct SSAVarInfo {
        std::string current_value;  // Current SSA value (register name or literal)
        std::string llvm_type;      // LLVM type string (e.g. "i32", "i64")
        TypeId      tether_type;    // Tether type for aggregate check
        bool        needs_alloca;   // true if address-taken or aggregate → use alloca
        std::string alloca_name;    // Alloca name (only valid if needs_alloca)
    };

    // Scoped variable map — each scope level has its own map
    // of variable names → SSAVarInfo. On block exit, the scope is popped.
    std::vector<std::unordered_map<std::string, SSAVarInfo>> scope_stack_;

    // Push a new variable scope (call on block entry)
    void pushScope() { scope_stack_.emplace_back(); }

    // Pop a variable scope (call on block exit)
    void popScope() { if (!scope_stack_.empty()) scope_stack_.pop_back(); }

    // Look up a variable's SSAVarInfo in the scope stack (innermost first)
    SSAVarInfo* lookupVar(const std::string& name) {
        for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) return &found->second;
        }
        // Fallback: check the flat map for function parameters (pre-scope)
        auto found = var_ssa_.find(name);
        if (found != var_ssa_.end()) return &found->second;
        return nullptr;
    }

    // Look up a variable name → alloca pointer (for address-taken / aggregate vars)
    // Returns nullptr if the variable is SSA-tracked (not alloca-backed)
    std::string* lookupVarAlloca(const std::string& name) {
        SSAVarInfo* info = lookupVar(name);
        if (info && info->needs_alloca) return &info->alloca_name;
        return nullptr;
    }

    // Register a variable in the current (innermost) scope
    void registerVar(const std::string& name, const SSAVarInfo& info) {
        if (!scope_stack_.empty()) {
            scope_stack_.back()[name] = info;
        } else {
            var_ssa_[name] = info;
        }
    }

    // Convenience: register with alloca (for aggregate/address-taken vars)
    void registerVarAlloca(const std::string& name, const std::string& alloca) {
        SSAVarInfo* existing = lookupVar(name);
        if (existing) {
            existing->alloca_name = alloca;
            existing->needs_alloca = true;
        }
    }

    // Update the current SSA value for a variable
    void updateVarValue(const std::string& name, const std::string& value) {
        SSAVarInfo* info = lookupVar(name);
        if (info) info->current_value = value;
    }

    // Get the current SSA value for a variable (returns "" if not found)
    std::string getVarValue(const std::string& name) {
        SSAVarInfo* info = lookupVar(name);
        return info ? info->current_value : "";
    }

    // Check if a variable needs alloca (aggregate or address-taken)
    bool varNeedsAlloca(const std::string& name) {
        SSAVarInfo* info = lookupVar(name);
        return info ? info->needs_alloca : false;
    }

    // =======================================================================
    // SROA (Scalar Replacement of Aggregates) helpers
    //
    // When a struct variable has sroa_eligible metadata, the IR generator
    // decomposes it into individual SSA variables for each field instead
    // of using alloca for the whole struct.
    // =======================================================================

    // Check if a variable is an SROA-decomposed variable
    bool isSROAVariable(const std::string& name) const {
        return sroa_vars_.count(name) > 0;
    }

    // Generate the SSA variable name for an SROA field
    // e.g., sroaFieldName("p", "x") → "p.x"
    static std::string sroaFieldName(const std::string& var_name,
                                     const std::string& field_name) {
        return var_name + "." + field_name;
    }

    // Track which variables are SROA'd and their field mappings.
    // Key: original variable name (e.g., "p")
    // Value: pair of (field_names, field_types) for the decomposed fields
    struct SROAVarInfo {
        std::string struct_type_name;          // e.g., "Point"
        std::vector<std::string> field_names;  // e.g., {"x", "y"}
        std::vector<TypeId> field_types;       // e.g., {f64, f64}
    };
    std::unordered_map<std::string, SROAVarInfo> sroa_vars_;

    // Determine if a variable should use SSA (scalar, non-address-taken)
    bool shouldUseSSA(TypeId type) const {
        if (!type) return false;
        // SimdVector types use SSA — LLVM vectors are first-class SSA values
        if (isa<SimdVectorType>(type)) return true;
        if (isAggregateType(type)) return false;
        // References and pointers are just ptr values — SSA friendly
        // Smart pointers: Box is ptr (SSA), Rc/Arc are aggregate (not SSA)
        if (isa<SmartPointerType>(type)) {
            auto& sp = cast<SmartPointerType>(type);
            return sp.smartPointerKind() == SmartPointerKind::Box;
        }
        // Error types: niched (pointer/integer niche) are SSA-friendly scalars,
        // struct fallback is an aggregate
        if (isa<ErrorType>(type)) return !isAggregateType(type);
        return true;
    }

    // Collect names of variables that are assigned within a subtree.
    // Used to demote SSA variables to alloca before loops (so LLVM's
    // mem2reg can handle loop-carried phi nodes correctly).
    std::unordered_set<std::string> collectAssignedVars(Stmt* stmt) {
        std::unordered_set<std::string> result;
        collectAssignedVarsImpl(stmt, result);
        return result;
    }
    std::unordered_set<std::string> collectAssignedVars(Expr* expr) {
        std::unordered_set<std::string> result;
        collectAssignedVarsImpl(expr, result);
        return result;
    }

    // Demote an SSA-tracked variable to alloca-based (for loop vars)
    void demoteSSAToAlloca(const std::string& name) {
        SSAVarInfo* info = lookupVar(name);
        if (!info || info->needs_alloca) return;
        // Create alloca, store current value
        std::string aname = makeAllocaName(name);
        alloca_ss_ << "  " << aname << " = alloca " << info->llvm_type << "\n";
        body_ss_ << "  store " << info->llvm_type << " " << info->current_value
                 << ", ptr " << aname << "\n";
        info->alloca_name = aname;
        info->needs_alloca = true;
    }

private:
    // Recursive implementation of collectAssignedVars
    void collectAssignedVarsImpl(Stmt* stmt, std::unordered_set<std::string>& result) {
        if (!stmt) return;
        switch (stmt->getKind()) {
            case NodeKind::VarDeclStmt: {
                auto& vd = cast<VarDeclStmt>(*stmt);
                result.insert(vd.name());
                break;
            }
            case NodeKind::ValDeclStmt: {
                auto& vd = cast<ValDeclStmt>(*stmt);
                result.insert(vd.name());
                break;
            }
            case NodeKind::AssignStmt: {
                auto& as = cast<AssignStmt>(*stmt);
                if (auto* ident = dyn_cast<IdentExpr>(as.target())) {
                    result.insert(ident->name());
                }
                // Also check RHS for nested assignments
                collectAssignedVarsImpl(as.value(), result);
                break;
            }
            case NodeKind::BlockStmt: {
                auto& block = cast<BlockStmt>(*stmt);
                for (const auto& s : block.stmts())
                    collectAssignedVarsImpl(s.get(), result);
                break;
            }
            case NodeKind::IfStmt: {
                auto& is = cast<IfStmt>(*stmt);
                collectAssignedVarsImpl(is.thenBlock(), result);
                if (is.hasElse()) collectAssignedVarsImpl(is.elseBlock(), result);
                break;
            }
            case NodeKind::WhileStmt: {
                auto& ws = cast<WhileStmt>(*stmt);
                collectAssignedVarsImpl(ws.body(), result);
                break;
            }
            case NodeKind::ExprStmt: {
                auto& es = cast<ExprStmt>(*stmt);
                collectAssignedVarsImpl(es.expr(), result);
                break;
            }
            case NodeKind::DeferStmt: {
                auto& ds = cast<DeferStmt>(*stmt);
                collectAssignedVarsImpl(ds.stmt(), result);
                break;
            }
            case NodeKind::ErrdeferStmt: {
                auto& es = static_cast<ErrdeferStmt&>(*stmt);
                collectAssignedVarsImpl(es.stmt(), result);
                break;
            }
            case NodeKind::MatchStmt: {
                auto& ms = cast<MatchStmt>(*stmt);
                for (const auto& arm : ms.arms()) {
                    if (arm.body) collectAssignedVarsImpl(arm.body.get(), result);
                }
                break;
            }
            default: break;
        }
    }
    void collectAssignedVarsImpl(Expr* expr, std::unordered_set<std::string>& result) {
        if (!expr) return;
        switch (expr->getKind()) {
            case NodeKind::BinaryExpr: {
                auto& bin = cast<BinaryExpr>(*expr);
                if (bin.op() == BinaryOp::Assign || 
                    (bin.op() >= BinaryOp::AddAssign && bin.op() <= BinaryOp::ShrAssign)) {
                    if (auto* ident = dyn_cast<IdentExpr>(bin.left())) {
                        result.insert(ident->name());
                    }
                }
                collectAssignedVarsImpl(bin.left(), result);
                collectAssignedVarsImpl(bin.right(), result);
                break;
            }
            case NodeKind::CallExpr: {
                auto& call = cast<CallExpr>(*expr);
                for (const auto& arg : call.args())
                    collectAssignedVarsImpl(arg.get(), result);
                break;
            }
            case NodeKind::ComptimeExpr: {
                auto& ce = cast<ComptimeExpr>(*expr);
                collectAssignedVarsImpl(ce.inner(), result);
                break;
            }
            case NodeKind::ReduceExpr: {
                auto& re = cast<ReduceExpr>(*expr);
                collectAssignedVarsImpl(re.iterable(), result);
                if (re.hasAxis()) collectAssignedVarsImpl(re.axis(), result);
                break;
            }
            default: break;
        }
    }

public:

    // =======================================================================
    // Phi Node Tracking
    //
    // At control-flow join points, we need phi nodes for SSA variables
    // that were assigned in multiple predecessor blocks.
    // =======================================================================

    // Snapshot of all SSA variable values at a point in time
    struct SSASnapshot {
        std::unordered_map<std::string, std::string> values;  // name → current_value
    };

    // Take a snapshot of all current SSA variable values
    SSASnapshot takeSnapshot() {
        SSASnapshot snap;
        // Walk all scopes (innermost first, so inner scopes shadow outer)
        for (auto it = scope_stack_.rbegin(); it != scope_stack_.rend(); ++it) {
            for (auto& [name, info] : *it) {
                if (!info.needs_alloca && !info.current_value.empty()) {
                    snap.values[name] = info.current_value;
                }
            }
        }
        // Also walk param-level vars
        for (auto& [name, info] : var_ssa_) {
            if (!info.needs_alloca && !info.current_value.empty()) {
                snap.values[name] = info.current_value;
            }
        }
        return snap;
    }

    // Emit phi nodes at a join block for variables that differ between
    // two predecessor snapshots. Returns the set of variables that got phis.
    std::unordered_map<std::string, std::string> emitPhiNodes(
        const SSASnapshot& left_snap, const std::string& left_label,
        const SSASnapshot& right_snap, const std::string& right_label)
    {
        std::unordered_map<std::string, std::string> phi_results;
        // Collect all variable names from both snapshots
        std::set<std::string> all_names;
        for (auto& [n, _] : left_snap.values) all_names.insert(n);
        for (auto& [n, _] : right_snap.values) all_names.insert(n);

        for (const auto& name : all_names) {
            auto left_it = left_snap.values.find(name);
            auto right_it = right_snap.values.find(name);
            std::string left_val = (left_it != left_snap.values.end()) ? left_it->second : "undef";
            std::string right_val = (right_it != right_snap.values.end()) ? right_it->second : "undef";

            // Only emit phi if values actually differ
            if (left_val == right_val) {
                // Same value from both sides — no phi needed, just update
                updateVarValue(name, left_val);
                continue;
            }

            // Look up the type
            SSAVarInfo* info = lookupVar(name);
            if (!info) continue;
            std::string ll_type = info->llvm_type;

            std::string phi_reg = nextReg();
            body_ss_ << "  " << phi_reg << " = phi " << ll_type
                     << " [ " << left_val << ", %" << left_label << " ]"
                     << ", [ " << right_val << ", %" << right_label << " ]\n";
            updateVarValue(name, phi_reg);
            phi_results[name] = phi_reg;
        }
        return phi_results;
    }

    // Overload: emit phi nodes when one side is missing (only one predecessor)
    void emitPhiNodesSinglePred(const SSASnapshot& snap, const std::string& pred_label) {
        for (auto& [name, val] : snap.values) {
            updateVarValue(name, val);
        }
    }

    std::unordered_set<std::string>               used_alloca_names_;

    // Function-level variable SSA info (for parameters, which are registered
    // before any scope is pushed). Block-scoped variables go into scope_stack_.
    std::unordered_map<std::string, SSAVarInfo> var_ssa_;

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
    int         opt_level_ = 2;          // LLVM optimization level (0-5) for codegen decisions
    TypeId      current_return_type_;
    TypeId      current_fn_error_type_;  // BUG FIX: stored for correct default terminator
    bool        current_can_error_   = false;
    bool        current_fn_has_simd_ = false;
    bool        current_fn_has_tailcall_ = false;  // @tailcall directive for current function
    bool        is_musttail_call_ = false;         // Next CallExpr should be emitted as musttail
    FnDecl*     current_fn_ = nullptr;             // Current function being compiled (for type lookups)
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

    // Track the current block label for phi node predecessor references.
    // Updated whenever we emit a new label (body_ss_ << label << ":\n").
    std::string current_block_label_ = "entry";

    // Emit a block label and update current_block_label_
    void emitBlockLabel(const std::string& label) {
        body_ss_ << label << ":\n";
        current_block_label_ = label;
    }

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
    // Tether metadata emission for LLVM pass plugin
    //
    // These methods emit Tether-specific named metadata that the
    // TetherAttrPass LLVM plugin reads to inject optimization attributes.
    // The metadata is emitted as module-level named metadata nodes
    // (!tether.fns, !tether.params, !tether.loops).
    // =======================================================================

    // Emit Tether metadata for a single function. Called after the function
    // body is emitted. Computes flags from the MetadataMap's NodeMeta and
    // records them for later emission by emitTetherMetadata().
    void emitTetherFnMetadata(FnDecl* fn);

    // Emit all accumulated Tether named metadata at the end of the module.
    // Called after all top-level declarations have been emitted.
    void emitTetherMetadata();

    // Per-function Tether metadata collected during emission
    struct TetherFnMeta {
        std::string fn_name;         // sanitized function name
        uint32_t fn_flags = 0;       // function-level flags bitmask
        std::vector<uint32_t> param_flags;  // per-parameter flags bitmask
        uint32_t loop_flags = 0;     // loop-level flags (vectorize/unroll)
        // Dereferenceable metadata: (param_index, dereferenceable_bytes)
        // For &T and &mut T parameters where the referent has a known size.
        std::vector<std::pair<size_t, uint64_t>> deref_params;
    };
    std::vector<TetherFnMeta> tether_fn_metas_;

    // =======================================================================
    // Metadata-driven emission helpers
    //
    // These methods check the MetadataMap for optimization metadata
    // computed by the unified metadata pipeline and emit the appropriate
    // LLVM IR.
    // =======================================================================

    // Check if a node has a cold_path flag; if so, emit branch weight
    // metadata on the enclosing branch instruction.
    // Returns the profile metadata string (e.g., "!prof !5") or empty string.
    std::string emitColdPathMetadata(const ASTNode* node);

    // Emit a prefetch intrinsic if the WhileStmt has a PrefetchSite annotation.
    void emitPrefetchIfAnnotated(WhileStmt* loop);

    // Emit yield counter initialization (alloca + store 0) in the pre-header
    // block, BEFORE the branch to while.cond. Must be called before the loop.
    void emitYieldCounterInitIfAnnotated(WhileStmt* loop);

    // Emit a yield check at the top of a loop body if the WhileStmt has
    // a YieldPoint annotation. Uses a counter to only check every N iterations.
    // Assumes emitYieldCounterInitIfAnnotated was called earlier.
    void emitYieldCheckIfAnnotated(WhileStmt* loop);

    // Look up a FnDecl by name in the program's top-level declarations.
    // Returns nullptr if not found. Used to get parameter types for call
    // argument type coercion (Bug 3 fix).
    FnDecl* findFnDecl(const std::string& name) const;

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

    // Metadata engine helpers: emit TBAA metadata on struct field loads
    std::string emitTBAAMetadataForField(const std::string& struct_name,
                                          const std::string& field_name);

    // Emit both a TBAA type descriptor and an access tag for a struct field.
    // Returns the access tag metadata string (e.g. ", !tbaa !7") for use on
    // load/store instructions. Also emits the type descriptor if needed.
    std::string emitTBAATypeAndAccessTags(const std::string& struct_name,
                                           const std::string& field_name,
                                           uint64_t offset, uint64_t size);

    // Metadata engine helpers: emit !range metadata on enum loads
    std::string emitRangeMetadataForEnum(TypeId type);

    // =======================================================================
    // Speculative optimization: deoptimization guard emission
    //
    // These methods check the MetadataMap for speculative assumptions
    // (from the SpeculativeOptimizerPass) and emit deoptimization guards.
    // If an assumption is violated at runtime, the deopt handler is called.
    // =======================================================================

    // Check if a node has a speculative assumption worth guarding
    bool hasSpeculativeAssumption(const ASTNode* node) const;

    // Emit a deoptimization guard for a branch (BranchNeverTaken assumption).
    // The unlikely branch leads to a deopt block that calls tether_deopt().
    // Returns the label of the fast-path block.
    std::string emitDeoptGuardForBranch(const ASTNode* node, const std::string& cond,
                                         const std::string& likely_label,
                                         const std::string& unlikely_label);

    // Emit a deoptimization block that calls the runtime deopt handler.
    // If `label` is provided, reuse it instead of generating a new one
    // (fixes the empty-label bug when the caller already emitted a branch
    // to a specific deopt label).
    void emitDeoptBlock(int deopt_id, const std::string& label = "");

    // Counter for unique deopt IDs within a function
    int deopt_counter_ = 0;

    // =======================================================================
    // Yield counter state (for split init/check emission)
    //
    // emitYieldCounterInitIfAnnotated sets these in the pre-header,
    // and emitYieldCheckIfAnnotated reads them in the loop body.
    // =======================================================================
    std::string current_yield_counter_alloca_;  // alloca name for current loop's counter
    int current_yield_interval_ = 256;          // yield interval for current loop
};

} // namespace tether
