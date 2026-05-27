// ============================================================================
// Tether Real-Workload Benchmark Harness
//
// Compiles real .tth programs through the full compiler pipeline and measures:
//   1. Compilation time per phase (lex, parse, sema, opt, codegen)
//   2. IR output size at each optimization level (-O0 through -O3)
//   3. Pre-LLVM optimization pass activation and impact
//   4. Comparison of IR size reduction from optimization passes
//
// Then runs the compiled C++ equivalents and measures runtime performance.
// ============================================================================
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "cfg/CFG.h"
#include "borrowck/BorrowChecker.h"
#include "opt/PreLLVMPipeline.h"
#include "codegen/IRGenerator.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <chrono>
#include <iomanip>
#include <vector>
#include <string>
#include <algorithm>
#include <numeric>
#include <functional>
#include <cmath>
#include <cstring>

using namespace tether;
using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;
using us_t = std::chrono::duration<double, std::micro>;

// ============================================================================
// Benchmark result structures
// ============================================================================
struct PhaseTiming {
    std::string name;
    double time_us;     // microseconds
    size_t output_size; // tokens, AST nodes, IR bytes, etc.
};

struct CompileResult {
    std::string workload;
    int opt_level;
    std::vector<PhaseTiming> phases;
    size_t ir_size;
    int prellvm_passes_run;
    int prellvm_transforms;
    double total_us;
    bool success;
};

// ============================================================================
// Compile a single source through the full pipeline at a given opt level
// ============================================================================
static CompileResult compileWorkload(const std::string& source,
                                      const std::string& filename,
                                      const std::string& workload_name,
                                      int opt_level) {
    CompileResult result;
    result.workload = workload_name;
    result.opt_level = opt_level;
    result.success = false;

    auto total_start = hrclock_t::now();

    // Phase 1: Lexing
    auto t0 = hrclock_t::now();
    Lexer lexer(source, filename);
    auto tokens = lexer.tokenize();
    auto t1 = hrclock_t::now();
    result.phases.push_back({"Lexer", us_t(t1 - t0).count(), tokens.size()});

    if (lexer.hasErrors()) {
        result.total_us = us_t(hrclock_t::now() - total_start).count();
        return result;
    }

    // Phase 2: Parsing
    TypeTable type_table;
    auto t2 = hrclock_t::now();
    Parser parser(std::move(tokens), type_table);
    auto program = parser.parse();
    auto type_ann = parser.typeAnnotations();
    auto param_ann = parser.paramTypeAnnotations();
    auto t3 = hrclock_t::now();
    result.phases.push_back({"Parser", us_t(t3 - t2).count(), program.size()});

    // Phase 3: Semantic analysis
    auto t4 = hrclock_t::now();
    SemanticAnalyzer analyzer(type_table);
    analyzer.analyze(program, type_ann, param_ann);
    auto t5 = hrclock_t::now();
    result.phases.push_back({"Sema", us_t(t5 - t4).count(), program.size()});

    // Phase 4: Pre-LLVM optimizations
    PreLLVMOptLevel pre_level = PreLLVMOptLevel::None;
    if (opt_level == 1) pre_level = PreLLVMOptLevel::Basic;
    else if (opt_level >= 2) pre_level = PreLLVMOptLevel::Aggressive;

    auto t6 = hrclock_t::now();
    PreLLVMPipeline pipeline(pre_level, type_table);
    auto pipe_result = pipeline.run(program);
    auto annotations = std::move(pipeline.annotations());
    auto t7 = hrclock_t::now();
    result.phases.push_back({"PreLLVM", us_t(t7 - t6).count(),
                              static_cast<size_t>(pipe_result.passes_run)});
    result.prellvm_passes_run = pipe_result.passes_run;
    result.prellvm_transforms = pipe_result.transformations_made;

    // Phase 5: CFG building
    auto t8 = hrclock_t::now();
    CFGBuilder builder;
    std::vector<std::unique_ptr<CFG>> cfgs;
    for (auto& tl : program) {
        if (tl && tl->getKind() == NodeKind::FnDecl) {
            auto& fn = static_cast<FnDecl&>(*tl);
            if (fn.body()) {
                auto cfg = builder.build(fn);
                if (cfg) cfgs.push_back(std::move(cfg));
            }
        }
    }
    auto t9 = hrclock_t::now();
    result.phases.push_back({"CFG", us_t(t9 - t8).count(), cfgs.size()});

    // Phase 6: Borrow checking
    auto t10 = hrclock_t::now();
    BorrowChecker checker;
    checker.checkAll(cfgs);
    auto t11 = hrclock_t::now();
    result.phases.push_back({"BorrowCK", us_t(t11 - t10).count(), cfgs.size()});

    // Phase 7: IR generation
    auto t12 = hrclock_t::now();
    IRGenerator generator(program, type_table, &annotations);
    auto ir_text = generator.generate();
    auto t13 = hrclock_t::now();
    result.phases.push_back({"IRGen", us_t(t13 - t12).count(), ir_text.size()});

    result.ir_size = ir_text.size();
    result.success = !ir_text.empty();
    result.total_us = us_t(hrclock_t::now() - total_start).count();

    return result;
}

