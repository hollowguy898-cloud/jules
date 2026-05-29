#include "driver/Driver.h"

#include <iostream>
#include <string>
#include <vector>
#include <cstdlib>

using namespace tether;

// ============================================================================
// Print usage information
// ============================================================================
static void printUsage(const char* prog_name) {
    std::cerr
        << "Usage: " << prog_name << " [options] <input.tth>\n"
        << "\n"
        << "Options:\n"
        << "  -o <output>       Output file path\n"
        << "  -O0               No optimization (default)\n"
        << "  -O1               Basic optimization\n"
        << "  -O2               Standard optimization\n"
        << "  -O3               Aggressive optimization\n"
        << "  -Os               Optimize for size\n"
        << "  -Oz               Aggressive size optimization\n"
        << "  --emit-obj        Emit an object file (.o)\n"
        << "  --emit-asm        Emit assembly (.s)\n"
        << "  --emit-ir         Emit LLVM IR (.ll)\n"
        << "  --emit-exe        Emit an executable (default)\n"
        << "  --profile-generate  Generate PGO profile data (-fprofile-generate)\n"
        << "  --pgo-generate      Alias for --profile-generate\n"
        << "  --profile-use <path> Use PGO profile data (-fprofile-use)\n"
        << "  --pgo-use <path>     Alias for --profile-use\n"
        << "  --target <triple>  Cross-compilation target (x86_64, aarch64, riscv64, wasm32)\n"
        << "  -v                Verbose mode (print each phase)\n"
        << "  --help            Show this help message\n"
        << "\n"
        << "Examples:\n"
        << "  " << prog_name << " hello.tth                     # Compile to executable 'hello'\n"
        << "  " << prog_name << " -O2 -o hello hello.tth         # Compile with -O2\n"
        << "  " << prog_name << " --emit-ir hello.tth            # Emit LLVM IR to hello.ll\n"
        << "  " << prog_name << " --emit-asm -v hello.tth        # Emit assembly with verbose output\n"
        << "  " << prog_name << " --profile-generate hello.tth   # Generate PGO profile\n"
        << "  " << prog_name << " --pgo-generate hello.tth       # Same as --profile-generate\n"
        << "  " << prog_name << " --target aarch64 hello.tth     # Cross-compile for AArch64\n"
        << "  " << prog_name << " --pgo-use=profdata hello.tth   # Use PGO profile data\n"
        << "\n"
        << "PGO Quick Start:\n"
        << "  1. ./scripts/pgo.sh hello.tth            # One-command PGO cycle\n"
        << "  2. Or manually:\n"
        << "     a) " << prog_name << " --pgo-generate hello.tth    # Instrument\n"
        << "     b) LLVM_PROFILE_FILE=prof.raw ./hello   # Run workload\n"
        << "     c) llvm-profdata merge -o prof.profdata prof.raw\n"
        << "     d) " << prog_name << " --pgo-use=prof.profdata hello.tth  # Recompile\n"
        << std::endl;
}

// ============================================================================
// Parse command-line arguments
// ============================================================================
int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 1;
    }

    std::string input_file;
    std::string output_file;
    int opt_level = 0;
    EmitType emit_type = EmitType::Executable;  // default: produce an executable
    bool verbose = false;
    bool profile_generate = false;
    std::string profile_use;
    std::string target_triple;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];

        if (arg == "--help" || arg == "-h") {
            printUsage(argv[0]);
            return 0;
        }
        else if (arg == "-o") {
            if (i + 1 >= argc) {
                std::cerr << "error: -o requires an argument" << std::endl;
                return 1;
            }
            output_file = argv[++i];
        }
        else if (arg == "-O0") {
            opt_level = 0;
        }
        else if (arg == "-O1") {
            opt_level = 1;
        }
        else if (arg == "-O2") {
            opt_level = 2;
        }
        else if (arg == "-O3") {
            opt_level = 3;
        }
        else if (arg == "-Os") {
            opt_level = 4;
        }
        else if (arg == "-Oz") {
            opt_level = 5;
        }
        else if (arg == "--emit-obj") {
            emit_type = EmitType::Object;
        }
        else if (arg == "--emit-asm") {
            emit_type = EmitType::Assembly;
        }
        else if (arg == "--emit-ir") {
            emit_type = EmitType::IR;
        }
        else if (arg == "--emit-exe") {
            emit_type = EmitType::Executable;
        }
        else if (arg == "--profile-generate") {
            profile_generate = true;
        }
        else if (arg == "--pgo-generate") {
            // Alias for --profile-generate
            profile_generate = true;
        }
        else if (arg == "--profile-use") {
            if (i + 1 >= argc) {
                std::cerr << "error: --profile-use requires an argument" << std::endl;
                return 1;
            }
            profile_use = argv[++i];
        }
        else if (arg == "--pgo-use") {
            // Alias for --profile-use
            if (i + 1 >= argc) {
                std::cerr << "error: --pgo-use requires an argument" << std::endl;
                return 1;
            }
            profile_use = argv[++i];
        }
        else if (arg.rfind("--pgo-use=", 0) == 0) {
            // --pgo-use=<path> form
            profile_use = arg.substr(10);
        }
        else if (arg.rfind("--profile-use=", 0) == 0) {
            // --profile-use=<path> form
            profile_use = arg.substr(14);
        }
        else if (arg == "--target") {
            if (i + 1 >= argc) {
                std::cerr << "error: --target requires an argument" << std::endl;
                return 1;
            }
            target_triple = argv[++i];
        }
        else if (arg == "-v") {
            verbose = true;
        }
        else if (arg.size() > 0 && arg[0] == '-') {
            std::cerr << "error: unknown option: " << arg << std::endl;
            printUsage(argv[0]);
            return 1;
        }
        else {
            // Positional argument: input file
            if (!input_file.empty()) {
                std::cerr << "error: multiple input files specified ("
                          << input_file << " and " << arg << ")" << std::endl;
                return 1;
            }
            input_file = arg;
        }
    }

    // Validate that we have an input file
    if (input_file.empty()) {
        std::cerr << "error: no input file specified" << std::endl;
        printUsage(argv[0]);
        return 1;
    }

    // Validate target triple if specified
    if (!target_triple.empty()) {
        if (target_triple != "x86_64" && target_triple != "aarch64" &&
            target_triple != "riscv64" && target_triple != "wasm32") {
            std::cerr << "warning: unrecognized target triple '" << target_triple
                      << "'; passing to clang as-is" << std::endl;
        }
    }

    // Create the driver and run the compilation pipeline
    Driver driver(input_file, output_file, opt_level, emit_type, verbose,
                  profile_generate, profile_use, target_triple);

    bool success = driver.compile();
    if (!success) {
        if (!driver.errorMessage().empty()) {
            std::cerr << driver.errorMessage() << std::endl;
        }
        return 1;
    }

    return 0;
}
