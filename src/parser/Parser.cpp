#include "parser/Parser.h"

#include <cstdlib>
#include <cstring>
#include <sstream>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
Parser::Parser(std::vector<Token> tokens, TypeTable& type_table)
    : tokens_(std::move(tokens))
    , pos_(0)
    , type_table_(type_table)
{}

// ============================================================================
// Main entry point
// ============================================================================
Program Parser::parse() {
    Program program;
    while (!isAtEnd()) {
        auto decl = parseTopLevel();
        if (decl) {
            program.push_back(std::move(decl));
        } else {
            // On error, skip to next likely top-level start
            synchronize();
        }
    }
    return program;
}

// ============================================================================
// Token management
// ============================================================================
Token Parser::peek() const {
    if (!pending_tokens_.empty()) {
        return pending_tokens_.front();
    }
    if (pos_ < tokens_.size()) {
        return tokens_[pos_];
    }
    return tokens_.back(); // EOF
}

Token Parser::peekNext() const {
    // Look past one token (including any pending tokens)
    if (!pending_tokens_.empty()) {
        if (pending_tokens_.size() > 1) {
            return pending_tokens_[1];
        }
        // past the pending token
        size_t idx = pos_;
        if (idx < tokens_.size()) idx++;
        if (idx < tokens_.size()) return tokens_[idx];
        return tokens_.back();
    }
    if (pos_ + 1 < tokens_.size()) {
        return tokens_[pos_ + 1];
    }
    return tokens_.back();
}

Token Parser::advance() {
    if (!pending_tokens_.empty()) {
        Token t = pending_tokens_.front();
        pending_tokens_.pop_front();
        return t;
    }
    if (pos_ < tokens_.size()) {
        return tokens_[pos_++];
    }
    return tokens_.back();
}

Token Parser::previous() const {
    if (pos_ > 0) {
        return tokens_[pos_ - 1];
    }
    return tokens_[0];
}

bool Parser::check(TokenKind kind) const {
    return peek().kind() == kind;
}

bool Parser::match(TokenKind kind) {
    if (check(kind)) {
        advance();
        return true;
    }
    return false;
}

Token Parser::consume(TokenKind kind, const std::string& message) {
    if (check(kind)) {
        return advance();
    }
    error(message);
    return peek();
}

Token Parser::consumeGT() {
    // Handle >> splitting in type context
    if (check(TokenKind::GT)) {
        return advance();
    }
    if (check(TokenKind::SHR)) {
        Token shr = advance(); // consume the >> token
        // Split into two > tokens
        Token first_gt(TokenKind::GT, ">",
                       shr.line(), shr.col(), shr.filename());
        Token second_gt(TokenKind::GT, ">",
                        shr.line(), shr.col() + 1, shr.filename());
        pending_tokens_.push_back(second_gt);
        return first_gt;
    }
    if (check(TokenKind::SHR_EQ)) {
        // Split >>= into > and >=
        Token shr_eq = advance();
        Token first_gt(TokenKind::GT, ">",
                       shr_eq.line(), shr_eq.col(), shr_eq.filename());
        Token second_ge(TokenKind::GE, ">=",
                        shr_eq.line(), shr_eq.col() + 1, shr_eq.filename());
        pending_tokens_.push_back(second_ge);
        return first_gt;
    }
    error("expected '>'");
    return peek();
}

bool Parser::isAtEnd() const {
    return peek().kind() == TokenKind::EOF_TOKEN;
}

void Parser::synchronize() {
    while (!isAtEnd()) {
        if (previous().kind() == TokenKind::SEMI) return;
        switch (peek().kind()) {
            case TokenKind::KW_FN:
            case TokenKind::KW_STRUCT:
            case TokenKind::KW_ENUM:
            case TokenKind::KW_IMPORT:
            case TokenKind::KW_VAL:
            case TokenKind::KW_VAR:
            case TokenKind::KW_IF:
            case TokenKind::KW_WHILE:
            case TokenKind::KW_DEFER:
            case TokenKind::KW_RETURN:
            case TokenKind::KW_BREAK:
            case TokenKind::KW_CONTINUE:
            case TokenKind::RBRACE:
                return;
            default:
                break;
        }
        advance();
    }
}

// ============================================================================
// Error reporting
// ============================================================================
void Parser::error(const std::string& message) {
    errorAt(peek(), message);
}

void Parser::errorAt(const Token& token, const std::string& message) {
    std::string msg = token.filename() + ":" +
                      std::to_string(token.line()) + ":" +
                      std::to_string(token.col()) + ": " + message;
    if (token.kind() != TokenKind::EOF_TOKEN) {
        msg += " (got '" + token.text() + "')";
    }
    errors_.push_back({locFrom(token), msg});
}

// ============================================================================
// Source-location helpers
// ============================================================================
SourceLocation Parser::loc() const {
    return locFrom(peek());
}

SourceLocation Parser::locFrom(const Token& token) const {
    return SourceLocation(token.line(), token.col(), token.filename());
}

// ============================================================================
// Top-level parsing
// ============================================================================
std::unique_ptr<TopLevel> Parser::parseTopLevel() {
    auto directives = parseDirectives();

    if (check(TokenKind::KW_FN)) {
        return parseFnDecl(std::move(directives));
    }
    if (check(TokenKind::KW_STRUCT) || check(TokenKind::KW_SOA)) {
        return parseStructDecl();
    }
    if (check(TokenKind::KW_ENUM)) {
        return parseEnumDecl();
    }
    if (check(TokenKind::KW_IMPORT)) {
        return parseImportDecl();
    }

    SourceLocation err_loc = loc();
    error("expected top-level declaration (fn, struct, enum, import)");
    synchronize();
    // Return a PoisonExpr wrapped as a top-level placeholder so the AST
    // remains structurally valid and later phases can continue.
    return nullptr;
}

std::vector<CompilerDirective> Parser::parseDirectives() {
    std::vector<CompilerDirective> directives;
    while (check(TokenKind::AT)) {
        Token at = advance(); // consume @
        if (!check(TokenKind::IDENTIFIER)) {
            error("expected directive name after '@'");
            continue;
        }
        Token name = advance();
        const std::string& text = name.text();
        if (text == "superoptimize") {
            directives.push_back(CompilerDirective::Superoptimize);
        } else if (text == "polly") {
            directives.push_back(CompilerDirective::Polly);
        } else if (text == "simd") {
            directives.push_back(CompilerDirective::Simd);
        } else {
            errorAt(name, "unknown compiler directive '@" + text + "'");
        }
    }
    return directives;
}

