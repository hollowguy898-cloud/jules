// ============================================================================
// Speed Benchmark for the Jules Compiler
//
// This benchmark measures the performance of each compilation phase:
//   1. Lexing throughput (bytes/ms, tokens/ms)
//   2. Parsing throughput (tokens/ms, AST nodes/ms)
//   3. Semantic analysis (declarations/ms)
//   4. CFG building (functions/ms)
//   5. Borrow checking (CFGs/ms)
//   6. IR generation (bytes of IR/ms)
//   7. Full pipeline (source bytes/ms)
//
// We generate synthetic .jl source of varying sizes to measure scaling.
// ============================================================================
#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/SemanticAnalyzer.h"
#include "cfg/CFG.h"
#include "borrowck/BorrowChecker.h"
#include "codegen/IRGenerator.h"

#include <iostream>
#include <fstream>
#include <chrono>
#include <sstream>
#include <iomanip>
#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>

using namespace tether;
using hrclock_t = std::chrono::high_resolution_clock;
using ms_t = std::chrono::duration<double, std::milli>;

// ============================================================================
// Generate synthetic Jules source of a given size (number of functions)
// ============================================================================
static std::string generateSource(int num_functions, int stmts_per_fn) {
    std::ostringstream src;
    src << "// Auto-generated benchmark source (" << num_functions
        << " functions, " << stmts_per_fn << " stmts each)\n\n";

    for (int i = 0; i < num_functions; ++i) {
        src << "fn bench_" << i << "() i64 {\n";
        src << "    val x: i64 = " << i << ";\n";
        src << "    var acc: i64 = 0;\n";
        for (int j = 0; j < stmts_per_fn; ++j) {
            src << "    acc = acc + x * " << (j + 1) << ";\n";
        }
        src << "    return acc;\n";
        src << "}\n\n";
    }
    return src.str();
}

// Generate source with struct types
static std::string generateStructSource(int num_structs, int fields_per_struct) {
    std::ostringstream src;
    for (int i = 0; i < num_structs; ++i) {
        src << "struct Point" << i << " {\n";
        for (int j = 0; j < fields_per_struct; ++j) {
            src << "    field" << j << ": f64";
            if (j + 1 < fields_per_struct) src << ",";
            src << "\n";
        }
        src << "}\n\n";
    }
    // Add a function using each struct
    for (int i = 0; i < num_structs; ++i) {
        src << "fn use_point" << i << "() f64 {\n";
        src << "    val p: Point" << i << " = Point" << i << " {\n";
        for (int j = 0; j < fields_per_struct; ++j) {
            src << "        .field" << j << " = " << (j + 1) << ".0";
            if (j + 1 < fields_per_struct) src << ",";
            src << "\n";
        }
        src << "    };\n";
        src << "    return 1.0;\n";
        src << "}\n\n";
    }
    return src.str();
}

// ============================================================================
// Benchmark result
// ============================================================================
struct BenchResult {
    std::string name;
    double time_ms;
    size_t input_size;
    double throughput; // bytes/ms or items/ms
};

