#pragma once

#include "lexer/Token.h"
#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "diag/ErrorReporter.h"
#include "sema/SemanticAnalyzer.h"
#include "cfg/CFG.h"
#include "borrowck/BorrowChecker.h"
#include "opt/PreLLVMPipeline.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace tether {

// ============================================================================
// EmitType - what kind of output the compiler should produce
// ============================================================================
enum class EmitType : uint8_t {
    Object,      // Compile to .o (object file)
    Assembly,    // Compile to .s (assembly)
    IR,          // Emit .ll (LLVM IR text)
    Executable   // Compile and link to an executable
};

// ============================================================================
// Driver - orchestrates the full compilation pipeline
//
// The driver is responsible for:
//   1. Reading the source file
//   2. Running the lexer
//   3. Running the parser
//   4. Running semantic analysis
//   5. Running the unified pre-LLVM optimization pipeline
//   6. Building CFGs for each function
//   7. Running the borrow checker
//   8. Generating LLVM IR
//   9. Writing the .ll file
//  10. (Optionally) invoking clang/llc to compile .ll to .o/.s
//  11. (Optionally) linking to produce an executable
//
// Each phase reports errors and aborts if there are failures.
// ============================================================================
class Driver {
public:
    // -----------------------------------------------------------------------
    // Constructor
    // -----------------------------------------------------------------------
    Driver(const std::string& input_file,
           const std::string& output_file,
           int opt_level,
           EmitType emit_type,
           bool verbose,
           bool profile_generate = false,
           const std::string& profile_use = "",
           const std::string& target_triple = "");

    // Run the full compilation pipeline.
    bool compile();

    // Access the error message (set when compile() returns false)
    const std::string& errorMessage() const { return error_message_; }

private:
    // Pipeline phases (each returns true on success)
    bool readSource();
    bool runLexer();
    bool runParser();
    bool runSemanticAnalysis();
    bool runPreLLVMOptimizations();  // Unified pipeline (analysis + transforms)
    bool runCFGBuilding();
    bool runBorrowChecking();
    bool runIRGeneration();
    bool writeIRFile();
    bool runBackend();
    bool runLinker();

    // Utility helpers
    static std::string findExecutable(const std::string& name);
    std::string findClang() const;
    std::string findLlc() const;
    std::string deriveOutputPath() const;
    static bool removeFile(const std::string& path);

    // Build the TetherAttrPass LLVM pass plugin shared library.
    // Returns the path to the built .so file, or empty string on failure.
    // If the plugin is already built and up-to-date, returns the existing path.
    // Uses clang and llvm-config to compile the plugin.
    std::string buildPassPlugin();

    // Find llvm-config on PATH (for building the pass plugin)
    static std::string findLlvmConfig();

    // Configuration
    std::string input_file_;
    std::string output_file_;
    int opt_level_;
    EmitType emit_type_;
    bool verbose_;
    bool profile_generate_;
    std::string profile_use_;
    std::string target_triple_;

    // Intermediate data
    std::string source_text_;
    std::shared_ptr<std::string> filename_ptr_;
    std::vector<Token> tokens_;
    Program program_;
    TypeTable type_table_;
    std::unordered_map<const ASTNode*, std::string> type_annotations_;
    std::unordered_map<std::string, std::string> param_type_annotations_;

    std::vector<std::unique_ptr<CFG>> cfgs_;

    std::string ir_text_;
    std::string ir_file_path_;
    std::string obj_file_path_;

    // Unified pre-LLVM optimization pipeline
    // Owns the MetadataMap and all analysis/transform layers
    std::unique_ptr<PreLLVMPipeline> pipeline_;

    // Error state
    std::string error_message_;
    bool parser_had_errors_ = false;
    bool sema_had_errors_ = false;
    bool borrowck_had_errors_ = false;
    std::vector<ParseError> parse_errors_;
    ErrorReporter sema_reporter_;
    std::vector<BorrowError> borrowck_errors_;

    // Cached path to the built TetherAttrPass plugin (.so)
    std::string pass_plugin_path_;
};

} // namespace tether