std::unique_ptr<FnDecl> Parser::parseFnDecl(
        std::vector<CompilerDirective> directives) {
    SourceLocation fn_loc = loc();
    consume(TokenKind::KW_FN, "expected 'fn'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected function name");
    std::string fn_name = name_tok.text();

    consume(TokenKind::LPAREN, "expected '(' after function name");
    auto params = parseFnParams();
    consume(TokenKind::RPAREN, "expected ')' after function parameters");

    // Rename pending parameter type annotations with the actual function name
    for (size_t i = 0; i < params.size(); i++) {
        std::string pending_key = "__pending__" + std::to_string(i);
        auto it = param_type_annotations_.find(pending_key);
        if (it != param_type_annotations_.end()) {
            std::string final_key = fn_name + ":" + std::to_string(i);
            param_type_annotations_[final_key] = it->second;
            param_type_annotations_.erase(it);
        }
    }

    // Check for error-returning function: ! before return type
    TypeId error_type;
    bool can_error = match(TokenKind::BANG);

    // Parse return type (optional)
    TypeId return_type;
    if (!check(TokenKind::LBRACE) && !check(TokenKind::KW_PURE)) {
        return_type = parseType();
    } else {
        return_type = type_table_.getVoid();
    }

    // If the function can error, wrap return type in ErrorType
    if (can_error) {
        error_type = type_table_.getError(return_type);
    }

    // Check for pure keyword
    bool is_pure = match(TokenKind::KW_PURE);

    // Parse body
    auto body = parseBlockStmt();

    return std::make_unique<FnDecl>(
        std::move(fn_loc), std::move(fn_name),
        std::move(params), return_type,
        std::move(body), is_pure, error_type,
        std::move(directives));
}

std::unique_ptr<StructDecl> Parser::parseStructDecl() {
    SourceLocation struct_loc = loc();
    bool is_soa = match(TokenKind::KW_SOA);
    consume(TokenKind::KW_STRUCT, "expected 'struct'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected struct name");
    std::string struct_name = name_tok.text();

    consume(TokenKind::LBRACE, "expected '{' after struct name");

    std::vector<StructFieldDecl> fields;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        SourceLocation field_loc = loc();
        Token field_name = consume(TokenKind::IDENTIFIER, "expected field name");
        consume(TokenKind::COLON, "expected ':' after field name");
        TypeId field_type = parseType();

        fields.emplace_back(field_name.text(), field_type, std::move(field_loc));

        if (!match(TokenKind::COMMA)) {
            break;
        }
    }

    consume(TokenKind::RBRACE, "expected '}' after struct fields");
    auto decl = std::make_unique<StructDecl>(
        std::move(struct_loc), std::move(struct_name), std::move(fields));
    decl->setSoA(is_soa);
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    SourceLocation enum_loc = loc();
    consume(TokenKind::KW_ENUM, "expected 'enum'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected enum name");
    std::string enum_name = name_tok.text();

    consume(TokenKind::LBRACE, "expected '{' after enum name");

    std::vector<EnumVariantDecl> variants;
    int64_t next_value = 0;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        SourceLocation variant_loc = loc();
        Token variant_name = consume(TokenKind::IDENTIFIER,
                                     "expected variant name");

        std::optional<int64_t> explicit_value;
        if (match(TokenKind::EQ)) {
            // Explicit value
            Token val_tok = consume(TokenKind::INT_LITERAL,
                                    "expected integer value for enum variant");
            explicit_value = std::stoll(val_tok.text());
            next_value = *explicit_value + 1;
        } else {
            explicit_value = std::nullopt;
            next_value++;
        }

        variants.emplace_back(variant_name.text(), explicit_value,
                              std::move(variant_loc));

        if (!match(TokenKind::COMMA)) {
            break;
        }
    }

    consume(TokenKind::RBRACE, "expected '}' after enum variants");
    return std::make_unique<EnumDecl>(
        std::move(enum_loc), std::move(enum_name), std::move(variants));
}

std::unique_ptr<ImportDecl> Parser::parseImportDecl() {
    SourceLocation import_loc = loc();
    consume(TokenKind::KW_IMPORT, "expected 'import'");

    // import "path" or import path (identifier or dotted path)
    std::string path;
    if (check(TokenKind::STRING_LITERAL)) {
        Token path_tok = advance();
        // Strip quotes
        path = path_tok.text().substr(1, path_tok.text().size() - 2);
    } else {
        // Identifier-based import path
        Token first = consume(TokenKind::IDENTIFIER,
                              "expected import path");
        path = first.text();
        while (match(TokenKind::DOT_DOT)) {
            Token part = consume(TokenKind::IDENTIFIER,
                                 "expected identifier in import path");
            path += "::" + part.text();
        }
    }

    consume(TokenKind::SEMI, "expected ';' after import");
    return std::make_unique<ImportDecl>(std::move(import_loc), std::move(path));
}

