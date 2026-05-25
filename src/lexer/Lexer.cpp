#include "lexer/Lexer.h"

#include <cctype>
#include <cstring>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
Lexer::Lexer(const std::string& source, std::string filename)
    : source_(source)
    , filename_(std::move(filename))
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
    std::vector<Token> tokens;

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
    tokens.emplace_back(TokenKind::EOF_TOKEN, "", line_, col_, filename_);
    return tokens;
}

// ============================================================================
// Character access
// ============================================================================
char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source_[current_];
}

char Lexer::peekNext() const {
    if (current_ + 1 >= source_.size()) return '\0';
    return source_[current_ + 1];
}

char Lexer::peekNextNext() const {
    if (current_ + 2 >= source_.size()) return '\0';
    return source_[current_ + 2];
}

char Lexer::advance() {
    char c = source_[current_];
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
    if (source_[current_] != expected) return false;
    advance();
    return true;
}

bool Lexer::isAtEnd() const {
    return current_ >= source_.size();
}

// ============================================================================
// Whitespace / comments
// ============================================================================
void Lexer::skipWhitespace() {
    while (!isAtEnd()) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\t':
            case '\r':
            case '\n':
                advance();
                break;
            case '/':
                if (peekNext() == '/') {
                    skipLineComment();
                } else if (peekNext() == '*') {
                    skipBlockComment();
                } else {
                    return;
                }
                break;
            default:
                return;
        }
    }
}

void Lexer::skipLineComment() {
    // Consume the '//'
    advance();
    advance();
    // Consume until end of line or end of file
    while (!isAtEnd() && peek() != '\n') {
        advance();
    }
}

void Lexer::skipBlockComment() {
    // Consume the '/*'
    advance();
    advance();

    uint32_t nesting = 1;
    while (!isAtEnd() && nesting > 0) {
        if (peek() == '/' && peekNext() == '*') {
            advance();
            advance();
            nesting++;
        } else if (peek() == '*' && peekNext() == '/') {
            advance();
            advance();
            nesting--;
        } else {
            advance();
        }
    }

    if (nesting > 0) {
        diagnostics_.push_back({line_, col_, filename_,
            "unterminated block comment"});
        has_errors_ = true;
    }
}

// ============================================================================
// Character classification
// ============================================================================
bool Lexer::isDigit(char c) const {
    return c >= '0' && c <= '9';
}

