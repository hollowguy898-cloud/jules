#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// EscapeAnalysis: Detects Box/Rc/Arc allocations that don't escape the
// function scope and annotates them for stack allocation.
//
// When a smart pointer is created and its reference never escapes the
// function (not returned, not stored in a global, not passed to another
// function that might store it), the heap allocation can be replaced
// with a stack allocation (alloca + memcpy), eliminating malloc/free overhead.
//
// LLVM CANNOT do this: malloc/free are opaque external calls to LLVM.
// Tether knows the Box/Rc/Arc semantics and can prove non-escape.
class EscapeAnalysisPass : public PreLLVMPass {
public:
    std::string name() const override { return "EscapeAnalysis"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }
    // NOTE: EscapeAnalysis runs in Phase 1c (before all transform passes)
    // despite being categorized as TetherSpecific. This is because
    // AllocatorLowerer (Phase 4) depends on its stack_allocated annotations.

    int boxesStackAllocated() const { return boxes_stack_allocated_; }
    int rcsStackAllocated() const { return rcs_stack_allocated_; }
    int arcsStackAllocated() const { return arcs_stack_allocated_; }

private:
    int boxes_stack_allocated_ = 0;
    int rcs_stack_allocated_ = 0;
    int arcs_stack_allocated_ = 0;

    // Check if a variable/expression escapes the function
    // Returns true if the value escapes (is returned, stored in a global,
    // passed as argument to an opaque function, etc.)
    bool checkEscape(Expr* expr, const std::unordered_set<std::string>& local_vars);

    // Overload that checks escape for a specific variable name
    bool checkEscape(Expr* expr, const std::string& var_name);

    // Analyze a single function for escape opportunities
    bool analyzeFn(FnDecl& fn, TypeTable& type_table);

    // Walk the function body to collect all local variable names
    void collectLocalVars(BlockStmt* block, std::unordered_set<std::string>& locals);

    // Walk the function body to check if a variable escapes
    bool walkForEscape(Expr* expr, const std::string& var_name);

    // Walk statements looking for escape patterns
    bool walkStmtForEscape(Stmt* stmt, const std::string& var_name);
    bool walkBlockForEscape(BlockStmt* block, const std::string& var_name);

    // Check if an expression references the given variable name
    bool exprReferencesVar(Expr* expr, const std::string& var_name);

    // Find the Box.new/Rc.new/Arc.new CallExpr within an initializer
    CallExpr* findAllocCall(Expr* expr, SmartPointerKind kind);

    // Check if a CallExpr is Box.new(), Rc.new(), or Arc.new()
    bool isSmartPointerNew(CallExpr& call, SmartPointerKind kind);
};

} // namespace tether