// ============================================================================
// Statement parsing
// ============================================================================
std::unique_ptr<Stmt> Parser::parseStmt() {
    // Check for compiler directives at statement level
    if (check(TokenKind::AT)) {
        // Consume directives (we don't have a way to attach them to blocks
        // in the current AST, so we just skip them for now at statement level)
        auto directives = parseDirectives();
        // Directives at statement level: parse the next statement
        // TODO: Store directives for the subsequent block when AST supports it
        auto stmt = parseStmt();
        if (!stmt) {
            // Error-resilient: return an ExprStmt wrapping a PoisonExpr
            SourceLocation poison_loc = loc();
            auto poison = std::make_unique<PoisonExpr>(poison_loc, "failed to parse statement after directives");
            return std::make_unique<ExprStmt>(poison_loc, std::move(poison));
        }
        return stmt;
    }

    switch (peek().kind()) {
        case TokenKind::KW_VAL:
            return parseValDecl();
        case TokenKind::KW_VAR:
            return parseVarDecl();
        case TokenKind::KW_IF:
            return parseIfStmt();
        case TokenKind::KW_WHILE:
            return parseWhileStmt();
        case TokenKind::KW_DEFER:
            return parseDeferStmt();
        case TokenKind::KW_RETURN:
            return parseReturnStmt();
        case TokenKind::KW_BREAK:
            return parseBreakStmt();
        case TokenKind::KW_CONTINUE:
            return parseContinueStmt();
        case TokenKind::LBRACE:
            return parseBlockStmt();
        case TokenKind::KW_UNSAFE: {
            // unsafe at statement level
            SourceLocation unsafe_loc = loc();
            advance(); // consume 'unsafe'
            if (check(TokenKind::LPAREN)) {
                // unsafe(expr) at statement level
                advance(); // consume (
                auto inner = parseExpr();
                consume(TokenKind::RPAREN, "expected ')' after unsafe expression");
                match(TokenKind::SEMI);
                auto unsafe_expr = std::make_unique<UnsafeExpr>(
                    std::move(unsafe_loc), std::move(inner));
                return std::make_unique<ExprStmt>(
                    SourceLocation(unsafe_loc), std::move(unsafe_expr));
            }
            if (check(TokenKind::LBRACE)) {
                // unsafe { block }
                auto block = parseBlockStmt();
                unsafe_blocks_.insert(block.get());
                return block;
            }
            // unsafe expr; at statement level
            auto inner = parseExpr();
            match(TokenKind::SEMI);
            auto unsafe_expr = std::make_unique<UnsafeExpr>(
                std::move(unsafe_loc), std::move(inner));
            return std::make_unique<ExprStmt>(
                SourceLocation(unsafe_loc), std::move(unsafe_expr));
        }
        default:
            break;
    }

    // Expression statement or assignment
    auto expr = parseExpr();

    // Error-resilient: if expression parsing failed, create a PoisonExpr
    if (!expr) {
        SourceLocation poison_loc = loc();
        expr = std::make_unique<PoisonExpr>(poison_loc, "failed to parse expression");
        synchronize();
        return std::make_unique<ExprStmt>(poison_loc, std::move(expr));
    }

    // Check for assignment
    if (match(TokenKind::EQ)) {
        // Simple assignment: target = value
        auto value = parseExpr();
        if (!value) {
            SourceLocation poison_loc = loc();
            value = std::make_unique<PoisonExpr>(poison_loc, "failed to parse assignment value");
        }
        consume(TokenKind::SEMI, "expected ';' after assignment");
        return std::make_unique<AssignStmt>(
            locFrom(previous()), std::move(expr), std::move(value));
    }

    if (peek().isCompoundAssignment()) {
        Token op_tok = advance();
        auto value = parseExpr();
        if (!value) {
            SourceLocation poison_loc = loc();
            value = std::make_unique<PoisonExpr>(poison_loc, "failed to parse compound assignment value");
        }
        consume(TokenKind::SEMI, "expected ';' after compound assignment");
        BinaryOp op = tokenToCompoundAssignOp(op_tok.kind());
        auto bin_expr = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(expr), std::move(value));
        return std::make_unique<ExprStmt>(
            locFrom(op_tok), std::move(bin_expr));
    }

    // Plain expression statement
    consume(TokenKind::SEMI, "expected ';' after expression");
    return std::make_unique<ExprStmt>(locFrom(previous()), std::move(expr));
}

std::unique_ptr<ValDeclStmt> Parser::parseValDecl() {
    SourceLocation val_loc = loc();
    consume(TokenKind::KW_VAL, "expected 'val'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected variable name");
    std::string name = name_tok.text();

    // Optional type annotation
    TypeId declared_type;
    if (match(TokenKind::COLON)) {
        declared_type = parseType();
    }

    consume(TokenKind::EQ, "expected '=' in val declaration");

    std::unique_ptr<Expr> init;
    if (!check(TokenKind::SEMI)) {
        init = parseExpr();
    }

    consume(TokenKind::SEMI, "expected ';' after val declaration");

    auto stmt = std::make_unique<ValDeclStmt>(
        std::move(val_loc), std::move(name), declared_type, std::move(init));

    // If we couldn't resolve the type, record the annotation text for later
    if (!declared_type.isNull()) {
        // Type was resolved during parsing (primitive or compound of primitives)
    }

    return stmt;
}

std::unique_ptr<VarDeclStmt> Parser::parseVarDecl() {
    SourceLocation var_loc = loc();
    consume(TokenKind::KW_VAR, "expected 'var'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected variable name");
    std::string name = name_tok.text();

    // Optional type annotation
    TypeId declared_type;
    if (match(TokenKind::COLON)) {
        declared_type = parseType();
    }

    consume(TokenKind::EQ, "expected '=' in var declaration");

    std::unique_ptr<Expr> init;
    if (!check(TokenKind::SEMI)) {
        init = parseExpr();
    }

    consume(TokenKind::SEMI, "expected ';' after var declaration");

    return std::make_unique<VarDeclStmt>(
        std::move(var_loc), std::move(name), declared_type, std::move(init));
}

std::unique_ptr<IfStmt> Parser::parseIfStmt() {
    SourceLocation if_loc = loc();
    consume(TokenKind::KW_IF, "expected 'if'");

    consume(TokenKind::LPAREN, "expected '(' after 'if'");
    auto condition = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after if condition");

    auto then_block = parseBlockStmt();

    std::unique_ptr<BlockStmt> else_block;
    if (match(TokenKind::KW_ELSE)) {
        if (check(TokenKind::KW_IF)) {
            // else if: wrap the nested IfStmt in a BlockStmt
            SourceLocation else_loc = locFrom(previous());
            auto else_if = parseIfStmt();
            std::vector<std::unique_ptr<Stmt>> stmts;
            stmts.push_back(std::move(else_if));
            else_block = std::make_unique<BlockStmt>(
                std::move(else_loc), std::move(stmts));
        } else {
            else_block = parseBlockStmt();
        }
    }

    return std::make_unique<IfStmt>(
        std::move(if_loc), std::move(condition),
        std::move(then_block), std::move(else_block));
}

std::unique_ptr<WhileStmt> Parser::parseWhileStmt() {
    SourceLocation while_loc = loc();
    consume(TokenKind::KW_WHILE, "expected 'while'");

    consume(TokenKind::LPAREN, "expected '(' after 'while'");
    auto condition = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after while condition");

    // Optional increment clause: while (cond) : (incr) { body }
    // The increment can be a simple expression or an assignment like i += 1
    std::unique_ptr<Expr> increment;
    if (match(TokenKind::COLON)) {
        consume(TokenKind::LPAREN, "expected '(' for while increment");
        // Parse the LHS expression first
        auto lhs = parseExpr();
        // Check for assignment operators
        if (match(TokenKind::EQ)) {
            auto rhs = parseExpr();
            increment = std::make_unique<BinaryExpr>(
                locFrom(previous()), BinaryOp::Assign, std::move(lhs), std::move(rhs));
        } else if (peek().isCompoundAssignment()) {
            Token op_tok = advance();
            auto rhs = parseExpr();
            BinaryOp op = tokenToCompoundAssignOp(op_tok.kind());
            increment = std::make_unique<BinaryExpr>(
                locFrom(op_tok), op, std::move(lhs), std::move(rhs));
        } else {
            increment = std::move(lhs);
        }
        consume(TokenKind::RPAREN, "expected ')' after while increment");
    }

    auto body = parseBlockStmt();

    return std::make_unique<WhileStmt>(
        std::move(while_loc), std::move(condition),
        std::move(body), std::move(increment));
}

