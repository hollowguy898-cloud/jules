#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// SROA (Scalar Replacement of Aggregates): Decomposes small struct-typed
// variables into individual SSA values BEFORE LLVM IR generation.
//
// Currently, Tether structs are emitted as LLVM struct types with
// alloca/load/store. LLVM's own SROA pass handles some of this after
// lowering, but Tether can decompose small structs into individual SSA
// values *before* LLVM IR generation. This eliminates alloca for struct
// temporaries entirely and gives the LLVM optimizer a head start.
//
// A struct variable is eligible for SROA when:
//   - The struct has ≤4 fields
//   - The variable is never address-taken (no &x, no pointer to x)
//   - The variable is never passed whole to a function call
//   - The variable has no whole-struct assignment or return
//   - At least one MemberExpr accesses the variable's fields
//
// When eligible, the pass writes metadata (sroa_eligible, sroa_field_names,
// sroa_field_types) into the MetadataMap on the VarDeclStmt/ValDeclStmt node.
// The IRGenerator then reads this metadata to emit individual SSA variables
// for each field instead of an alloca for the whole struct.
//
// This is NOT redundant with LLVM because LLVM's SROA operates on LLVM IR
// (alloca/load/store/GEP) — by decomposing at the Tether level, we avoid
// emitting those instructions in the first place, reducing IR size and
// giving LLVM's optimizer simpler code to work with.
class SROAPass : public PreLLVMPass {
public:
    std::string name() const override { return "SROA"; }
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }
    bool run(Program& program, TypeTable& type_table) override;

    int varsDecomposed() const { return vars_decomposed_; }

private:
    int vars_decomposed_ = 0;

    // Analyze a single function for SROA opportunities
    bool analyzeFn(FnDecl& fn, TypeTable& type_table);

    // Check if a variable is address-taken within a function body
    bool isAddressTaken(BlockStmt* block, const std::string& var_name);
    bool isAddressTakenStmt(Stmt* stmt, const std::string& var_name);
    bool isAddressTakenExpr(Expr* expr, const std::string& var_name);

    // Check if a variable is passed whole to a function call
    bool isPassedWholeToCall(BlockStmt* block, const std::string& var_name);
    bool isPassedWholeToCallStmt(Stmt* stmt, const std::string& var_name);
    bool isPassedWholeToCallExpr(Expr* expr, const std::string& var_name);

    // Check if a variable has any whole-struct operations (assignment, return)
    bool hasWholeStructOp(BlockStmt* block, const std::string& var_name);
    bool hasWholeStructOpStmt(Stmt* stmt, const std::string& var_name);
    bool hasWholeStructOpExpr(Expr* expr, const std::string& var_name);

    // Check if any MemberExpr accesses the variable's fields
    bool hasFieldAccess(BlockStmt* block, const std::string& var_name);
    bool hasFieldAccessStmt(Stmt* stmt, const std::string& var_name);
    bool hasFieldAccessExpr(Expr* expr, const std::string& var_name);
};

} // namespace tether