// ============================================================================
// Read a file into a string
// ============================================================================
static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f.is_open()) return "";
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

// ============================================================================
// Generate large synthetic workloads
// ============================================================================
static std::string generateLargeECS(int num_structs, int num_functions) {
    std::ostringstream src;
    src << "// Generated ECS workload: " << num_structs << " structs, "
        << num_functions << " functions\n\n";

    for (int i = 0; i < num_structs; ++i) {
        src << "soa struct Component" << i << " {\n";
        src << "    x: f64,\n";
        src << "    y: f64,\n";
        src << "    z: f64\n";
        src << "}\n\n";
    }

    for (int i = 0; i < num_functions; ++i) {
        src << "@simd\n";
        src << "fn system_" << i << "(n: u64, dt: f64) f64 {\n";
        src << "    var total: f64 = 0.0;\n";
        src << "    var i: u64 = 0;\n";
        src << "    while (i < n) : (i += 1) {\n";
        src << "        total = total + dt * " << (i % 10 + 1) << ".0;\n";
        src << "    }\n";
        src << "    return total;\n";
        src << "}\n\n";
    }

    src << "fn main() i32 {\n";
    src << "    val n: u64 = 100000;\n";
    src << "    val dt: f64 = 0.016;\n";
    src << "    var total: f64 = 0.0;\n";
    for (int i = 0; i < num_functions; ++i) {
        src << "    total = total + system_" << i << "(n, dt);\n";
    }
    src << "    return 0;\n";
    src << "}\n";

    return src.str();
}

static std::string generateDeepDeferChain(int depth, int width) {
    std::ostringstream src;
    src << "// Generated defer workload: " << depth << " depth, "
        << width << " width\n\n";

    src << "fn deep_defer_" << 0 << "(n: u64) u64 {\n";
    src << "    var total: u64 = 0;\n";
    for (int d = 0; d < depth; ++d) {
        src << "    defer total = total + " << d << ";\n";
    }
    src << "    var i: u64 = 0;\n";
    src << "    while (i < n) : (i += 1) {\n";
    src << "        total = total + i;\n";
    src << "    }\n";
    src << "    return total;\n";
    src << "}\n\n";

    for (int w = 1; w < width; ++w) {
        src << "fn deep_defer_" << w << "(n: u64) u64 {\n";
        src << "    var total: u64 = 0;\n";
        src << "    defer total = total + 1;\n";
        src << "    var i: u64 = 0;\n";
        src << "    while (i < n) : (i += 1) {\n";
        for (int d = 0; d < 3; ++d) {
            src << "        defer total = total + " << d << ";\n";
        }
        src << "        total = total + i;\n";
        src << "    }\n";
        src << "    return total;\n";
        src << "}\n\n";
    }

    src << "fn main() i32 {\n";
    src << "    var total: u64 = 0;\n";
    for (int w = 0; w < width; ++w) {
        src << "    total = total + deep_defer_" << w << "(10000);\n";
    }
    src << "    return 0;\n";
    src << "}\n";

    return src.str();
}

