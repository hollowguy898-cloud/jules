#pragma once

#include <string>
#include <cstdint>

namespace tether {

// ============================================================================
// Token Kind Enumeration
// ============================================================================
enum class TokenKind : uint16_t {
    // Keywords
    KW_VAL,
    KW_VAR,
    KW_STRUCT,
    KW_ENUM,
    KW_FN,
    KW_PURE,
    KW_UNSAFE,
    KW_IF,
    KW_ELSE,
    KW_WHILE,
    KW_DEFER,
    KW_RETURN,
    KW_BREAK,
    KW_CONTINUE,
    KW_TRUE,
    KW_FALSE,
    KW_VOID,
    KW_SELECT,
    KW_CAST,
    KW_MUT,
    KW_SIZEOF,
    KW_IMPORT,
    KW_SOA,

    // Literals
    INT_LITERAL,
    FLOAT_LITERAL,
    STRING_LITERAL,
    CHAR_LITERAL,

    // Identifier
    IDENTIFIER,

    // Operators
    PLUS,           // +
    MINUS,          // -
    STAR,           // *
    SLASH,          // /
    PERCENT,        // %
    AMP,            // &
    PIPE,           // |
    CARET,          // ^
    TILDE,          // ~
    AMP_AMP,        // &&
    PIPE_PIPE,      // ||
    BANG,           // !
    EQ,             // =
    EQ_EQ,          // ==
    BANG_EQ,        // !=
    LT,             // <
    GT,             // >
    LE,             // <=
    GE,             // >=
    SHL,            // <<
    SHR,            // >>
    PLUS_EQ,        // +=
    MINUS_EQ,       // -=
    STAR_EQ,        // *=
    SLASH_EQ,       // /=
    PERCENT_EQ,     // %=
    AMP_EQ,         // &=
    PIPE_EQ,        // |=
    CARET_EQ,       // ^=
    SHL_EQ,         // <<=
    SHR_EQ,         // >>=
    COLON_EQ,       // :=
    COLON_COLON,    // ::
    ARROW,          // ->
    DOT,            // .
    DOT_DOT,        // ..

    // Punctuation
    LPAREN,         // (
    RPAREN,         // )
    LBRACE,         // {
    RBRACE,         // }
    LBRACKET,       // [
    RBRACKET,       // ]
    SEMI,           // ;
    COMMA,          // ,
    COLON,          // :
    HASH,           // #
    AT,             // @

    // Special
    EOF_TOKEN,
    UNKNOWN
};

// ============================================================================
// TokenKind to string
// ============================================================================
inline const char* tokenKindToString(TokenKind kind) {
    switch (kind) {
        // Keywords
        case TokenKind::KW_VAL:         return "val";
        case TokenKind::KW_VAR:         return "var";
        case TokenKind::KW_STRUCT:      return "struct";
        case TokenKind::KW_ENUM:        return "enum";
        case TokenKind::KW_FN:          return "fn";
        case TokenKind::KW_PURE:        return "pure";
        case TokenKind::KW_UNSAFE:      return "unsafe";
        case TokenKind::KW_IF:          return "if";
        case TokenKind::KW_ELSE:        return "else";
        case TokenKind::KW_WHILE:       return "while";
        case TokenKind::KW_DEFER:       return "defer";
        case TokenKind::KW_RETURN:      return "return";
        case TokenKind::KW_BREAK:       return "break";
        case TokenKind::KW_CONTINUE:    return "continue";
        case TokenKind::KW_TRUE:        return "true";
        case TokenKind::KW_FALSE:       return "false";
        case TokenKind::KW_VOID:        return "void";
        case TokenKind::KW_SELECT:      return "select";
        case TokenKind::KW_CAST:        return "cast";
        case TokenKind::KW_MUT:         return "mut";
        case TokenKind::KW_SIZEOF:      return "sizeof";
        case TokenKind::KW_IMPORT:      return "import";
        case TokenKind::KW_SOA:         return "soa";

        // Literals
        case TokenKind::INT_LITERAL:    return "integer literal";
        case TokenKind::FLOAT_LITERAL:  return "float literal";
        case TokenKind::STRING_LITERAL: return "string literal";
        case TokenKind::CHAR_LITERAL:   return "char literal";

        // Identifier
        case TokenKind::IDENTIFIER:     return "identifier";

        // Operators
        case TokenKind::PLUS:           return "+";
        case TokenKind::MINUS:          return "-";
        case TokenKind::STAR:           return "*";
        case TokenKind::SLASH:          return "/";
        case TokenKind::PERCENT:        return "%";
        case TokenKind::AMP:            return "&";
        case TokenKind::PIPE:           return "|";
        case TokenKind::CARET:          return "^";
        case TokenKind::TILDE:          return "~";
        case TokenKind::AMP_AMP:        return "&&";
        case TokenKind::PIPE_PIPE:      return "||";
        case TokenKind::BANG:           return "!";
        case TokenKind::EQ:             return "=";
        case TokenKind::EQ_EQ:          return "==";
        case TokenKind::BANG_EQ:        return "!=";
        case TokenKind::LT:             return "<";
        case TokenKind::GT:             return ">";
        case TokenKind::LE:             return "<=";
        case TokenKind::GE:             return ">=";
        case TokenKind::SHL:            return "<<";
        case TokenKind::SHR:            return ">>";
        case TokenKind::PLUS_EQ:        return "+=";
        case TokenKind::MINUS_EQ:       return "-=";
        case TokenKind::STAR_EQ:        return "*=";
        case TokenKind::SLASH_EQ:       return "/=";
        case TokenKind::PERCENT_EQ:     return "%=";
        case TokenKind::AMP_EQ:         return "&=";
        case TokenKind::PIPE_EQ:        return "|=";
        case TokenKind::CARET_EQ:       return "^=";
        case TokenKind::SHL_EQ:         return "<<=";
        case TokenKind::SHR_EQ:         return ">>=";
        case TokenKind::COLON_EQ:       return ":=";
        case TokenKind::COLON_COLON:    return "::";
        case TokenKind::ARROW:          return "->";
        case TokenKind::DOT:            return ".";
        case TokenKind::DOT_DOT:        return "..";

        // Punctuation
        case TokenKind::LPAREN:         return "(";
        case TokenKind::RPAREN:         return ")";
        case TokenKind::LBRACE:         return "{";
        case TokenKind::RBRACE:         return "}";
        case TokenKind::LBRACKET:       return "[";
        case TokenKind::RBRACKET:       return "]";
        case TokenKind::SEMI:           return ";";
        case TokenKind::COMMA:          return ",";
        case TokenKind::COLON:          return ":";
        case TokenKind::HASH:           return "#";
        case TokenKind::AT:             return "@";

        // Special
        case TokenKind::EOF_TOKEN:      return "end of file";
        case TokenKind::UNKNOWN:        return "unknown token";
    }
    return "invalid token kind";
}

// ============================================================================
// Token Class
// ============================================================================
class Token {
public:
    Token()
        : kind_(TokenKind::UNKNOWN), line_(0), col_(0) {}