std::unique_ptr<Stmt> Parser::parseDeferStmt() {
    SourceLocation defer_loc = loc();
    consume(TokenKind::KW_DEFER, "expected 'defer'");

    // Parse expression, which may be followed by an assignment
    auto lhs = parseExpr();
    if (match(TokenKind::EQ)) {
        auto rhs = parseExpr();
        consume(TokenKind::SEMI, "expected ';' after defer assignment");
        auto assign = std::make_unique<AssignStmt>(
            SourceLocation(defer_loc), std::move(lhs), std::move(rhs));
        return std::make_unique<DeferStmt>(
            std::move(defer_loc), std::move(assign));
    }
    if (peek().isCompoundAssignment()) {
        Token op_tok = advance();
        auto rhs = parseExpr();
        consume(TokenKind::SEMI, "expected ';' after defer assignment");
        BinaryOp op = tokenToCompoundAssignOp(op_tok.kind());
        auto bin_expr = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(lhs), std::move(rhs));
        auto expr_stmt = std::make_unique<ExprStmt>(
            SourceLocation(defer_loc), std::move(bin_expr));
        return std::make_unique<DeferStmt>(
            std::move(defer_loc), std::move(expr_stmt));
    }
    consume(TokenKind::SEMI, "expected ';' after defer expression");
    auto expr_stmt = std::make_unique<ExprStmt>(
        SourceLocation(defer_loc), std::move(lhs));
    return std::make_unique<DeferStmt>(
        std::move(defer_loc), std::move(expr_stmt));
}

std::unique_ptr<ReturnStmt> Parser::parseReturnStmt() {
    SourceLocation ret_loc = loc();
    consume(TokenKind::KW_RETURN, "expected 'return'");

    std::unique_ptr<Expr> value;
    if (!check(TokenKind::SEMI) && !check(TokenKind::RBRACE)) {
        value = parseExpr();
    }

    consume(TokenKind::SEMI, "expected ';' after return");
    return std::make_unique<ReturnStmt>(std::move(ret_loc), std::move(value));
}

std::unique_ptr<BreakStmt> Parser::parseBreakStmt() {
    SourceLocation break_loc = loc();
    consume(TokenKind::KW_BREAK, "expected 'break'");
    consume(TokenKind::SEMI, "expected ';' after break");
    return std::make_unique<BreakStmt>(std::move(break_loc));
}

std::unique_ptr<ContinueStmt> Parser::parseContinueStmt() {
    SourceLocation cont_loc = loc();
    consume(TokenKind::KW_CONTINUE, "expected 'continue'");
    consume(TokenKind::SEMI, "expected ';' after continue");
    return std::make_unique<ContinueStmt>(std::move(cont_loc));
}

std::unique_ptr<BlockStmt> Parser::parseBlockStmt() {
    SourceLocation block_loc = loc();
    consume(TokenKind::LBRACE, "expected '{'");

    std::vector<std::unique_ptr<Stmt>> stmts;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        auto stmt = parseStmt();
        if (stmt) {
            stmts.push_back(std::move(stmt));
        } else {
            synchronize();
        }
    }

    consume(TokenKind::RBRACE, "expected '}'");
    return std::make_unique<BlockStmt>(
        std::move(block_loc), std::move(stmts));
}

// ============================================================================
// Expression parsing — recursive descent with operator precedence
// ============================================================================
std::unique_ptr<Expr> Parser::parseExpr() {
    return parseOrExpr();
}

// ||
std::unique_ptr<Expr> Parser::parseOrExpr() {
    auto left = parseAndExpr();
    while (match(TokenKind::PIPE_PIPE)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = parseAndExpr();
        left = std::make_unique<BinaryExpr>(
            std::move(op_loc), BinaryOp::Or,
            std::move(left), std::move(right));
    }
    return left;
}

// &&
std::unique_ptr<Expr> Parser::parseAndExpr() {
    auto left = parseBitOrExpr();
    while (match(TokenKind::AMP_AMP)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = parseBitOrExpr();
        left = std::make_unique<BinaryExpr>(
            std::move(op_loc), BinaryOp::And,
            std::move(left), std::move(right));
    }
    return left;
}

// | (bitwise or)
std::unique_ptr<Expr> Parser::parseBitOrExpr() {
    auto left = parseBitXorExpr();
    while (match(TokenKind::PIPE)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = parseBitXorExpr();
        left = std::make_unique<BinaryExpr>(
            std::move(op_loc), BinaryOp::BitOr,
            std::move(left), std::move(right));
    }
    return left;
}

// ^ (bitwise xor)
std::unique_ptr<Expr> Parser::parseBitXorExpr() {
    auto left = parseBitAndExpr();
    while (match(TokenKind::CARET)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = parseBitAndExpr();
        left = std::make_unique<BinaryExpr>(
            std::move(op_loc), BinaryOp::BitXor,
            std::move(left), std::move(right));
    }
    return left;
}

// & (bitwise and)
std::unique_ptr<Expr> Parser::parseBitAndExpr() {
    auto left = parseEqualityExpr();
    while (match(TokenKind::AMP)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = parseEqualityExpr();
        left = std::make_unique<BinaryExpr>(
            std::move(op_loc), BinaryOp::BitAnd,
            std::move(left), std::move(right));
    }
    return left;
}

// == !=
std::unique_ptr<Expr> Parser::parseEqualityExpr() {
    auto left = parseComparisonExpr();
    while (check(TokenKind::EQ_EQ) || check(TokenKind::BANG_EQ)) {
        Token op_tok = advance();
        BinaryOp op = (op_tok.kind() == TokenKind::EQ_EQ)
                          ? BinaryOp::Eq : BinaryOp::Ne;
        auto right = parseComparisonExpr();
        left = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(left), std::move(right));
    }
    return left;
}

// < > <= >=
std::unique_ptr<Expr> Parser::parseComparisonExpr() {
    auto left = parseShiftExpr();
    while (check(TokenKind::LT) || check(TokenKind::GT) ||
           check(TokenKind::LE) || check(TokenKind::GE)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.kind()) {
            case TokenKind::LT: op = BinaryOp::Lt; break;
            case TokenKind::GT: op = BinaryOp::Gt; break;
            case TokenKind::LE: op = BinaryOp::Le; break;
            case TokenKind::GE: op = BinaryOp::Ge; break;
            default: op = BinaryOp::Lt; break; // unreachable
        }
        auto right = parseShiftExpr();
        left = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(left), std::move(right));
    }
    return left;
}