// ============================================================================
// Run a single benchmark iteration, returns time in ms
// ============================================================================
static double benchmarkLexer(const std::string& source, const std::string& filename,
                              std::vector<Token>& tokens) {
    auto start = hrclock_t::now();
    Lexer lexer(source, filename);
    tokens = lexer.tokenize();
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

static double benchmarkParser(std::vector<Token> tokens, TypeTable& type_table,
                               Program& program,
                               std::unordered_map<const ASTNode*, std::string>& type_ann,
                               std::unordered_map<std::string, std::string>& param_ann) {
    auto start = hrclock_t::now();
    Parser parser(std::move(tokens), type_table);
    program = parser.parse();
    type_ann = parser.typeAnnotations();
    param_ann = parser.paramTypeAnnotations();
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

static double benchmarkSema(Program& program, TypeTable& type_table,
                             const std::unordered_map<const ASTNode*, std::string>& type_ann,
                             const std::unordered_map<std::string, std::string>& param_ann) {
    auto start = hrclock_t::now();
    SemanticAnalyzer analyzer(type_table);
    analyzer.analyze(program, type_ann, param_ann);
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

static double benchmarkCFG(Program& program,
                            std::vector<std::unique_ptr<CFG>>& cfgs) {
    auto start = hrclock_t::now();
    CFGBuilder builder;
    cfgs.clear();
    for (auto& tl : program) {
        if (tl && tl->getKind() == NodeKind::FnDecl) {
            auto& fn = static_cast<FnDecl&>(*tl);
            if (fn.body()) {
                auto cfg = builder.build(fn);
                if (cfg) cfgs.push_back(std::move(cfg));
            }
        }
    }
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

static double benchmarkBorrowCheck(std::vector<std::unique_ptr<CFG>>& cfgs) {
    auto start = hrclock_t::now();
    BorrowChecker checker;
    checker.checkAll(cfgs);
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

static double benchmarkIRGen(Program& program, TypeTable& type_table,
                              std::string& ir_text) {
    auto start = hrclock_t::now();
    IRGenerator gen(program, type_table);
    ir_text = gen.generate();
    auto end = hrclock_t::now();
    return ms_t(end - start).count();
}

// ============================================================================
// Print results
// ============================================================================
static void printResults(const std::vector<BenchResult>& results) {
    std::cout << "\n+--------------------------------------------------------------+\n";
    std::cout << "|           JULES COMPILER - SPEED BENCHMARK RESULTS           |\n";
    std::cout << "+--------------------------------------------------------------+\n";
    std::cout << "| Phase              | Time (ms)  | Input Size | Throughput    |\n";
    std::cout << "+--------------------+------------+------------+--------------+\n";

    for (const auto& r : results) {
        std::cout << "| " << std::left << std::setw(18) << r.name << "| "
                  << std::right << std::setw(10) << std::fixed << std::setprecision(2) << r.time_ms << " | "
                  << std::setw(10) << r.input_size << " | "
                  << std::setw(12) << std::fixed << std::setprecision(1) << r.throughput << " |\n";
    }

    std::cout << "+--------------------------------------------------------------+\n\n";
}

// ============================================================================
// Main benchmark driver
// ============================================================================
int main() {
    const int WARMUP = 3;
    const int ITERATIONS = 10;

    std::cout << "+--------------------------------------------------------------+\n";
    std::cout << "|       JULES COMPILER - COMPREHENSIVE SPEED BENCHMARK         |\n";
    std::cout << "+--------------------------------------------------------------+\n\n";

    // ---- Benchmark configurations ----
    struct BenchConfig {
        std::string label;
        int num_functions;
        int stmts_per_fn;
        int num_structs;
        int fields_per_struct;
        bool use_structs;
    };

    std::vector<BenchConfig> configs = {
        {"Small (10 fn, 5 stmts)", 10, 5, 0, 0, false},
        {"Medium (100 fn, 10 stmts)", 100, 10, 0, 0, false},
        {"Large (500 fn, 20 stmts)", 500, 20, 0, 0, false},
        {"XL (1000 fn, 50 stmts)", 1000, 50, 0, 0, false},
        {"Structs (50 structs, 8 fields)", 0, 0, 50, 8, true},
    };

    for (const auto& cfg : configs) {
        std::cout << "--- " << cfg.label << " ---\n";

        // Generate source
        std::string source;
        if (cfg.use_structs) {
            source = generateStructSource(cfg.num_structs, cfg.fields_per_struct);
        } else {
            source = generateSource(cfg.num_functions, cfg.stmts_per_fn);
        }
        size_t source_bytes = source.size();

        // Warmup
        for (int w = 0; w < WARMUP; ++w) {
            TypeTable tt;
            std::vector<Token> tok;
            benchmarkLexer(source, "bench.jl", tok);
        }

        // ---- Measure each phase ----
        double lex_ms = 0, parse_ms = 0, sema_ms = 0, cfg_ms = 0, borrow_ms = 0, ir_ms = 0;
        double total_ms = 0;
        size_t token_count = 0, ir_size = 0;

        for (int i = 0; i < ITERATIONS; ++i) {
            TypeTable type_table;

            // Phase 1: Lexing
            std::vector<Token> tokens;
            double t_lex = benchmarkLexer(source, "bench.jl", tokens);
            token_count = tokens.size();
            lex_ms += t_lex;

            // Phase 2: Parsing
            Program program;
            std::unordered_map<const ASTNode*, std::string> type_ann;
            std::unordered_map<std::string, std::string> param_ann;
            double t_parse = benchmarkParser(std::move(tokens), type_table,
                                              program, type_ann, param_ann);
            parse_ms += t_parse;

            // Phase 3: Semantic analysis
            double t_sema = benchmarkSema(program, type_table, type_ann, param_ann);
            sema_ms += t_sema;

            // Phase 4: CFG building
            std::vector<std::unique_ptr<CFG>> cfgs;
            double t_cfg = benchmarkCFG(program, cfgs);
            cfg_ms += t_cfg;

            // Phase 5: Borrow checking
            double t_borrow = benchmarkBorrowCheck(cfgs);
            borrow_ms += t_borrow;

            // Phase 6: IR generation
            std::string ir_text;
            double t_ir = benchmarkIRGen(program, type_table, ir_text);
            ir_size = ir_text.size();
            ir_ms += t_ir;

            total_ms += t_lex + t_parse + t_sema + t_cfg + t_borrow + t_ir;
        }

        // Average
        lex_ms /= ITERATIONS;
        parse_ms /= ITERATIONS;
        sema_ms /= ITERATIONS;
        cfg_ms /= ITERATIONS;
        borrow_ms /= ITERATIONS;
        ir_ms /= ITERATIONS;
        total_ms /= ITERATIONS;

        size_t decl_count = cfg.use_structs ? cfg.num_structs * 2 : cfg.num_functions;

        std::vector<BenchResult> results = {
            {"Lexing", lex_ms, source_bytes, source_bytes / lex_ms},
            {"Parsing", parse_ms, token_count, token_count / parse_ms},
            {"Semantic Analysis", sema_ms, decl_count, decl_count / sema_ms},
            {"CFG Building", cfg_ms, decl_count, decl_count / cfg_ms},
            {"Borrow Checking", borrow_ms, decl_count, decl_count / borrow_ms},
            {"IR Generation", ir_ms, ir_size, ir_size / ir_ms},
            {"TOTAL PIPELINE", total_ms, source_bytes, source_bytes / total_ms},
        };

        printResults(results);
    }

    // ---- Stress test: find the throughput ceiling ----
    std::cout << "--- THROUGHPUT CEILING TEST ---\n\n";
    std::cout << "Generating a very large source file (2000 functions, 100 stmts each)...\n";

    std::string stress_source = generateSource(2000, 100);
    size_t stress_bytes = stress_source.size();
    std::cout << "  Source size: " << stress_bytes << " bytes ("
              << stress_bytes / 1024.0 << " KB)\n\n";

    TypeTable type_table;
    std::vector<Token> tokens;

    double t_lex = benchmarkLexer(stress_source, "stress.jl", tokens);
    size_t n_tokens = tokens.size();

    Program program;
    std::unordered_map<const ASTNode*, std::string> type_ann;
    std::unordered_map<std::string, std::string> param_ann;
    double t_parse = benchmarkParser(std::move(tokens), type_table,
                                      program, type_ann, param_ann);

    double t_sema = benchmarkSema(program, type_table, type_ann, param_ann);

    std::vector<std::unique_ptr<CFG>> cfgs;
    double t_cfg = benchmarkCFG(program, cfgs);

    double t_borrow = benchmarkBorrowCheck(cfgs);

    std::string ir_text;
    double t_ir = benchmarkIRGen(program, type_table, ir_text);

    double t_total = t_lex + t_parse + t_sema + t_cfg + t_borrow + t_ir;

    std::cout << "  Stress Test Results:\n";
    std::cout << "  +---------------------+--------------+-----------------+\n";
    std::cout << "  | Phase               | Time (ms)    | Throughput      |\n";
    std::cout << "  +---------------------+--------------+-----------------+\n";

    auto printRow = [&](const char* name, double ms, const char* unit, double throughput) {
        char line[256];
        snprintf(line, sizeof(line), "  | %-19s | %10.2f   | %9.1f %s |",
                 name, ms, throughput, unit);
        std::cout << line << "\n";
    };

    printRow("Lexing", t_lex, "KB/s", (stress_bytes / 1024.0) / (t_lex / 1000.0));
    printRow("Parsing", t_parse, "tok/s", n_tokens / (t_parse / 1000.0));
    printRow("Semantic Analysis", t_sema, "decl/s", 2000.0 / (t_sema / 1000.0));
    printRow("CFG Building", t_cfg, "fn/s", 2000.0 / (t_cfg / 1000.0));
    printRow("Borrow Checking", t_borrow, "fn/s", 2000.0 / (t_borrow / 1000.0));
    printRow("IR Generation", t_ir, "KB/s", (ir_text.size() / 1024.0) / (t_ir / 1000.0));
    printRow("TOTAL PIPELINE", t_total, "KB/s", (stress_bytes / 1024.0) / (t_total / 1000.0));

    std::cout << "  +---------------------+--------------+-----------------+\n\n";

    std::cout << "  Source: " << stress_bytes << " bytes | "
              << n_tokens << " tokens | "
              << "2000 functions | "
              << ir_text.size() << " bytes IR output\n\n";

    std::cout << "Benchmark complete.\n";
    return 0;
}
