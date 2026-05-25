#pragma once

#include "ast/AST.h"

#include <type_traits>

namespace tether {

// ============================================================================
// RecursiveASTVisitor - CRTP-based visitor template with default traversal
//
// Usage:
//   class MyVisitor : public RecursiveASTVisitor<MyVisitor, MyReturnType> {
//       MyReturnType visitIntLiteral(IntLiteral& node) { ... }
//       ...
//   };
//
// The Derived class can override any visitXxx method. The default
// implementations traverse children recursively and return a
// default-constructed Ret.
//
// For void return type, the default traversal simply visits all children.
// For non-void return types, the default traversal returns Ret{}.
// ============================================================================

template<typename Derived, typename Ret = void>
class RecursiveASTVisitor {
public:
    // -----------------------------------------------------------------------
    // Top-level dispatch entry point
    // -----------------------------------------------------------------------
    Ret visit(ASTNode& node) {
        switch (node.getKind()) {
            // Expressions
            case NodeKind::IntLiteral:
                return derived().visitIntLiteral(static_cast<IntLiteral&>(node));
            case NodeKind::FloatLiteral:
                return derived().visitFloatLiteral(static_cast<FloatLiteral&>(node));
            case NodeKind::BoolLiteral:
                return derived().visitBoolLiteral(static_cast<BoolLiteral&>(node));
            case NodeKind::StringLiteral:
                return derived().visitStringLiteral(static_cast<StringLiteral&>(node));
            case NodeKind::IdentExpr:
                return derived().visitIdentExpr(static_cast<IdentExpr&>(node));
            case NodeKind::BinaryExpr:
                return derived().visitBinaryExpr(static_cast<BinaryExpr&>(node));
            case NodeKind::UnaryExpr:
                return derived().visitUnaryExpr(static_cast<UnaryExpr&>(node));
            case NodeKind::CallExpr:
                return derived().visitCallExpr(static_cast<CallExpr&>(node));
            case NodeKind::MemberExpr:
                return derived().visitMemberExpr(static_cast<MemberExpr&>(node));
            case NodeKind::IndexExpr:
                return derived().visitIndexExpr(static_cast<IndexExpr&>(node));
            case NodeKind::DerefExpr:
                return derived().visitDerefExpr(static_cast<DerefExpr&>(node));
            case NodeKind::AddrOfExpr:
                return derived().visitAddrOfExpr(static_cast<AddrOfExpr&>(node));
            case NodeKind::CastExpr:
                return derived().visitCastExpr(static_cast<CastExpr&>(node));
            case NodeKind::SelectExpr:
                return derived().visitSelectExpr(static_cast<SelectExpr&>(node));
            case NodeKind::StructInitExpr:
                return derived().visitStructInitExpr(static_cast<StructInitExpr&>(node));
            case NodeKind::ArrayInitExpr:
                return derived().visitArrayInitExpr(static_cast<ArrayInitExpr&>(node));
            case NodeKind::SizeofExpr:
                return derived().visitSizeofExpr(static_cast<SizeofExpr&>(node));
            case NodeKind::UnsafeExpr:
                return derived().visitUnsafeExpr(static_cast<UnsafeExpr&>(node));
            case NodeKind::PoisonExpr:
                return derived().visitPoisonExpr(static_cast<PoisonExpr&>(node));

            // Statements
            case NodeKind::VarDeclStmt:
                return derived().visitVarDeclStmt(static_cast<VarDeclStmt&>(node));
            case NodeKind::ValDeclStmt:
                return derived().visitValDeclStmt(static_cast<ValDeclStmt&>(node));
            case NodeKind::AssignStmt:
                return derived().visitAssignStmt(static_cast<AssignStmt&>(node));
            case NodeKind::DeferStmt:
                return derived().visitDeferStmt(static_cast<DeferStmt&>(node));
            case NodeKind::IfStmt:
                return derived().visitIfStmt(static_cast<IfStmt&>(node));
            case NodeKind::WhileStmt:
                return derived().visitWhileStmt(static_cast<WhileStmt&>(node));
            case NodeKind::ReturnStmt:
                return derived().visitReturnStmt(static_cast<ReturnStmt&>(node));
            case NodeKind::BreakStmt:
                return derived().visitBreakStmt(static_cast<BreakStmt&>(node));
            case NodeKind::ContinueStmt:
                return derived().visitContinueStmt(static_cast<ContinueStmt&>(node));
            case NodeKind::ExprStmt:
                return derived().visitExprStmt(static_cast<ExprStmt&>(node));
            case NodeKind::BlockStmt:
                return derived().visitBlockStmt(static_cast<BlockStmt&>(node));

            // Top-level declarations
            case NodeKind::FnDecl:
                return derived().visitFnDecl(static_cast<FnDecl&>(node));
            case NodeKind::StructDecl:
                return derived().visitStructDecl(static_cast<StructDecl&>(node));
            case NodeKind::EnumDecl:
                return derived().visitEnumDecl(static_cast<EnumDecl&>(node));
            case NodeKind::ImportDecl:
                return derived().visitImportDecl(static_cast<ImportDecl&>(node));
        }
        return retDefault();
    }

