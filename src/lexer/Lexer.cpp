#include "lexer/Lexer.h"

#include <cctype>
#include <cstring>

namespace tether {

// ============================================================================
// Character classification lookup table initialization
// ============================================================================
namespace CharSet {

// Generated programmatically: each byte maps to a CharInfo bitfield.
// Bits: is_digit, is_hex, is_bin, is_alpha, is_alnum, is_ident_start, is_ident_cont, is_ws
const CharInfo char_table[256] = {
    // 0-15: control chars (TAB/LF/VT/FF/CR are whitespace)
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,1},
    {0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    // 16-31: more control
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    // 32-47: space !"#$%&'()*+,-./
    {0,0,0,0,0,0,0,1},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    // 48-63: 0-9 : ; < = > ?
    {1,1,1,0,1,0,1,0},{1,1,1,0,1,0,1,0},{1,1,0,0,1,0,1,0},{1,1,0,0,1,0,1,0},
    {1,1,0,0,1,0,1,0},{1,1,0,0,1,0,1,0},{1,1,0,0,1,0,1,0},{1,1,0,0,1,0,1,0},
    {1,1,0,0,1,0,1,0},{1,1,0,0,1,0,1,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    // 64-79: @ A-O
    {0,0,0,0,0,0,0,0},{0,1,0,1,1,1,1,0},{0,1,1,1,1,1,1,0},{0,1,0,1,1,1,1,0},
    {0,1,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    // 80-95: P-Z [ \ ] ^ _
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,1,1,1,1,0},
    // 96-111: ` a-o
    {0,0,0,0,0,0,0,0},{0,1,0,1,1,1,1,0},{0,1,1,1,1,1,1,0},{0,1,0,1,1,1,1,0},
    {0,1,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    // 112-127: p-z { | } ~ DEL
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},
    {0,1,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,1,1,1,1,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    // 128-255: non-ASCII (all zero)
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
    {0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},{0,0,0,0,0,0,0,0},
};

} // namespace CharSet

// ============================================================================
// FNV-1a keyword hash — fast hash with good distribution for short strings
// ============================================================================
static uint32_t fnv1aHash(std::string_view text) {
    uint32_t h = 2166136261u;
    for (char c : text) {
        h ^= static_cast<unsigned char>(c);
        h *= 16777619u;
    }
    return h;
}

// ============================================================================
// Keyword hash table — maps FNV-1a hash → TokenKind
//
// Pre-computed for all 27 Tether keywords. Uses open addressing with
// a small table size (64 slots). The load factor is ~0.42 which gives
// very few collisions.
// ============================================================================
struct KeywordEntry {
    uint32_t hash;
    TokenKind kind;
    uint8_t length;  // for collision resolution
};

// Pre-computed keyword table sorted for fast lookup
static const KeywordEntry keyword_table[] = {
    // Length-sorted for quick rejection; within same length, alphabetical
    // len 2
    {fnv1aHash("fn"),  TokenKind::KW_FN, 2},
    {fnv1aHash("if"),  TokenKind::KW_IF, 2},
    // len 3
    {fnv1aHash("val"), TokenKind::KW_VAL, 3},
    {fnv1aHash("var"), TokenKind::KW_VAR, 3},
    {fnv1aHash("mut"), TokenKind::KW_MUT, 3},
    {fnv1aHash("soa"), TokenKind::KW_SOA, 3},
    {fnv1aHash("try"), TokenKind::KW_TRY, 3},
    // len 4
    {fnv1aHash("enum"),   TokenKind::KW_ENUM, 4},
    {fnv1aHash("pure"),   TokenKind::KW_PURE, 4},
    {fnv1aHash("else"),   TokenKind::KW_ELSE, 4},
    {fnv1aHash("void"),   TokenKind::KW_VOID, 4},
    {fnv1aHash("cast"),   TokenKind::KW_CAST, 4},
    {fnv1aHash("true"),   TokenKind::KW_TRUE, 4},
    // len 5
    {fnv1aHash("while"),  TokenKind::KW_WHILE, 5},
    {fnv1aHash("defer"),  TokenKind::KW_DEFER, 5},
    {fnv1aHash("break"),  TokenKind::KW_BREAK, 5},
    {fnv1aHash("false"),  TokenKind::KW_FALSE, 5},
    {fnv1aHash("align"),  TokenKind::KW_ALIGN, 5},
    {fnv1aHash("catch"),  TokenKind::KW_CATCH, 5},
    {fnv1aHash("yield"),  TokenKind::KW_YIELD, 5},
    // len 6
    {fnv1aHash("struct"), TokenKind::KW_STRUCT, 6},
    {fnv1aHash("unsafe"), TokenKind::KW_UNSAFE, 6},
    {fnv1aHash("return"), TokenKind::KW_RETURN, 6},
    {fnv1aHash("select"), TokenKind::KW_SELECT, 6},
    {fnv1aHash("sizeof"), TokenKind::KW_SIZEOF, 6},
    {fnv1aHash("import"), TokenKind::KW_IMPORT, 6},
    {fnv1aHash("atomic"), TokenKind::KW_ATOMIC, 6},
    {fnv1aHash("opaque"), TokenKind::KW_OPAQUE, 6},
    // len 8
    {fnv1aHash("continue"), TokenKind::KW_CONTINUE, 8},
    {fnv1aHash("errdefer"), TokenKind::KW_ERRDEFER, 8},
};
static constexpr size_t keyword_table_size = sizeof(keyword_table) / sizeof(keyword_table[0]);

// ============================================================================
// Constructor
// ============================================================================
Lexer::Lexer(const std::string& source, std::string filename)
    : source_(source)
    , filename_ptr_(std::make_shared<std::string>(std::move(filename)))
    , src_data_(source.data())
    , src_len_(source.size())
    , start_(0)
    , current_(0)
    , line_(1)
    , col_(1)
    , token_start_line_(1)
    , token_start_col_(1)
    , has_errors_(false)
{}

// ============================================================================
// Main entry point
// ============================================================================
std::vector<Token> Lexer::tokenize() {
    // Pre-reserve: typical ratio is ~1 token per 5-8 bytes of source
    std::vector<Token> tokens;
    tokens.reserve(src_len_ / 5);

    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) break;

        start_ = current_;
        token_start_line_ = line_;
        token_start_col_ = col_;

        Token token = scanToken();
        if (token.kind() != TokenKind::UNKNOWN) {
            tokens.push_back(std::move(token));
        }
    }