// << >>
std::unique_ptr<Expr> Parser::parseShiftExpr() {
    auto left = parseAdditiveExpr();
    while (check(TokenKind::SHL) || check(TokenKind::SHR)) {
        Token op_tok = advance();
        BinaryOp op = (op_tok.kind() == TokenKind::SHL)
                          ? BinaryOp::Shl : BinaryOp::Shr;
        auto right = parseAdditiveExpr();
        left = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(left), std::move(right));
    }
    return left;
}

// + -
std::unique_ptr<Expr> Parser::parseAdditiveExpr() {
    auto left = parseMultiplicativeExpr();
    while (check(TokenKind::PLUS) || check(TokenKind::MINUS)) {
        Token op_tok = advance();
        BinaryOp op = (op_tok.kind() == TokenKind::PLUS)
                          ? BinaryOp::Add : BinaryOp::Sub;
        auto right = parseMultiplicativeExpr();
        left = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(left), std::move(right));
    }
    return left;
}

// * / %
std::unique_ptr<Expr> Parser::parseMultiplicativeExpr() {
    auto left = parseUnaryExpr();
    while (check(TokenKind::STAR) || check(TokenKind::SLASH) ||
           check(TokenKind::PERCENT)) {
        Token op_tok = advance();
        BinaryOp op;
        switch (op_tok.kind()) {
            case TokenKind::STAR:    op = BinaryOp::Mul; break;
            case TokenKind::SLASH:   op = BinaryOp::Div; break;
            case TokenKind::PERCENT: op = BinaryOp::Mod; break;
            default: op = BinaryOp::Mul; break; // unreachable
        }
        auto right = parseUnaryExpr();
        left = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(left), std::move(right));
    }
    return left;
}

// Unary: - ! & * ~
std::unique_ptr<Expr> Parser::parseUnaryExpr() {
    if (check(TokenKind::MINUS) || check(TokenKind::BANG) ||
        check(TokenKind::AMP) || check(TokenKind::STAR) ||
        check(TokenKind::TILDE)) {
        Token op_tok = advance();
        SourceLocation op_loc = locFrom(op_tok);

        auto operand = parseUnaryExpr();

        switch (op_tok.kind()) {
            case TokenKind::MINUS:
                return std::make_unique<UnaryExpr>(
                    std::move(op_loc), UnaryOp::Neg, std::move(operand));
            case TokenKind::BANG:
                return std::make_unique<UnaryExpr>(
                    std::move(op_loc), UnaryOp::Not, std::move(operand));
            case TokenKind::TILDE:
                return std::make_unique<UnaryExpr>(
                    std::move(op_loc), UnaryOp::BitNot, std::move(operand));
            case TokenKind::AMP: {
                // Check for &mut expr
                bool is_mut = match(TokenKind::KW_MUT);
                return std::make_unique<AddrOfExpr>(
                    std::move(op_loc), std::move(operand), is_mut);
            }
            case TokenKind::STAR:
                return std::make_unique<DerefExpr>(
                    std::move(op_loc), std::move(operand));
            default:
                break; // unreachable
        }
    }

    return parsePostfixExpr(parseCastExpr());
}

// Postfix: . [] () !  (left-to-right)
std::unique_ptr<Expr> Parser::parsePostfixExpr(std::unique_ptr<Expr> lhs) {
    while (true) {
        if (check(TokenKind::DOT)) {
            lhs = parseMemberExpr(std::move(lhs));
        } else if (check(TokenKind::LBRACKET)) {
            lhs = parseIndexExpr(std::move(lhs));
        } else if (check(TokenKind::LPAREN)) {
            lhs = parseCallExpr(std::move(lhs));
        } else if (check(TokenKind::BANG)) {
            // Error propagation: expr!
            // Wrap in a UnaryExpr::Not so semantic analysis can detect
            // error-type propagation (the semantic analyzer distinguishes
            // logical-NOT from error-propagation based on the operand type).
            advance(); // consume '!'
            SourceLocation bang_loc = locFrom(previous());
            lhs = std::make_unique<UnaryExpr>(
                std::move(bang_loc), UnaryOp::Not, std::move(lhs));
        } else {
            break;
        }
    }
    return lhs;
}

// cast
std::unique_ptr<Expr> Parser::parseCastExpr() {
    auto expr = parsePrimaryExpr();

    if (match(TokenKind::KW_CAST)) {
        SourceLocation cast_loc = locFrom(previous());
        TypeId target_type = parseType();
        // Record type annotation for unresolved types
        if (!target_type.isNull()) {
            recordTypeAnnotation(expr.get(), target_type->toString());
        }
        return std::make_unique<CastExpr>(
            std::move(cast_loc), std::move(expr), target_type);
    }

    return expr;
}

