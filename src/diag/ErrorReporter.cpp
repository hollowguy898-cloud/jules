#include "diag/ErrorReporter.h"

#include <iostream>
#include <sstream>
#include <algorithm>

namespace tether {

void ErrorReporter::error(const SourceSpan& span, const std::string& msg, const std::string& code) {
    diagnostics_.emplace_back(DiagLevel::Error, span, msg, code);
    has_errors_ = true;
    error_count_++;
}

void ErrorReporter::warning(const SourceSpan& span, const std::string& msg, const std::string& code) {
    diagnostics_.emplace_back(DiagLevel::Warning, span, msg, code);
    warning_count_++;
}

void ErrorReporter::note(const SourceSpan& span, const std::string& msg) {
    diagnostics_.emplace_back(DiagLevel::Note, span, msg);
}

void ErrorReporter::help(const SourceSpan& span, const std::string& msg) {
    diagnostics_.emplace_back(DiagLevel::Help, span, msg);
}

std::string ErrorReporter::getSourceLine(uint32_t line) const {
    if (!source_ || line == 0) return "";
    
    uint32_t current_line = 1;
    size_t start = 0;
    
    for (size_t i = 0; i < source_->size(); ++i) {
        if (current_line == line) {
            start = i;
            // Find end of line
            size_t end = source_->find('\n', i);
            if (end == std::string::npos) end = source_->size();
            return source_->substr(start, end - start);
        }
        if ((*source_)[i] == '\n') {
            current_line++;
        }
    }
    return "";
}

std::string ErrorReporter::levelPrefix(DiagLevel level) const {
    switch (level) {
        case DiagLevel::Error:   return "error";
        case DiagLevel::Warning: return "warning";
        case DiagLevel::Note:    return "note";
        case DiagLevel::Help:    return "help";
    }
    return "unknown";
}

std::string ErrorReporter::levelColor(DiagLevel level) const {
    if (!use_color_) return "";
    switch (level) {
        case DiagLevel::Error:   return colors::red();
        case DiagLevel::Warning: return colors::yellow();
        case DiagLevel::Note:    return colors::cyan();
        case DiagLevel::Help:    return colors::green();
    }
    return "";
}

std::string ErrorReporter::caretLine(uint32_t col, uint32_t width) const {
    std::string result;
    // Add spaces to reach the column
    for (uint32_t i = 1; i < col && i < 200; ++i) {
        result += ' ';
    }
    // Add carets for the width
    for (uint32_t i = 0; i < width && (col + i) < 200; ++i) {
        result += '^';
    }
    return result;
}

std::string ErrorReporter::formatSourceSnippet(const SourceSpan& span) const {
    if (!span.isValid()) return "";
    
    std::ostringstream ss;
    std::string line_text = getSourceLine(span.start_line);
    
    if (!line_text.empty()) {
        // Line number prefix with padding
        std::string line_num = std::to_string(span.start_line);
        ss << "  " << line_num << " | " << line_text << "\n";
        
        // Caret line
        uint32_t width = (span.end_col > span.start_col) 
                         ? (span.end_col - span.start_col) : 1;
        std::string carets = caretLine(span.start_col, width);
        ss << "  " << std::string(line_num.size(), ' ') << " | ";
        if (use_color_) ss << colors::bold() << colors::red();
        ss << carets;
        if (use_color_) ss << colors::reset();
        ss << "\n";
    }
    
    return ss.str();
}

std::string ErrorReporter::formatDiagnostic(const Diagnostic& diag) const {
    std::ostringstream ss;
    
    // File:line:col: level: message
    if (diag.span.isValid()) {
        ss << diag.span.filename << ":" << diag.span.start_line 
           << ":" << diag.span.start_col << ": ";
    }
    
    if (use_color_) ss << levelColor(diag.level);
    ss << levelPrefix(diag.level);
    if (!diag.code.empty()) ss << "[" << diag.code << "]";
    ss << ": ";
    if (use_color_) ss << colors::reset();
    
    if (use_color_ && diag.level == DiagLevel::Error) ss << colors::bold();
    ss << diag.message;
    if (use_color_ && diag.level == DiagLevel::Error) ss << colors::reset();
    ss << "\n";
    
    // Source snippet with caret
    ss << formatSourceSnippet(diag.span);
    
    // Notes
    for (const auto& note : diag.notes) {
        ss << "  ";
        if (use_color_) ss << colors::cyan();
        ss << "note: ";
        if (use_color_) ss << colors::reset();
        ss << note << "\n";
    }
    
    return ss.str();
}

void ErrorReporter::printAll(std::ostream& os) const {
    for (const auto& diag : diagnostics_) {
        os << formatDiagnostic(diag);
    }
    
    if (error_count_ > 0 || warning_count_ > 0) {
        os << "\n";
        if (use_color_) os << colors::bold();
        os << "Compilation ";
        if (error_count_ > 0) {
            if (use_color_) os << colors::red();
            os << "failed";
            if (use_color_) os << colors::reset() << colors::bold();
        } else {
            if (use_color_) os << colors::yellow();
            os << "completed with warnings";
            if (use_color_) os << colors::reset() << colors::bold();
        }
        os << ": ";
        if (error_count_ > 0) os << error_count_ << " error(s)";
        if (error_count_ > 0 && warning_count_ > 0) os << ", ";
        if (warning_count_ > 0) os << warning_count_ << " warning(s)";
        if (use_color_) os << colors::reset();
        os << "\n";
    }
}

void ErrorReporter::clear() {
    diagnostics_.clear();
    has_errors_ = false;
    error_count_ = 0;
    warning_count_ = 0;
}

} // namespace tether