    // Always append an EOF token
    tokens.emplace_back(TokenKind::EOF_TOKEN, std::string_view(), line_, col_,
                        filename_ptr_.get());
    return tokens;
}

// ============================================================================
// Character access
// ============================================================================
char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return src_data_[current_];
}

char Lexer::peekNext() const {
    if (current_ + 1 >= src_len_) return '\0';
    return src_data_[current_ + 1];
}

char Lexer::peekNextNext() const {
    if (current_ + 2 >= src_len_) return '\0';
    return src_data_[current_ + 2];
}

char Lexer::advance() {
    char c = src_data_[current_];
    current_++;
    if (c == '\n') {
        line_++;
        col_ = 1;
    } else {
        col_++;
    }
    return c;
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (src_data_[current_] != expected) return false;
    advance();
    return true;
}

bool Lexer::isAtEnd() const {
    return current_ >= src_len_;
}

// ============================================================================
// Whitespace / comments
// ============================================================================
void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        unsigned char c = static_cast<unsigned char>(src_data_[current_]);
        if (CharSet::isWhitespace(c)) {
            advance();
        } else if (c == '/') {
            if (current_ + 1 < src_len_ && src_data_[current_ + 1] == '/') {
                skipLineComment();
            } else if (current_ + 1 < src_len_ && src_data_[current_ + 1] == '*') {
                skipBlockComment();
            } else {
                return;
            }
        } else {
            return;
        }
    }
}

