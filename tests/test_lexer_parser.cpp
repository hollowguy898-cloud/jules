#include "lexer/Lexer.h"
#include "parser/Parser.h"
#include "sema/Type.h"

#include <iostream>
#include <cassert>

using namespace tether;

void test_lexer() {
    std::cout << "=== Lexer Tests ===" << std::endl;

    // Test 1: Keywords
    {
        Lexer lex("val var struct enum fn pure unsafe if else while defer return break continue true false void cast mut sizeof import match const module use as async await typeof alignof parallel reflect", "test");
        auto tokens = lex.tokenize();
        assert(tokens[0].kind() == TokenKind::KW_VAL);
        assert(tokens[1].kind() == TokenKind::KW_VAR);
        assert(tokens[2].kind() == TokenKind::KW_STRUCT);
        assert(tokens[3].kind() == TokenKind::KW_ENUM);
        assert(tokens[4].kind() == TokenKind::KW_FN);
        assert(tokens[5].kind() == TokenKind::KW_PURE);
        assert(tokens[6].kind() == TokenKind::KW_UNSAFE);
        assert(tokens[7].kind() == TokenKind::KW_IF);
        assert(tokens[8].kind() == TokenKind::KW_ELSE);
        assert(tokens[9].kind() == TokenKind::KW_WHILE);
        assert(tokens[10].kind() == TokenKind::KW_DEFER);
        assert(tokens[11].kind() == TokenKind::KW_RETURN);
        assert(tokens[12].kind() == TokenKind::KW_BREAK);
        assert(tokens[13].kind() == TokenKind::KW_CONTINUE);
        assert(tokens[14].kind() == TokenKind::KW_TRUE);
        assert(tokens[15].kind() == TokenKind::KW_FALSE);
        assert(tokens[16].kind() == TokenKind::KW_VOID);
        assert(tokens[17].kind() == TokenKind::KW_SELECT);
        assert(tokens[18].kind() == TokenKind::KW_CAST);
        assert(tokens[19].kind() == TokenKind::KW_MUT);
        assert(tokens[20].kind() == TokenKind::KW_SIZEOF);
        assert(tokens[21].kind() == TokenKind::KW_IMPORT);
        std::cout << "  Keywords: PASS" << std::endl;
    }

    // Test 2: Operators
    {
        Lexer lex("+ - * / % == != < > <= >= && || ! & = += -= *= /= := :: . .. ; , : ( ) { } [ ] -> # @ ~ ^ | << >> &= |= ^= <<= >>= %= @superoptimize", "test");
        auto tokens = lex.tokenize();
        assert(tokens[0].kind() == TokenKind::PLUS);
        assert(tokens[1].kind() == TokenKind::MINUS);
        assert(tokens[2].kind() == TokenKind::STAR);
        assert(tokens[3].kind() == TokenKind::SLASH);
        assert(tokens[4].kind() == TokenKind::PERCENT);
        assert(tokens[5].kind() == TokenKind::EQ_EQ);
        assert(tokens[6].kind() == TokenKind::BANG_EQ);
        assert(tokens[7].kind() == TokenKind::LT);
        assert(tokens[8].kind() == TokenKind::GT);
        assert(tokens[9].kind() == TokenKind::LE);
        assert(tokens[10].kind() == TokenKind::GE);
        assert(tokens[11].kind() == TokenKind::AMP_AMP);
        assert(tokens[12].kind() == TokenKind::PIPE_PIPE);
        assert(tokens[13].kind() == TokenKind::BANG);
        assert(tokens[14].kind() == TokenKind::AMP);
        assert(tokens[15].kind() == TokenKind::EQ);
        assert(tokens[16].kind() == TokenKind::PLUS_EQ);
        assert(tokens[17].kind() == TokenKind::MINUS_EQ);
        assert(tokens[18].kind() == TokenKind::STAR_EQ);
        assert(tokens[19].kind() == TokenKind::SLASH_EQ);
        assert(tokens[20].kind() == TokenKind::COLON_EQ);
        assert(tokens[21].kind() == TokenKind::COLON_COLON);
        assert(tokens[22].kind() == TokenKind::DOT);
        assert(tokens[23].kind() == TokenKind::DOT_DOT);
        assert(tokens[24].kind() == TokenKind::SEMI);
        assert(tokens[25].kind() == TokenKind::COMMA);
        assert(tokens[26].kind() == TokenKind::COLON);
        assert(tokens[27].kind() == TokenKind::LPAREN);
        assert(tokens[28].kind() == TokenKind::RPAREN);
        assert(tokens[29].kind() == TokenKind::LBRACE);
        assert(tokens[30].kind() == TokenKind::RBRACE);
        assert(tokens[31].kind() == TokenKind::LBRACKET);
        assert(tokens[32].kind() == TokenKind::RBRACKET);
        assert(tokens[33].kind() == TokenKind::ARROW);
        assert(tokens[34].kind() == TokenKind::HASH);
        assert(tokens[35].kind() == TokenKind::AT);
        assert(tokens[36].kind() == TokenKind::TILDE);
        assert(tokens[37].kind() == TokenKind::CARET);
        assert(tokens[38].kind() == TokenKind::PIPE);
        assert(tokens[39].kind() == TokenKind::SHL);
        assert(tokens[40].kind() == TokenKind::SHR);
        assert(tokens[41].kind() == TokenKind::AMP_EQ);
        assert(tokens[42].kind() == TokenKind::PIPE_EQ);
        assert(tokens[43].kind() == TokenKind::CARET_EQ);
        assert(tokens[44].kind() == TokenKind::SHL_EQ);
        assert(tokens[45].kind() == TokenKind::SHR_EQ);
        assert(tokens[46].kind() == TokenKind::PERCENT_EQ);
        assert(tokens[47].kind() == TokenKind::AT);
        assert(tokens[48].kind() == TokenKind::IDENTIFIER);
        assert(tokens[48].text() == "superoptimize");
        std::cout << "  Operators: PASS" << std::endl;
    }

    // Test 3: Literals
    {
        Lexer lex("42 0xFF 0b1010 3.14 3.14f32 42i32 \"hello\" 'a'", "test");
        auto tokens = lex.tokenize();
        assert(tokens[0].kind() == TokenKind::INT_LITERAL);
        assert(tokens[0].text() == "42");
        assert(tokens[1].kind() == TokenKind::INT_LITERAL);
        assert(tokens[1].text() == "0xFF");
        assert(tokens[2].kind() == TokenKind::INT_LITERAL);
        assert(tokens[2].text() == "0b1010");
        assert(tokens[3].kind() == TokenKind::FLOAT_LITERAL);
        assert(tokens[3].text() == "3.14");
        assert(tokens[4].kind() == TokenKind::FLOAT_LITERAL);
        assert(tokens[4].text() == "3.14f32");
        assert(tokens[5].kind() == TokenKind::INT_LITERAL);
        assert(tokens[5].text() == "42i32");
        assert(tokens[6].kind() == TokenKind::STRING_LITERAL);
        assert(tokens[6].text() == "\"hello\"");
        assert(tokens[7].kind() == TokenKind::CHAR_LITERAL);
        assert(tokens[7].text() == "'a'");
        std::cout << "  Literals: PASS" << std::endl;
    }

    // Test 4: Comments
    {
        Lexer lex("// line comment\n42 /* block */ 10", "test");
        auto tokens = lex.tokenize();
        assert(tokens[0].kind() == TokenKind::INT_LITERAL);
        assert(tokens[0].text() == "42");
        assert(tokens[1].kind() == TokenKind::INT_LITERAL);
        assert(tokens[1].text() == "10");
        std::cout << "  Comments: PASS" << std::endl;
    }

    // Test 5: Nested block comments
    {
        Lexer lex("/* outer /* inner */ still outer */ 42", "test");
        auto tokens = lex.tokenize();
        assert(tokens[0].kind() == TokenKind::INT_LITERAL);
        assert(!lex.hasErrors());
        std::cout << "  Nested block comments: PASS" << std::endl;
    }

    // Test 6: Line/column tracking
    {
        Lexer lex("foo\nbar", "test.jules");
        auto tokens = lex.tokenize();
        assert(tokens[0].line() == 1);
        assert(tokens[0].col() == 1);
        assert(tokens[1].line() == 2);
        assert(tokens[1].col() == 1);
        std::cout << "  Line/column tracking: PASS" << std::endl;
    }
}

