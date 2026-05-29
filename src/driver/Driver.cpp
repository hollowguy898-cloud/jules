#include "driver/Driver.h"

#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "cfg/CFG.h"
#include "borrowck/BorrowChecker.h"
#include "opt/PreLLVMPipeline.h"
#include "codegen/IRGenerator.h"
#include "metadata/MetaTypes.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <cstdlib>
#include <cstdio>
#include <algorithm>
#include <filesystem>

namespace tether {

// ============================================================================
// Constructor
// ============================================================================
Driver::Driver(const std::string& input_file,
               const std::string& output_file,
               int opt_level,
               EmitType emit_type,
               bool verbose,
               bool profile_generate,
               const std::string& profile_use,
               const std::string& target_triple)
    : input_file_(input_file)
    , output_file_(output_file)
    , opt_level_(std::clamp(opt_level, 0, 5))
    , emit_type_(emit_type)
    , verbose_(verbose)
    , profile_generate_(profile_generate)
    , profile_use_(profile_use)
    , target_triple_(target_triple)
{
    if (output_file_.empty()) {
        output_file_ = deriveOutputPath();
    }
}

// ============================================================================
// Main compilation entry point
// ============================================================================
bool Driver::compile() {
    if (verbose_) {
        std::cerr << "[tether] Compiling: " << input_file_ << std::endl;
    }

    // Error-resilient compilation: always run all phases to collect
    // as many errors as possible. Only halt before final linking.
    readSource();
    runLexer();
    runParser();
    runSemanticAnalysis();
    runPreLLVMOptimizations();
    runCFGBuilding();
    runBorrowChecking();
    runIRGeneration();
    writeIRFile();

    // Collect all errors from every phase
    // Note: tokens_ may be empty after being moved to the parser - that's not an error
    bool has_errors = parser_had_errors_
        || sema_had_errors_
        || borrowck_had_errors_
        || ir_text_.empty();

    // If there were parse errors, report but continue
    // If there were semantic errors, report but continue
    if (has_errors) {
        // Print all collected errors from every phase
        for (const auto& err : parse_errors_) {
            std::cerr << err.loc.toString() << ": error: " << err.message << std::endl;
        }
        for (const auto& diag : sema_reporter_.diagnostics()) {
            if (diag.level == DiagLevel::Error) {
                std::cerr << sema_reporter_.formatDiagnostic(diag) << std::endl;
            }
        }
        for (const auto& err : borrowck_errors_) {
            std::cerr << err.toString() << std::endl;
        }
        if (!error_message_.empty()) {
            std::cerr << error_message_ << std::endl;
        }
        if (verbose_) {
            std::cerr << "[tether] Compilation failed with errors" << std::endl;
        }
        return false;  // Never link or produce output with errors
    }

    // For IR-only output, we are done after writing the .ll file
    if (emit_type_ == EmitType::IR) {
        if (verbose_) {
            std::cerr << "[tether] IR written to: " << output_file_ << std::endl;
        }
        return true;
    }

    if (!runBackend())           return false;

    // For object and assembly output, we are done after the backend
    if (emit_type_ == EmitType::Object || emit_type_ == EmitType::Assembly) {
        if (verbose_) {
            std::cerr << "[tether] Output written to: " << output_file_ << std::endl;
        }
        return true;
    }

    // For executable output, we also need to link
    if (!runLinker())            return false;

    if (verbose_) {
        std::cerr << "[tether] Executable written to: " << output_file_ << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 1: Read source file
// ============================================================================
bool Driver::readSource() {
    if (verbose_) {
        std::cerr << "[tether] Phase 1: Reading source file..." << std::endl;
    }

    std::ifstream file(input_file_);
    if (!file.is_open()) {
        error_message_ = "error: cannot open input file: " + input_file_;
        std::cerr << error_message_ << std::endl;
        return false;
    }

    std::ostringstream ss;
    ss << file.rdbuf();
    source_text_ = ss.str();

    if (source_text_.empty()) {
        error_message_ = "error: input file is empty: " + input_file_;
        std::cerr << error_message_ << std::endl;
        return false;
    }

    if (verbose_) {
        std::cerr << "[tether]   Read " << source_text_.size() << " bytes" << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 2: Lexical analysis
// ============================================================================
bool Driver::runLexer() {
    if (verbose_) {
        std::cerr << "[tether] Phase 2: Lexical analysis..." << std::endl;
    }

    Lexer lexer(source_text_, input_file_);
    tokens_ = lexer.tokenize();
    filename_ptr_ = lexer.filenamePtr();  // Keep filename alive for Token string_views

    if (lexer.hasErrors()) {
        for (const auto& diag : lexer.diagnostics()) {
            std::cerr << input_file_ << ":" << diag.line << ":" << diag.col
                      << ": error: " << diag.message << std::endl;
        }
        error_message_ = "error: lexical analysis failed";
        return false;
    }

    if (verbose_) {
        std::cerr << "[tether]   Produced " << tokens_.size() << " tokens" << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 3: Parsing
// ============================================================================
bool Driver::runParser() {
    if (verbose_) {
        std::cerr << "[tether] Phase 3: Parsing..." << std::endl;
    }

    Parser parser(std::move(tokens_), type_table_);
    program_ = parser.parse();
    type_annotations_ = parser.typeAnnotations();
    param_type_annotations_ = parser.paramTypeAnnotations();

    // Error-resilient: record errors but don't abort
    parser_had_errors_ = parser.hasErrors();
    parse_errors_ = parser.errors();

    if (parser_had_errors_) {
        // Don't abort - continue with type checking
        // Errors will be printed at the end of compilation
    }

    if (verbose_) {
        std::cerr << "[tether]   Parsed " << program_.size() << " top-level declarations" << std::endl;
    }

    return true;  // Always continue for error-resilient compilation
}

// ============================================================================
// Phase 4: Semantic analysis
// ============================================================================
bool Driver::runSemanticAnalysis() {
    if (verbose_) {
        std::cerr << "[tether] Phase 4: Semantic analysis..." << std::endl;
    }

    SemanticAnalyzer analyzer(type_table_);
    analyzer.setErrorReporter(&sema_reporter_);
    analyzer.analyze(program_, type_annotations_, param_type_annotations_);

    // Error-resilient: record errors but don't abort
    sema_had_errors_ = analyzer.hasErrors();

    if (sema_had_errors_) {
        // Don't abort - continue with CFG building and borrow checking
    }

    // Print warnings immediately (they are not errors)
    if (verbose_) {
        for (const auto& diag : sema_reporter_.diagnostics()) {
            if (diag.level == DiagLevel::Warning) {
                std::cerr << sema_reporter_.formatDiagnostic(diag) << std::endl;
            }
        }
    }

    if (verbose_ && !sema_had_errors_) {
        std::cerr << "[tether]   Semantic analysis passed" << std::endl;
    } else if (verbose_ && sema_had_errors_) {
        std::cerr << "[tether]   Semantic analysis completed with errors (continuing)" << std::endl;
    }

    return true;  // Always continue for error-resilient compilation
}

// ============================================================================
// Phase 4.5: Pre-LLVM optimizations (UNIFIED pipeline)
//
// The unified pipeline includes both the MetadataEngine analysis layers
// (L1-L6) AND the pre-LLVM transform passes in a single correct order.
// No more two-track duplication or stale metadata.
// ============================================================================
bool Driver::runPreLLVMOptimizations() {
    if (verbose_) {
        std::cerr << "[tether] Phase 4.5: Pre-LLVM optimizations (unified pipeline)..." << std::endl;
    }

    // Map LLVM opt level to Tether pre-LLVM opt level
    PreLLVMOptLevel pre_level = PreLLVMOptLevel::None;
    if (opt_level_ == 1 || opt_level_ == 4 || opt_level_ == 5) {
        pre_level = PreLLVMOptLevel::Basic;
    } else if (opt_level_ >= 2) {
        pre_level = PreLLVMOptLevel::Aggressive;
    }

    if (pre_level == PreLLVMOptLevel::None) {
        if (verbose_) {
            std::cerr << "[tether]   Pre-LLVM optimizations skipped (-O0)" << std::endl;
        }
        return true;
    }

    pipeline_ = std::make_unique<PreLLVMPipeline>(pre_level, type_table_);

    // Load profile data if specified
    if (!profile_use_.empty()) {
        if (!pipeline_->loadProfile(profile_use_)) {
            if (verbose_) {
                std::cerr << "[tether]   Warning: could not load profile data from: "
                          << profile_use_ << std::endl;
            }
        }
    }

    auto result = pipeline_->run(program_);

    if (verbose_) {
        std::cerr << "[tether]   Ran " << result.passes_run
                  << " unified pipeline passes, "
                  << result.transformations_made << " transformations made"
                  << std::endl;
        for (const auto& log_entry : result.pass_log) {
            std::cerr << "[tether]     " << log_entry << std::endl;
        }
    }

    return true;
}

// ============================================================================
// Phase 6: CFG building
// ============================================================================
bool Driver::runCFGBuilding() {
    if (verbose_) {
        std::cerr << "[tether] Phase 5: Building CFGs..." << std::endl;
    }

    CFGBuilder builder;
    cfgs_.clear();

    for (auto& tl : program_) {
        if (tl->getKind() == NodeKind::FnDecl) {
            auto& fn = static_cast<FnDecl&>(*tl);
            // Only build CFGs for functions that have a body
            if (fn.body()) {
                auto cfg = builder.build(fn);
                if (cfg) {
                    cfgs_.push_back(std::move(cfg));
                }
            }
        }
    }

    if (verbose_) {
        std::cerr << "[tether]   Built " << cfgs_.size() << " CFGs" << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 6: Borrow checking
// ============================================================================
bool Driver::runBorrowChecking() {
    if (verbose_) {
        std::cerr << "[tether] Phase 6: Borrow checking..." << std::endl;
    }

    BorrowChecker checker;
    checker.checkAll(cfgs_);

    // Error-resilient: record errors but don't abort
    borrowck_had_errors_ = checker.hasErrors();
    borrowck_errors_ = checker.errors();

    if (borrowck_had_errors_) {
        // Don't abort - continue with IR generation
    }

    // Plumb borrow checker's noalias results into the MetadataMap
    // so IRGenerator can emit noalias attributes on function parameters
    if (pipeline_ && !checker.noAliasParams().empty()) {
        MetadataMap& meta = pipeline_->metadata();
        for (const auto& na : checker.noAliasParams()) {
            // Find the corresponding FnParam in the program AST
            for (const auto& tl : program_) {
                if (auto* fn = dyn_cast<FnDecl>(tl.get())) {
                    for (auto& param : fn->params()) {
                        if (param.name == na.param_name) {
                            auto& nm = meta.getOrCreate(&param);
                            nm.aliasing = AliasingKind::NoAlias;
                            nm.is_restrict = true;
                            break;
                        }
                    }
                }
            }
        }
    }

    if (verbose_ && !borrowck_had_errors_) {
        std::cerr << "[tether]   Borrow checking passed" << std::endl;
    } else if (verbose_ && borrowck_had_errors_) {
        std::cerr << "[tether]   Borrow checking completed with errors (continuing)" << std::endl;
    }

    return true;  // Always continue for error-resilient compilation
}

// ============================================================================
// Phase 7: IR generation
// ============================================================================
bool Driver::runIRGeneration() {
    if (verbose_) {
        std::cerr << "[tether] Phase 7: IR generation..." << std::endl;
    }

    IRGenerator generator(program_, type_table_,
                           pipeline_ ? &pipeline_->metadata() : nullptr);
    ir_text_ = generator.generate();

    if (ir_text_.empty()) {
        error_message_ = "error: IR generation produced no output";
        std::cerr << error_message_ << std::endl;
        return false;
    }

    if (verbose_) {
        std::cerr << "[tether]   Generated " << ir_text_.size() << " bytes of LLVM IR" << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 8: Write .ll file
// ============================================================================
bool Driver::writeIRFile() {
    if (verbose_) {
        std::cerr << "[tether] Phase 8: Writing IR file..." << std::endl;
    }

    // Derive the .ll path from the output file
    // The .ll file is always written (it's the main output for --emit-ir,
    // and an intermediate for other emit types)
    if (emit_type_ == EmitType::IR) {
        ir_file_path_ = output_file_;
    } else {
        // Place the .ll file next to the output, with .ll extension
        namespace fs = std::filesystem;
        fs::path out_p(output_file_);
        ir_file_path_ = (out_p.parent_path() / out_p.stem()).string() + ".ll";
    }

    std::ofstream out(ir_file_path_, std::ios::binary);
    if (!out.is_open()) {
        error_message_ = "error: cannot write IR file: " + ir_file_path_;
        std::cerr << error_message_ << std::endl;
        return false;
    }

    out << ir_text_;
    out.close();

    if (verbose_) {
        std::cerr << "[tether]   Wrote " << ir_file_path_ << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 9: Backend compilation (LLVM IR -> object/assembly)
// ============================================================================
bool Driver::runBackend() {
    if (verbose_) {
        std::cerr << "[tether] Phase 9: Backend compilation..." << std::endl;
    }

    std::string clang = findClang();
    std::string llc = findLlc();

    // Prefer clang for compiling .ll to .o or .s, since it also links
    // the standard C library implicitly. Fall back to llc if clang is absent.
    std::string compiler = clang;
    bool using_clang = !clang.empty();

    if (compiler.empty()) {
        compiler = llc;
        if (compiler.empty()) {
            error_message_ = "error: neither clang nor llc found on PATH; "
                             "cannot compile LLVM IR to native code";
            std::cerr << error_message_ << std::endl;
            return false;
        }
    }

    // Build the command
    std::ostringstream cmd;

    if (using_clang) {
        // clang can directly compile .ll files
        cmd << compiler;
        // Map Tether opt levels to LLVM opt flags:
        // 0=-O0, 1=-O1, 2=-O2, 3=-O3, 4=-Os, 5=-Oz
        switch (opt_level_) {
            case 0: cmd << " -O0"; break;
            case 1: cmd << " -O1"; break;
            case 2: cmd << " -O2"; break;
            case 3: cmd << " -O3"; break;
            case 4: cmd << " -Os"; break;
            case 5: cmd << " -Oz"; break;
            default: cmd << " -O2"; break;
        }

        if (emit_type_ == EmitType::Assembly) {
            cmd << " -S";
        }

        cmd << " -c " << ir_file_path_;

        if (emit_type_ == EmitType::Assembly) {
            cmd << " -o " << output_file_;
        } else {
            // Object output: compile to .o first, then we'll link if needed
            namespace fs = std::filesystem;
            fs::path out_p(output_file_);
            obj_file_path_ = (out_p.parent_path() / out_p.stem()).string() + ".o";
            cmd << " -o " << obj_file_path_;
        }
    } else {
        // Using llc
        cmd << compiler;
        switch (opt_level_) {
            case 0: cmd << " -O0"; break;
            case 1: cmd << " -O1"; break;
            case 2: cmd << " -O2"; break;
            case 3: cmd << " -O3"; break;
            case 4: cmd << " -Os"; break;
            case 5: cmd << " -Oz"; break;
            default: cmd << " -O2"; break;
        }

        if (emit_type_ == EmitType::Assembly) {
            cmd << " --filetype=asm";
        } else {
            cmd << " --filetype=obj";
        }

        if (emit_type_ == EmitType::Assembly) {
            cmd << " -o " << output_file_;
        } else {
            namespace fs = std::filesystem;
            fs::path out_p(output_file_);
            obj_file_path_ = (out_p.parent_path() / out_p.stem()).string() + ".o";
            cmd << " -o " << obj_file_path_;
        }

        cmd << " " << ir_file_path_;
    }

    if (verbose_) {
        std::cerr << "[tether]   Running: " << cmd.str() << std::endl;
    }

    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        error_message_ = "error: backend compilation failed (exit code " +
                         std::to_string(ret) + ")";
        std::cerr << error_message_ << std::endl;
        return false;
    }

    // If the emit type is Object, copy/rename the .o to the output path
    if (emit_type_ == EmitType::Object && !obj_file_path_.empty()) {
        if (obj_file_path_ != output_file_) {
            namespace fs = std::filesystem;
            std::error_code ec;
            fs::copy_file(obj_file_path_, output_file_,
                          fs::copy_options::overwrite_existing, ec);
            if (ec) {
                error_message_ = "error: cannot copy object file to output: " +
                                 ec.message();
                std::cerr << error_message_ << std::endl;
                return false;
            }
            // Remove intermediate .o
            removeFile(obj_file_path_);
        }
    }

    // Clean up the intermediate .ll file if we're not emitting IR
    if (emit_type_ != EmitType::IR) {
        removeFile(ir_file_path_);
    }

    if (verbose_) {
        std::cerr << "[tether]   Backend compilation succeeded" << std::endl;
    }

    return true;
}

// ============================================================================
// Phase 10: Linking
// ============================================================================
bool Driver::runLinker() {
    if (verbose_) {
        std::cerr << "[tether] Phase 10: Linking..." << std::endl;
    }

    std::string clang = findClang();

    if (clang.empty()) {
        error_message_ = "error: clang not found on PATH; cannot link executable";
        std::cerr << error_message_ << std::endl;
        return false;
    }

    // Determine the object file path
    namespace fs = std::filesystem;
    fs::path out_p(output_file_);
    std::string obj_path;
    if (!obj_file_path_.empty()) {
        obj_path = obj_file_path_;
    } else {
        obj_path = (out_p.parent_path() / out_p.stem()).string() + ".o";
    }

    // Look for the runtime library
    // Search in several locations relative to the compiler executable
    std::string runtime_lib;
    const char* exe_path_str = std::getenv("JULES_COMPILER_PATH");
    std::vector<std::string> search_paths;

    if (exe_path_str) {
        fs::path exe_dir = fs::path(exe_path_str).parent_path();
        search_paths.push_back((exe_dir / ".." / "lib" / "libjules_runtime.a").string());
        search_paths.push_back((exe_dir / "lib" / "libjules_runtime.a").string());
    }

    // Also search relative to current working directory
    search_paths.push_back("runtime/libjules_runtime.a");
    search_paths.push_back("../runtime/libjules_runtime.a");
    search_paths.push_back("lib/libjules_runtime.a");

    for (const auto& p : search_paths) {
        if (fs::exists(p)) {
            runtime_lib = p;
            break;
        }
    }

    // Build the link command
    std::ostringstream cmd;
    cmd << clang;
    // Map Tether opt levels to LLVM link flags
    switch (opt_level_) {
        case 0: cmd << " -O0"; break;
        case 1: cmd << " -O1"; break;
        case 2: cmd << " -O2"; break;
        case 3: cmd << " -O3"; break;
        case 4: cmd << " -Os"; break;
        case 5: cmd << " -Oz"; break;
        default: cmd << " -O2"; break;
    }

    // PGO: profile-generate / profile-use flags
    if (profile_generate_) {
        cmd << " -fprofile-generate";
    }
    if (!profile_use_.empty()) {
        cmd << " -fprofile-use=" << profile_use_;
    }

    // Target triple for cross-compilation
    if (!target_triple_.empty()) {
        cmd << " -target " << target_triple_;
    }

    cmd << " " << obj_path;

    if (!runtime_lib.empty()) {
        cmd << " " << runtime_lib;
    }

    cmd << " -lm";  // Link math library
    cmd << " -o " << output_file_;

    if (verbose_) {
        std::cerr << "[tether]   Running: " << cmd.str() << std::endl;
    }

    int ret = std::system(cmd.str().c_str());
    if (ret != 0) {
        error_message_ = "error: linking failed (exit code " +
                         std::to_string(ret) + ")";
        std::cerr << error_message_ << std::endl;
        return false;
    }

    // Clean up intermediate object file
    removeFile(obj_path);

    if (verbose_) {
        std::cerr << "[tether]   Linking succeeded" << std::endl;
    }

    return true;
}

// ============================================================================
// Utility: find an executable on PATH
// ============================================================================
std::string Driver::findExecutable(const std::string& name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return "";

    std::string path_str(path_env);
    std::istringstream paths(path_str);
    std::string dir;

#ifdef _WIN32
    const char sep = ';';
    const char file_sep = '\\';
    const char* ext = ".exe";
#else
    const char sep = ':';
    const char file_sep = '/';
    const char* ext = "";
#endif

    while (std::getline(paths, dir, sep)) {
        if (dir.empty()) continue;
        std::string candidate = dir + file_sep + name + ext;

        // Check if the file exists and is executable
        std::ifstream test(candidate);
        if (test.is_open()) {
            test.close();
            return candidate;
        }
    }

    return "";
}

// ============================================================================
// Utility: find clang
// ============================================================================
std::string Driver::findClang() const {
    // Try versioned names first (common on Linux)
    static const char* clang_names[] = {
        "clang-18", "clang-17", "clang-16", "clang-15",
        "clang-14", "clang-13", "clang-12",
        "clang"
    };

    for (const char* name : clang_names) {
        std::string result = findExecutable(name);
        if (!result.empty()) return result;
    }

    return "";
}

// ============================================================================
// Utility: find llc
// ============================================================================
std::string Driver::findLlc() const {
    static const char* llc_names[] = {
        "llc-18", "llc-17", "llc-16", "llc-15",
        "llc-14", "llc-13", "llc-12",
        "llc"
    };

    for (const char* name : llc_names) {
        std::string result = findExecutable(name);
        if (!result.empty()) return result;
    }

    return "";
}

// ============================================================================
// Utility: derive output path from input and emit type
// ============================================================================
std::string Driver::deriveOutputPath() const {
    namespace fs = std::filesystem;

    fs::path in_p(input_file_);
    std::string stem = in_p.stem().string();

    switch (emit_type_) {
        case EmitType::IR:
            return stem + ".ll";
        case EmitType::Assembly:
            return stem + ".s";
        case EmitType::Object:
            return stem + ".o";
        case EmitType::Executable:
            return stem;
    }

    return stem + ".out";
}

// ============================================================================
// Utility: remove a file
// ============================================================================
bool Driver::removeFile(const std::string& path) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::remove(path, ec);
    return !ec;
}

} // namespace tether