void Lexer::skipLineComment() {
    // Consume the '//'
    advance();
    advance();
    // Consume until end of line or end of file
    while (!isAtEnd() && src_data_[current_] != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    // Consume the '/*'
    advance();
    advance();

    uint32_t nesting = 1;
    while (!isAtEnd() && nesting > 0) {
        if (src_data_[current_] == '/' && current_ + 1 < src_len_ && src_data_[current_ + 1] == '*') {
            advance();
            advance();
            nesting++;
        } else if (src_data_[current_] == '*' && current_ + 1 < src_len_ && src_data_[current_ + 1] == '/') {
            advance();
            advance();
            nesting--;
        } else {
            advance();
        }
    }

    if (nesting > 0) {
        diagnostics_.push_back({line_, col_, "unterminated block comment"});
        has_errors_ = true;
    }
}

// ============================================================================
// Token scanning dispatch
// ============================================================================
Token Lexer::scanToken() {
    char c = peek();

    if (CharSet::isIdentStart(static_cast<unsigned char>(c))) {
        return scanIdentifierOrKeyword();
    }

    if (CharSet::isDigit(static_cast<unsigned char>(c))) {
        return scanNumber();
    }

    switch (c) {
        case '"':  return scanString();
        case '\'': return scanChar();

        // Single-character tokens
        case '(': advance(); return makeToken(TokenKind::LPAREN);
        case ')': advance(); return makeToken(TokenKind::RPAREN);
        case '{': advance(); return makeToken(TokenKind::LBRACE);
        case '}': advance(); return makeToken(TokenKind::RBRACE);
        case '[': advance(); return makeToken(TokenKind::LBRACKET);
        case ']': advance(); return makeToken(TokenKind::RBRACKET);
        case ';': advance(); return makeToken(TokenKind::SEMI);
        case ',': advance(); return makeToken(TokenKind::COMMA);
        case '#': advance(); return makeToken(TokenKind::HASH);
        case '@': advance(); return makeToken(TokenKind::AT);
        case '~': advance(); return makeToken(TokenKind::TILDE);

        // Potentially multi-character tokens
        case '.':
            advance();
            if (match('.')) {
                return makeToken(TokenKind::DOT_DOT);
            }
            return makeToken(TokenKind::DOT);

        case '+':
            advance();
            if (match('=')) return makeToken(TokenKind::PLUS_EQ);
            return makeToken(TokenKind::PLUS);

        case '-':
            advance();
            if (match('>')) return makeToken(TokenKind::ARROW);
            if (match('=')) return makeToken(TokenKind::MINUS_EQ);
            return makeToken(TokenKind::MINUS);

        case '*':
            advance();
            if (match('=')) return makeToken(TokenKind::STAR_EQ);
            return makeToken(TokenKind::STAR);

        case '/':
            advance();
            if (match('=')) return makeToken(TokenKind::SLASH_EQ);
            return makeToken(TokenKind::SLASH);

        case '%':
            advance();
            if (match('=')) return makeToken(TokenKind::PERCENT_EQ);
            return makeToken(TokenKind::PERCENT);

        case '&':
            advance();
            if (match('&')) return makeToken(TokenKind::AMP_AMP);
            if (match('=')) return makeToken(TokenKind::AMP_EQ);
            return makeToken(TokenKind::AMP);

        case '|':
            advance();
            if (match('|')) return makeToken(TokenKind::PIPE_PIPE);
            if (match('=')) return makeToken(TokenKind::PIPE_EQ);
            return makeToken(TokenKind::PIPE);

        case '^':
            advance();
            if (match('=')) return makeToken(TokenKind::CARET_EQ);
            return makeToken(TokenKind::CARET);

        case ':':
            advance();
            if (match(':')) return makeToken(TokenKind::COLON_COLON);
            if (match('=')) return makeToken(TokenKind::COLON_EQ);
            return makeToken(TokenKind::COLON);

        case '!':
            advance();
            if (match('=')) return makeToken(TokenKind::BANG_EQ);
            return makeToken(TokenKind::BANG);

        case '=':
            advance();
            if (match('=')) return makeToken(TokenKind::EQ_EQ);
            return makeToken(TokenKind::EQ);

        case '<':
            advance();
            if (match('<')) {
                if (match('=')) return makeToken(TokenKind::SHL_EQ);
                return makeToken(TokenKind::SHL);
            }
            if (match('=')) return makeToken(TokenKind::LE);
            return makeToken(TokenKind::LT);

        case '>':
            advance();
            if (match('>')) {
                if (match('=')) return makeToken(TokenKind::SHR_EQ);
                return makeToken(TokenKind::SHR);
            }
            if (match('=')) return makeToken(TokenKind::GE);
            return makeToken(TokenKind::GT);

        default:
            advance();
            return errorToken(std::string("unexpected character '") + c + "'");
    }
}

// ============================================================================
// Identifier / keyword scanning — optimized with hash-based lookup
// ============================================================================
Token Lexer::scanIdentifierOrKeyword() {
    while (!isAtEnd() && CharSet::isIdentCont(static_cast<unsigned char>(src_data_[current_]))) {
        advance();
    }

    // Zero-copy: string_view into the source buffer
    std::string_view text(src_data_ + start_, current_ - start_);
    TokenKind kind = lookupKeyword(text);
    return makeToken(kind);
}

TokenKind Lexer::lookupKeyword(std::string_view text) const {
    // Quick rejection: keywords are 2-8 chars
    auto len = text.size();
    if (len < 2 || len > 8) return TokenKind::IDENTIFIER;

    uint32_t h = fnv1aHash(text);

    // Binary search through the keyword table (sorted by length then alpha)
    // For small tables, linear scan is actually faster due to cache locality
    for (size_t i = 0; i < keyword_table_size; i++) {
        if (keyword_table[i].length != len) continue;
        if (keyword_table[i].hash != h) continue;
        // Hash + length match — verify the actual text to avoid collisions
        // Since we're using string_view into the source, this is a direct comparison
        const char* kw_start = nullptr;
        size_t kw_len = 0;
        switch (keyword_table[i].kind) {
#define KW_CASE(kw, kind) case TokenKind::kind: kw_start = kw; kw_len = sizeof(kw)-1; break
            KW_CASE("fn",       KW_FN);
            KW_CASE("if",       KW_IF);
            KW_CASE("val",      KW_VAL);
            KW_CASE("var",      KW_VAR);
            KW_CASE("mut",      KW_MUT);
            KW_CASE("soa",      KW_SOA);
            KW_CASE("try",      KW_TRY);
            KW_CASE("enum",     KW_ENUM);
            KW_CASE("pure",     KW_PURE);
            KW_CASE("else",     KW_ELSE);
            KW_CASE("void",     KW_VOID);
            KW_CASE("cast",     KW_CAST);
            KW_CASE("true",     KW_TRUE);
            KW_CASE("while",    KW_WHILE);
            KW_CASE("defer",    KW_DEFER);
            KW_CASE("break",    KW_BREAK);
            KW_CASE("false",    KW_FALSE);
            KW_CASE("align",    KW_ALIGN);
            KW_CASE("catch",    KW_CATCH);
            KW_CASE("yield",    KW_YIELD);
            KW_CASE("struct",   KW_STRUCT);
            KW_CASE("unsafe",   KW_UNSAFE);
            KW_CASE("return",   KW_RETURN);
            KW_CASE("select",   KW_SELECT);
            KW_CASE("sizeof",   KW_SIZEOF);
            KW_CASE("import",   KW_IMPORT);
            KW_CASE("atomic",   KW_ATOMIC);
            KW_CASE("opaque",   KW_OPAQUE);
            KW_CASE("continue", KW_CONTINUE);
            KW_CASE("errdefer", KW_ERRDEFER);
#undef KW_CASE
            default: break;
        }
        if (kw_start && text == std::string_view(kw_start, kw_len)) {
            return keyword_table[i].kind;
        }
    }
    return TokenKind::IDENTIFIER;
}

// ============================================================================
// Number scanning (integer and float literals)
// ============================================================================
Token Lexer::scanNumber() {
    bool is_float = false;

    // Handle hex prefix 0x / 0X
    if (peek() == '0' && (peekNext() == 'x' || peekNext() == 'X')) {
        advance(); // consume '0'
        advance(); // consume 'x'/'X'

        if (isAtEnd() || !CharSet::isHexDigit(static_cast<unsigned char>(peek()))) {
            return errorToken("expected hexadecimal digit after '0x'");
        }

        while (!isAtEnd() && CharSet::isHexDigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        tryConsumeTypeSuffix(is_float);
        return makeToken(TokenKind::INT_LITERAL);
    }

    // Handle binary prefix 0b / 0B
    if (peek() == '0' && (peekNext() == 'b' || peekNext() == 'B')) {
        advance(); // consume '0'
        advance(); // consume 'b'/'B'

        if (isAtEnd() || !CharSet::isBinDigit(static_cast<unsigned char>(peek()))) {
            return errorToken("expected binary digit after '0b'");
        }

        while (!isAtEnd() && CharSet::isBinDigit(static_cast<unsigned char>(peek()))) {
            advance();
        }

        tryConsumeTypeSuffix(is_float);
        return makeToken(TokenKind::INT_LITERAL);
    }

    // Decimal integer or float
    while (!isAtEnd() && CharSet::isDigit(static_cast<unsigned char>(peek()))) {
        advance();
    }

    // Decimal point: must be followed by a digit to be a float
    if (peek() == '.' && !isAtEnd() &&
        current_ + 1 < src_len_ && CharSet::isDigit(static_cast<unsigned char>(src_data_[current_ + 1]))) {
        is_float = true;
        advance(); // consume '.'
        while (!isAtEnd() && CharSet::isDigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    // Exponent: e/E followed by optional +/- and digits
    if (!isAtEnd() && (peek() == 'e' || peek() == 'E')) {
        is_float = true;
        advance(); // consume 'e'/'E'
        if (!isAtEnd() && (peek() == '+' || peek() == '-')) {
            advance();
        }
        if (isAtEnd() || !CharSet::isDigit(static_cast<unsigned char>(peek()))) {
            return errorToken("expected digit in float exponent");
        }
        while (!isAtEnd() && CharSet::isDigit(static_cast<unsigned char>(peek()))) {
            advance();
        }
    }

    // Check for type suffix (f32, f64, i32, u8, etc.)
    if (!isAtEnd() && CharSet::isAlpha(static_cast<unsigned char>(peek()))) {
        tryConsumeTypeSuffix(is_float);
    }

    return makeToken(is_float ? TokenKind::FLOAT_LITERAL : TokenKind::INT_LITERAL);
}

bool Lexer::matchesSuffix(const char* suffix, size_t len) const {
    if (current_ + len > src_len_) return false;
    if (std::memcmp(src_data_ + current_, suffix, len) != 0) return false;
    // Ensure the suffix is not part of a longer identifier
    if (current_ + len < src_len_ && CharSet::isIdentCont(static_cast<unsigned char>(src_data_[current_ + len]))) {
        return false;
    }
    return true;
}

void Lexer::tryConsumeTypeSuffix(bool& is_float) {
    // Known integer suffixes (longest first to avoid partial matches)
    static const struct { const char* suffix; size_t len; } int_suffixes[] = {
        {"usize", 5}, {"isize", 5}, {"u64", 3}, {"i64", 3},
        {"u32", 3}, {"i32", 3}, {"u16", 3}, {"i16", 3},
        {"u8", 2}, {"i8", 2}
    };
    // Known float suffixes
    static const struct { const char* suffix; size_t len; } float_suffixes[] = {
        {"f64", 3}, {"f32", 3}
    };

    // Try integer suffixes first
    for (const auto& s : int_suffixes) {
        if (matchesSuffix(s.suffix, s.len)) {
            current_ += s.len;
            // Update column
            col_ += static_cast<uint32_t>(s.len);
            return;
        }
    }

    // Try float suffixes
    for (const auto& s : float_suffixes) {
        if (matchesSuffix(s.suffix, s.len)) {
            is_float = true;
            current_ += s.len;
            col_ += static_cast<uint32_t>(s.len);
            return;
        }
    }

    // No known suffix matched — the number token ends here.
}

// ============================================================================
// String literal scanning
// ============================================================================
Token Lexer::scanString() {
    advance(); // consume opening "

    while (!isAtEnd() && peek() != '"') {
        if (peek() == '\n') {
            return errorToken("unterminated string literal");
        }
        if (peek() == '\\') {
            advance(); // consume backslash
            if (!isAtEnd()) advance(); // consume escaped character
        } else {
            advance();
        }
    }

    if (isAtEnd()) {
        return errorToken("unterminated string literal");
    }

    advance(); // consume closing "
    return makeToken(TokenKind::STRING_LITERAL);
}

// ============================================================================
// Char literal scanning
// ============================================================================
Token Lexer::scanChar() {
    advance(); // consume opening '

    if (isAtEnd()) {
        return errorToken("unterminated char literal");
    }

    if (peek() == '\\') {
        advance(); // consume backslash
        if (!isAtEnd()) advance(); // consume escaped character
    } else {
        advance(); // consume the character
    }

    if (isAtEnd() || peek() != '\'') {
        return errorToken("unterminated char literal");
    }

    advance(); // consume closing '
    return makeToken(TokenKind::CHAR_LITERAL);
}

// ============================================================================
// Token construction helpers — zero-copy via string_view
// ============================================================================
Token Lexer::makeToken(TokenKind kind) const {
    std::string_view text(src_data_ + start_, current_ - start_);
    return Token(kind, text, token_start_line_, token_start_col_, filename_ptr_.get());
}

Token Lexer::errorToken(const std::string& msg) {
    diagnostics_.push_back({token_start_line_, token_start_col_, msg});
    has_errors_ = true;
    std::string_view text(src_data_ + start_, current_ - start_);
    return Token(TokenKind::UNKNOWN, text, token_start_line_,
                 token_start_col_, filename_ptr_.get());
}

} // namespace tether
