#pragma once

#include "lexer/Token.h"

#include <string>
#include <vector>
#include <memory>

namespace tether {

// ============================================================================
// Diagnostic reported by the lexer
// ============================================================================
struct LexDiagnostic {
    uint32_t line;
    uint32_t col;
    std::string message;  // no per-diagnostic filename copy; use the Lexer's
};

// ============================================================================
// Lexer — optimized for throughput
//
// Key optimizations:
//   1. Character classification via 256-byte lookup table (no branching)
//   2. Tokens hold string_view into source_ (zero-copy tokenization)
//   3. Filename shared via const std::string* (one alloc per file)
//   4. Pre-reserved token vector based on source size
//   5. Direct pointer arithmetic for scan loops (fewer bounds checks)
//   6. Keyword lookup via FNV-1a hash + small perfect-hint table
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

    /// Return the filename (for error reporting in the Driver).
    const std::string& filename() const { return *filename_ptr_; }

    /// Return a shared_ptr to the filename string so callers can keep it alive.
    /// This is needed because Token stores raw pointers into this string.
    std::shared_ptr<std::string> filenamePtr() const { return filename_ptr_; }

private:
    // -----------------------------------------------------------------------
    // Character access — pointer-based for speed
    // -----------------------------------------------------------------------
    char peek() const;
    char peekNext() const;
    char peekNextNext() const;
    char advance();
    bool match(char expected);
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
    TokenKind lookupKeyword(std::string_view text) const;
    void tryConsumeTypeSuffix(bool& is_float);
    bool matchesSuffix(const char* suffix, size_t len) const;

    // -----------------------------------------------------------------------
    // Token construction helpers
    // -----------------------------------------------------------------------
    Token makeToken(TokenKind kind) const;
    Token errorToken(const std::string& msg);

    // -----------------------------------------------------------------------
    // Source state
    // -----------------------------------------------------------------------
    const std::string& source_;           // reference to external source string
    std::shared_ptr<std::string> filename_ptr_;  // shared filename for all tokens
    const char* src_data_;                // source_.data() cache
    size_t src_len_;                      // source_.size() cache
    size_t start_;                        // start offset of current token
    size_t current_;                      // current read position
    uint32_t line_;                       // current line (1-based)
    uint32_t col_;                        // current column (1-based)
    uint32_t token_start_line_;
    uint32_t token_start_col_;

    // -----------------------------------------------------------------------
    // Diagnostics
    // -----------------------------------------------------------------------
    std::vector<LexDiagnostic> diagnostics_;
    bool has_errors_;
};

// ============================================================================
// Character classification lookup table — branch-free
// ============================================================================
namespace CharSet {

struct CharInfo {
    bool is_digit : 1;
    bool is_hex_digit : 1;
    bool is_bin_digit : 1;
    bool is_alpha : 1;          // a-z, A-Z, _
    bool is_alnum : 1;          // alpha | digit
    bool is_ident_start : 1;    // alpha
    bool is_ident_cont : 1;     // alnum
    bool is_whitespace : 1;
};

extern const CharInfo char_table[256];

inline bool isDigit(unsigned char c)     { return char_table[c].is_digit; }
inline bool isHexDigit(unsigned char c)  { return char_table[c].is_hex_digit; }
inline bool isBinDigit(unsigned char c)  { return char_table[c].is_bin_digit; }
inline bool isAlpha(unsigned char c)     { return char_table[c].is_alpha; }
inline bool isAlnum(unsigned char c)     { return char_table[c].is_alnum; }
inline bool isIdentStart(unsigned char c){ return char_table[c].is_ident_start; }
inline bool isIdentCont(unsigned char c) { return char_table[c].is_ident_cont; }
inline bool isWhitespace(unsigned char c){ return char_table[c].is_whitespace; }

} // namespace CharSet

} // namespace tether
