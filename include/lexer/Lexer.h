#pragma once

#include "lexer/Token.h"

#include <string>
#include <vector>

namespace jules {

// ============================================================================
// Diagnostic reported by the lexer
// ============================================================================
struct LexDiagnostic {
    uint32_t line;
    uint32_t col;
    std::string filename;
    std::string message;
};

// ============================================================================
// Lexer - converts source text into a stream of tokens
// ============================================================================
class Lexer {
public:
    explicit Lexer(const std::string& source, std::string filename);

    /// Tokenize the entire source and return the token list (including EOF).
    std::vector<Token> tokenize();

    /// Return any diagnostics accumulated during tokenization.
    const std::vector<LexDiagnostic>& diagnostics() const { return diagnostics_; }

    /// Whether any errors were reported.
    bool hasErrors() const { return has_errors_; }

private:
    // -----------------------------------------------------------------------
    // Character access
    // -----------------------------------------------------------------------
    char peek() const;
    char peekNext() const;       // one character ahead of peek()
    char peekNextNext() const;   // two characters ahead of peek()
    char advance();              // consume current char, return it
    bool match(char expected);   // if current char matches, advance and return true
    bool isAtEnd() const;

    // -----------------------------------------------------------------------
    // Whitespace / comments
    // -----------------------------------------------------------------------
    void skipWhitespace();
    void skipLineComment();
    void skipBlockComment();

    // -----------------------------------------------------------------------
    // Token scanning
    // -----------------------------------------------------------------------
    Token scanToken();
    Token scanIdentifierOrKeyword();
    Token scanNumber();
    Token scanString();
    Token scanChar();

    // Helpers for scanning
    TokenKind lookupKeyword(const std::string& text) const;
    bool isHexDigit(char c) const;
    bool isBinaryDigit(char c) const;
    bool isDigit(char c) const;
    bool isAlpha(char c) const;
    bool isAlphanumeric(char c) const;
    bool isIdentifierStart(char c) const;
    bool isIdentifierContinue(char c) const;
    void tryConsumeTypeSuffix(bool& is_float);
    bool matchesSuffix(const char* suffix) const;

    // -----------------------------------------------------------------------
    // Token construction helpers
    // -----------------------------------------------------------------------
    Token makeToken(TokenKind kind) const;
    Token errorToken(const std::string& msg);

    // -----------------------------------------------------------------------
    // Source state
    // -----------------------------------------------------------------------
    std::string source_;
    std::string filename_;
    size_t start_;             // start offset of current token
    size_t current_;           // current read position
    uint32_t line_;            // current line (1-based)
    uint32_t col_;             // current column (1-based)
    uint32_t token_start_line_;
    uint32_t token_start_col_;

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    std::vector<LexDiagnostic> diagnostics_;
    bool has_errors_;
};

} // namespace jules