// Primary expressions
std::unique_ptr<Expr> Parser::parsePrimaryExpr() {
    switch (peek().kind()) {
        // Error recovery: if we hit an unexpected token, produce PoisonExpr
        case TokenKind::EOF_TOKEN: {
            SourceLocation poison_loc = loc();
            error("unexpected end of file in expression");
            return std::make_unique<PoisonExpr>(poison_loc, "unexpected end of file");
        }
        // Literals
        case TokenKind::INT_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Parse value, stripping any type suffix
            std::string text = tok.text();
            uint64_t value = 0;
            bool is_signed = false;

            // Strip suffix
            size_t digit_end = 0;
            if (text.size() > 2 && text[0] == '0' &&
                (text[1] == 'x' || text[1] == 'X')) {
                // Hex literal
                digit_end = 2;
                while (digit_end < text.size() &&
                       (isxdigit(text[digit_end]) || text[digit_end] == '_')) {
                    digit_end++;
                }
                std::string hex_str = text.substr(2, digit_end - 2);
                value = std::stoull(hex_str, nullptr, 16);
            } else if (text.size() > 2 && text[0] == '0' &&
                       (text[1] == 'b' || text[1] == 'B')) {
                // Binary literal
                digit_end = 2;
                while (digit_end < text.size() &&
                       (text[digit_end] == '0' || text[digit_end] == '1' ||
                        text[digit_end] == '_')) {
                    digit_end++;
                }
                std::string bin_str = text.substr(2, digit_end - 2);
                value = std::stoull(bin_str, nullptr, 2);
            } else {
                // Decimal
                digit_end = 0;
                while (digit_end < text.size() &&
                       (isdigit(text[digit_end]) || text[digit_end] == '_')) {
                    digit_end++;
                }
                std::string dec_str = text.substr(0, digit_end);
                value = std::stoull(dec_str, nullptr, 10);
            }

            // Determine signedness from suffix
            std::string suffix = text.substr(digit_end);
            if (suffix.find('i') != std::string::npos) {
                is_signed = true;
            }

            return std::make_unique<IntLiteral>(
                std::move(tok_loc), value, is_signed);
        }

        case TokenKind::FLOAT_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Parse value, stripping any type suffix
            std::string text = tok.text();
            size_t last_digit = text.find_last_of("0123456789");
            double value = 0.0;
            if (last_digit != std::string::npos) {
                std::string num_str = text.substr(0, last_digit + 1);
                value = std::stod(num_str);
            }
            return std::make_unique<FloatLiteral>(std::move(tok_loc), value);
        }

        case TokenKind::STRING_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Strip quotes and process escape sequences
            std::string raw = tok.text().substr(1, tok.text().size() - 2);
            std::string value;
            value.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '\\' && i + 1 < raw.size()) {
                    switch (raw[i + 1]) {
                        case 'n':  value += '\n'; break;
                        case 'r':  value += '\r'; break;
                        case 't':  value += '\t'; break;
                        case '\\': value += '\\'; break;
                        case '"':  value += '"';  break;
                        case '\'': value += '\''; break;
                        case '0':  value += '\0'; break;
                        case 'x': {
                            // Hex escape \xHH
                            if (i + 3 < raw.size()) {
                                std::string hex = raw.substr(i + 2, 2);
                                char ch = static_cast<char>(
                                    std::stoul(hex, nullptr, 16));
                                value += ch;
                                i += 3;
                            }
                            break;
                        }
                        case 'u': {
                            // Unicode escape \uHHHH
                            if (i + 5 < raw.size()) {
                                std::string hex = raw.substr(i + 2, 4);
                                uint32_t codepoint =
                                    static_cast<uint32_t>(
                                        std::stoul(hex, nullptr, 16));
                                // Simple UTF-8 encoding
                                if (codepoint < 0x80) {
                                    value += static_cast<char>(codepoint);
                                } else if (codepoint < 0x800) {
                                    value += static_cast<char>(0xC0 | (codepoint >> 6));
                                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                                } else {
                                    value += static_cast<char>(0xE0 | (codepoint >> 12));
                                    value += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                                    value += static_cast<char>(0x80 | (codepoint & 0x3F));
                                }
                                i += 5;
                            }
                            break;
                        }
                        default:
                            value += raw[i + 1];
                            break;
                    }
                    i++; // skip the escaped char
                } else {
                    value += raw[i];
                }
            }
            return std::make_unique<StringLiteral>(std::move(tok_loc), std::move(value));
        }

        case TokenKind::CHAR_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Strip quotes and process escape sequences
            std::string raw = tok.text().substr(1, tok.text().size() - 2);
            char value = '\0';
            if (!raw.empty()) {
                if (raw[0] == '\\' && raw.size() > 1) {
                    switch (raw[1]) {
                        case 'n':  value = '\n'; break;
                        case 'r':  value = '\r'; break;
                        case 't':  value = '\t'; break;
                        case '\\': value = '\\'; break;
                        case '\'': value = '\''; break;
                        case '0':  value = '\0'; break;
                        case 'x': {
                            if (raw.size() > 3) {
                                std::string hex = raw.substr(2, 2);
                                value = static_cast<char>(
                                    std::stoul(hex, nullptr, 16));
                            }
                            break;
                        }
                        default: value = raw[1]; break;
                    }
                } else {
                    value = raw[0];
                }
            }
            // Store as IntLiteral for simplicity (char → u8 value)
            return std::make_unique<IntLiteral>(
                std::move(tok_loc), static_cast<uint64_t>(value), false);
        }

        case TokenKind::KW_TRUE: {
            Token tok = advance();
            return std::make_unique<BoolLiteral>(locFrom(tok), true);
        }
        case TokenKind::KW_FALSE: {
            Token tok = advance();
            return std::make_unique<BoolLiteral>(locFrom(tok), false);
        }

        // Grouping: (expr)
        case TokenKind::LPAREN: {
            SourceLocation paren_loc = loc();
            advance(); // consume (
            auto expr = parseExpr();
            consume(TokenKind::RPAREN, "expected ')' after expression");
            return expr;
        }

        // select(cond, a, b)
        case TokenKind::KW_SELECT:
            return parseSelectExpr();

        // unsafe(expr) in expression context
        case TokenKind::KW_UNSAFE:
            return parseUnsafeExpr();

        // sizeof(type_or_expr)
        case TokenKind::KW_SIZEOF:
            return parseSizeofExpr();

        // Identifier or struct init
        case TokenKind::IDENTIFIER: {
            Token ident_tok = peek();

            // Check for smart pointer type names: Box<T>, Rc<T>, Arc<T>
            if ((ident_tok.text() == "Box" || ident_tok.text() == "Rc" ||
                 ident_tok.text() == "Arc") &&
                peekNext().kind() == TokenKind::LT) {
                // Parse as a smart pointer type name, possibly followed by struct init
                SourceLocation name_loc = loc();
                advance(); // consume Box/Rc/Arc
                std::string sp_name = ident_tok.text();

                consume(TokenKind::LT,
                        "expected '<' after smart pointer name");
                TypeId inner_type = parseType();
                consumeGT(); // consume >

                // Create the full type name for struct init
                std::string full_name = sp_name + "<" + inner_type->toString() + ">";

                if (check(TokenKind::LBRACE)) {
                    return parseStructInitExpr(full_name);
                }

                // Just the type name as an identifier expression
                return std::make_unique<IdentExpr>(
                    std::move(name_loc), std::move(full_name));
            }

            advance(); // consume identifier

            // Struct initialization: TypeName { .field = value, ... }
            if (check(TokenKind::LBRACE)) {
                return parseStructInitExpr(ident_tok.text());
            }

            return std::make_unique<IdentExpr>(
                locFrom(ident_tok), ident_tok.text());
        }

        // Array literal: [a, b, c]
        case TokenKind::LBRACKET: {
            SourceLocation bracket_loc = loc();
            advance(); // consume [

            std::vector<std::unique_ptr<Expr>> elements;
            if (!check(TokenKind::RBRACKET)) {
                elements.push_back(parseExpr());
                while (match(TokenKind::COMMA)) {
                    if (check(TokenKind::RBRACKET)) break;
                    elements.push_back(parseExpr());
                }
            }
            consume(TokenKind::RBRACKET, "expected ']' after array literal");
            return std::make_unique<ArrayInitExpr>(
                std::move(bracket_loc), std::move(elements));
        }

        default:
            break;
    }

    error("expected expression");
    advance(); // consume the unexpected token to avoid infinite loops
    return std::make_unique<PoisonExpr>(loc(), "failed to parse expression");
}

