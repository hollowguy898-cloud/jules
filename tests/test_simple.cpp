#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/Type.h"

#include <iostream>
#include <cassert>

using namespace tether;

int main() {
    // Test 1: Simple function
    std::cout << "Test 1: Simple function..." << std::endl;
    {
        const char* src = "fn add(a: i32, b: i32) i32 { return a + b; }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        std::cout << "  Tokens: " << tokens.size() << std::endl;
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        assert(program[0]->getKind() == NodeKind::FnDecl);
        auto& fn = cast<FnDecl>(*program[0]);
        assert(fn.name() == "add");
        assert(fn.paramCount() == 2);
        std::cout << "  PASS" << std::endl;
    }

    // Test 2: Struct declaration
    std::cout << "Test 2: Struct declaration..." << std::endl;
    {
        const char* src = "struct Point { x: f64, y: f64 }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        auto& st = cast<StructDecl>(*program[0]);
        assert(st.name() == "Point");
        assert(st.fieldCount() == 2);
        std::cout << "  PASS" << std::endl;
    }

    // Test 3: Enum declaration
    std::cout << "Test 3: Enum declaration..." << std::endl;
    {
        const char* src = "enum Color { Red, Green = 2, Blue }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        auto& en = cast<EnumDecl>(*program[0]);
        assert(en.name() == "Color");
        assert(en.variantCount() == 3);
        std::cout << "  PASS" << std::endl;
    }

    // Test 4: Pure error-returning function
    std::cout << "Test 4: Pure error-returning function..." << std::endl;
    {
        const char* src = "fn safe_div(a: f64, b: f64) !f64 pure { return a / b; }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        auto& fn = cast<FnDecl>(*program[0]);
        assert(fn.name() == "safe_div");
        assert(fn.isPure());
        assert(fn.canError());
        std::cout << "  PASS" << std::endl;
    }

    // Test 5: Compiler directives
    std::cout << "Test 5: Compiler directives..." << std::endl;
    {
        const char* src = "@superoptimize fn fast() void { } @polly fn parallel() void { }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        assert(program.size() == 2);
        auto& fn1 = cast<FnDecl>(*program[0]);
        assert(fn1.hasDirective(CompilerDirective::Superoptimize));
        auto& fn2 = cast<FnDecl>(*program[1]);
        assert(fn2.hasDirective(CompilerDirective::Polly));
        std::cout << "  PASS" << std::endl;
    }

    // Test 6: Complex function body
    std::cout << "Test 6: Complex function body..." << std::endl;
    {
        const char* src = R"(
            fn test(x: i32) void {
                val y = x + 1;
                var z: i32 = 0;
                if (x > 0) {
                    z = x;
                } else {
                    z = -x;
                }
                while (z > 0) : (z = z - 1) {
                    defer z = z - 1;
                }
                return;
            }
        )";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 7: Pointer/reference/slice types
    std::cout << "Test 7: Pointer/reference/slice types..." << std::endl;
    {
        const char* src = "fn ptr_test(p: *i32, r: &f64, m: &mut bool, s: []u8) void { }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 8: Smart pointer types
    std::cout << "Test 8: Smart pointer types..." << std::endl;
    {
        const char* src = "fn sp_test(b: Box<i32>, r: Rc<f64>, a: Arc<bool>) void { }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 9: Select/cast/unsafe expressions
    std::cout << "Test 9: Select/cast/unsafe expressions..." << std::endl;
    {
        const char* src = R"(
            fn exprs(a: i32) i32 {
                val b = select(a > 0, a, -a);
                val c = a cast i64;
                val d = unsafe(a + 1);
                return b;
            }
        )";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 10: Struct init with designated initializers
    std::cout << "Test 10: Struct init..." << std::endl;
    {
        const char* src = R"(
            fn init_test() void {
                val p = Point{ .x = 1.0, .y = 2.0 };
            }
        )";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 11: Nested smart pointer (Box<Box<i32>>)
    std::cout << "Test 11: Nested smart pointer types..." << std::endl;
    {
        const char* src = "fn nested() Box<Box<i32>> { }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 12: Import
    std::cout << "Test 12: Import declaration..." << std::endl;
    {
        const char* src = "import \"std/io\";";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        auto& imp = cast<ImportDecl>(*program[0]);
        assert(imp.path() == "std/io");
        std::cout << "  PASS" << std::endl;
    }

    // Test 13: Break and continue
    std::cout << "Test 13: Break/continue..." << std::endl;
    {
        const char* src = R"(
            fn loop_test() void {
                while (true) {
                    break;
                    continue;
                }
            }
        )";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    // Test 14: Allocator parameter
    std::cout << "Test 14: Allocator parameter..." << std::endl;
    {
        const char* src = "fn alloc_test(a: Allocator, data: []u8) ![]u8 { }";
        Lexer lex(src, "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        if (parser.hasErrors()) {
            for (const auto& e : parser.errors()) {
                std::cerr << "  ERROR: " << e.message << std::endl;
            }
        }
        assert(!parser.hasErrors());
        std::cout << "  PASS" << std::endl;
    }

    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
