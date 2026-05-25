#pragma once

#include "ast/AST.h"
#include "lexer/Token.h"
#include "sema/Type.h"

#include <string>
#include <vector>
#include <memory>
#include <deque>
#include <unordered_map>
#include <unordered_set>

namespace tether {

// ============================================================================
// Parse Error
// ============================================================================
struct ParseError {
    SourceLocation loc;
    std::string message;
};

// ============================================================================
// Program = list of top-level declarations
// ============================================================================
using Program = std::vector<std::unique_ptr<TopLevel>>;

// ============================================================================
// Parser - recursive descent parser producing AST nodes
// ============================================================================
class Parser {
public:
    Parser(std::vector<Token> tokens, TypeTable& type_table);

    /// Parse the entire token stream and return the program.
    Program parse();

    /// Whether any errors were encountered.
    bool hasErrors() const { return !errors_.empty(); }

    /// Return all parse errors.
    const std::vector<ParseError>& errors() const { return errors_; }

    /// Map from AST node to the original type-name text for unresolved types.
    /// Semantic analysis uses this to resolve types that the parser could not
    /// (e.g. user-defined struct/enum names).
    const std::unordered_map<const ASTNode*, std::string>& typeAnnotations() const {
        return type_annotations_;
    }

    const std::unordered_map<std::string, std::string>& paramTypeAnnotations() const {
        return param_type_annotations_;
    }

    /// Set of block statements that were introduced by the `unsafe { }` syntax.
    /// The AST has no intrinsic "unsafe block" marker, so we record them here.
    const std::unordered_set<const BlockStmt*>& unsafeBlocks() const {
        return unsafe_blocks_;
    }

private:
    // -----------------------------------------------------------------------
    // Token management
    // -----------------------------------------------------------------------
    Token peek() const;
    Token peekNext() const;
    Token advance();
    Token previous() const;
    bool check(TokenKind kind) const;
    bool match(TokenKind kind);
    Token consume(TokenKind kind, const std::string& message);
    bool isAtEnd() const;

    /// Consume a '>' token in type-parsing context, handling >> splitting.
    Token consumeGT();

    /// Synchronise after an error: skip tokens until a likely recovery point.
    void synchronize();

    // -----------------------------------------------------------------------
    // Error reporting
    // -----------------------------------------------------------------------
    void error(const std::string& message);
    void errorAt(const Token& token, const std::string& message);

    // -----------------------------------------------------------------------
    // Source-location helpers
    // -----------------------------------------------------------------------
    SourceLocation loc() const;
    SourceLocation locFrom(const Token& token) const;

    // -----------------------------------------------------------------------
    // Top-level parsing
    // -----------------------------------------------------------------------
    std::unique_ptr<TopLevel> parseTopLevel();
    std::vector<CompilerDirective> parseDirectives();
    std::unique_ptr<FnDecl> parseFnDecl(std::vector<CompilerDirective> directives);
    std::unique_ptr<StructDecl> parseStructDecl();
    std::unique_ptr<EnumDecl> parseEnumDecl();
    std::unique_ptr<ImportDecl> parseImportDecl();

    // -----------------------------------------------------------------------
    // Statement parsing
    // -----------------------------------------------------------------------
    std::unique_ptr<Stmt> parseStmt();
    std::unique_ptr<ValDeclStmt> parseValDecl();
    std::unique_ptr<VarDeclStmt> parseVarDecl();
    std::unique_ptr<IfStmt> parseIfStmt();
    std::unique_ptr<WhileStmt> parseWhileStmt();
    std::unique_ptr<Stmt> parseDeferStmt();
    std::unique_ptr<ReturnStmt> parseReturnStmt();
    std::unique_ptr<BreakStmt> parseBreakStmt();
    std::unique_ptr<ContinueStmt> parseContinueStmt();
    std::unique_ptr<BlockStmt> parseBlockStmt();

    // -----------------------------------------------------------------------
    // Expression parsing (recursive descent with precedence levels)
    // -----------------------------------------------------------------------
    std::unique_ptr<Expr> parseExpr();
    std::unique_ptr<Expr> parseOrExpr();
    std::unique_ptr<Expr> parseAndExpr();
    std::unique_ptr<Expr> parseBitOrExpr();
    std::unique_ptr<Expr> parseBitXorExpr();
    std::unique_ptr<Expr> parseBitAndExpr();
    std::unique_ptr<Expr> parseEqualityExpr();
    std::unique_ptr<Expr> parseComparisonExpr();
    std::unique_ptr<Expr> parseShiftExpr();
    std::unique_ptr<Expr> parseAdditiveExpr();
    std::unique_ptr<Expr> parseMultiplicativeExpr();
    std::unique_ptr<Expr> parseUnaryExpr();
    std::unique_ptr<Expr> parsePostfixExpr(std::unique_ptr<Expr> lhs);
    std::unique_ptr<Expr> parseCastExpr();
    std::unique_ptr<Expr> parsePrimaryExpr();

    // Postfix helpers
    std::unique_ptr<Expr> parseCallExpr(std::unique_ptr<Expr> callee);
    std::unique_ptr<Expr> parseMemberExpr(std::unique_ptr<Expr> object);
    std::unique_ptr<Expr> parseIndexExpr(std::unique_ptr<Expr> object);
    std::unique_ptr<Expr> parseStructInitExpr(const std::string& type_name);
    std::unique_ptr<Expr> parseSelectExpr();
    std::unique_ptr<Expr> parseUnsafeExpr();
    std::unique_ptr<Expr> parseSizeofExpr();

    // -----------------------------------------------------------------------
    // Type parsing
    // -----------------------------------------------------------------------
    TypeId parseType();
    TypeId resolveNamedType(const std::string& name);

    // -----------------------------------------------------------------------
    // Helpers
    // -----------------------------------------------------------------------
    std::vector<FnParam> parseFnParams();
    std::vector<DesignatedInit> parseDesignatedInits();
    std::vector<std::unique_ptr<Expr>> parseCallArgs();

    /// Record a type annotation for later resolution.
    void recordTypeAnnotation(const ASTNode* node, const std::string& type_text);

    /// Map a token's BinaryOp (compound-assign or otherwise) to a BinaryOp enum.
    BinaryOp tokenToBinaryOp(TokenKind kind) const;
    BinaryOp tokenToCompoundAssignOp(TokenKind kind) const;

    // -----------------------------------------------------------------------
    // Data members
    // -----------------------------------------------------------------------
    std::vector<Token> tokens_;
    size_t pos_;
    TypeTable& type_table_;
    std::vector<ParseError> errors_;
    std::deque<Token> pending_tokens_;   // for >> splitting in type context

    /// Side-channel: maps AST nodes to their unresolved type-name text.
    std::unordered_map<const ASTNode*, std::string> type_annotations_;

    /// Side-channel: records BlockStmts that are `unsafe { ... }`.
    std::unordered_set<const BlockStmt*> unsafe_blocks_;

    /// Side-channel: maps (function_name + ":" + param_index) to unresolved type name text.
    /// Used by semantic analysis to resolve parameter types after struct/enum registration.
    std::unordered_map<std::string, std::string> param_type_annotations_;
};

} // namespace tether
