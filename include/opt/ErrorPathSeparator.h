#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Error Path Separation: Annotates try/catch/errdefer code paths as "cold"
// so that LLVM places them away from the hot (success) path.
//
// LLVM cannot determine which paths are error vs success paths on its own.
// By adding `cold` attributes and branch weight metadata to error paths,
// LLVM makes better code layout and inlining decisions.
//
// This pass:
// 1. Marks catch blocks with `cold` attribute
// 2. Adds branch weight metadata to try expressions (likely success, unlikely error)
// 3. Moves errdefer blocks to cold sections
//
// LLVM CANNOT do this: it has no semantic knowledge of which branches
// represent error propagation vs normal control flow.
class ErrorPathSeparatorPass : public PreLLVMPass {
public:
    std::string name() const override { return "ErrorPathSeparation"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }

    int annotatedTries() const { return annotated_tries_; }
    int annotatedCatches() const { return annotated_catches_; }
    int annotatedErrdefers() const { return annotated_errdefers_; }

private:
    int annotated_tries_ = 0;
    int annotated_catches_ = 0;
    int annotated_errdefers_ = 0;

    void annotateBlock(BlockStmt* block);
    void annotateTryExpr(TryExpr& expr);
    void annotateErrdefer(ErrdeferStmt& stmt);
    void walkExpr(Expr* expr);
    void walkStmt(Stmt* stmt);
    void walkBlock(BlockStmt* block);
    void walkFn(FnDecl& fn);
};

} // namespace tether