bool Lexer::isHexDigit(char c) const {
    return isDigit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

bool Lexer::isBinaryDigit(char c) const {
    return c == '0' || c == '1';
}

bool Lexer::isAlpha(char c) const {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

bool Lexer::isAlphanumeric(char c) const {
    return isAlpha(c) || isDigit(c);
}

bool Lexer::isIdentifierStart(char c) const {
    return isAlpha(c);
}

bool Lexer::isIdentifierContinue(char c) const {
    return isAlphanumeric(c);
}

// ============================================================================
// Token scanning dispatch
// ============================================================================
Token Lexer::scanToken() {
    char c = peek();

    if (isIdentifierStart(c)) {
        return scanIdentifierOrKeyword();
    }

    if (isDigit(c)) {
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
// Identifier / keyword scanning
// ============================================================================
Token Lexer::scanIdentifierOrKeyword() {
    while (!isAtEnd() && isIdentifierContinue(peek())) {
        advance();
    }

    std::string text = source_.substr(start_, current_ - start_);
    TokenKind kind = lookupKeyword(text);
    return makeToken(kind);
}

TokenKind Lexer::lookupKeyword(const std::string& text) const {
    if (text.size() == 2) {
        if (text == "fn")  return TokenKind::KW_FN;
        if (text == "if")  return TokenKind::KW_IF;
        return TokenKind::IDENTIFIER;
    }
    if (text.size() == 3) {
        if (text == "val") return TokenKind::KW_VAL;
        if (text == "var") return TokenKind::KW_VAR;
        if (text == "mut") return TokenKind::KW_MUT;
        if (text == "soa") return TokenKind::KW_SOA;
        if (text == "try") return TokenKind::KW_TRY;
        return TokenKind::IDENTIFIER;
    }
    if (text.size() == 4) {
        if (text == "enum")   return TokenKind::KW_ENUM;
        if (text == "pure")   return TokenKind::KW_PURE;
        if (text == "else")   return TokenKind::KW_ELSE;
        if (text == "void")   return TokenKind::KW_VOID;
        if (text == "cast")   return TokenKind::KW_CAST;
        if (text == "true")   return TokenKind::KW_TRUE;
        return TokenKind::IDENTIFIER;
    }
    if (text.size() == 5) {
        if (text == "while")  return TokenKind::KW_WHILE;
        if (text == "defer")  return TokenKind::KW_DEFER;
        if (text == "break")  return TokenKind::KW_BREAK;
        if (text == "false")  return TokenKind::KW_FALSE;
        if (text == "align")  return TokenKind::KW_ALIGN;
        if (text == "catch")  return TokenKind::KW_CATCH;
        if (text == "yield")  return TokenKind::KW_YIELD;
        return TokenKind::IDENTIFIER;
    }
    if (text.size() == 6) {
        if (text == "struct") return TokenKind::KW_STRUCT;
        if (text == "unsafe") return TokenKind::KW_UNSAFE;
        if (text == "return") return TokenKind::KW_RETURN;
        if (text == "select") return TokenKind::KW_SELECT;
        if (text == "sizeof") return TokenKind::KW_SIZEOF;
        if (text == "import") return TokenKind::KW_IMPORT;
        if (text == "atomic") return TokenKind::KW_ATOMIC;
        if (text == "opaque") return TokenKind::KW_OPAQUE;
        return TokenKind::IDENTIFIER;
    }
    if (text.size() == 8) {
        if (text == "continue") return TokenKind::KW_CONTINUE;
        if (text == "errdefer") return TokenKind::KW_ERRDEFER;
        return TokenKind::IDENTIFIER;
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

        if (isAtEnd() || !isHexDigit(peek())) {
            return errorToken("expected hexadecimal digit after '0x'");
        }

        while (!isAtEnd() && isHexDigit(peek())) {
            advance();
        }

        // Check for integer type suffix after hex digits
        tryConsumeTypeSuffix(is_float);
        return makeToken(TokenKind::INT_LITERAL);
    }

    // Handle binary prefix 0b / 0B
    if (peek() == '0' && (peekNext() == 'b' || peekNext() == 'B')) {
        advance(); // consume '0'
        advance(); // consume 'b'/'B'

        if (isAtEnd() || !isBinaryDigit(peek())) {
            return errorToken("expected binary digit after '0b'");
        }

        while (!isAtEnd() && isBinaryDigit(peek())) {
            advance();
        }

        tryConsumeTypeSuffix(is_float);
        return makeToken(TokenKind::INT_LITERAL);
    }

    // Decimal integer or float
    while (!isAtEnd() && isDigit(peek())) {
        advance();
    }

    // Decimal point: must be followed by a digit to be a float
    if (peek() == '.' && !isAtEnd() &&
        current_ + 1 < source_.size() && isDigit(source_[current_ + 1])) {
        is_float = true;
        advance(); // consume '.'
        while (!isAtEnd() && isDigit(peek())) {
            advance();
        }
    }

    // Exponent: e/E followed by optional +/- and digits
    if (!isAtEnd() && (peek() == 'e' || peek() == 'E')) {
        // Only treat as exponent if there are digits before it
        is_float = true;
        advance(); // consume 'e'/'E'
        if (!isAtEnd() && (peek() == '+' || peek() == '-')) {
            advance();
        }
        if (isAtEnd() || !isDigit(peek())) {
            return errorToken("expected digit in float exponent");
        }
        while (!isAtEnd() && isDigit(peek())) {
            advance();
        }
    }

    // Check for type suffix (f32, f64, i32, u8, etc.)
    if (!isAtEnd() && isAlpha(peek())) {
        tryConsumeTypeSuffix(is_float);
    }

    return makeToken(is_float ? TokenKind::FLOAT_LITERAL : TokenKind::INT_LITERAL);
}

bool Lexer::matchesSuffix(const char* suffix) const {
    size_t len = std::strlen(suffix);
    if (current_ + len > source_.size()) return false;
    if (source_.compare(current_, len, suffix) != 0) return false;
    // Ensure the suffix is not part of a longer identifier
    if (current_ + len < source_.size() && isIdentifierContinue(source_[current_ + len])) {
        return false;
    }
    return true;
}

void Lexer::tryConsumeTypeSuffix(bool& is_float) {
    // Known integer suffixes (longest first to avoid partial matches)
    static const char* int_suffixes[] = {
        "usize", "isize", "u64", "i64", "u32", "i32", "u16", "i16", "u8", "i8"
    };
    // Known float suffixes
    static const char* float_suffixes[] = {
        "f64", "f32"
    };

    // Try integer suffixes first
    for (const char* suffix : int_suffixes) {
        if (matchesSuffix(suffix)) {
            size_t len = std::strlen(suffix);
            for (size_t i = 0; i < len; i++) advance();
            return;
        }
    }

    // Try float suffixes (these also make an integer token become float)
    for (const char* suffix : float_suffixes) {
        if (matchesSuffix(suffix)) {
            is_float = true;
            size_t len = std::strlen(suffix);
            for (size_t i = 0; i < len; i++) advance();
            return;
        }
    }

    // No known suffix matched — the number token ends here.
    // The alphabetic character will be the start of the next token.
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
// Token construction helpers
// ============================================================================
Token Lexer::makeToken(TokenKind kind) const {
    std::string text = source_.substr(start_, current_ - start_);
    return Token(kind, std::move(text), token_start_line_, token_start_col_, filename_);
}

Token Lexer::errorToken(const std::string& msg) {
    diagnostics_.push_back({token_start_line_, token_start_col_, filename_, msg});
    has_errors_ = true;
    std::string text = source_.substr(start_, current_ - start_);
    return Token(TokenKind::UNKNOWN, std::move(text), token_start_line_,
                 token_start_col_, filename_);
}

} // namespace tether