    // Const overload
    Ret visit(const ASTNode& node) {
        switch (node.getKind()) {
            case NodeKind::IntLiteral:
                return derived().visitIntLiteral(static_cast<const IntLiteral&>(node));
            case NodeKind::FloatLiteral:
                return derived().visitFloatLiteral(static_cast<const FloatLiteral&>(node));
            case NodeKind::BoolLiteral:
                return derived().visitBoolLiteral(static_cast<const BoolLiteral&>(node));
            case NodeKind::StringLiteral:
                return derived().visitStringLiteral(static_cast<const StringLiteral&>(node));
            case NodeKind::IdentExpr:
                return derived().visitIdentExpr(static_cast<const IdentExpr&>(node));
            case NodeKind::BinaryExpr:
                return derived().visitBinaryExpr(static_cast<const BinaryExpr&>(node));
            case NodeKind::UnaryExpr:
                return derived().visitUnaryExpr(static_cast<const UnaryExpr&>(node));
            case NodeKind::CallExpr:
                return derived().visitCallExpr(static_cast<const CallExpr&>(node));
            case NodeKind::MemberExpr:
                return derived().visitMemberExpr(static_cast<const MemberExpr&>(node));
            case NodeKind::IndexExpr:
                return derived().visitIndexExpr(static_cast<const IndexExpr&>(node));
            case NodeKind::DerefExpr:
                return derived().visitDerefExpr(static_cast<const DerefExpr&>(node));
            case NodeKind::AddrOfExpr:
                return derived().visitAddrOfExpr(static_cast<const AddrOfExpr&>(node));
            case NodeKind::CastExpr:
                return derived().visitCastExpr(static_cast<const CastExpr&>(node));
            case NodeKind::SelectExpr:
                return derived().visitSelectExpr(static_cast<const SelectExpr&>(node));
            case NodeKind::StructInitExpr:
                return derived().visitStructInitExpr(static_cast<const StructInitExpr&>(node));
            case NodeKind::ArrayInitExpr:
                return derived().visitArrayInitExpr(static_cast<const ArrayInitExpr&>(node));
            case NodeKind::SizeofExpr:
                return derived().visitSizeofExpr(static_cast<const SizeofExpr&>(node));
            case NodeKind::UnsafeExpr:
                return derived().visitUnsafeExpr(static_cast<const UnsafeExpr&>(node));
            case NodeKind::PoisonExpr:
                return derived().visitPoisonExpr(static_cast<const PoisonExpr&>(node));
            case NodeKind::VarDeclStmt:
                return derived().visitVarDeclStmt(static_cast<const VarDeclStmt&>(node));
            case NodeKind::ValDeclStmt:
                return derived().visitValDeclStmt(static_cast<const ValDeclStmt&>(node));
            case NodeKind::AssignStmt:
                return derived().visitAssignStmt(static_cast<const AssignStmt&>(node));
            case NodeKind::DeferStmt:
                return derived().visitDeferStmt(static_cast<const DeferStmt&>(node));
            case NodeKind::IfStmt:
                return derived().visitIfStmt(static_cast<const IfStmt&>(node));
            case NodeKind::WhileStmt:
                return derived().visitWhileStmt(static_cast<const WhileStmt&>(node));
            case NodeKind::ReturnStmt:
                return derived().visitReturnStmt(static_cast<const ReturnStmt&>(node));
            case NodeKind::BreakStmt:
                return derived().visitBreakStmt(static_cast<const BreakStmt&>(node));
            case NodeKind::ContinueStmt:
                return derived().visitContinueStmt(static_cast<const ContinueStmt&>(node));
            case NodeKind::ExprStmt:
                return derived().visitExprStmt(static_cast<const ExprStmt&>(node));
            case NodeKind::BlockStmt:
                return derived().visitBlockStmt(static_cast<const BlockStmt&>(node));
            case NodeKind::FnDecl:
                return derived().visitFnDecl(static_cast<const FnDecl&>(node));
            case NodeKind::StructDecl:
                return derived().visitStructDecl(static_cast<const StructDecl&>(node));
            case NodeKind::EnumDecl:
                return derived().visitEnumDecl(static_cast<const EnumDecl&>(node));
            case NodeKind::ImportDecl:
                return derived().visitImportDecl(static_cast<const ImportDecl&>(node));
        }
        return retDefault();
    }

    // -----------------------------------------------------------------------
    // Default visit methods with child traversal
    // Override these in the Derived class to customize behavior.
    // By default, they traverse all children recursively.
    // -----------------------------------------------------------------------