// ============================================================================
// Postfix expression helpers
// ============================================================================
std::unique_ptr<Expr> Parser::parseCallExpr(std::unique_ptr<Expr> callee) {
    SourceLocation call_loc = loc();
    advance(); // consume (
    auto args = parseCallArgs();
    consume(TokenKind::RPAREN, "expected ')' after call arguments");
    return std::make_unique<CallExpr>(
        std::move(call_loc), std::move(callee), std::move(args));
}

std::unique_ptr<Expr> Parser::parseMemberExpr(std::unique_ptr<Expr> object) {
    SourceLocation dot_loc = loc();
    advance(); // consume .
    Token field = consume(TokenKind::IDENTIFIER,
                          "expected field name after '.'");
    return std::make_unique<MemberExpr>(
        std::move(dot_loc), std::move(object), field.text());
}

std::unique_ptr<Expr> Parser::parseIndexExpr(std::unique_ptr<Expr> object) {
    SourceLocation bracket_loc = loc();
    advance(); // consume [
    auto index = parseExpr();
    consume(TokenKind::RBRACKET, "expected ']' after index expression");
    return std::make_unique<IndexExpr>(
        std::move(bracket_loc), std::move(object), std::move(index));
}

std::unique_ptr<Expr> Parser::parseStructInitExpr(const std::string& type_name) {
    SourceLocation init_loc = loc();
    consume(TokenKind::LBRACE, "expected '{' for struct initialization");

    auto inits = parseDesignatedInits();

    consume(TokenKind::RBRACE,
            "expected '}' after struct initialization");
    return std::make_unique<StructInitExpr>(
        std::move(init_loc), type_name, std::move(inits));
}

std::unique_ptr<Expr> Parser::parseSelectExpr() {
    SourceLocation sel_loc = loc();
    consume(TokenKind::KW_SELECT, "expected 'select'");
    consume(TokenKind::LPAREN, "expected '(' after 'select'");

    auto condition = parseExpr();
    consume(TokenKind::COMMA, "expected ',' after select condition");

    auto true_expr = parseExpr();
    consume(TokenKind::COMMA, "expected ',' after select true expression");

    auto false_expr = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after select expression");

    return std::make_unique<SelectExpr>(
        std::move(sel_loc), std::move(condition),
        std::move(true_expr), std::move(false_expr));
}

std::unique_ptr<Expr> Parser::parseUnsafeExpr() {
    SourceLocation unsafe_loc = loc();
    consume(TokenKind::KW_UNSAFE, "expected 'unsafe'");
    consume(TokenKind::LPAREN, "expected '(' after 'unsafe' in expression");

    auto inner = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after unsafe expression");

    return std::make_unique<UnsafeExpr>(
        std::move(unsafe_loc), std::move(inner));
}

std::unique_ptr<Expr> Parser::parseSizeofExpr() {
    SourceLocation sizeof_loc = loc();
    consume(TokenKind::KW_SIZEOF, "expected 'sizeof'");
    consume(TokenKind::LPAREN, "expected '(' after 'sizeof'");

    // Parse as expression; semantic analysis will determine if it's a type
    auto inner = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after sizeof argument");

    // If inner is just an identifier, it might be a type name.
    // We create SizeofExpr with expression operand; semantic analysis
    // can convert it to type operand later.
    return std::make_unique<SizeofExpr>(
        std::move(sizeof_loc), std::move(inner));
}

// ============================================================================
// Type parsing
// ============================================================================
TypeId Parser::parseType() {
    // Pointer type: *T
    if (match(TokenKind::STAR)) {
        TypeId inner = parseType();
        return type_table_.getPointer(inner);
    }

    // Reference type: &T or &mut T
    if (match(TokenKind::AMP)) {
        bool is_mut = match(TokenKind::KW_MUT);
        TypeId inner = parseType();
        if (is_mut) {
            return type_table_.getMutReference(inner);
        }
        return type_table_.getReference(inner);
    }

    // Slice type: []T
    if (check(TokenKind::LBRACKET) && peekNext().kind() == TokenKind::RBRACKET) {
        advance(); // consume [
        advance(); // consume ]
        TypeId inner = parseType();
        return type_table_.getSlice(inner);
    }

    // Primitive type keywords
    switch (peek().kind()) {
        case TokenKind::KW_VOID:   advance(); return type_table_.getVoid();
        case TokenKind::KW_TRUE:
        case TokenKind::KW_FALSE:
            // 'bool' is not a keyword but a primitive type name
            break;
        default:
            break;
    }

    // Primitive type identifiers
    if (check(TokenKind::IDENTIFIER)) {
        std::string name = peek().text();

        // Primitives
        if (name == "u8")    { advance(); return type_table_.getU8(); }
        if (name == "u16")   { advance(); return type_table_.getU16(); }
        if (name == "u32")   { advance(); return type_table_.getU32(); }
        if (name == "u64")   { advance(); return type_table_.getU64(); }
        if (name == "usize") { advance(); return type_table_.getUSize(); }
        if (name == "i8")    { advance(); return type_table_.getI8(); }
        if (name == "i16")   { advance(); return type_table_.getI16(); }
        if (name == "i32")   { advance(); return type_table_.getI32(); }
        if (name == "i64")   { advance(); return type_table_.getI64(); }
        if (name == "isize") { advance(); return type_table_.getISize(); }
        if (name == "f32")   { advance(); return type_table_.getF32(); }
        if (name == "f64")   { advance(); return type_table_.getF64(); }
        if (name == "bool")  { advance(); return type_table_.getBool(); }

        // Allocator type
        if (name == "Allocator") {
            advance();
            return type_table_.getAllocator();
        }

        // Smart pointer types: Box<T>, Rc<T>, Arc<T>
        if ((name == "Box" || name == "Rc" || name == "Arc") &&
            peekNext().kind() == TokenKind::LT) {
            advance(); // consume Box/Rc/Arc
            SmartPointerKind sp_kind;
            if (name == "Box") sp_kind = SmartPointerKind::Box;
            else if (name == "Rc") sp_kind = SmartPointerKind::Rc;
            else sp_kind = SmartPointerKind::Arc;

            consume(TokenKind::LT,
                    "expected '<' after smart pointer name");
            TypeId inner = parseType();
            consumeGT(); // consume > (handles >> splitting)

            return type_table_.getSmartPointer(inner, sp_kind);
        }

        // Named type (struct/enum/alias)
        return resolveNamedType(name);
    }

    error("expected type");
    return TypeId();
}

