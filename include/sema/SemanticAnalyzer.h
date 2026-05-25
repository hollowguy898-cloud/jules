#pragma once

#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "sema/SymbolTable.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <functional>

namespace tether {

// ============================================================================
// Diagnostic - represents a semantic error or warning
// ============================================================================
enum class DiagnosticKind : uint8_t {
    Error,
    Warning,
    Note
};

struct Diagnostic {
    DiagnosticKind kind;
    SourceLocation loc;
    std::string message;

    Diagnostic(DiagnosticKind k, SourceLocation l, std::string msg)
        : kind(k), loc(std::move(l)), message(std::move(msg)) {}

    std::string toString() const {
        std::string prefix;
        switch (kind) {
            case DiagnosticKind::Error:   prefix = "error"; break;
            case DiagnosticKind::Warning: prefix = "warning"; break;
            case DiagnosticKind::Note:    prefix = "note"; break;
        }
        return loc.toString() + ": " + prefix + ": " + message;
    }

    bool isError() const { return kind == DiagnosticKind::Error; }
    bool isWarning() const { return kind == DiagnosticKind::Warning; }
};

// ============================================================================
// SemanticAnalyzer - walks the AST after parsing and performs:
//   1. Type resolution (resolves parser's unresolved type names)
//   2. Symbol binding (resolves identifier references to symbols)
//   3. Type checking (verifies operations have compatible types)
//   4. Pure function validation
//   5. Error-type propagation validation
//   6. Produces a fully typed AST (all Expr nodes have TypeId set)
// ============================================================================
class SemanticAnalyzer {
public:
    explicit SemanticAnalyzer(TypeTable& type_table);

    // -----------------------------------------------------------------------
    // Main entry point: analyze an entire program (list of top-level decls)
    // -----------------------------------------------------------------------
    void analyze(Program& program,
                 const std::unordered_map<const ASTNode*, std::string>& type_annotations,
                 const std::unordered_map<std::string, std::string>& param_type_annotations);

    // -----------------------------------------------------------------------
    // Query results after analysis
    // -----------------------------------------------------------------------
    bool hasErrors() const;
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // -----------------------------------------------------------------------
    // Symbol table access (for downstream passes like borrow checking)
    // -----------------------------------------------------------------------
    SymbolTable& symbolTable() { return symtab_; }
    const SymbolTable& symbolTable() const { return symtab_; }

    // -----------------------------------------------------------------------
    // Type table access
    // -----------------------------------------------------------------------
    TypeTable& typeTable() { return type_table_; }
    const TypeTable& typeTable() const { return type_table_; }

    // -----------------------------------------------------------------------
    // Lookup whether a function is pure (by name, for cross-function checks)
    // -----------------------------------------------------------------------
    bool isFunctionPure(const std::string& fn_name) const;

    // -----------------------------------------------------------------------
    // Lookup a function's FnType by name
    // -----------------------------------------------------------------------
    TypeId lookupFnType(const std::string& fn_name) const;

    // -----------------------------------------------------------------------
    // Get the current function being analyzed (nullptr if at top level)
    // -----------------------------------------------------------------------
    FnDecl* currentFn() { return current_fn_; }
    const FnDecl* currentFn() const { return current_fn_; }

private:
    // -----------------------------------------------------------------------
    // Diagnostic helpers
    // -----------------------------------------------------------------------
    void emitError(const SourceLocation& loc, const std::string& msg);
    void emitWarning(const SourceLocation& loc, const std::string& msg);
    void emitNote(const SourceLocation& loc, const std::string& msg);

    // -----------------------------------------------------------------------
    // Type resolution
    // -----------------------------------------------------------------------
    // Resolve unresolved type annotations left by the parser
    void resolveTypeAnnotations(
        const std::unordered_map<const ASTNode*, std::string>& type_annotations);

    // Resolve a type name string to a TypeId (looks up struct/enum in symtab)
    TypeId resolveTypeName(const std::string& name, const SourceLocation& loc);

    // -----------------------------------------------------------------------
    // Top-level declaration analysis
    // -----------------------------------------------------------------------
    void analyzeFnDecl(FnDecl& fn);
    void analyzeStructDecl(StructDecl& sd);
    void analyzeEnumDecl(EnumDecl& ed);
    void analyzeImportDecl(ImportDecl& id);

    // -----------------------------------------------------------------------
    // First pass: register all top-level declarations in the symbol table
    // (so that forward references work)
    // -----------------------------------------------------------------------
    void registerTopLevelDecls(Program& program);

    // -----------------------------------------------------------------------
    // Statement analysis
    // -----------------------------------------------------------------------
    void analyzeStmt(Stmt& stmt);
    void analyzeBlockStmt(BlockStmt& block);
    void analyzeVarDeclStmt(VarDeclStmt& vd);
    void analyzeValDeclStmt(ValDeclStmt& vd);
    void analyzeAssignStmt(AssignStmt& as);
    void analyzeDeferStmt(DeferStmt& ds);
    void analyzeIfStmt(IfStmt& is);
    void analyzeWhileStmt(WhileStmt& ws);
    void analyzeReturnStmt(ReturnStmt& rs);
    void analyzeBreakStmt(BreakStmt& bs);
    void analyzeContinueStmt(ContinueStmt& cs);
    void analyzeExprStmt(ExprStmt& es);

