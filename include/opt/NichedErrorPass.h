#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Niched Error Type Optimization Pass
//
// Analyzes ErrorType usages in the program and determines which ones can
// use niche optimization (pointer tag bits or integer high bit) instead of
// the traditional { T value, i1 error_flag } struct representation.
//
// Niche optimization eliminates the separate error flag and the padding waste,
// reducing the size of error types and improving cache locality on the
// success (hot) path.
//
// Three niche strategies:
//   PointerNiche: For pointer-like success types (ptr, &T, &mut T, Box<T>, Rc<T>, Arc<T>).
//                 The error state is encoded as a sentinel pointer with the lowest bit set
//                 (since valid pointers are always aligned, their lowest bit is 0).
//                 Representation: single `ptr` instead of `{ ptr, i1 }`.
//
//   IntegerNiche: For small integer success types (i8..i32, u8..u32).
//                 The error state is encoded by setting the high bit of a wider integer.
//                 E.g., i32 success → i64 with high bit as error flag.
//                 Representation: single `i64` instead of `{ i32, i1 }`.
//
//   StructFallback: For aggregate or other types that don't fit either niche.
//                   Uses the traditional { T, i1 } representation.
//
// This pass writes `niched_error = true` and `niched_error_kind` to NodeMeta
// for each ErrorType usage. The IRGenerator reads this when emitting error
// check code and type representations.
class NichedErrorPass : public PreLLVMPass {
public:
    std::string name() const override { return "NichedErrorOptimization"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::TetherSpecific; }

    int pointerNiches() const { return pointer_niches_; }
    int integerNiches() const { return integer_niches_; }
    int structFallbacks() const { return struct_fallbacks_; }

private:
    int pointer_niches_ = 0;
    int integer_niches_ = 0;
    int struct_fallbacks_ = 0;

    // Classify the niche kind for an ErrorType's success type
    NodeMeta::NichedErrorKind classifyNiche(TypeId success_type) const;

    // Walk the AST to find all ErrorType usages
    void walkExpr(Expr* expr);
    void walkStmt(Stmt* stmt);
    void walkBlock(BlockStmt* block);
    void walkFn(FnDecl& fn);
    void annotateErrorType(TypeId error_type);
};

} // namespace tether
