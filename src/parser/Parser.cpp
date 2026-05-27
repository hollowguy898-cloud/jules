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
    int safety_counter = 0;
    const int MAX_SAFETY = 10000;
    while (!isAtEnd()) {
        auto decl = parseTopLevel();
        if (decl) {
            program.push_back(std::move(decl));
            safety_counter = 0;  // reset on successful parse
        } else {
            // On error, skip to next likely top-level start
            Token before = peek();
            synchronize();
            // BUG FIX: Force progress if synchronize didn't advance
            if (peek().kind() == before.kind() &&
                peek().text() == before.text() &&
                !isAtEnd()) {
                advance();
            }
            if (++safety_counter > MAX_SAFETY) {
                error("too many errors; aborting parse");
                break;
            }
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
    // Error recovery: advance past the unexpected token to prevent infinite loops
    if (!isAtEnd()) {
        return advance();
    }
    return peek();
}

Token Parser::consumeGT() {
    // Handle >> splitting in type context
    if (check(TokenKind::GT)) {
        return advance();
    }
    if (check(TokenKind::SHR)) {
        Token shr = advance(); // consume the >> token
        // Split into two > tokens — string_view from static literals (safe)
        const std::string* fn_ptr = &shr.filename();
        Token first_gt(TokenKind::GT, std::string_view(">"),
                       shr.line(), shr.col(), fn_ptr);
        Token second_gt(TokenKind::GT, std::string_view(">"),
                        shr.line(), shr.col() + 1, fn_ptr);
        pending_tokens_.push_back(second_gt);
        return first_gt;
    }
    if (check(TokenKind::SHR_EQ)) {
        // Split >>= into > and >=
        Token shr_eq = advance();
        const std::string* fn_ptr = &shr_eq.filename();
        Token first_gt(TokenKind::GT, std::string_view(">"),
                       shr_eq.line(), shr_eq.col(), fn_ptr);
        Token second_ge(TokenKind::GE, std::string_view(">="),
                        shr_eq.line(), shr_eq.col() + 1, fn_ptr);
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
            case TokenKind::KW_ERRDEFER:
            case TokenKind::KW_ATOMIC:
            case TokenKind::KW_YIELD:
            case TokenKind::KW_SWITCH:
            case TokenKind::KW_TRAIT:
            case TokenKind::KW_IMPL:
            case TokenKind::KW_COMPTIME:
            case TokenKind::KW_SPAWN:
            case TokenKind::KW_REDUCE:
            case TokenKind::KW_TRY:
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
        msg += " (got '";
        msg.append(token.text());
        msg += "')";
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

    // Handle fn modifiers: inline, noalloc — these prefix fn declarations
    if (check(TokenKind::KW_INLINE) || check(TokenKind::KW_NOALLOC)) {
        return parseFnDecl(std::move(directives));
    }

    if (check(TokenKind::KW_FN)) {
        return parseFnDecl(std::move(directives));
    }
    if (check(TokenKind::KW_STRUCT) || check(TokenKind::KW_SOA)) {
        return parseStructDecl();
    }
    // Handle align(N) struct at top level
    if (check(TokenKind::KW_ALIGN) && peekNext().kind() == TokenKind::LPAREN) {
        return parseStructDecl();
    }
    if (check(TokenKind::KW_ENUM)) {
        return parseEnumDecl();
    }
    if (check(TokenKind::KW_TRAIT)) {
        return parseTraitDecl();
    }
    if (check(TokenKind::KW_IMPL)) {
        return parseImplDecl();
    }
    if (check(TokenKind::KW_IMPORT)) {
        return parseImportDecl();
    }
    if (check(TokenKind::KW_OPAQUE)) {
        SourceLocation opaque_loc = loc();
        consume(TokenKind::KW_OPAQUE, "expected 'opaque'");
        Token name_tok = consume(TokenKind::IDENTIFIER, "expected opaque type name");
        consume(TokenKind::SEMI, "expected ';' after opaque type declaration");
        // We represent opaque types as a special struct with no fields and alignment info
        // Semantic analysis will register it as an OpaqueType
        auto decl = std::make_unique<StructDecl>(
            std::move(opaque_loc), std::string(name_tok.text()), std::vector<StructFieldDecl>{});
        decl->setSoA(false);
        return decl;
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
        std::string_view text = name.text();
        if (text == "superoptimize") {
            directives.push_back(CompilerDirective::Superoptimize);
        } else if (text == "polly") {
            directives.push_back(CompilerDirective::Polly);
        } else if (text == "simd") {
            directives.push_back(CompilerDirective::Simd);
        } else {
            std::string err_msg = "unknown compiler directive '@";
            err_msg.append(text);
            err_msg += "'";
            errorAt(name, err_msg);
        }
    }
    return directives;
}

std::unique_ptr<FnDecl> Parser::parseFnDecl(
        std::vector<CompilerDirective> directives) {
    SourceLocation fn_loc = loc();
    // Handle fn modifiers: inline, noalloc
    bool is_inline = match(TokenKind::KW_INLINE);
    bool is_noalloc = match(TokenKind::KW_NOALLOC);
    consume(TokenKind::KW_FN, "expected 'fn'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected function name");
    std::string fn_name(name_tok.text());

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
        // Also handle compound type annotations
        std::string pending_full_key = "__pending__" + std::to_string(i) + "_full";
        auto it2 = param_type_annotations_.find(pending_full_key);
        if (it2 != param_type_annotations_.end()) {
            std::string final_key = fn_name + ":" + std::to_string(i) + "_full";
            param_type_annotations_[final_key] = it2->second;
            param_type_annotations_.erase(it2);
        }
    }

    // Check for error-returning function: ! before return type
    TypeId error_type;
    bool can_error = match(TokenKind::BANG);

    // Parse return type (optional)
    TypeId return_type;
    std::string return_type_text;  // Store text for later resolution
    if (!check(TokenKind::LBRACE) && !check(TokenKind::KW_PURE)) {
        size_t rt_start = pos_;
        return_type = parseType();
        size_t rt_end = pos_;
        // If return type is null or has null inner types, store the text
        if (return_type.isNull() || hasNullInnerTypeHelper(return_type)) {
            for (size_t i = rt_start; i < rt_end; ++i) {
                if (i > rt_start) return_type_text += " ";
                return_type_text += std::string(tokens_[i].text());
            }
        }
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

    auto result = std::make_unique<FnDecl>(
        std::move(fn_loc), std::move(fn_name),
        std::move(params), return_type,
        std::move(body), is_pure, error_type,
        std::move(directives));
    result->setInline(is_inline);
    result->setNoalloc(is_noalloc);
    if (!return_type_text.empty()) {
        result->unresolved_return_type_name = return_type_text;
    }
    return result;
}

std::unique_ptr<StructDecl> Parser::parseStructDecl() {
    SourceLocation struct_loc = loc();

    // Handle alignment specifier BEFORE struct keyword: align(N) struct Name
    // Also handles: align(N) soa struct Name
    uint32_t alignment = 0;
    if (match(TokenKind::KW_ALIGN)) {
        consume(TokenKind::LPAREN, "expected '(' after 'align'");
        Token align_tok = consume(TokenKind::INT_LITERAL, "expected alignment value");
        alignment = static_cast<uint32_t>(std::stoull(std::string(align_tok.text())));
        consume(TokenKind::RPAREN, "expected ')' after alignment value");
    }

    bool is_soa = match(TokenKind::KW_SOA);
    consume(TokenKind::KW_STRUCT, "expected 'struct'");

    // Check for alignment specifier AFTER struct keyword: struct align(N) Name
    if (alignment == 0 && match(TokenKind::KW_ALIGN)) {
        consume(TokenKind::LPAREN, "expected '(' after 'align'");
        Token align_tok = consume(TokenKind::INT_LITERAL, "expected alignment value");
        alignment = static_cast<uint32_t>(std::stoull(std::string(align_tok.text())));
        consume(TokenKind::RPAREN, "expected ')' after alignment value");
    }

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected struct name");
    std::string struct_name(name_tok.text());

    consume(TokenKind::LBRACE, "expected '{' after struct name");

    std::vector<StructFieldDecl> fields;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        SourceLocation field_loc = loc();
        // Accept keywords as field names (e.g., 'val', 'type', 'align')
        Token field_name;
        if (peek().isKeyword() || check(TokenKind::IDENTIFIER)) {
            field_name = advance();
        } else {
            error("expected field name");
            break;
        }
        consume(TokenKind::COLON, "expected ':' after field name");
        // BUG FIX: Store the type name token text before parsing, so that
        // forward-referenced types (like 'Inner' in 'struct Outer { inner: Inner }')
        // can be resolved later by the semantic analyzer.
        Token type_name_tok = peek();
        TypeId field_type = parseType();

        fields.emplace_back(std::string(field_name.text()), field_type, std::move(field_loc));
        // If the type couldn't be resolved by the parser, save the name text
        if (field_type.isNull() && type_name_tok.kind() == TokenKind::IDENTIFIER) {
            fields.back().unresolved_type_name = std::string(type_name_tok.text());
        }

        if (!match(TokenKind::COMMA)) {
            break;
        }
    }

    consume(TokenKind::RBRACE, "expected '}' after struct fields");
    auto decl = std::make_unique<StructDecl>(
        std::move(struct_loc), std::move(struct_name), std::move(fields));
    decl->setSoA(is_soa);
    decl->setAlignment(alignment);
    return decl;
}

std::unique_ptr<EnumDecl> Parser::parseEnumDecl() {
    SourceLocation enum_loc = loc();
    consume(TokenKind::KW_ENUM, "expected 'enum'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected enum name");
    std::string enum_name(name_tok.text());

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
            explicit_value = std::stoll(std::string(val_tok.text()));
            next_value = *explicit_value + 1;
        } else {
            explicit_value = std::nullopt;
            next_value++;
        }

        variants.emplace_back(std::string(variant_name.text()), explicit_value,
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
        path = std::string(path_tok.text().substr(1, path_tok.text().size() - 2));
    } else {
        // Identifier-based import path
        Token first = consume(TokenKind::IDENTIFIER,
                              "expected import path");
        path = first.text();
        while (match(TokenKind::DOT_DOT)) {
            Token part = consume(TokenKind::IDENTIFIER,
                                 "expected identifier in import path");
            path += "::";
            path.append(part.text());
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
        case TokenKind::KW_ERRDEFER:
            return parseErrdeferStmt();
        case TokenKind::KW_ATOMIC:
            return parseAtomicStmt();
        case TokenKind::KW_YIELD:
            return parseYieldStmt();
        case TokenKind::KW_SWITCH:
            return parseSwitchStmt();
        case TokenKind::KW_SPAWN:
            return parseSpawnStmt();
        case TokenKind::KW_UNSAFE: {
            // unsafe at statement level
            SourceLocation unsafe_loc = loc();
            advance(); // consume 'unsafe'
            if (check(TokenKind::LPAREN)) {
                // unsafe(stmt) at statement level — supports assignment expressions
                // like unsafe(*ptr = 42) or unsafe(x += 1)
                advance(); // consume (
                auto lhs = parseExpr();
                if (match(TokenKind::EQ)) {
                    // Simple assignment inside unsafe()
                    auto rhs = parseExpr();
                    consume(TokenKind::RPAREN, "expected ')' after unsafe assignment");
                    match(TokenKind::SEMI);
                    // Wrap assignment in UnsafeExpr via a block
                    std::vector<std::unique_ptr<Stmt>> stmts;
                    stmts.push_back(std::make_unique<AssignStmt>(
                        SourceLocation(unsafe_loc), std::move(lhs), std::move(rhs)));
                    auto block = std::make_unique<BlockStmt>(
                        SourceLocation(unsafe_loc), std::move(stmts));
                    unsafe_blocks_.insert(block.get());
                    return block;
                }
                if (peek().isCompoundAssignment()) {
                    Token op_tok = advance();
                    auto rhs = parseExpr();
                    consume(TokenKind::RPAREN, "expected ')' after unsafe compound assignment");
                    match(TokenKind::SEMI);
                    BinaryOp op = tokenToCompoundAssignOp(op_tok.kind());
                    auto bin_expr = std::make_unique<BinaryExpr>(
                        locFrom(op_tok), op, std::move(lhs), std::move(rhs));
                    std::vector<std::unique_ptr<Stmt>> stmts;
                    stmts.push_back(std::make_unique<ExprStmt>(
                        SourceLocation(unsafe_loc), std::move(bin_expr)));
                    auto block = std::make_unique<BlockStmt>(
                        SourceLocation(unsafe_loc), std::move(stmts));
                    unsafe_blocks_.insert(block.get());
                    return block;
                }
                // Plain expression inside unsafe()
                consume(TokenKind::RPAREN, "expected ')' after unsafe expression");
                match(TokenKind::SEMI);
                auto unsafe_expr = std::make_unique<UnsafeExpr>(
                    std::move(unsafe_loc), std::move(lhs));
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

    // comptime expression at statement level
    if (check(TokenKind::KW_COMPTIME)) {
        SourceLocation comptime_loc = loc();
        advance(); // consume 'comptime'
        std::unique_ptr<Expr> inner;
        if (check(TokenKind::LBRACE)) {
            // comptime { block } - wrap block as an expression statement
            auto block = parseBlockStmt();
            // Create an ExprStmt wrapping a PoisonExpr temporarily for the block
            // (blocks-as-expressions aren't fully supported yet, use first stmt)
            auto poison = std::make_unique<PoisonExpr>(comptime_loc, "comptime block not yet supported in expression context");
            inner = std::move(poison);
        } else {
            inner = parseExpr();
        }
        auto comptime_expr = std::make_unique<ComptimeExpr>(std::move(comptime_loc), std::move(inner));
        consume(TokenKind::SEMI, "expected ';' after comptime expression");
        return std::make_unique<ExprStmt>(SourceLocation(comptime_loc), std::move(comptime_expr));
    }

    // try expression at statement level
    if (check(TokenKind::KW_TRY)) {
        SourceLocation try_loc = loc();
        advance(); // consume 'try'
        auto operand = parseExpr();
        auto try_expr = std::make_unique<TryExpr>(std::move(try_loc), std::move(operand));
        consume(TokenKind::SEMI, "expected ';' after try expression");
        return std::make_unique<ExprStmt>(SourceLocation(try_loc), std::move(try_expr));
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
    std::string name(name_tok.text());

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
    std::string name(name_tok.text());

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

std::unique_ptr<Stmt> Parser::parseErrdeferStmt() {
    SourceLocation errdefer_loc = loc();
    consume(TokenKind::KW_ERRDEFER, "expected 'errdefer'");

    // Parse expression, which may be followed by an assignment
    auto lhs = parseExpr();
    if (match(TokenKind::EQ)) {
        auto rhs = parseExpr();
        consume(TokenKind::SEMI, "expected ';' after errdefer assignment");
        auto assign = std::make_unique<AssignStmt>(
            SourceLocation(errdefer_loc), std::move(lhs), std::move(rhs));
        return std::make_unique<ErrdeferStmt>(
            std::move(errdefer_loc), std::move(assign));
    }
    if (peek().isCompoundAssignment()) {
        Token op_tok = advance();
        auto rhs = parseExpr();
        consume(TokenKind::SEMI, "expected ';' after errdefer assignment");
        BinaryOp op = tokenToCompoundAssignOp(op_tok.kind());
        auto bin_expr = std::make_unique<BinaryExpr>(
            locFrom(op_tok), op, std::move(lhs), std::move(rhs));
        auto expr_stmt = std::make_unique<ExprStmt>(
            SourceLocation(errdefer_loc), std::move(bin_expr));
        return std::make_unique<ErrdeferStmt>(
            std::move(errdefer_loc), std::move(expr_stmt));
    }
    consume(TokenKind::SEMI, "expected ';' after errdefer expression");
    auto expr_stmt = std::make_unique<ExprStmt>(
        SourceLocation(errdefer_loc), std::move(lhs));
    return std::make_unique<ErrdeferStmt>(
        std::move(errdefer_loc), std::move(expr_stmt));
}

std::unique_ptr<Stmt> Parser::parseAtomicStmt() {
    SourceLocation atomic_loc = loc();
    consume(TokenKind::KW_ATOMIC, "expected 'atomic'");

    // Default to SeqCst ordering
    auto ordering = AtomicStmt::Ordering::SeqCst;

    // Check for optional ordering specifier: atomic(relaxed), atomic(acquire), atomic(release), atomic(acqrel), atomic(seqcst)
    if (match(TokenKind::LPAREN)) {
        Token ordering_tok = consume(TokenKind::IDENTIFIER, "expected atomic ordering (relaxed, acquire, release, acqrel, seqcst)");
        std::string_view ordering_text = ordering_tok.text();
        if (ordering_text == "relaxed") ordering = AtomicStmt::Ordering::Relaxed;
        else if (ordering_text == "acquire") ordering = AtomicStmt::Ordering::Acquire;
        else if (ordering_text == "release") ordering = AtomicStmt::Ordering::Release;
        else if (ordering_text == "acqrel") ordering = AtomicStmt::Ordering::AcqRel;
        else if (ordering_text == "seqcst" || ordering_text == "seq_cst") ordering = AtomicStmt::Ordering::SeqCst;
        else {
            std::string err_msg = "unknown atomic ordering '";
            err_msg.append(ordering_text);
            err_msg += "'";
            errorAt(ordering_tok, err_msg);
        }
        consume(TokenKind::RPAREN, "expected ')' after atomic ordering");
    }

    // Parse the inner statement (usually an assignment or compound assignment)
    auto inner = parseStmt();
    if (!inner) {
        SourceLocation poison_loc = loc();
        auto poison = std::make_unique<PoisonExpr>(poison_loc, "failed to parse atomic statement body");
        inner = std::make_unique<ExprStmt>(poison_loc, std::move(poison));
    }

    return std::make_unique<AtomicStmt>(
        std::move(atomic_loc), std::move(inner), ordering);
}

std::unique_ptr<YieldStmt> Parser::parseYieldStmt() {
    SourceLocation yield_loc = loc();
    consume(TokenKind::KW_YIELD, "expected 'yield'");

    std::unique_ptr<Expr> value;
    if (!check(TokenKind::SEMI) && !check(TokenKind::RBRACE)) {
        value = parseExpr();
    }

    consume(TokenKind::SEMI, "expected ';' after yield");
    return std::make_unique<YieldStmt>(std::move(yield_loc), std::move(value));
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
    int safety_counter = 0;
    const int MAX_SAFETY = 10000;  // BUG FIX: prevent infinite loops on malformed input
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        if (++safety_counter > MAX_SAFETY) {
            error("too many errors in block; skipping to end");
            break;
        }
        auto stmt = parseStmt();
        if (stmt) {
            stmts.push_back(std::move(stmt));
            safety_counter = 0;  // reset on successful parse
        } else {
            // BUG FIX: ensure we always make progress — if synchronize() didn't
            // advance past the current token, force-advance one token.
            Token before = peek();
            synchronize();
            if (peek().kind() == before.kind() &&
                peek().text() == before.text() &&
                !isAtEnd()) {
                advance();  // force progress to avoid infinite loop
            }
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

// BUG FIX: Helper to replace null expression with PoisonExpr so that
// downstream code (semantic analyzer, code generator) never sees a null
// unique_ptr<Expr> which would cause crashes.
static std::unique_ptr<Expr> ensureExpr(std::unique_ptr<Expr> e, SourceLocation loc = SourceLocation()) {
    if (!e) {
        return std::make_unique<PoisonExpr>(loc, "missing expression");
    }
    return e;
}

// ||
std::unique_ptr<Expr> Parser::parseOrExpr() {
    auto left = parseAndExpr();
    while (match(TokenKind::PIPE_PIPE)) {
        SourceLocation op_loc = locFrom(previous());
        auto right = ensureExpr(parseAndExpr());
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
        auto right = ensureExpr(parseBitOrExpr());
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
        auto right = ensureExpr(parseBitXorExpr());
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
        auto right = ensureExpr(parseBitAndExpr());
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
        auto right = ensureExpr(parseEqualityExpr());
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
        auto right = ensureExpr(parseComparisonExpr());
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
        auto right = ensureExpr(parseShiftExpr());
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
        auto right = ensureExpr(parseAdditiveExpr());
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
        auto right = ensureExpr(parseMultiplicativeExpr());
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
        auto right = ensureExpr(parseUnaryExpr());
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
            std::string_view text = tok.text();
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
                value = std::stoull(std::string(text.substr(2, digit_end - 2)), nullptr, 16);
            } else if (text.size() > 2 && text[0] == '0' &&
                       (text[1] == 'b' || text[1] == 'B')) {
                // Binary literal
                digit_end = 2;
                while (digit_end < text.size() &&
                       (text[digit_end] == '0' || text[digit_end] == '1' ||
                        text[digit_end] == '_')) {
                    digit_end++;
                }
                value = std::stoull(std::string(text.substr(2, digit_end - 2)), nullptr, 2);
            } else {
                // Decimal
                digit_end = 0;
                while (digit_end < text.size() &&
                       (isdigit(text[digit_end]) || text[digit_end] == '_')) {
                    digit_end++;
                }
                value = std::stoull(std::string(text.substr(0, digit_end)), nullptr, 10);
            }

            // Determine signedness from suffix
            std::string_view suffix = text.substr(digit_end);
            if (suffix.find('i') != std::string_view::npos) {
                is_signed = true;
            }

            return std::make_unique<IntLiteral>(
                std::move(tok_loc), value, is_signed);
        }

        case TokenKind::FLOAT_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Parse value, stripping any type suffix
            std::string_view text = tok.text();
            size_t last_digit = text.find_last_of("0123456789");
            double value = 0.0;
            if (last_digit != std::string_view::npos) {
                value = std::stod(std::string(text.substr(0, last_digit + 1)));
            }
            return std::make_unique<FloatLiteral>(std::move(tok_loc), value);
        }

        case TokenKind::STRING_LITERAL: {
            Token tok = advance();
            SourceLocation tok_loc = locFrom(tok);
            // Strip quotes and process escape sequences
            std::string_view raw = tok.text().substr(1, tok.text().size() - 2);
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
                                char ch = static_cast<char>(
                                    std::stoul(std::string(raw.substr(i + 2, 2)), nullptr, 16));
                                value += ch;
                                i += 3;
                            }
                            break;
                        }
                        case 'u': {
                            // Unicode escape \uHHHH
                            if (i + 5 < raw.size()) {
                                uint32_t codepoint =
                                    static_cast<uint32_t>(
                                        std::stoul(std::string(raw.substr(i + 2, 4)), nullptr, 16));
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
            std::string_view raw = tok.text().substr(1, tok.text().size() - 2);
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
                                value = static_cast<char>(
                                    std::stoul(std::string(raw.substr(2, 2)), nullptr, 16));
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

        case TokenKind::KW_TRY: {
            SourceLocation try_loc = loc();
            advance(); // consume 'try'
            auto operand = parseExpr();
            return std::make_unique<TryExpr>(std::move(try_loc), std::move(operand));
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

        // comptime expression: comptime expr
        case TokenKind::KW_COMPTIME: {
            SourceLocation comptime_loc = loc();
            advance(); // consume 'comptime'
            std::unique_ptr<Expr> inner;
            if (check(TokenKind::LBRACE)) {
                // comptime { block } — wrap block statements into an expression
                // by creating a block and treating the last expression as the value.
                // For now, parse the first expression from the block.
                auto block = parseBlockStmt();
                // Create a ComptimeExpr wrapping a PoisonExpr with a note that
                // full block-as-expression support requires the comptime interpreter.
                // The block is analyzed normally by the semantic analyzer.
                auto poison = std::make_unique<PoisonExpr>(comptime_loc,
                    "comptime block evaluation requires interpreter");
                inner = std::move(poison);
            } else {
                inner = parseExpr();
            }
            return std::make_unique<ComptimeExpr>(std::move(comptime_loc), std::move(inner));
        }

        // reduce expression: reduce(op, iterable, axis=N)
        case TokenKind::KW_REDUCE: {
            SourceLocation reduce_loc = loc();
            advance(); // consume 'reduce'
            consume(TokenKind::LPAREN, "expected '(' after 'reduce'");

            // Parse reduction operation: add, mul, max, min, and, or, bit_and, bit_or
            Token op_tok = consume(TokenKind::IDENTIFIER, "expected reduction operation (add, mul, max, min, and, or, bit_and, bit_or)");
            std::string_view op_text = op_tok.text();
            ReduceExpr::ReduceOp op;
            if (op_text == "add") op = ReduceExpr::ReduceOp::Add;
            else if (op_text == "mul") op = ReduceExpr::ReduceOp::Mul;
            else if (op_text == "max") op = ReduceExpr::ReduceOp::Max;
            else if (op_text == "min") op = ReduceExpr::ReduceOp::Min;
            else if (op_text == "and") op = ReduceExpr::ReduceOp::And;
            else if (op_text == "or") op = ReduceExpr::ReduceOp::Or;
            else if (op_text == "bit_and" || op_text == "band") op = ReduceExpr::ReduceOp::BitAnd;
            else if (op_text == "bit_or" || op_text == "bor") op = ReduceExpr::ReduceOp::BitOr;
            else {
                std::string err_msg = "unknown reduction operation '";
                err_msg.append(op_text);
                err_msg += "'";
                errorAt(op_tok, err_msg);
                op = ReduceExpr::ReduceOp::Add; // fallback
            }

            consume(TokenKind::COMMA, "expected ',' after reduction operation");
            auto iterable = parseExpr();

            // Optional axis parameter
            std::unique_ptr<Expr> axis;
            if (match(TokenKind::COMMA)) {
                // axis = N
                if (check(TokenKind::IDENTIFIER) && peek().text() == "axis") {
                    advance(); // consume 'axis'
                    consume(TokenKind::EQ, "expected '=' after 'axis'");
                }
                axis = parseExpr();
            }

            consume(TokenKind::RPAREN, "expected ')' after reduce arguments");
            return std::make_unique<ReduceExpr>(std::move(reduce_loc), op,
                                                 std::move(iterable), std::move(axis));
        }

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
                std::string sp_name(ident_tok.text());

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
                return parseStructInitExpr(std::string(ident_tok.text()));
            }

            return std::make_unique<IdentExpr>(
                locFrom(ident_tok), std::string(ident_tok.text()));
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

    // Allow keywords as field names after '.' (e.g., obj.val, obj.type)
    // This is standard practice — field names should not conflict with keywords.
    std::string field_name;
    if (peek().kind() == TokenKind::IDENTIFIER) {
        field_name = std::string(peek().text());
        advance();
    } else if (peek().isKeyword()) {
        field_name = std::string(peek().text());
        advance();
    } else {
        error("expected field name after '.'");
        // Try to recover: skip the token
        advance();
        field_name = "_error_";
    }

    return std::make_unique<MemberExpr>(
        std::move(dot_loc), std::move(object), std::move(field_name));
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
    // Check for alignment qualifier: align(N) type
    if (match(TokenKind::KW_ALIGN)) {
        consume(TokenKind::LPAREN, "expected '(' after 'align'");
        Token align_tok = consume(TokenKind::INT_LITERAL, "expected alignment value");
        uint32_t alignment = static_cast<uint32_t>(std::stoull(std::string(align_tok.text())));
        consume(TokenKind::RPAREN, "expected ')' after alignment value");
        TypeId inner = parseType();
        return type_table_.getAligned(inner, alignment);
    }

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
        std::string name(peek().text());

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

        // shape[N, M, K] — multi-dimensional tensor boundary type
        if (name == "shape" && peekNext().kind() == TokenKind::LBRACKET) {
            advance(); // consume 'shape'
            consume(TokenKind::LBRACKET, "expected '[' after 'shape'");
            std::vector<uint64_t> dims;
            if (!check(TokenKind::RBRACKET)) {
                Token dim_tok = consume(TokenKind::INT_LITERAL, "expected dimension value");
                dims.push_back(std::stoull(std::string(dim_tok.text())));
                while (match(TokenKind::COMMA)) {
                    dim_tok = consume(TokenKind::INT_LITERAL, "expected dimension value");
                    dims.push_back(std::stoull(std::string(dim_tok.text())));
                }
            }
            consume(TokenKind::RBRACKET, "expected ']' after shape dimensions");
            return type_table_.getShape(std::move(dims));
        }

        // stride[N, M, row_major] — tensor memory stride type
        if (name == "stride" && peekNext().kind() == TokenKind::LBRACKET) {
            advance(); // consume 'stride'
            consume(TokenKind::LBRACKET, "expected '[' after 'stride'");
            std::vector<uint64_t> strides;
            StrideType::Layout layout = StrideType::Layout::RowMajor;
            if (!check(TokenKind::RBRACKET)) {
                Token stride_tok = consume(TokenKind::INT_LITERAL, "expected stride value");
                strides.push_back(std::stoull(std::string(stride_tok.text())));
                while (match(TokenKind::COMMA)) {
                    // Check for layout specifier instead of another number
                    if (check(TokenKind::IDENTIFIER)) {
                        std::string layout_name(peek().text());
                        if (layout_name == "row_major" || layout_name == "row") {
                            advance();
                            layout = StrideType::Layout::RowMajor;
                        } else if (layout_name == "column_major" || layout_name == "col") {
                            advance();
                            layout = StrideType::Layout::ColumnMajor;
                        } else {
                            // Treat as another stride value
                            stride_tok = consume(TokenKind::INT_LITERAL, "expected stride value");
                            strides.push_back(std::stoull(std::string(stride_tok.text())));
                        }
                    } else {
                        stride_tok = consume(TokenKind::INT_LITERAL, "expected stride value");
                        strides.push_back(std::stoull(std::string(stride_tok.text())));
                    }
                }
            }
            consume(TokenKind::RBRACKET, "expected ']' after stride values");
            return type_table_.getStride(std::move(strides), layout);
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
        param.is_restrict = false;

        // Check for 'var' keyword for mutable parameter
        if (check(TokenKind::KW_VAR)) {
            param.is_mutable = true;
            advance();
        }

        // Check for 'restrict' keyword for no-alias pointer parameter
        if (check(TokenKind::KW_RESTRICT)) {
            param.is_restrict = true;
            advance();
        }

        Token name_tok = consume(TokenKind::IDENTIFIER,
                                 "expected parameter name");
        param.name = std::string(name_tok.text());

        consume(TokenKind::COLON, "expected ':' after parameter name");
        size_t type_start = pos_;  // Record position before type parsing
        Token type_name_tok = peek();
        param.type = parseType();
        size_t type_end = pos_;  // Record position after type parsing

        // If the type couldn't be resolved (null), or is a compound type that
        // might have unresolved inner types, record the full type text for
        // later resolution by the semantic analyzer.
        bool needs_annotation = param.type.isNull();
        // Also check if the type is a compound type with null inner parts
        if (!needs_annotation && param.type) {
            // Walk the type tree to check for null inner types
            TypeId check_type = param.type;
            while (check_type) {
                if (isa<PointerType>(check_type)) {
                    TypeId inner = cast<PointerType>(check_type).pointee();
                    if (inner.isNull()) { needs_annotation = true; break; }
                    check_type = inner;
                } else if (isa<ReferenceType>(check_type)) {
                    TypeId inner = cast<ReferenceType>(check_type).referent();
                    if (inner.isNull()) { needs_annotation = true; break; }
                    check_type = inner;
                } else if (isa<MutReferenceType>(check_type)) {
                    TypeId inner = cast<MutReferenceType>(check_type).referent();
                    if (inner.isNull()) { needs_annotation = true; break; }
                    check_type = inner;
                } else if (isa<SliceType>(check_type)) {
                    TypeId inner = cast<SliceType>(check_type).element();
                    if (inner.isNull()) { needs_annotation = true; break; }
                    check_type = inner;
                } else if (isa<SmartPointerType>(check_type)) {
                    TypeId inner = cast<SmartPointerType>(check_type).pointee();
                    if (inner.isNull()) { needs_annotation = true; break; }
                    check_type = inner;
                } else {
                    break;
                }
            }
        }

        if (needs_annotation) {
            // Reconstruct the full type text from the tokens consumed
            std::string full_type_text;
            for (size_t i = type_start; i < type_end; ++i) {
                if (i > type_start) full_type_text += " ";
                full_type_text += std::string(tokens_[i].text());
            }
            // Store the unresolved type name on the param itself
            param.unresolved_type_name = full_type_text;
            // Also store via the old annotation mechanism for backward compat
            if (type_name_tok.kind() == TokenKind::IDENTIFIER && param.type.isNull()) {
                param_type_annotations_["__pending__" + std::to_string(params.size())] = std::string(type_name_tok.text());
            }
            // For compound types, store the full type text
            if (!full_type_text.empty()) {
                param_type_annotations_["__pending__" + std::to_string(params.size()) + "_full"] = full_type_text;
            }
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
            // Allow keywords as field names in struct init (e.g., .val = 5)
            std::string fname;
            if (peek().kind() == TokenKind::IDENTIFIER) {
                fname = std::string(peek().text());
                advance();
            } else if (peek().isKeyword()) {
                fname = std::string(peek().text());
                advance();
            } else {
                error("expected field name after '.'");
                advance();
                fname = "_error_";
            }
            init.field_name = std::move(fname);
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

bool Parser::hasNullInnerTypeHelper(TypeId type) const {
    if (type.isNull()) return true;
    TypeId check = type;
    while (check) {
        if (isa<PointerType>(check)) {
            TypeId inner = cast<PointerType>(check).pointee();
            if (inner.isNull()) return true;
            check = inner;
        } else if (isa<ReferenceType>(check)) {
            TypeId inner = cast<ReferenceType>(check).referent();
            if (inner.isNull()) return true;
            check = inner;
        } else if (isa<MutReferenceType>(check)) {
            TypeId inner = cast<MutReferenceType>(check).referent();
            if (inner.isNull()) return true;
            check = inner;
        } else if (isa<SliceType>(check)) {
            TypeId inner = cast<SliceType>(check).element();
            if (inner.isNull()) return true;
            check = inner;
        } else if (isa<SmartPointerType>(check)) {
            TypeId inner = cast<SmartPointerType>(check).pointee();
            if (inner.isNull()) return true;
            check = inner;
        } else {
            break;
        }
    }
    return false;
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

std::unique_ptr<Stmt> Parser::parseSwitchStmt() {
    SourceLocation switch_loc = loc();
    consume(TokenKind::KW_SWITCH, "expected 'switch'");

    consume(TokenKind::LPAREN, "expected '(' after 'switch'");
    auto subject = parseExpr();
    consume(TokenKind::RPAREN, "expected ')' after switch subject");

    consume(TokenKind::LBRACE, "expected '{' after switch subject");

    std::vector<SwitchArm> arms;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        SwitchArm arm;
        if (check(TokenKind::KW_VAL) || check(TokenKind::KW_VAR)) {
            // val/var pattern binding
            arm.pattern = parseExpr();
        } else {
            arm.pattern = parseExpr();
        }
        consume(TokenKind::ARROW, "expected '->' in switch arm");
        arm.body = parseBlockStmt();
        arms.push_back(std::move(arm));
        // Commas between switch arms are optional
        match(TokenKind::COMMA);
    }

    consume(TokenKind::RBRACE, "expected '}' after switch arms");
    return std::make_unique<SwitchStmt>(std::move(switch_loc), std::move(subject), std::move(arms));
}

std::unique_ptr<Stmt> Parser::parseSpawnStmt() {
    SourceLocation spawn_loc = loc();
    consume(TokenKind::KW_SPAWN, "expected 'spawn'");

    auto task = parseExpr();
    consume(TokenKind::SEMI, "expected ';' after spawn expression");
    return std::make_unique<SpawnStmt>(std::move(spawn_loc), std::move(task));
}

std::unique_ptr<TraitDecl> Parser::parseTraitDecl() {
    SourceLocation trait_loc = loc();
    consume(TokenKind::KW_TRAIT, "expected 'trait'");

    Token name_tok = consume(TokenKind::IDENTIFIER, "expected trait name");
    std::string trait_name(name_tok.text());

    consume(TokenKind::LBRACE, "expected '{' after trait name");

    std::vector<TraitMethodDecl> methods;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        SourceLocation method_loc = loc();
        // Parse: fn name(params) ReturnType or fn name(params) !ReturnType
        consume(TokenKind::KW_FN, "expected 'fn' in trait method");
        Token method_name = consume(TokenKind::IDENTIFIER, "expected method name");
        consume(TokenKind::LPAREN, "expected '(' after method name");
        auto params = parseFnParams();
        consume(TokenKind::RPAREN, "expected ')' after method parameters");

        TypeId error_type;
        bool can_error = match(TokenKind::BANG);

        TypeId return_type;
        if (!check(TokenKind::SEMI) && !check(TokenKind::COMMA) && !check(TokenKind::RBRACE)) {
            return_type = parseType();
        } else {
            return_type = type_table_.getVoid();
        }

        if (can_error) {
            error_type = type_table_.getError(return_type);
        }

        methods.emplace_back(TraitMethodDecl{
            std::string(method_name.text()),
            std::move(params),
            return_type,
            error_type,
            std::move(method_loc)
        });

        match(TokenKind::SEMI);
        if (!match(TokenKind::COMMA)) {
            break;
        }
    }

    consume(TokenKind::RBRACE, "expected '}' after trait methods");
    return std::make_unique<TraitDecl>(
        std::move(trait_loc), std::move(trait_name), std::move(methods));
}

std::unique_ptr<ImplDecl> Parser::parseImplDecl() {
    SourceLocation impl_loc = loc();
    consume(TokenKind::KW_IMPL, "expected 'impl'");

    Token first_name = consume(TokenKind::IDENTIFIER, "expected name after 'impl'");
    std::string name1(first_name.text());

    std::string trait_name;
    std::string struct_name;

    // Check for "impl TraitName for StructName" vs "impl StructName"
    if (check(TokenKind::IDENTIFIER) && peekNext().kind() != TokenKind::LBRACE) {
        // Look for "for" keyword (treat as identifier since it's not a keyword)
        // Actually peek for 'for' - it could be an identifier
        // If next token is an identifier that matches "for", consume it
        if (peek().text() == "for" && peek().kind() == TokenKind::IDENTIFIER) {
            advance(); // consume 'for'
            trait_name = std::move(name1);
            Token struct_tok = consume(TokenKind::IDENTIFIER, "expected struct name after 'for'");
            struct_name = std::string(struct_tok.text());
        } else {
            struct_name = std::move(name1);
        }
    } else {
        struct_name = std::move(name1);
    }

    consume(TokenKind::LBRACE, "expected '{' after impl declaration");

    std::vector<std::unique_ptr<FnDecl>> methods;
    while (!check(TokenKind::RBRACE) && !isAtEnd()) {
        auto directives = parseDirectives();
        if (check(TokenKind::KW_FN)) {
            auto fn = parseFnDecl(std::move(directives));
            if (fn) {
                methods.push_back(std::move(fn));
            }
        } else {
            error("expected 'fn' in impl block");
            break;
        }
    }

    consume(TokenKind::RBRACE, "expected '}' after impl methods");

    return std::make_unique<ImplDecl>(
        std::move(impl_loc), std::move(trait_name), std::move(struct_name),
        std::move(methods));
}

} // namespace tether