    // -----------------------------------------------------------------------
    // Expression analysis — returns the TypeId of the expression
    // and sets it on the Expr node via setType()
    // -----------------------------------------------------------------------
    TypeId analyzeExpr(Expr& expr);
    TypeId analyzeIntLiteral(IntLiteral& lit);
    TypeId analyzeFloatLiteral(FloatLiteral& lit);
    TypeId analyzeBoolLiteral(BoolLiteral& lit);
    TypeId analyzeStringLiteral(StringLiteral& lit);
    TypeId analyzeIdentExpr(IdentExpr& ie);
    TypeId analyzeBinaryExpr(BinaryExpr& be);
    TypeId analyzeUnaryExpr(UnaryExpr& ue);
    TypeId analyzeCallExpr(CallExpr& ce);
    TypeId analyzeMemberExpr(MemberExpr& me);
    TypeId analyzeIndexExpr(IndexExpr& ie);
    TypeId analyzeDerefExpr(DerefExpr& de);
    TypeId analyzeAddrOfExpr(AddrOfExpr& ae);
    TypeId analyzeCastExpr(CastExpr& ce);
    TypeId analyzeSelectExpr(SelectExpr& se);
    TypeId analyzeStructInitExpr(StructInitExpr& sie);
    TypeId analyzeArrayInitExpr(ArrayInitExpr& aie);
    TypeId analyzeSizeofExpr(SizeofExpr& se);
    TypeId analyzeUnsafeExpr(UnsafeExpr& ue);
    TypeId analyzePoisonExpr(PoisonExpr& pe);

    // -----------------------------------------------------------------------
    // Type checking helpers
    // -----------------------------------------------------------------------

    // Check if two types are compatible for assignment / binary operation
    bool typesCompatible(TypeId lhs, TypeId rhs) const;

    // Check if two types are exactly equal (structural equality)
    bool typesEqual(TypeId a, TypeId b) const;

    // Get the common type for arithmetic binary operations (coercion)
    TypeId commonType(TypeId lhs, TypeId rhs) const;

    // Check if a binary operator is an arithmetic operator
    bool isArithmeticOp(BinaryOp op) const;

    // Check if a binary operator is a comparison operator
    bool isComparisonOp(BinaryOp op) const;

    // Check if a binary operator is a logical operator
    bool isLogicalOp(BinaryOp op) const;

    // Check if a binary operator is a bitwise operator
    bool isBitwiseOp(BinaryOp op) const;

    // Check if a binary operator is a shift operator
    bool isShiftOp(BinaryOp op) const;

    // Check if a binary operator is an assignment operator
    bool isAssignmentOp(BinaryOp op) const;

    // Check if a type is assignable (not void, not error-typed without unwrap)
    bool isAssignableType(TypeId t) const;

    // Unwrap an ErrorType to get the success type; returns null TypeId if not an error type
    TypeId unwrapErrorType(TypeId t) const;

    // Check if a type is or wraps an error type
    bool isErrorType(TypeId t) const;

    // Check if a TypeId points to a PoisonType (error-resilient compilation)
    bool isPoisonType(TypeId tid) const {
        return tid && isa<PoisonType>(*tid.raw());
    }

    // -----------------------------------------------------------------------
    // Pure function validation
    // -----------------------------------------------------------------------
    void checkPureFunctionCall(const SourceLocation& loc, const std::string& fn_name);
    void checkPureMutation(const SourceLocation& loc, const std::string& var_name);

    // -----------------------------------------------------------------------
    // Error propagation validation
    // -----------------------------------------------------------------------
    // The `!` operator (UnaryOp::Not on an error-returning expression) is
    // the error-propagation operator. It can only be applied to expressions
    // whose type is ErrorType.
    void checkErrorPropagation(UnaryExpr& ue, TypeId operand_type);

    // -----------------------------------------------------------------------
    // L-value detection (can this expression be assigned to?)
    // -----------------------------------------------------------------------
    bool isLValue(Expr& expr) const;

    // Get the variable name from an lvalue expression (for borrow checking)
    std::optional<std::string> getLValueName(Expr& expr) const;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    TypeTable& type_table_;
    SymbolTable symtab_;
    std::vector<Diagnostic> diagnostics_;

    // Current function being analyzed
    FnDecl* current_fn_ = nullptr;

    // Whether we're inside a pure function
    bool in_pure_fn_ = false;

    // Unresolved type annotations from the parser
    std::unordered_map<const ASTNode*, std::string> type_annotations_;

    // Unresolved parameter type annotations: key = "fn_name:param_index"
    std::unordered_map<std::string, std::string> param_type_annotations_;

    // Set of function names that are declared pure
    std::unordered_set<std::string> pure_functions_;

    // Map from function name to its FnType TypeId
    std::unordered_map<std::string, TypeId> function_types_;

    // Counter for error-type propagation depth (to allow `!!expr`)
    int error_prop_depth_ = 0;

    // Set of nodes whose types have been resolved (to avoid re-resolution)
    std::unordered_set<const ASTNode*> resolved_nodes_;
};

} // namespace tether