void test_parser() {
    std::cout << "\n=== Parser Tests ===" << std::endl;

    // Test 1: Simple val declaration
    {
        Lexer lex("val x = 10;", "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        // Note: val at top level would be an error normally, but our parser
        // will try to parse it as a statement wrapped in... actually our parser
        // expects fn/struct/enum/import at top level. Let me test inside a fn.
        std::cout << "  (val decl tested via fn body below)" << std::endl;
    }

    // Test 2: Function declaration
    {
        Lexer lex("fn add(a: i32, b: i32) i32 { return a + b; }", "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        assert(program[0]->getKind() == NodeKind::FnDecl);
        auto& fn = cast<FnDecl>(*program[0]);
        assert(fn.name() == "add");
        assert(fn.paramCount() == 2);
        assert(!fn.isPure());
        assert(!fn.canError());
        std::cout << "  Function declaration: PASS" << std::endl;
    }

    // Test 3: Pure function with error type
    {
        Lexer lex("fn safe_div(a: f64, b: f64) !f64 pure { return a / b; }", "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        auto& fn = cast<FnDecl>(*program[0]);
        assert(fn.name() == "safe_div");
        assert(fn.isPure());
        assert(fn.canError());
        std::cout << "  Pure error-returning function: PASS" << std::endl;
    }

    // Test 4: Struct declaration
    {
        Lexer lex("struct Point { x: f64, y: f64 }", "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        auto& st = cast<StructDecl>(*program[0]);
        assert(st.name() == "Point");
        assert(st.fieldCount() == 2);
        std::cout << "  Struct declaration: PASS" << std::endl;
    }

    // Test 5: Enum declaration
    {
        Lexer lex("enum Color { Red, Green = 2, Blue }", "test");
        auto tokens = lex.tokenize();
        TypeTable type_table;
        Parser parser(std::move(tokens), type_table);
        auto program = parser.parse();
        assert(!parser.hasErrors());
        assert(program.size() == 1);
        auto& en = cast<EnumDecl>(*program[0]);
        assert(en.name() == "Color");
        assert(en.variantCount() == 3);
        std::cout << "  Enum declaration: PASS" << std::endl;
    }

    // Test 6: Complex function body
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
        assert(program.size() == 1);
        std::cout << "  Complex function body: PASS" << std::endl;
    }

    // Test 7: Select, cast, unsafe expressions
    {
        const char* src = R"(
            fn exprs(a: i32) i32 {
                val b = typeof(a);
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
        std::cout << "  Typeof/cast/unsafe expressions: PASS" << std::endl;
    }

    // Test 8: Pointer types
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
        std::cout << "  Pointer/reference/slice types: PASS" << std::endl;
    }

    // Test 9: Smart pointer types
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
        std::cout << "  Smart pointer types: PASS" << std::endl;
    }

    // Test 10: Compiler directives
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
        std::cout << "  Compiler directives: PASS" << std::endl;
    }

    // Test 11: Allocator parameter
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
        std::cout << "  Allocator parameter: PASS" << std::endl;
    }

    // Test 12: Struct init with designated initializers
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
        std::cout << "  Struct init: PASS" << std::endl;
    }

    // Test 13: Operator precedence
    {
        const char* src = R"(
            fn prec() void {
                val a = 1 + 2 * 3;
                val b = 1 * 2 + 3;
                val c = 1 + 2 == 3;
                val d = true && false || true;
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
        std::cout << "  Operator precedence: PASS" << std::endl;
    }

    // Test 14: Bitwise operators
    {
        const char* src = R"(
            fn bitwise() void {
                val a = 1 & 2 | 3 ^ 4;
                val b = 1 << 2 >> 3;
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
        std::cout << "  Bitwise operators: PASS" << std::endl;
    }

    // Test 15: Unary operators and dereference
    {
        const char* src = R"(
            fn unary_test(x: i32, p: *i32) void {
                val a = -x;
                val b = !true;
                val c = *p;
                val d = &x;
                val e = ~x;
                val f = &mut x;
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
        std::cout << "  Unary operators: PASS" << std::endl;
    }

    // Test 16: Else if chains
    {
        const char* src = R"(
            fn elseif_test(x: i32) void {
                if (x > 0) {
                } else if (x < 0) {
                } else {
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
        std::cout << "  Else-if chains: PASS" << std::endl;
    }

    // Test 17: Import declaration
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
        assert(program.size() == 1);
        auto& imp = cast<ImportDecl>(*program[0]);
        assert(imp.path() == "std/io");
        std::cout << "  Import declaration: PASS" << std::endl;
    }

    // Test 18: Nested smart pointer types (Box<Box<i32>>)
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
        std::cout << "  Nested smart pointer types: PASS" << std::endl;
    }

    // Test 19: Compound assignment
    {
        const char* src = R"(
            fn compound() void {
                var x: i32 = 0;
                x += 1;
                x -= 2;
                x *= 3;
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
        std::cout << "  Compound assignment: PASS" << std::endl;
    }

    // Test 20: Break and continue
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
        std::cout << "  Break/continue: PASS" << std::endl;
    }
}

int main() {
    test_lexer();
    test_parser();
    std::cout << "\nAll tests passed!" << std::endl;
    return 0;
}