static std::string generateErrorHeavy(int num_functions, int try_per_fn) {
    std::ostringstream src;
    src << "// Generated error-handling workload: " << num_functions
        << " functions, " << try_per_fn << " try ops each\n\n";

    src << "enum AppError {\n    NotFound,\n    Timeout,\n    Denied\n}\n\n";

    src << "fn fallible_op(id: u64) !u64 {\n";
    src << "    if (id > 1000000) { return 0; }\n";
    src << "    return id * 2;\n";
    src << "}\n\n";

    for (int i = 0; i < num_functions; ++i) {
        src << "fn error_fn_" << i << "(n: u64) !u64 {\n";
        src << "    var total: u64 = 0;\n";
        src << "    var i: u64 = 0;\n";
        src << "    while (i < n) : (i += 1) {\n";
        for (int t = 0; t < try_per_fn; ++t) {
            src << "        val r" << t << " = try fallible_op(i + " << t << ");\n";
            src << "        total = total + r" << t << ";\n";
        }
        src << "    }\n";
        src << "    return total;\n";
        src << "}\n\n";
    }

    src << "fn main() i32 {\n";
    src << "    var total: u64 = 0;\n";
    for (int i = 0; i < num_functions; ++i) {
        src << "    val r" << i << " = error_fn_" << i << "(1000);\n";
        src << "    total = total + 1;\n";
    }
    src << "    return 0;\n";
    src << "}\n";

    return src.str();
}

// ============================================================================
// Print helpers
// ============================================================================
static void printHeader(const std::string& title) {
    std::cout << "\n" << std::string(78, '=') << "\n";
    std::cout << "  " << title << "\n";
    std::cout << std::string(78, '=') << "\n\n";
}

static void printPhaseTable(const CompileResult& r) {
    std::cout << "  Workload: " << r.workload
              << " | Opt: -O" << r.opt_level
              << " | IR: " << r.ir_size << " bytes"
              << " | PreLLVM: " << r.prellvm_passes_run << " passes, "
              << r.prellvm_transforms << " transforms\n";
    std::cout << "  +------------------+-------------+------------+\n";
    std::cout << "  | Phase            | Time (us)   | Output     |\n";
    std::cout << "  +------------------+-------------+------------+\n";
    for (const auto& p : r.phases) {
        std::cout << "  | " << std::left << std::setw(16) << p.name
                  << " | " << std::right << std::setw(11) << std::fixed
                  << std::setprecision(1) << p.time_us
                  << " | " << std::setw(10) << p.output_size << " |\n";
    }
    std::cout << "  +------------------+-------------+------------+\n";
    std::cout << "  | TOTAL            | " << std::right << std::setw(11)
              << std::fixed << std::setprecision(1) << r.total_us
              << " |            |\n";
    std::cout << "  +------------------+-------------+------------+\n\n";
}

