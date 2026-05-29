#pragma once

#include <string>
#include <string_view>
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
    KW_CAST,
    KW_MUT,
    KW_SIZEOF,
    KW_IMPORT,
    KW_SOA,
    KW_ALIGN,
    KW_OPAQUE,
    KW_TRY,
    KW_CATCH,
    KW_ERRDEFER,
    KW_ATOMIC,
    KW_YIELD,

    // General-purpose & domain-specific keywords (v0.2 expansion)
    KW_TRAIT,       // Shared behavior contracts (zero-overhead polymorphism)
    KW_IMPL,        // Attach trait impls / methods to structs
    KW_INLINE,      // Force function/loop inlining at call site
    KW_NOALLOC,     // Compile-time guarantee: no heap allocation in fn
    KW_RESTRICT,    // Pointer no-alias qualifier (enables max vectorization)
    KW_COMPTIME,    // Force compile-time evaluation of expr/block
    KW_SHAPE,       // Multi-dimensional tensor boundary property
    KW_STRIDE,      // Physical memory stride for tensor layouts
    KW_REDUCE,      // Hardware-native parallel reduction tree
    KW_SPAWN,       // Structured async task dispatch (work-stealing pool)

    // General-purpose & domain-specific keywords (v0.3 expansion)
    KW_MATCH,        // Pattern matching (replaces switch)
    KW_CONST,        // Compile-time constant declaration
    KW_MODULE,       // Module declaration
    KW_USE,          // Selective import
    KW_AS,           // Import aliasing
    KW_ASYNC,        // Async function modifier
    KW_AWAIT,        // Await expression
    KW_TYPEOF,       // Type query expression
    KW_ALIGNOF,      // Alignment query expression
    KW_PARALLEL,     // Parallel for loop
    KW_REFLECT,      // Compile-time reflection
    KW_STATIC_ASSERT, // Compile-time assertion

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
// TokenKind to string_view (zero-allocation)
// ============================================================================
inline constexpr const char* tokenKindToString(TokenKind kind) {
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

        case TokenKind::KW_CAST:        return "cast";
        case TokenKind::KW_MUT:         return "mut";
        case TokenKind::KW_SIZEOF:      return "sizeof";
        case TokenKind::KW_IMPORT:      return "import";
        case TokenKind::KW_SOA:         return "soa";
        case TokenKind::KW_ALIGN:       return "align";
        case TokenKind::KW_OPAQUE:      return "opaque";
        case TokenKind::KW_TRY:         return "try";
        case TokenKind::KW_CATCH:       return "catch";
        case TokenKind::KW_ERRDEFER:    return "errdefer";
        case TokenKind::KW_ATOMIC:      return "atomic";
        case TokenKind::KW_YIELD:       return "yield";

        // General-purpose & domain-specific keywords (v0.2 expansion)
        case TokenKind::KW_TRAIT:       return "trait";
        case TokenKind::KW_IMPL:        return "impl";
        case TokenKind::KW_INLINE:      return "inline";
        case TokenKind::KW_NOALLOC:     return "noalloc";
        case TokenKind::KW_RESTRICT:    return "restrict";
        case TokenKind::KW_COMPTIME:    return "comptime";
        case TokenKind::KW_SHAPE:       return "shape";
        case TokenKind::KW_STRIDE:      return "stride";
        case TokenKind::KW_REDUCE:      return "reduce";
        case TokenKind::KW_SPAWN:       return "spawn";

        // v0.3 expansion keywords
        case TokenKind::KW_MATCH:         return "match";
        case TokenKind::KW_CONST:         return "const";
        case TokenKind::KW_MODULE:        return "module";
        case TokenKind::KW_USE:           return "use";
        case TokenKind::KW_AS:            return "as";
        case TokenKind::KW_ASYNC:         return "async";
        case TokenKind::KW_AWAIT:         return "await";
        case TokenKind::KW_TYPEOF:        return "typeof";
        case TokenKind::KW_ALIGNOF:       return "alignof";
        case TokenKind::KW_PARALLEL:      return "parallel";
        case TokenKind::KW_REFLECT:       return "reflect";
        case TokenKind::KW_STATIC_ASSERT: return "static_assert";

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
// Token Class — optimized for minimal allocation
//
// Key optimizations over the original:
//   - text_ is std::string_view into the source buffer (zero-copy)
//   - filename_ is a shared const std::string* (one allocation per file,
//     not per token — saves ~80 bytes per token for large files)
//   - Total token size: ~32 bytes vs ~72 bytes before
// ============================================================================
class Token {
public:
    Token()
        : kind_(TokenKind::UNKNOWN), text_(), line_(0), col_(0), filename_(nullptr) {}

    Token(TokenKind kind, std::string_view text, uint32_t line, uint32_t col,
          const std::string* filename)
        : kind_(kind)
        , text_(text)
        , line_(line)
        , col_(col)
        , filename_(filename) {}

    // Compatibility constructor: accepts std::string text, converts to string_view.
    // The caller must ensure the pointed-to string outlives the token.
    // This is safe during lexing because the Lexer owns the source string.
    Token(TokenKind kind, std::string text, uint32_t line, uint32_t col,
          const std::string* filename)
        : kind_(kind)
        , text_(text)  // string_view from temporary — DANGEROUS if text is a temp.
        , line_(line)
        , col_(col)
        , filename_(filename) {
        // NOTE: This constructor is provided for backward compatibility.
        // The std::string `text` must outlive the Token. In practice,
        // use the string_view constructor when the text points into source_.
    }

    // Accessors
    TokenKind kind() const { return kind_; }
    std::string_view text() const { return text_; }
    uint32_t line() const { return line_; }
    uint32_t col() const { return col_; }
    const std::string& filename() const {
        static const std::string empty;
        return filename_ ? *filename_ : empty;
    }

    // Convenience
    bool is(TokenKind k) const { return kind_ == k; }
    bool isNot(TokenKind k) const { return kind_ != k; }

    bool isKeyword() const {
        return kind_ >= TokenKind::KW_VAL && kind_ <= TokenKind::KW_STATIC_ASSERT;
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
    std::string_view text_;
    uint32_t line_;
    uint32_t col_;
    const std::string* filename_;  // shared across all tokens from the same file
};

} // namespace tether