TypeId Parser::resolveNamedType(const std::string& name) {
    advance(); // consume the identifier

    // Don't create placeholder types. Instead, check if the type already exists
    // via the alias mechanism (for forward references to structs defined earlier in the file)
    auto alias = type_table_.lookupAlias(name);
    if (alias) return *alias;

    // Check if it's a known type in the type table
    auto found = type_table_.lookup(name);
    if (found) return *found;

    // Return null - the semantic analyzer will resolve it later
    return TypeId();
}

// ============================================================================
// Helpers
// ============================================================================
std::vector<FnParam> Parser::parseFnParams() {
    std::vector<FnParam> params;

    if (check(TokenKind::RPAREN)) {
        return params;
    }

    do {
        FnParam param;
        param.is_mutable = false;

        // Check for 'var' keyword for mutable parameter
        if (check(TokenKind::KW_VAR)) {
            param.is_mutable = true;
            advance();
        }

        Token name_tok = consume(TokenKind::IDENTIFIER,
                                 "expected parameter name");
        param.name = name_tok.text();

        consume(TokenKind::COLON, "expected ':' after parameter name");
        Token type_name_tok = peek();
        param.type = parseType();
        // If the type couldn't be resolved, record the type name for later resolution
        if (param.type.isNull() && type_name_tok.kind() == TokenKind::IDENTIFIER) {
            // We need the function name, which is set after parseFnParams returns.
            // Store by param index temporarily; parseFnDecl will add the function name prefix.
            param_type_annotations_["__pending__" + std::to_string(params.size())] = type_name_tok.text();
        }

        params.push_back(std::move(param));
    } while (match(TokenKind::COMMA));

    return params;
}

std::vector<DesignatedInit> Parser::parseDesignatedInits() {
    std::vector<DesignatedInit> inits;

    if (check(TokenKind::RBRACE)) {
        return inits;
    }

    do {
        DesignatedInit init;

        // Expect .fieldname = value (Zig-style designated initializer)
        if (match(TokenKind::DOT)) {
            Token field = consume(TokenKind::IDENTIFIER,
                                  "expected field name after '.'");
            init.field_name = field.text();
            consume(TokenKind::EQ, "expected '=' after field name in struct init");
        } else {
            // Could also support positional init, but spec says designated
            error("expected '.' before field name in struct initializer");
            break;
        }

        init.value = parseExpr();
        inits.push_back(std::move(init));
    } while (match(TokenKind::COMMA) && !check(TokenKind::RBRACE));

    return inits;
}

std::vector<std::unique_ptr<Expr>> Parser::parseCallArgs() {
    std::vector<std::unique_ptr<Expr>> args;

    if (check(TokenKind::RPAREN)) {
        return args;
    }

    do {
        args.push_back(parseExpr());
    } while (match(TokenKind::COMMA));

    return args;
}

void Parser::recordTypeAnnotation(const ASTNode* node,
                                  const std::string& type_text) {
    type_annotations_[node] = type_text;
}

BinaryOp Parser::tokenToBinaryOp(TokenKind kind) const {
    switch (kind) {
        case TokenKind::PLUS:     return BinaryOp::Add;
        case TokenKind::MINUS:    return BinaryOp::Sub;
        case TokenKind::STAR:     return BinaryOp::Mul;
        case TokenKind::SLASH:    return BinaryOp::Div;
        case TokenKind::PERCENT:  return BinaryOp::Mod;
        case TokenKind::AMP:      return BinaryOp::BitAnd;
        case TokenKind::PIPE:     return BinaryOp::BitOr;
        case TokenKind::CARET:    return BinaryOp::BitXor;
        case TokenKind::SHL:      return BinaryOp::Shl;
        case TokenKind::SHR:      return BinaryOp::Shr;
        case TokenKind::AMP_AMP:  return BinaryOp::And;
        case TokenKind::PIPE_PIPE:return BinaryOp::Or;
        case TokenKind::EQ_EQ:    return BinaryOp::Eq;
        case TokenKind::BANG_EQ:  return BinaryOp::Ne;
        case TokenKind::LT:       return BinaryOp::Lt;
        case TokenKind::GT:       return BinaryOp::Gt;
        case TokenKind::LE:       return BinaryOp::Le;
        case TokenKind::GE:       return BinaryOp::Ge;
        case TokenKind::EQ:       return BinaryOp::Assign;
        case TokenKind::PLUS_EQ:  return BinaryOp::AddAssign;
        case TokenKind::MINUS_EQ: return BinaryOp::SubAssign;
        case TokenKind::STAR_EQ:  return BinaryOp::MulAssign;
        case TokenKind::SLASH_EQ: return BinaryOp::DivAssign;
        case TokenKind::PERCENT_EQ:return BinaryOp::ModAssign;
        case TokenKind::AMP_EQ:   return BinaryOp::AndAssign;
        case TokenKind::PIPE_EQ:  return BinaryOp::OrAssign;
        case TokenKind::CARET_EQ: return BinaryOp::XorAssign;
        case TokenKind::SHL_EQ:   return BinaryOp::ShlAssign;
        case TokenKind::SHR_EQ:   return BinaryOp::ShrAssign;
        default:                  return BinaryOp::Add; // fallback
    }
}

BinaryOp Parser::tokenToCompoundAssignOp(TokenKind kind) const {
    switch (kind) {
        case TokenKind::PLUS_EQ:    return BinaryOp::AddAssign;
        case TokenKind::MINUS_EQ:   return BinaryOp::SubAssign;
        case TokenKind::STAR_EQ:    return BinaryOp::MulAssign;
        case TokenKind::SLASH_EQ:   return BinaryOp::DivAssign;
        case TokenKind::PERCENT_EQ: return BinaryOp::ModAssign;
        case TokenKind::AMP_EQ:     return BinaryOp::AndAssign;
        case TokenKind::PIPE_EQ:    return BinaryOp::OrAssign;
        case TokenKind::CARET_EQ:   return BinaryOp::XorAssign;
        case TokenKind::SHL_EQ:     return BinaryOp::ShlAssign;
        case TokenKind::SHR_EQ:     return BinaryOp::ShrAssign;
        default:                    return BinaryOp::AddAssign; // fallback
    }
}

} // namespace tether