    Token(TokenKind kind, std::string text, uint32_t line, uint32_t col,
          std::string filename)
        : kind_(kind)
        , text_(std::move(text))
        , line_(line)
        , col_(col)
        , filename_(std::move(filename)) {}

    // Accessors
    TokenKind kind() const { return kind_; }
    const std::string& text() const { return text_; }
    uint32_t line() const { return line_; }
    uint32_t col() const { return col_; }
    const std::string& filename() const { return filename_; }

    // Convenience
    bool is(TokenKind k) const { return kind_ == k; }
    bool isNot(TokenKind k) const { return kind_ != k; }

    bool isKeyword() const {
        return kind_ >= TokenKind::KW_VAL && kind_ <= TokenKind::KW_SOA;
    }

    bool isLiteral() const {
        return kind_ >= TokenKind::INT_LITERAL &&
               kind_ <= TokenKind::CHAR_LITERAL;
    }

    bool isBinaryOperator() const {
        switch (kind_) {
            case TokenKind::PLUS:
            case TokenKind::MINUS:
            case TokenKind::STAR:
            case TokenKind::SLASH:
            case TokenKind::PERCENT:
            case TokenKind::AMP:
            case TokenKind::PIPE:
            case TokenKind::CARET:
            case TokenKind::AMP_AMP:
            case TokenKind::PIPE_PIPE:
            case TokenKind::EQ_EQ:
            case TokenKind::BANG_EQ:
            case TokenKind::LT:
            case TokenKind::GT:
            case TokenKind::LE:
            case TokenKind::GE:
            case TokenKind::SHL:
            case TokenKind::SHR:
            case TokenKind::EQ:
            case TokenKind::PLUS_EQ:
            case TokenKind::MINUS_EQ:
            case TokenKind::STAR_EQ:
            case TokenKind::SLASH_EQ:
            case TokenKind::PERCENT_EQ:
            case TokenKind::AMP_EQ:
            case TokenKind::PIPE_EQ:
            case TokenKind::CARET_EQ:
            case TokenKind::SHL_EQ:
            case TokenKind::SHR_EQ:
                return true;
            default:
                return false;
        }
    }

    bool isCompoundAssignment() const {
        switch (kind_) {
            case TokenKind::PLUS_EQ:
            case TokenKind::MINUS_EQ:
            case TokenKind::STAR_EQ:
            case TokenKind::SLASH_EQ:
            case TokenKind::PERCENT_EQ:
            case TokenKind::AMP_EQ:
            case TokenKind::PIPE_EQ:
            case TokenKind::CARET_EQ:
            case TokenKind::SHL_EQ:
            case TokenKind::SHR_EQ:
                return true;
            default:
                return false;
        }
    }

    bool isAssignmentOperator() const {
        return kind_ == TokenKind::EQ || isCompoundAssignment();
    }

private:
    TokenKind kind_;
    std::string text_;
    uint32_t line_;
    uint32_t col_;
    std::string filename_;
};

} // namespace tether
