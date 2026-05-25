#pragma once

#include "lexer/Token.h"
#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "cfg/CFG.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>

namespace jules {

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
//   5. Building CFGs for each function
//   6. Running the borrow checker
//   7. Generating LLVM IR
//   8. Writing the .ll file
//   9. (Optionally) invoking clang/llc to compile .ll to .o/.s
//  10. (Optionally) linking to produce an executable
//
// Each phase reports errors and aborts if there are failures.
// ============================================================================
class Driver {
public:
    // -----------------------------------------------------------------------
    // Constructor
    //
    // input_file:  path to the .jl source file
    // output_file: path for the output (empty string = derive from input)
    // opt_level:   optimization level 0-3
    // emit_type:   what to emit (object, assembly, IR, executable)
    // verbose:     print each compilation phase as it runs
    // -----------------------------------------------------------------------
    Driver(const std::string& input_file,
           const std::string& output_file,
           int opt_level,
           EmitType emit_type,
           bool verbose);

    // -----------------------------------------------------------------------
    // Run the full compilation pipeline.
    // Returns true on success, false on error.
    // -----------------------------------------------------------------------
    bool compile();

    // -----------------------------------------------------------------------
    // Access the error message (set when compile() returns false)
    // -----------------------------------------------------------------------
    const std::string& errorMessage() const { return error_message_; }

private:
    // -----------------------------------------------------------------------
    // Pipeline phases (each returns true on success)
    // -----------------------------------------------------------------------
    bool readSource();
    bool runLexer();
    bool runParser();
    bool runSemanticAnalysis();
    bool runCFGBuilding();
    bool runBorrowChecking();
    bool runIRGeneration();
    bool writeIRFile();
    bool runBackend();          // Compile .ll to .o/.s via clang/llc
    bool runLinker();           // Link .o + runtime to produce executable

    // -----------------------------------------------------------------------
    // Utility helpers
    // -----------------------------------------------------------------------

    // Find an executable on PATH. Returns empty string if not found.
    static std::string findExecutable(const std::string& name);

    // Find clang on the system
    std::string findClang() const;

    // Find llc on the system
    std::string findLlc() const;

    // Derive output path from input path and emit type
    std::string deriveOutputPath() const;

    // Remove a file (used for intermediate cleanup)
    static bool removeFile(const std::string& path);

    // -----------------------------------------------------------------------
    // Configuration
    // -----------------------------------------------------------------------
    std::string input_file_;
    std::string output_file_;
    int opt_level_;            // 0-3
    EmitType emit_type_;
    bool verbose_;

    // -----------------------------------------------------------------------
    // Intermediate data (populated during compilation)
    // -----------------------------------------------------------------------
    std::string source_text_;                                    // Phase 1
    std::vector<Token> tokens_;                                  // Phase 2
    Program program_;                                            // Phase 3
    TypeTable type_table_;                                       // Shared type table
    std::unordered_map<const ASTNode*, std::string> type_annotations_; // From parser
    std::unordered_map<std::string, std::string> param_type_annotations_; // From parser

    std::vector<std::unique_ptr<CFG>> cfgs_;                     // Phase 5: per-function CFGs

    std::string ir_text_;                                        // Phase 7
    std::string ir_file_path_;                                   // Phase 8
    std::string obj_file_path_;                                  // Phase 9

    // -----------------------------------------------------------------------
    // Error state
    // -----------------------------------------------------------------------
    std::string error_message_;
};

} // namespace jules