    // --- Expressions (leaf nodes: no children to traverse) ---

    Ret visitIntLiteral(IntLiteral&) { return retDefault(); }
    Ret visitFloatLiteral(FloatLiteral&) { return retDefault(); }
    Ret visitBoolLiteral(BoolLiteral&) { return retDefault(); }
    Ret visitStringLiteral(StringLiteral&) { return retDefault(); }
    Ret visitIdentExpr(IdentExpr&) { return retDefault(); }

    // --- Expressions (composite nodes: traverse children) ---

    Ret visitBinaryExpr(BinaryExpr& node) {
        traverseExpr(node.left());
        traverseExpr(node.right());
        return retDefault();
    }

    Ret visitUnaryExpr(UnaryExpr& node) {
        traverseExpr(node.operand());
        return retDefault();
    }

    Ret visitCallExpr(CallExpr& node) {
        traverseExpr(node.callee());
        for (auto& arg : node.args()) {
            traverseExpr(arg.get());
        }
        return retDefault();
    }

    Ret visitMemberExpr(MemberExpr& node) {
        traverseExpr(node.object());
        return retDefault();
    }

    Ret visitIndexExpr(IndexExpr& node) {
        traverseExpr(node.object());
        traverseExpr(node.index());
        return retDefault();
    }

    Ret visitDerefExpr(DerefExpr& node) {
        traverseExpr(node.operand());
        return retDefault();
    }

    Ret visitAddrOfExpr(AddrOfExpr& node) {
        traverseExpr(node.operand());
        return retDefault();
    }

    Ret visitCastExpr(CastExpr& node) {
        traverseExpr(node.expr());
        return retDefault();
    }

    Ret visitSelectExpr(SelectExpr& node) {
        traverseExpr(node.condition());
        traverseExpr(node.trueExpr());
        traverseExpr(node.falseExpr());
        return retDefault();
    }

    Ret visitStructInitExpr(StructInitExpr& node) {
        for (auto& init : node.inits()) {
            traverseExpr(init.value.get());
        }
        return retDefault();
    }

    Ret visitArrayInitExpr(ArrayInitExpr& node) {
        for (auto& elem : node.elements()) {
            traverseExpr(elem.get());
        }
        return retDefault();
    }

    Ret visitSizeofExpr(SizeofExpr& node) {
        if (node.isExprOperand()) {
            traverseExpr(node.expr());
        }
        return retDefault();
    }

    Ret visitUnsafeExpr(UnsafeExpr& node) {
        traverseExpr(node.inner());
        return retDefault();
    }

    Ret visitPoisonExpr(PoisonExpr&) { return retDefault(); }

    // --- Statements ---

    Ret visitVarDeclStmt(VarDeclStmt& node) {
        if (node.hasInit()) {
            traverseExpr(node.init());
        }
        return retDefault();
    }

    Ret visitValDeclStmt(ValDeclStmt& node) {
        if (node.hasInit()) {
            traverseExpr(node.init());
        }
        return retDefault();
    }

    Ret visitAssignStmt(AssignStmt& node) {
        traverseExpr(node.target());
        traverseExpr(node.value());
        return retDefault();
    }

    Ret visitDeferStmt(DeferStmt& node) {
        traverseStmt(node.stmt());
        return retDefault();
    }

    Ret visitIfStmt(IfStmt& node) {
        traverseExpr(node.condition());
        traverseStmt(node.thenBlock());
        if (node.hasElse()) {
            traverseStmt(node.elseBlock());
        }
        return retDefault();
    }

    Ret visitWhileStmt(WhileStmt& node) {
        traverseExpr(node.condition());
        traverseStmt(node.body());
        if (node.hasIncrement()) {
            traverseExpr(node.increment());
        }
        return retDefault();
    }

    Ret visitReturnStmt(ReturnStmt& node) {
        if (node.hasValue()) {
            traverseExpr(node.value());
        }
        return retDefault();
    }

    Ret visitBreakStmt(BreakStmt&) { return retDefault(); }
    Ret visitContinueStmt(ContinueStmt&) { return retDefault(); }

    Ret visitExprStmt(ExprStmt& node) {
        traverseExpr(node.expr());
        return retDefault();
    }

    Ret visitBlockStmt(BlockStmt& node) {
        for (auto& stmt : node.stmts()) {
            traverseStmt(stmt.get());
        }
        return retDefault();
    }

    // --- Top-level declarations ---

    Ret visitFnDecl(FnDecl& node) {
        if (node.body()) {
            traverseStmt(node.body());
        }
        return retDefault();
    }

    Ret visitStructDecl(StructDecl&) { return retDefault(); }
    Ret visitEnumDecl(EnumDecl&) { return retDefault(); }
    Ret visitImportDecl(ImportDecl&) { return retDefault(); }

