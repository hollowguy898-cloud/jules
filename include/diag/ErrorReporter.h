#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <sstream>
#include <algorithm>

namespace jules {

// ============================================================================
// Diagnostic Level
// ============================================================================
enum class DiagLevel : uint8_t {
    Error,
    Warning,
    Note,
    Help
};

// ============================================================================
// Color codes for terminal output
// ============================================================================
namespace colors {
    inline const char* reset()   { return "\033[0m"; }
    inline const char* red()     { return "\033[31m"; }
    inline const char* green()   { return "\033[32m"; }
    inline const char* yellow()  { return "\033[33m"; }
    inline const char* blue()    { return "\033[34m"; }
    inline const char* cyan()    { return "\033[36m"; }
    inline const char* bold()    { return "\033[1m"; }
}

// ============================================================================
// SourceSpan - represents a range in source code
// ============================================================================
struct SourceSpan {
    uint32_t start_line;
    uint32_t start_col;
    uint32_t end_line;
    uint32_t end_col;
    std::string filename;

    SourceSpan()
        : start_line(0), start_col(0), end_line(0), end_col(0) {}

    SourceSpan(uint32_t sl, uint32_t sc, uint32_t el, uint32_t ec, std::string fn)
        : start_line(sl), start_col(sc), end_line(el), end_col(ec)
        , filename(std::move(fn)) {}

    bool isValid() const { return start_line > 0; }
};

// ============================================================================
// Diagnostic - a single compiler diagnostic message
// ============================================================================
struct Diagnostic {
    DiagLevel level;
    SourceSpan span;
    std::string message;
    std::string code;       // e.g. "E0001" for error codes
    std::vector<std::string> notes;  // Additional note lines

    Diagnostic(DiagLevel l, SourceSpan s, std::string msg, std::string c = "")
        : level(l), span(std::move(s)), message(std::move(msg)), code(std::move(c)) {}
};

// ============================================================================
// ErrorReporter - collects and formats beautiful error diagnostics
//
// Features:
//   - Source snippet display with caret pointers
//   - Color-coded output (errors red, warnings yellow, notes cyan)
//   - Error codes (e.g. E0001, W0001)
//   - Multiple notes per error
//   - Error count tracking for "collect all errors" mode
//   - Source line extraction from stored source text
// ============================================================================
class ErrorReporter {
public:
    ErrorReporter() : has_errors_(false), use_color_(true), source_(nullptr) {}

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    void setUseColor(bool use) { use_color_ = use; }
    void setSource(const std::string* source) { source_ = source; }

    // -----------------------------------------------------------------------
    // Report diagnostics
    // -----------------------------------------------------------------------
    void error(const SourceSpan& span, const std::string& msg, const std::string& code = "");
    void warning(const SourceSpan& span, const std::string& msg, const std::string& code = "");
    void note(const SourceSpan& span, const std::string& msg);
    void help(const SourceSpan& span, const std::string& msg);

    // -----------------------------------------------------------------------
    // Query
    // -----------------------------------------------------------------------
    bool hasErrors() const { return has_errors_; }
    size_t errorCount() const { return error_count_; }
    size_t warningCount() const { return warning_count_; }
    const std::vector<Diagnostic>& diagnostics() const { return diagnostics_; }

    // -----------------------------------------------------------------------
    // Output
    // -----------------------------------------------------------------------
    void printAll(std::ostream& os) const;
    void clear();

    // -----------------------------------------------------------------------
    // Source line extraction
    // -----------------------------------------------------------------------
    std::string getSourceLine(uint32_t line) const;

    // -----------------------------------------------------------------------
    // Format a single diagnostic as a beautiful string
    // -----------------------------------------------------------------------
    std::string formatDiagnostic(const Diagnostic& diag) const;

private:
    std::string levelPrefix(DiagLevel level) const;
    std::string levelColor(DiagLevel level) const;
    std::string formatSourceSnippet(const SourceSpan& span) const;
    std::string caretLine(uint32_t col, uint32_t width) const;

    std::vector<Diagnostic> diagnostics_;
    bool has_errors_;
    bool use_color_;
    size_t error_count_ = 0;
    size_t warning_count_ = 0;
    const std::string* source_;  // Non-owning pointer to source text
};

} // namespace jules