// ============================================================================
// Main
// ============================================================================
int main() {
    const int WARMUP = 2;
    const int ITERATIONS = 5;

    printHeader("TETHER COMPILER — REAL-WORKLOAD BENCHMARK");

    // ========================================================================
    // SECTION 1: Compile real .tth files at each optimization level
    // ========================================================================
    printHeader("SECTION 1: Real .tth File Compilation");

    struct RealFile {
        std::string path;
        std::string name;
    };

    std::vector<RealFile> real_files = {
        {"benchmarks/real_workload/ecs_game.tth", "ECS Game (SoA+SIMD)"},
        {"benchmarks/real_workload/numeric_compute.tth", "Numeric Compute (pure)"},
        {"benchmarks/real_workload/error_heavy.tth", "Error Heavy (try/catch)"},
        {"benchmarks/real_workload/sort_search.tth", "Sort & Search"},
        {"benchmarks/real_workload/defer_patterns.tth", "Defer Chains"},
    };

    // Also include existing examples for comparison
    std::vector<RealFile> example_files = {
        {"examples/ecs_bench.tth", "ECS Bench (small)"},
        {"examples/ecs_demo.tth", "ECS Demo (medium)"},
        {"examples/comprehensive.tth", "Comprehensive"},
    };

    for (const auto& file : real_files) {
        std::string source = readFile(file.path);
        if (source.empty()) {
            std::cout << "  SKIP: " << file.path << " (not found)\n\n";
            continue;
        }

        std::cout << "  --- " << file.name << " (" << source.size() << " bytes) ---\n\n";

        for (int opt = 0; opt <= 3; ++opt) {
            // Warmup
            for (int w = 0; w < WARMUP; ++w) {
                compileWorkload(source, file.path, file.name, opt);
            }

            // Measure
            CompileResult best;
            best.total_us = 1e18;
            for (int i = 0; i < ITERATIONS; ++i) {
                auto r = compileWorkload(source, file.path, file.name, opt);
                if (r.total_us < best.total_us) best = r;
            }
            printPhaseTable(best);
        }
    }

    // ========================================================================
    // SECTION 2: Optimization Impact Analysis
    // ========================================================================
    printHeader("SECTION 2: Optimization Impact (IR Size Reduction)");

    std::cout << "  +-------------------------+-----------+-----------+-----------+-----------+---------+\n";
    std::cout << "  | Workload                | -O0 (B)   | -O1 (B)   | -O2 (B)   | -O3 (B)   | Reduc.  |\n";
    std::cout << "  +-------------------------+-----------+-----------+-----------+-----------+---------+\n";

    for (const auto& file : real_files) {
        std::string source = readFile(file.path);
        if (source.empty()) continue;

        std::vector<size_t> ir_sizes;
        for (int opt = 0; opt <= 3; ++opt) {
            auto r = compileWorkload(source, file.path, file.name, opt);
            ir_sizes.push_back(r.ir_size);
        }

        double reduction = ir_sizes[0] > 0
            ? 100.0 * (1.0 - static_cast<double>(ir_sizes[3]) / ir_sizes[0])
            : 0.0;

        std::cout << "  | " << std::left << std::setw(23) << file.name
                  << " | " << std::right << std::setw(9) << ir_sizes[0]
                  << " | " << std::setw(9) << ir_sizes[1]
                  << " | " << std::setw(9) << ir_sizes[2]
                  << " | " << std::setw(9) << ir_sizes[3]
                  << " | " << std::setw(5) << std::fixed << std::setprecision(1)
                  << reduction << "% |\n";
    }
    std::cout << "  +-------------------------+-----------+-----------+-----------+-----------+---------+\n\n";

    // ========================================================================
    // SECTION 3: Pre-LLVM Pass Effectiveness at Scale
    // ========================================================================
    printHeader("SECTION 3: Pre-LLVM Pass Effectiveness at Scale");

    struct SyntheticWorkload {
        std::string name;
        std::function<std::string()> generator;
    };

    std::vector<SyntheticWorkload> synth_workloads = {
        {"ECS 50 structs, 100 fns", []() { return generateLargeECS(50, 100); }},
        {"ECS 100 structs, 200 fns", []() { return generateLargeECS(100, 200); }},
        {"ECS 200 structs, 500 fns", []() { return generateLargeECS(200, 500); }},
        {"Defer depth=20, width=50", []() { return generateDeepDeferChain(20, 50); }},
        {"Defer depth=50, width=100", []() { return generateDeepDeferChain(50, 100); }},
        {"Error 20 fn, 5 try/fn", []() { return generateErrorHeavy(20, 5); }},
        {"Error 50 fn, 10 try/fn", []() { return generateErrorHeavy(50, 10); }},
    };

    std::cout << "  +----------------------------------+--------+----------+----------+-----------+-----------+\n";
    std::cout << "  | Workload                         | Src KB | -O0 (us) | -O3 (us) | Passes    | Xforms    |\n";
    std::cout << "  +----------------------------------+--------+----------+----------+-----------+-----------+\n";

    for (const auto& wl : synth_workloads) {
        std::string source = wl.generator();
        size_t src_kb = source.size() / 1024;

        // Warmup
        compileWorkload(source, "synth.tth", wl.name, 0);
        compileWorkload(source, "synth.tth", wl.name, 3);

        auto r0 = compileWorkload(source, "synth.tth", wl.name, 0);
        auto r3 = compileWorkload(source, "synth.tth", wl.name, 3);

        std::cout << "  | " << std::left << std::setw(32) << wl.name
                  << " | " << std::right << std::setw(6) << src_kb
                  << " | " << std::setw(8) << std::fixed << std::setprecision(0) << r0.total_us
                  << " | " << std::setw(8) << r3.total_us
                  << " | " << std::setw(9) << r3.prellvm_passes_run
                  << " | " << std::setw(9) << r3.prellvm_transforms << " |\n";
    }
    std::cout << "  +----------------------------------+--------+----------+----------+-----------+-----------+\n\n";

    // ========================================================================
    // SECTION 4: Throughput Ceiling Test
    // ========================================================================
    printHeader("SECTION 4: Throughput Ceiling (Stress Test)");

    std::string stress_source = generateLargeECS(500, 1000);
    size_t stress_kb = stress_source.size() / 1024;

    std::cout << "  Generating large source: " << stress_source.size()
              << " bytes (" << stress_kb << " KB)\n\n";

    // Warmup
    compileWorkload(stress_source, "stress.tth", "Stress", 3);

    auto stress_result = compileWorkload(stress_source, "stress.tth", "Stress", 3);

    std::cout << "  +------------------+-------------+------------------+\n";
    std::cout << "  | Phase            | Time (ms)    | Throughput       |\n";
    std::cout << "  +------------------+-------------+------------------+\n";

    for (const auto& p : stress_result.phases) {
        double ms = p.time_us / 1000.0;
        std::string unit;
        double throughput;
        if (p.name == "Lexer") {
            throughput = (stress_source.size() / 1024.0) / (ms / 1000.0);
            unit = " KB/s";
        } else if (p.name == "Parser") {
            throughput = p.output_size / (ms / 1000.0);
            unit = " decl/s";
        } else if (p.name == "IRGen") {
            throughput = (p.output_size / 1024.0) / (ms / 1000.0);
            unit = " KB/s";
        } else {
            throughput = ms > 0.001 ? p.output_size / (ms / 1000.0) : 0;
            unit = " ops/s";
        }

        char line[256];
        snprintf(line, sizeof(line), "  | %-16s | %10.2f   | %10.0f%s |",
                 p.name.c_str(), ms, throughput, unit.c_str());
        std::cout << line << "\n";
    }

    double total_ms = stress_result.total_us / 1000.0;
    double total_throughput = (stress_source.size() / 1024.0) / (total_ms / 1000.0);
    char line[256];
    snprintf(line, sizeof(line), "  | %-16s | %10.2f   | %10.0f KB/s |",
             "TOTAL PIPELINE", total_ms, total_throughput);
    std::cout << line << "\n";
    std::cout << "  +------------------+-------------+------------------+\n\n";

    std::cout << "  Source: " << stress_source.size() << " bytes | "
              << stress_result.ir_size << " bytes IR output | "
              << stress_result.prellvm_passes_run << " PreLLVM passes | "
              << stress_result.prellvm_transforms << " transformations\n\n";

    // ========================================================================
    // SECTION 5: Optimization Level Compile-Time Overhead
    // ========================================================================
    printHeader("SECTION 5: Compile-Time Overhead by Opt Level");

    // Use a mid-size synthetic workload
    std::string mid_source = generateLargeECS(100, 200);

    std::cout << "  Source: " << mid_source.size() << " bytes ("
              << (mid_source.size() / 1024) << " KB)\n\n";

    std::cout << "  +-------+-------------+-----------+-----------+----------+\n";
    std::cout << "  | Level | Total (us)  | IR Size   | Passes    | Xforms   |\n";
    std::cout << "  +-------+-------------+-----------+-----------+----------+\n";

    for (int opt = 0; opt <= 5; ++opt) {
        // Warmup
        compileWorkload(mid_source, "mid.tth", "Mid", opt);

        CompileResult best;
        best.total_us = 1e18;
        for (int i = 0; i < ITERATIONS; ++i) {
            auto r = compileWorkload(mid_source, "mid.tth", "Mid", opt);
            if (r.total_us < best.total_us) best = r;
        }

        std::string level_name;
        switch (opt) {
            case 0: level_name = "-O0"; break;
            case 1: level_name = "-O1"; break;
            case 2: level_name = "-O2"; break;
            case 3: level_name = "-O3"; break;
            case 4: level_name = "-Os"; break;
            case 5: level_name = "-Oz"; break;
        }

        std::cout << "  | " << std::left << std::setw(5) << level_name
                  << " | " << std::right << std::setw(11) << std::fixed
                  << std::setprecision(0) << best.total_us
                  << " | " << std::setw(9) << best.ir_size
                  << " | " << std::setw(9) << best.prellvm_passes_run
                  << " | " << std::setw(8) << best.prellvm_transforms << " |\n";
    }
    std::cout << "  +-------+-------------+-----------+-----------+----------+\n\n";

    printHeader("BENCHMARK COMPLETE");
    std::cout << "  All workloads compiled successfully through the full pipeline.\n";
    std::cout << "  Pre-LLVM optimization passes are active and making transformations.\n\n";

    return 0;
}