    // -----------------------------------------------------------------------
    // Const visit overloads (traverse const AST)
    // -----------------------------------------------------------------------

    Ret visitIntLiteral(const IntLiteral&) { return retDefault(); }
    Ret visitFloatLiteral(const FloatLiteral&) { return retDefault(); }
    Ret visitBoolLiteral(const BoolLiteral&) { return retDefault(); }
    Ret visitStringLiteral(const StringLiteral&) { return retDefault(); }
    Ret visitIdentExpr(const IdentExpr&) { return retDefault(); }

    Ret visitBinaryExpr(const BinaryExpr&) { return retDefault(); }
    Ret visitUnaryExpr(const UnaryExpr&) { return retDefault(); }
    Ret visitCallExpr(const CallExpr&) { return retDefault(); }
    Ret visitMemberExpr(const MemberExpr&) { return retDefault(); }
    Ret visitIndexExpr(const IndexExpr&) { return retDefault(); }
    Ret visitDerefExpr(const DerefExpr&) { return retDefault(); }
    Ret visitAddrOfExpr(const AddrOfExpr&) { return retDefault(); }
    Ret visitCastExpr(const CastExpr&) { return retDefault(); }
    Ret visitSelectExpr(const SelectExpr&) { return retDefault(); }
    Ret visitStructInitExpr(const StructInitExpr&) { return retDefault(); }
    Ret visitArrayInitExpr(const ArrayInitExpr&) { return retDefault(); }
    Ret visitSizeofExpr(const SizeofExpr&) { return retDefault(); }
    Ret visitUnsafeExpr(const UnsafeExpr&) { return retDefault(); }
    Ret visitPoisonExpr(const PoisonExpr&) { return retDefault(); }

    Ret visitVarDeclStmt(const VarDeclStmt&) { return retDefault(); }
    Ret visitValDeclStmt(const ValDeclStmt&) { return retDefault(); }
    Ret visitAssignStmt(const AssignStmt&) { return retDefault(); }
    Ret visitDeferStmt(const DeferStmt&) { return retDefault(); }
    Ret visitIfStmt(const IfStmt&) { return retDefault(); }
    Ret visitWhileStmt(const WhileStmt&) { return retDefault(); }
    Ret visitReturnStmt(const ReturnStmt&) { return retDefault(); }
    Ret visitBreakStmt(const BreakStmt&) { return retDefault(); }
    Ret visitContinueStmt(const ContinueStmt&) { return retDefault(); }
    Ret visitExprStmt(const ExprStmt&) { return retDefault(); }
    Ret visitBlockStmt(const BlockStmt&) { return retDefault(); }

    Ret visitFnDecl(const FnDecl&) { return retDefault(); }
    Ret visitStructDecl(const StructDecl&) { return retDefault(); }
    Ret visitEnumDecl(const EnumDecl&) { return retDefault(); }
    Ret visitImportDecl(const ImportDecl&) { return retDefault(); }

protected:
    // -----------------------------------------------------------------------
    // Helper methods for traversing children
    // -----------------------------------------------------------------------

    // Traverse an expression node (dispatches through the visitor)
    Ret traverseExpr(Expr* expr) {
        if (expr) {
            return derived().visit(*expr);
        }
        return retDefault();
    }

    Ret traverseExpr(const Expr* expr) {
        if (expr) {
            return derived().visit(*expr);
        }
        return retDefault();
    }

    // Traverse a statement node (dispatches through the visitor)
    Ret traverseStmt(Stmt* stmt) {
        if (stmt) {
            return derived().visit(*stmt);
        }
        return retDefault();
    }

    Ret traverseStmt(const Stmt* stmt) {
        if (stmt) {
            return derived().visit(*stmt);
        }
        return retDefault();
    }

    // Traverse a top-level node (dispatches through the visitor)
    Ret traverseTopLevel(TopLevel* tl) {
        if (tl) {
            return derived().visit(*tl);
        }
        return retDefault();
    }

    Ret traverseTopLevel(const TopLevel* tl) {
        if (tl) {
            return derived().visit(*tl);
        }
        return retDefault();
    }

private:
    Derived& derived() {
        return *static_cast<Derived*>(this);
    }

    const Derived& derived() const {
        return *static_cast<const Derived*>(this);
    }

    // Return a default-constructed Ret. Handles void correctly.
    static Ret retDefault() {
        if constexpr (std::is_same_v<Ret, void>) {
            return;
        } else {
            return Ret{};
        }
    }
};

// ============================================================================
// TraversalOnlyVisitor - Simple void visitor that just traverses the AST
// Useful as a base class when you only need to inspect nodes without
// returning values.
// ============================================================================
class TraversalOnlyVisitor : public RecursiveASTVisitor<TraversalOnlyVisitor, void> {
public:
    // Override specific visitXxx methods as needed
};

} // namespace tether
