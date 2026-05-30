#pragma once

#include "sema/Type.h"

#include <string>
#include <vector>
#include <memory>
#include <optional>
#include <cstdint>
#include <cassert>
#include <utility>

namespace tether {

// ============================================================================
// Source Location
// ============================================================================
struct SourceLocation {
    uint32_t line;
    uint32_t col;
    std::string filename;

    SourceLocation() : line(0), col(0) {}
    SourceLocation(uint32_t line, uint32_t col, std::string filename)
        : line(line), col(col), filename(std::move(filename)) {}

    bool isValid() const { return line > 0; }

    std::string toString() const {
        return filename + ":" + std::to_string(line) + ":" + std::to_string(col);
    }
};

// ============================================================================
// Binary Operator Enumeration
// ============================================================================
enum class BinaryOp : uint8_t {
    Add,        // +
    Sub,        // -
    Mul,        // *
    Div,        // /
    Mod,        // %
    And,        // &&
    Or,         // ||
    BitAnd,     // &
    BitOr,      // |
    BitXor,     // ^
    Shl,        // <<
    Shr,        // >>
    Eq,         // ==
    Ne,         // !=
    Lt,         // <
    Le,         // <=
    Gt,         // >
    Ge,         // >=
    Assign,     // =
    AddAssign,  // +=
    SubAssign,  // -=
    MulAssign,  // *=
    DivAssign,  // /=
    ModAssign,  // %=
    AndAssign,  // &=
    OrAssign,   // |=
    XorAssign,  // ^=
    ShlAssign,  // <<=
    ShrAssign   // >>=
};

std::string binaryOpToString(BinaryOp op);

// ============================================================================
// Unary Operator Enumeration
// ============================================================================
enum class UnaryOp : uint8_t {
    Neg,    // - (arithmetic negation)
    Not,    // ! (logical not)
    BitNot, // ~ (bitwise not)
    Deref,  // * (dereference - handled by DerefExpr, kept for completeness)
    Addr    // & (address-of - handled by AddrOfExpr, kept for completeness)
};

std::string unaryOpToString(UnaryOp op);

// ============================================================================
// Compiler Directive Enumeration
// ============================================================================
enum class CompilerDirective : uint8_t {
    Superoptimize,
    Polly,
    Simd,
    Tailcall,  // @tailcall directive — guarantees tail call optimization
    Unsafe     // @unsafe directive or unsafe block
};

std::string compilerDirectiveToString(CompilerDirective d);

// ============================================================================
// Node Kind Enumeration (for LLVM-style RTTI)
// ============================================================================
enum class NodeKind : uint16_t {
    // Expressions
    IntLiteral,
    FloatLiteral,
    BoolLiteral,
    StringLiteral,
    IdentExpr,
    BinaryExpr,
    UnaryExpr,
    CallExpr,
    MemberExpr,
    IndexExpr,
    DerefExpr,
    AddrOfExpr,
    CastExpr,
    StructInitExpr,
    ArrayInitExpr,
    SliceExpr,
    SizeofExpr,
    UnsafeExpr,
    PoisonExpr,
    TryExpr,
    // v0.2 expansion expressions
    ComptimeExpr,     // Compile-time evaluation: comptime expr
    ReduceExpr,       // Parallel reduction: reduce(op, expr, axis=N)
    // v0.3 expansion expressions
    TypeofExpr,       // Type query: typeof(expr)
    AlignofExpr,      // Alignment query: alignof(type)
    ReflectExpr,      // Compile-time reflection: reflect(T)
    AwaitExpr,        // Async await: await expr
    // Statements
    VarDeclStmt,
    ValDeclStmt,
    AssignStmt,
    DeferStmt,
    IfStmt,
    WhileStmt,
    ReturnStmt,
    BreakStmt,
    ContinueStmt,
    ExprStmt,
    BlockStmt,
    ErrdeferStmt,
    AtomicStmt,
    YieldStmt,
    // v0.2 expansion statements
    SpawnStmt,        // Structured async task dispatch
    // v0.3 expansion statements
    ConstDeclStmt,    // Compile-time constant: const name = value
    MatchStmt,        // Pattern matching: match expr { ... } (replaces SwitchStmt)
    ParallelForStmt,  // Parallel loop: parallel for item in iter { body }
    StaticAssertStmt, // Compile-time assertion: static_assert(cond, "msg")
    UnsafeBlockStmt,  // unsafe { ... } block
    // Top-level declarations
    FnDecl,
    StructDecl,
    EnumDecl,
    ImportDecl,
    // v0.2 expansion top-level declarations
    TraitDecl,        // Shared behavior contracts
    ImplDecl,         // Trait/behavior implementation
    // v0.3 expansion top-level declarations
    ModuleDecl,       // Module declaration: module name;
    UseDecl,          // Selective import: use module::name as alias;
    // v0.4 strict borrow checker additions
    UnsafeBlockStmt_, // Sentinel: end of UnsafeBlockStmt range (for classof)
};

// ============================================================================
// Forward declarations for visitor
// ============================================================================
class ASTVisitor;
class ConstASTVisitor;

// ============================================================================
// ASTNode - base class for all AST nodes
// ============================================================================
class ASTNode {
public:
    virtual ~ASTNode() = default;

    NodeKind getKind() const { return kind_; }
    const SourceLocation& sourceLoc() const { return loc_; }

    // Visitor pattern
    virtual void accept(ASTVisitor& visitor) = 0;
    virtual void accept(ConstASTVisitor& visitor) const = 0;

    static bool classof(const ASTNode*) { return true; }

protected:
    ASTNode(NodeKind kind, SourceLocation loc)
        : kind_(kind), loc_(std::move(loc)) {}

private:
    NodeKind kind_;
    SourceLocation loc_;
};

// ============================================================================
// Casting helpers for AST nodes
// ============================================================================
template<typename T>
bool isa(const ASTNode& node) {
    return T::classof(&node);
}

template<typename T>
bool isa(const ASTNode* node) {
    return node && T::classof(node);
}

template<typename T>
T& cast(ASTNode& node) {
    assert(T::classof(&node) && "Invalid AST node cast");
    return static_cast<T&>(node);
}

template<typename T>
const T& cast(const ASTNode& node) {
    assert(T::classof(&node) && "Invalid AST node cast");
    return static_cast<const T&>(node);
}

template<typename T>
T* dyn_cast(ASTNode* node) {
    if (node && T::classof(node)) return static_cast<T*>(node);
    return nullptr;
}

template<typename T>
const T* dyn_cast(const ASTNode* node) {
    if (node && T::classof(node)) return static_cast<const T*>(node);
    return nullptr;
}

// ============================================================================
// Expr - base class for all expression nodes
// ============================================================================
class Expr : public ASTNode {
public:
    // Type annotation, filled in during semantic analysis
    TypeId getType() const { return type_; }
    void setType(TypeId t) { type_ = t; }
    bool hasType() const { return !type_.isNull(); }

    static bool classof(const ASTNode* n) {
        auto k = n->getKind();
        return k >= NodeKind::IntLiteral && k <= NodeKind::AwaitExpr;
    }

protected:
    Expr(NodeKind kind, SourceLocation loc)
        : ASTNode(kind, std::move(loc)) {}

private:
    TypeId type_;
};

// ============================================================================
// Stmt - base class for all statement nodes
// ============================================================================
class Stmt : public ASTNode {
public:
    static bool classof(const ASTNode* n) {
        auto k = n->getKind();
        return k >= NodeKind::VarDeclStmt && k <= NodeKind::UnsafeBlockStmt;
    }

protected:
    Stmt(NodeKind kind, SourceLocation loc)
        : ASTNode(kind, std::move(loc)) {}
};

// ============================================================================
// TopLevel - base class for top-level declaration nodes
// ============================================================================
class TopLevel : public ASTNode {
public:
    static bool classof(const ASTNode* n) {
        auto k = n->getKind();
        return k >= NodeKind::FnDecl && k <= NodeKind::UnsafeBlockStmt_;
    }

protected:
    TopLevel(NodeKind kind, SourceLocation loc)
        : ASTNode(kind, std::move(loc)) {}
};

// ============================================================================
// Expression Nodes
// ============================================================================

// ---- IntLiteral ----
class IntLiteral : public Expr {
public:
    IntLiteral(SourceLocation loc, uint64_t value, bool is_signed = false)
        : Expr(NodeKind::IntLiteral, std::move(loc))
        , value_(value)
        , is_signed_(is_signed)
    {}

    uint64_t value() const { return value_; }
    bool isSigned() const { return is_signed_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::IntLiteral;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    uint64_t value_;
    bool is_signed_;
};

// ---- FloatLiteral ----
class FloatLiteral : public Expr {
public:
    FloatLiteral(SourceLocation loc, double value)
        : Expr(NodeKind::FloatLiteral, std::move(loc))
        , value_(value)
    {}

    double value() const { return value_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::FloatLiteral;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    double value_;
};

// ---- BoolLiteral ----
class BoolLiteral : public Expr {
public:
    BoolLiteral(SourceLocation loc, bool value)
        : Expr(NodeKind::BoolLiteral, std::move(loc))
        , value_(value)
    {}

    bool value() const { return value_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::BoolLiteral;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    bool value_;
};

// ---- StringLiteral ----
class StringLiteral : public Expr {
public:
    StringLiteral(SourceLocation loc, std::string value)
        : Expr(NodeKind::StringLiteral, std::move(loc))
        , value_(std::move(value))
    {}

    const std::string& value() const { return value_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::StringLiteral;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string value_;
};

// ---- IdentExpr ----
class IdentExpr : public Expr {
public:
    IdentExpr(SourceLocation loc, std::string name)
        : Expr(NodeKind::IdentExpr, std::move(loc))
        , name_(std::move(name))
    {}

    const std::string& name() const { return name_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::IdentExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
};

// ---- BinaryExpr ----
class BinaryExpr : public Expr {
public:
    BinaryExpr(SourceLocation loc, BinaryOp op,
               std::unique_ptr<Expr> left, std::unique_ptr<Expr> right)
        : Expr(NodeKind::BinaryExpr, std::move(loc))
        , op_(op)
        , left_(std::move(left))
        , right_(std::move(right))
    {}

    BinaryOp op() const { return op_; }
    Expr* left() { return left_.get(); }
    const Expr* left() const { return left_.get(); }
    Expr* right() { return right_.get(); }
    const Expr* right() const { return right_.get(); }

    std::unique_ptr<Expr> takeLeft() { return std::move(left_); }
    std::unique_ptr<Expr> takeRight() { return std::move(right_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::BinaryExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    BinaryOp op_;
    std::unique_ptr<Expr> left_;
    std::unique_ptr<Expr> right_;
};

// ---- UnaryExpr ----
class UnaryExpr : public Expr {
public:
    UnaryExpr(SourceLocation loc, UnaryOp op, std::unique_ptr<Expr> operand)
        : Expr(NodeKind::UnaryExpr, std::move(loc))
        , op_(op)
        , operand_(std::move(operand))
    {}

    UnaryOp op() const { return op_; }
    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::UnaryExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    UnaryOp op_;
    std::unique_ptr<Expr> operand_;
};

// ---- CallExpr ----
class CallExpr : public Expr {
public:
    CallExpr(SourceLocation loc, std::unique_ptr<Expr> callee,
             std::vector<std::unique_ptr<Expr>> args)
        : Expr(NodeKind::CallExpr, std::move(loc))
        , callee_(std::move(callee))
        , args_(std::move(args))
    {}

    Expr* callee() { return callee_.get(); }
    const Expr* callee() const { return callee_.get(); }
    std::unique_ptr<Expr> takeCallee() { return std::move(callee_); }

    const std::vector<std::unique_ptr<Expr>>& args() const { return args_; }
    std::vector<std::unique_ptr<Expr>>& args() { return args_; }
    size_t argCount() const { return args_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::CallExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> callee_;
    std::vector<std::unique_ptr<Expr>> args_;
};

// ---- MemberExpr ----
class MemberExpr : public Expr {
public:
    MemberExpr(SourceLocation loc, std::unique_ptr<Expr> object,
               std::string field)
        : Expr(NodeKind::MemberExpr, std::move(loc))
        , object_(std::move(object))
        , field_(std::move(field))
    {}

    Expr* object() { return object_.get(); }
    const Expr* object() const { return object_.get(); }
    std::unique_ptr<Expr> takeObject() { return std::move(object_); }
    const std::string& field() const { return field_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::MemberExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> object_;
    std::string field_;
};

// ---- IndexExpr ----
class IndexExpr : public Expr {
public:
    IndexExpr(SourceLocation loc, std::unique_ptr<Expr> object,
              std::unique_ptr<Expr> index)
        : Expr(NodeKind::IndexExpr, std::move(loc))
        , object_(std::move(object))
        , index_(std::move(index))
    {}

    Expr* object() { return object_.get(); }
    const Expr* object() const { return object_.get(); }
    Expr* index() { return index_.get(); }
    const Expr* index() const { return index_.get(); }

    std::unique_ptr<Expr> takeObject() { return std::move(object_); }
    std::unique_ptr<Expr> takeIndex() { return std::move(index_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::IndexExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> index_;
};

// ---- DerefExpr ----
class DerefExpr : public Expr {
public:
    DerefExpr(SourceLocation loc, std::unique_ptr<Expr> operand)
        : Expr(NodeKind::DerefExpr, std::move(loc))
        , operand_(std::move(operand))
    {}

    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::DerefExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> operand_;
};

// ---- AddrOfExpr ----
class AddrOfExpr : public Expr {
public:
    AddrOfExpr(SourceLocation loc, std::unique_ptr<Expr> operand, bool is_mut)
        : Expr(NodeKind::AddrOfExpr, std::move(loc))
        , operand_(std::move(operand))
        , is_mut_(is_mut)
    {}

    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }
    bool isMutable() const { return is_mut_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::AddrOfExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> operand_;
    bool is_mut_;
};

// ---- CastExpr ----
class CastExpr : public Expr {
public:
    CastExpr(SourceLocation loc, std::unique_ptr<Expr> expr, TypeId target_type)
        : Expr(NodeKind::CastExpr, std::move(loc))
        , expr_(std::move(expr))
        , target_type_(target_type)
    {}

    Expr* expr() { return expr_.get(); }
    const Expr* expr() const { return expr_.get(); }
    std::unique_ptr<Expr> takeExpr() { return std::move(expr_); }
    TypeId targetType() const { return target_type_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::CastExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> expr_;
    TypeId target_type_;
};

// ---- DesignatedInit ----
// Zig-style designated initializer: .field = value
struct DesignatedInit {
    std::string field_name;
    std::unique_ptr<Expr> value;
};

// ---- StructInitExpr ----
class StructInitExpr : public Expr {
public:
    StructInitExpr(SourceLocation loc, std::string type_name,
                   std::vector<DesignatedInit> inits)
        : Expr(NodeKind::StructInitExpr, std::move(loc))
        , type_name_(std::move(type_name))
        , inits_(std::move(inits))
    {}

    const std::string& typeName() const { return type_name_; }
    const std::vector<DesignatedInit>& inits() const { return inits_; }
    std::vector<DesignatedInit>& inits() { return inits_; }
    size_t initCount() const { return inits_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::StructInitExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string type_name_;
    std::vector<DesignatedInit> inits_;
};

// ---- ArrayInitExpr ----
class ArrayInitExpr : public Expr {
public:
    ArrayInitExpr(SourceLocation loc, std::vector<std::unique_ptr<Expr>> elements)
        : Expr(NodeKind::ArrayInitExpr, std::move(loc))
        , elements_(std::move(elements))
    {}

    const std::vector<std::unique_ptr<Expr>>& elements() const { return elements_; }
    std::vector<std::unique_ptr<Expr>>& elements() { return elements_; }
    size_t elementCount() const { return elements_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ArrayInitExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::vector<std::unique_ptr<Expr>> elements_;
};

// ---- SliceExpr ----
// Creates a subslice from an existing slice/array: arr[start..end]
// Produces a new { ptr, i64 } with ptr = arr.ptr + start, len = end - start
// This is a zero-copy operation — it just adjusts the pointer and length.
class SliceExpr : public Expr {
public:
    SliceExpr(SourceLocation loc,
              std::unique_ptr<Expr> object,
              std::unique_ptr<Expr> start,
              std::unique_ptr<Expr> end)
        : Expr(NodeKind::SliceExpr, std::move(loc))
        , object_(std::move(object))
        , start_(std::move(start))
        , end_(std::move(end))
    {}

    // Slice with start only (open-ended): arr[start..]
    SliceExpr(SourceLocation loc,
              std::unique_ptr<Expr> object,
              std::unique_ptr<Expr> start)
        : Expr(NodeKind::SliceExpr, std::move(loc))
        , object_(std::move(object))
        , start_(std::move(start))
        , end_(nullptr)
    {}

    Expr* object() { return object_.get(); }
    const Expr* object() const { return object_.get(); }
    Expr* start() { return start_.get(); }
    const Expr* start() const { return start_.get(); }
    Expr* end() { return end_.get(); }
    const Expr* end() const { return end_.get(); }
    bool hasEnd() const { return end_ != nullptr; }

    std::unique_ptr<Expr> takeObject() { return std::move(object_); }
    std::unique_ptr<Expr> takeStart() { return std::move(start_); }
    std::unique_ptr<Expr> takeEnd() { return std::move(end_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::SliceExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> object_;
    std::unique_ptr<Expr> start_;
    std::unique_ptr<Expr> end_;  // nullptr = open-ended slice (to end of array)
};

// ---- SizeofExpr ----
class SizeofExpr : public Expr {
public:
    // Sizeof can operate on a type or an expression
    SizeofExpr(SourceLocation loc, TypeId target_type)
        : Expr(NodeKind::SizeofExpr, std::move(loc))
        , target_type_(target_type)
        , expr_(nullptr)
    {}

    SizeofExpr(SourceLocation loc, std::unique_ptr<Expr> expr)
        : Expr(NodeKind::SizeofExpr, std::move(loc))
        , target_type_()
        , expr_(std::move(expr))
    {}

    TypeId targetType() const { return target_type_; }
    Expr* expr() { return expr_.get(); }
    const Expr* expr() const { return expr_.get(); }
    bool isTypeOperand() const { return !target_type_.isNull(); }
    bool isExprOperand() const { return expr_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::SizeofExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    TypeId target_type_;
    std::unique_ptr<Expr> expr_;
};

// ---- UnsafeExpr ----
class UnsafeExpr : public Expr {
public:
    UnsafeExpr(SourceLocation loc, std::unique_ptr<Expr> inner)
        : Expr(NodeKind::UnsafeExpr, std::move(loc))
        , inner_(std::move(inner))
    {}

    Expr* inner() { return inner_.get(); }
    const Expr* inner() const { return inner_.get(); }
    std::unique_ptr<Expr> takeInner() { return std::move(inner_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::UnsafeExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> inner_;
};

// ---- PoisonExpr ----
// Represents a syntactically or semantically invalid expression.
// Injected by the error-resilient parser or type-checker so that the rest
// of the AST remains structurally valid and compilation can continue to
// collect further errors.
class PoisonExpr : public Expr {
public:
    explicit PoisonExpr(SourceLocation loc, std::string message = "")
        : Expr(NodeKind::PoisonExpr, std::move(loc))
        , message_(std::move(message))
    {}

    const std::string& message() const { return message_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::PoisonExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string message_;
};

// ---- TryExpr ----
// Zig-style error propagation: try expr
// If expr evaluates to an error, immediately return it up the stack.
// Otherwise, evaluates to the success value.
class TryExpr : public Expr {
public:
    TryExpr(SourceLocation loc, std::unique_ptr<Expr> operand)
        : Expr(NodeKind::TryExpr, std::move(loc))
        , operand_(std::move(operand))
    {}

    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::TryExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> operand_;
};

// ---- ComptimeExpr ----
// Compile-time evaluation: comptime expr or comptime { block }
// Forces the enclosed expression or block to be fully evaluated during compilation.
// If evaluation fails or depends on runtime values, the compiler emits an error.
// This is the Zig-style secret weapon: eliminates macro languages entirely.
class ComptimeExpr : public Expr {
public:
    // comptime can wrap either a single expression or a block
    ComptimeExpr(SourceLocation loc, std::unique_ptr<Expr> inner)
        : Expr(NodeKind::ComptimeExpr, std::move(loc))
        , inner_(std::move(inner))
    {}

    Expr* inner() { return inner_.get(); }
    const Expr* inner() const { return inner_.get(); }
    std::unique_ptr<Expr> takeInner() { return std::move(inner_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ComptimeExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> inner_;
};

// ---- ReduceExpr ----
// Hardware-native parallel reduction: reduce(op, iterable, axis=N)
// Emits optimized vector reduction trees (tree-reduction for SIMD lanes)
// without branch penalty. The op is a reduction operation (add, max, min, etc.)
// and axis specifies the reduction dimension for multi-dimensional data.
class ReduceExpr : public Expr {
public:
    enum class ReduceOp : uint8_t {
        Add,    // Sum reduction
        Mul,    // Product reduction
        Max,    // Maximum reduction
        Min,    // Minimum reduction
        And,    // Logical AND reduction
        Or,     // Logical OR reduction
        BitAnd, // Bitwise AND reduction
        BitOr   // Bitwise OR reduction
    };

    ReduceExpr(SourceLocation loc, ReduceOp op, std::unique_ptr<Expr> iterable,
               std::unique_ptr<Expr> axis = nullptr)
        : Expr(NodeKind::ReduceExpr, std::move(loc))
        , op_(op)
        , iterable_(std::move(iterable))
        , axis_(std::move(axis))
    {}

    ReduceOp op() const { return op_; }
    Expr* iterable() { return iterable_.get(); }
    const Expr* iterable() const { return iterable_.get(); }
    Expr* axis() { return axis_.get(); }
    const Expr* axis() const { return axis_.get(); }
    bool hasAxis() const { return axis_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ReduceExpr;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    ReduceOp op_;
    std::unique_ptr<Expr> iterable_;
    std::unique_ptr<Expr> axis_;
};

// ============================================================================
// Statement Nodes
// ============================================================================

// ---- BlockStmt ----
class BlockStmt : public Stmt {
public:
    BlockStmt(SourceLocation loc, std::vector<std::unique_ptr<Stmt>> stmts)
        : Stmt(NodeKind::BlockStmt, std::move(loc))
        , stmts_(std::move(stmts))
    {}

    const std::vector<std::unique_ptr<Stmt>>& stmts() const { return stmts_; }
    std::vector<std::unique_ptr<Stmt>>& stmts() { return stmts_; }
    size_t stmtCount() const { return stmts_.size(); }
    bool empty() const { return stmts_.empty(); }

    void prepend(std::unique_ptr<Stmt> stmt) {
        stmts_.insert(stmts_.begin(), std::move(stmt));
    }

    void append(std::unique_ptr<Stmt> stmt) {
        stmts_.push_back(std::move(stmt));
    }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::BlockStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::vector<std::unique_ptr<Stmt>> stmts_;
};

// ---- VarDeclStmt ----
class VarDeclStmt : public Stmt {
public:
    VarDeclStmt(SourceLocation loc, std::string name, TypeId type,
                std::unique_ptr<Expr> init)
        : Stmt(NodeKind::VarDeclStmt, std::move(loc))
        , name_(std::move(name))
        , type_(type)
        , init_(std::move(init))
    {}

    const std::string& name() const { return name_; }
    TypeId declaredType() const { return type_; }
    bool hasType() const { return !type_.isNull(); }
    Expr* init() { return init_.get(); }
    const Expr* init() const { return init_.get(); }
    std::unique_ptr<Expr> takeInit() { return std::move(init_); }
    bool hasInit() const { return init_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::VarDeclStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    TypeId type_;
    std::unique_ptr<Expr> init_;
};

// ---- ValDeclStmt ----
class ValDeclStmt : public Stmt {
public:
    ValDeclStmt(SourceLocation loc, std::string name, TypeId type,
                std::unique_ptr<Expr> init)
        : Stmt(NodeKind::ValDeclStmt, std::move(loc))
        , name_(std::move(name))
        , type_(type)
        , init_(std::move(init))
    {}

    const std::string& name() const { return name_; }
    TypeId declaredType() const { return type_; }
    bool hasType() const { return !type_.isNull(); }
    Expr* init() { return init_.get(); }
    const Expr* init() const { return init_.get(); }
    std::unique_ptr<Expr> takeInit() { return std::move(init_); }
    bool hasInit() const { return init_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ValDeclStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    TypeId type_;
    std::unique_ptr<Expr> init_;
};

// ---- AssignStmt ----
class AssignStmt : public Stmt {
public:
    AssignStmt(SourceLocation loc, std::unique_ptr<Expr> target,
               std::unique_ptr<Expr> value)
        : Stmt(NodeKind::AssignStmt, std::move(loc))
        , target_(std::move(target))
        , value_(std::move(value))
    {}

    Expr* target() { return target_.get(); }
    const Expr* target() const { return target_.get(); }
    Expr* value() { return value_.get(); }
    const Expr* value() const { return value_.get(); }

    std::unique_ptr<Expr> takeTarget() { return std::move(target_); }
    std::unique_ptr<Expr> takeValue() { return std::move(value_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::AssignStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> target_;
    std::unique_ptr<Expr> value_;
};

// ---- DeferStmt ----
class DeferStmt : public Stmt {
public:
    DeferStmt(SourceLocation loc, std::unique_ptr<Stmt> stmt)
        : Stmt(NodeKind::DeferStmt, std::move(loc))
        , stmt_(std::move(stmt))
    {}

    Stmt* stmt() { return stmt_.get(); }
    const Stmt* stmt() const { return stmt_.get(); }
    std::unique_ptr<Stmt> takeStmt() { return std::move(stmt_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::DeferStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Stmt> stmt_;
};

// ---- ErrdeferStmt ----
// Error-scoped defer: executes the statement only if the function exits with an error.
// Like Zig's errdefer: guarantees cleanup on error paths only.
class ErrdeferStmt : public Stmt {
public:
    ErrdeferStmt(SourceLocation loc, std::unique_ptr<Stmt> stmt)
        : Stmt(NodeKind::ErrdeferStmt, std::move(loc))
        , stmt_(std::move(stmt))
    {}

    Stmt* stmt() { return stmt_.get(); }
    const Stmt* stmt() const { return stmt_.get(); }
    std::unique_ptr<Stmt> takeStmt() { return std::move(stmt_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ErrdeferStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Stmt> stmt_;
};

// ---- AtomicStmt ----
// Atomic operation: wraps a statement with CPU-level atomic memory ordering.
// The inner statement must be a simple assignment or compound assignment.
// Generates LLVM atomics (e.g., atomicrmw, fence, cmpxchg).
class AtomicStmt : public Stmt {
public:
    enum class Ordering : uint8_t {
        Relaxed,
        Acquire,
        Release,
        AcqRel,
        SeqCst
    };

    AtomicStmt(SourceLocation loc, std::unique_ptr<Stmt> inner, Ordering ordering = Ordering::SeqCst)
        : Stmt(NodeKind::AtomicStmt, std::move(loc))
        , inner_(std::move(inner))
        , ordering_(ordering)
    {}

    Stmt* inner() { return inner_.get(); }
    const Stmt* inner() const { return inner_.get(); }
    std::unique_ptr<Stmt> takeInner() { return std::move(inner_); }
    Ordering ordering() const { return ordering_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::AtomicStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Stmt> inner_;
    Ordering ordering_;
};

// ---- YieldStmt ----
// Cooperative yielding: pauses execution and hands control back to
// a user-defined scheduler. In bare-metal/fiber contexts, this is
// a direct context switch costing only a few CPU cycles.
// May optionally yield a value.
class YieldStmt : public Stmt {
public:
    explicit YieldStmt(SourceLocation loc, std::unique_ptr<Expr> value = nullptr)
        : Stmt(NodeKind::YieldStmt, std::move(loc))
        , value_(std::move(value))
    {}

    Expr* value() { return value_.get(); }
    const Expr* value() const { return value_.get(); }
    std::unique_ptr<Expr> takeValue() { return std::move(value_); }
    bool hasValue() const { return value_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::YieldStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> value_;
};

// ---- MatchStmt ----
// Pattern matching: match (expr) { pattern => body, ... }
// Works in both statement and expression position.
// Compiles into highly optimized hardware jump tables or branchless cascades.
// The compiler enforces exhaustive checking: every enum variant must be handled.
struct MatchArm {
    std::unique_ptr<Expr> pattern;  // Pattern to match (enum variant, literal, or _)
    std::unique_ptr<BlockStmt> body;
};

class MatchStmt : public Stmt {
public:
    MatchStmt(SourceLocation loc, std::unique_ptr<Expr> subject,
              std::vector<MatchArm> arms)
        : Stmt(NodeKind::MatchStmt, std::move(loc))
        , subject_(std::move(subject))
        , arms_(std::move(arms))
    {}

    Expr* subject() { return subject_.get(); }
    const Expr* subject() const { return subject_.get(); }
    const std::vector<MatchArm>& arms() const { return arms_; }
    std::vector<MatchArm>& arms() { return arms_; }
    size_t armCount() const { return arms_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::MatchStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> subject_;
    std::vector<MatchArm> arms_;
};

// ---- ConstDeclStmt ----
// Compile-time constant: const name = value
class ConstDeclStmt : public Stmt {
public:
    ConstDeclStmt(SourceLocation loc, std::string name, TypeId type,
                  std::unique_ptr<Expr> init)
        : Stmt(NodeKind::ConstDeclStmt, std::move(loc))
        , name_(std::move(name))
        , type_(type)
        , init_(std::move(init))
    {}
    const std::string& name() const { return name_; }
    TypeId declaredType() const { return type_; }
    bool hasType() const { return !type_.isNull(); }
    Expr* init() { return init_.get(); }
    const Expr* init() const { return init_.get(); }
    std::unique_ptr<Expr> takeInit() { return std::move(init_); }
    bool hasInit() const { return init_ != nullptr; }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::ConstDeclStmt; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::string name_;
    TypeId type_;
    std::unique_ptr<Expr> init_;
};

// ---- ParallelForStmt ----
// Parallel loop: parallel for name in iterable { body }
class ParallelForStmt : public Stmt {
public:
    ParallelForStmt(SourceLocation loc, std::string iterator_name,
                    std::unique_ptr<Expr> iterable,
                    std::unique_ptr<BlockStmt> body)
        : Stmt(NodeKind::ParallelForStmt, std::move(loc))
        , iterator_name_(std::move(iterator_name))
        , iterable_(std::move(iterable))
        , body_(std::move(body))
    {}
    const std::string& iteratorName() const { return iterator_name_; }
    Expr* iterable() { return iterable_.get(); }
    const Expr* iterable() const { return iterable_.get(); }
    BlockStmt* body() { return body_.get(); }
    const BlockStmt* body() const { return body_.get(); }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::ParallelForStmt; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::string iterator_name_;
    std::unique_ptr<Expr> iterable_;
    std::unique_ptr<BlockStmt> body_;
};

// ---- StaticAssertStmt ----
// Compile-time assertion: static_assert(condition, "message")
class StaticAssertStmt : public Stmt {
public:
    StaticAssertStmt(SourceLocation loc, std::unique_ptr<Expr> condition,
                     std::string message = "")
        : Stmt(NodeKind::StaticAssertStmt, std::move(loc))
        , condition_(std::move(condition))
        , message_(std::move(message))
    {}
    Expr* condition() { return condition_.get(); }
    const Expr* condition() const { return condition_.get(); }
    const std::string& message() const { return message_; }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::StaticAssertStmt; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::unique_ptr<Expr> condition_;
    std::string message_;
};

// ---- UnsafeBlockStmt ----
// unsafe { ... } block — relaxes borrow checking within the block
// The compiler still tracks everything, but enforcement is relaxed:
// borrow errors become warnings, bounds check failures are suppressed,
// and raw pointer operations are allowed.
class UnsafeBlockStmt : public Stmt {
public:
    UnsafeBlockStmt(SourceLocation loc, std::unique_ptr<BlockStmt> body)
        : Stmt(NodeKind::UnsafeBlockStmt, std::move(loc))
        , body_(std::move(body)) {}

    BlockStmt& body() { return *body_; }
    const BlockStmt& body() const { return *body_; }
    BlockStmt* bodyPtr() { return body_.get(); }
    const BlockStmt* bodyPtr() const { return body_.get(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::UnsafeBlockStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<BlockStmt> body_;
};

// ---- TypeofExpr ----
// Type query: typeof(expr) — returns the TypeId of the expression's type
class TypeofExpr : public Expr {
public:
    TypeofExpr(SourceLocation loc, std::unique_ptr<Expr> operand)
        : Expr(NodeKind::TypeofExpr, std::move(loc))
        , operand_(std::move(operand))
    {}
    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::TypeofExpr; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::unique_ptr<Expr> operand_;
};

// ---- AlignofExpr ----
// Alignment query: alignof(type) — returns the alignment of the type
class AlignofExpr : public Expr {
public:
    AlignofExpr(SourceLocation loc, TypeId target_type)
        : Expr(NodeKind::AlignofExpr, std::move(loc))
        , target_type_(target_type)
        , expr_(nullptr)
    {}
    AlignofExpr(SourceLocation loc, std::unique_ptr<Expr> expr)
        : Expr(NodeKind::AlignofExpr, std::move(loc))
        , target_type_()
        , expr_(std::move(expr))
    {}
    TypeId targetType() const { return target_type_; }
    Expr* expr() { return expr_.get(); }
    const Expr* expr() const { return expr_.get(); }
    bool isTypeOperand() const { return !target_type_.isNull(); }
    bool isExprOperand() const { return expr_ != nullptr; }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::AlignofExpr; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    TypeId target_type_;
    std::unique_ptr<Expr> expr_;
};

// ---- ReflectExpr ----
// Compile-time reflection: reflect(T) — returns type metadata
class ReflectExpr : public Expr {
public:
    ReflectExpr(SourceLocation loc, TypeId target_type)
        : Expr(NodeKind::ReflectExpr, std::move(loc))
        , target_type_(target_type)
    {}
    TypeId targetType() const { return target_type_; }
    void setTargetType(TypeId t) { target_type_ = t; }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::ReflectExpr; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    TypeId target_type_;
};

// ---- AwaitExpr ----
// Async await: await expr — suspends until the async operation completes
class AwaitExpr : public Expr {
public:
    AwaitExpr(SourceLocation loc, std::unique_ptr<Expr> operand)
        : Expr(NodeKind::AwaitExpr, std::move(loc))
        , operand_(std::move(operand))
    {}
    Expr* operand() { return operand_.get(); }
    const Expr* operand() const { return operand_.get(); }
    std::unique_ptr<Expr> takeOperand() { return std::move(operand_); }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::AwaitExpr; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::unique_ptr<Expr> operand_;
};

// ---- SpawnStmt ----
// Structured async task dispatch: spawn expr;
// Schedules a function directly onto a lock-free work-stealing thread pool.
// Returns a handle that can be awaited for the result.
class SpawnStmt : public Stmt {
public:
    SpawnStmt(SourceLocation loc, std::unique_ptr<Expr> task)
        : Stmt(NodeKind::SpawnStmt, std::move(loc))
        , task_(std::move(task))
    {}

    Expr* task() { return task_.get(); }
    const Expr* task() const { return task_.get(); }
    std::unique_ptr<Expr> takeTask() { return std::move(task_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::SpawnStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> task_;
};

// ---- IfStmt ----
class IfStmt : public Stmt {
public:
    IfStmt(SourceLocation loc, std::unique_ptr<Expr> condition,
           std::unique_ptr<BlockStmt> then_block,
           std::unique_ptr<BlockStmt> else_block = nullptr)
        : Stmt(NodeKind::IfStmt, std::move(loc))
        , condition_(std::move(condition))
        , then_block_(std::move(then_block))
        , else_block_(std::move(else_block))
    {}

    Expr* condition() { return condition_.get(); }
    const Expr* condition() const { return condition_.get(); }
    BlockStmt* thenBlock() { return then_block_.get(); }
    const BlockStmt* thenBlock() const { return then_block_.get(); }
    BlockStmt* elseBlock() { return else_block_.get(); }
    const BlockStmt* elseBlock() const { return else_block_.get(); }
    bool hasElse() const { return else_block_ != nullptr; }

    std::unique_ptr<Expr> takeCondition() { return std::move(condition_); }
    std::unique_ptr<BlockStmt> takeThenBlock() { return std::move(then_block_); }
    std::unique_ptr<BlockStmt> takeElseBlock() { return std::move(else_block_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::IfStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<BlockStmt> then_block_;
    std::unique_ptr<BlockStmt> else_block_;
};

// ---- WhileStmt ----
// Supports Zig-style: while (cond) : (incr) { body }
class WhileStmt : public Stmt {
public:
    WhileStmt(SourceLocation loc, std::unique_ptr<Expr> condition,
              std::unique_ptr<BlockStmt> body,
              std::unique_ptr<Expr> increment = nullptr)
        : Stmt(NodeKind::WhileStmt, std::move(loc))
        , condition_(std::move(condition))
        , body_(std::move(body))
        , increment_(std::move(increment))
    {}

    Expr* condition() { return condition_.get(); }
    const Expr* condition() const { return condition_.get(); }
    BlockStmt* body() { return body_.get(); }
    const BlockStmt* body() const { return body_.get(); }
    Expr* increment() { return increment_.get(); }
    const Expr* increment() const { return increment_.get(); }
    bool hasIncrement() const { return increment_ != nullptr; }

    std::unique_ptr<Expr> takeCondition() { return std::move(condition_); }
    std::unique_ptr<BlockStmt> takeBody() { return std::move(body_); }
    std::unique_ptr<Expr> takeIncrement() { return std::move(increment_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::WhileStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> condition_;
    std::unique_ptr<BlockStmt> body_;
    std::unique_ptr<Expr> increment_;
};

// ---- ReturnStmt ----
class ReturnStmt : public Stmt {
public:
    explicit ReturnStmt(SourceLocation loc, std::unique_ptr<Expr> value = nullptr)
        : Stmt(NodeKind::ReturnStmt, std::move(loc))
        , value_(std::move(value))
    {}

    Expr* value() { return value_.get(); }
    const Expr* value() const { return value_.get(); }
    std::unique_ptr<Expr> takeValue() { return std::move(value_); }
    bool hasValue() const { return value_ != nullptr; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ReturnStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> value_;
};

// ---- BreakStmt ----
class BreakStmt : public Stmt {
public:
    explicit BreakStmt(SourceLocation loc)
        : Stmt(NodeKind::BreakStmt, std::move(loc))
    {}

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::BreakStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
};

// ---- ContinueStmt ----
class ContinueStmt : public Stmt {
public:
    explicit ContinueStmt(SourceLocation loc)
        : Stmt(NodeKind::ContinueStmt, std::move(loc))
    {}

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ContinueStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
};

// ---- ExprStmt ----
class ExprStmt : public Stmt {
public:
    ExprStmt(SourceLocation loc, std::unique_ptr<Expr> expr)
        : Stmt(NodeKind::ExprStmt, std::move(loc))
        , expr_(std::move(expr))
    {}

    Expr* expr() { return expr_.get(); }
    const Expr* expr() const { return expr_.get(); }
    std::unique_ptr<Expr> takeExpr() { return std::move(expr_); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ExprStmt;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::unique_ptr<Expr> expr_;
};

// ============================================================================
// Top-Level Declaration Nodes
// ============================================================================

// FnParam is defined in sema/Type.h and reused here in the AST

// ---- StructFieldDecl - field in a struct declaration ----
struct StructFieldDecl {
    std::string name;
    TypeId type;
    SourceLocation loc;
    std::string unresolved_type_name;  // BUG FIX: stores type name text when type is unresolved

    StructFieldDecl(std::string n, TypeId t, SourceLocation l)
        : name(std::move(n)), type(t), loc(std::move(l)) {}
};

// ---- EnumVariantDecl - variant in an enum declaration ----
struct EnumVariantDecl {
    std::string name;
    std::optional<int64_t> value;
    SourceLocation loc;

    EnumVariantDecl(std::string n, std::optional<int64_t> v, SourceLocation l)
        : name(std::move(n)), value(v), loc(std::move(l)) {}
};

// ---- FnDecl ----
class FnDecl : public TopLevel {
public:
    FnDecl(SourceLocation loc, std::string name,
           std::vector<FnParam> params, TypeId return_type,
           std::unique_ptr<BlockStmt> body,
           bool is_pure = false,
           TypeId error_type = TypeId(),
           std::vector<CompilerDirective> directives = {},
           std::vector<std::string> type_params = {},
           std::vector<TypeId> type_param_bounds = {})
        : TopLevel(NodeKind::FnDecl, std::move(loc))
        , name_(std::move(name))
        , params_(std::move(params))
        , return_type_(return_type)
        , body_(std::move(body))
        , is_pure_(is_pure)
        , is_inline_(false)
        , is_noalloc_(false)
        , is_async_(false)
        , error_type_(error_type)
        , directives_(std::move(directives))
        , type_params_(std::move(type_params))
        , type_param_bounds_(std::move(type_param_bounds))
    {}

    const std::string& name() const { return name_; }
    const std::vector<FnParam>& params() const { return params_; }
    std::vector<FnParam>& params() { return params_; }
    void setParamType(size_t index, TypeId type) {
        if (index < params_.size()) params_[index].type = type;
    }
    TypeId returnType() const { return return_type_; }
    BlockStmt* body() { return body_.get(); }
    const BlockStmt* body() const { return body_.get(); }
    std::unique_ptr<BlockStmt> takeBody() { return std::move(body_); }
    bool isPure() const { return is_pure_; }
    bool isInline() const { return is_inline_; }
    void setInline(bool v) { is_inline_ = v; }
    bool isNoalloc() const { return is_noalloc_; }
    void setNoalloc(bool v) { is_noalloc_ = v; }
    bool isAsync() const { return is_async_; }
    void setAsync(bool v) { is_async_ = v; }
    TypeId errorType() const { return error_type_; }
    bool canError() const { return !error_type_.isNull(); }
    const std::vector<CompilerDirective>& directives() const { return directives_; }
    size_t paramCount() const { return params_.size(); }

    // Check if this function has a specific directive
    bool hasDirective(CompilerDirective d) const {
        for (const auto& dir : directives_) {
            if (dir == d) return true;
        }
        return false;
    }

    // Type parameters for generic functions (e.g., ["T"] for fn max<T>(a: T, b: T) -> T)
    const std::vector<std::string>& typeParams() const { return type_params_; }
    const std::vector<TypeId>& typeParamBounds() const { return type_param_bounds_; }
    bool isGeneric() const { return !type_params_.empty(); }
    size_t typeParamCount() const { return type_params_.size(); }

    // Check if any parameter is an allocator parameter
    bool hasAllocatorParam() const {
        for (const auto& p : params_) {
            if (p.type && isa<AllocatorType>(p.type)) return true;
        }
        return false;
    }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::FnDecl;
    }

    // Unresolved return type name (for forward references)
    std::string unresolved_return_type_name;
    void setReturnType(TypeId type) { return_type_ = type; }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    std::vector<FnParam> params_;
    TypeId return_type_;
    std::unique_ptr<BlockStmt> body_;
    bool is_pure_;
    bool is_inline_;
    bool is_noalloc_;
    bool is_async_ = false;
    TypeId error_type_;
    std::vector<CompilerDirective> directives_;
    std::vector<std::string> type_params_;       // e.g., ["T"] for fn max<T>
    std::vector<TypeId> type_param_bounds_;      // e.g., [Ord] for fn max<T: Ord>
};

// ---- StructDecl ----
class StructDecl : public TopLevel {
public:
    StructDecl(SourceLocation loc, std::string name,
               std::vector<StructFieldDecl> fields)
        : TopLevel(NodeKind::StructDecl, std::move(loc))
        , name_(std::move(name))
        , fields_(std::move(fields))
    {}

    const std::string& name() const { return name_; }
    const std::vector<StructFieldDecl>& fields() const { return fields_; }
    std::vector<StructFieldDecl>& fields() { return fields_; }
    size_t fieldCount() const { return fields_.size(); }

    const StructFieldDecl* findField(const std::string& fname) const {
        for (const auto& f : fields_) {
            if (f.name == fname) return &f;
        }
        return nullptr;
    }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::StructDecl;
    }

    void setSoA(bool v) { is_soa_ = v; }
    bool isSoA() const { return is_soa_; }

    void setAlignment(uint32_t a) { alignment_ = a; }
    uint32_t alignment() const { return alignment_; }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    std::vector<StructFieldDecl> fields_;
    bool is_soa_ = false;
    uint32_t alignment_ = 0;
};

// ---- EnumDecl ----
class EnumDecl : public TopLevel {
public:
    EnumDecl(SourceLocation loc, std::string name,
             std::vector<EnumVariantDecl> variants)
        : TopLevel(NodeKind::EnumDecl, std::move(loc))
        , name_(std::move(name))
        , variants_(std::move(variants))
    {}

    const std::string& name() const { return name_; }
    const std::vector<EnumVariantDecl>& variants() const { return variants_; }
    size_t variantCount() const { return variants_.size(); }

    const EnumVariantDecl* findVariant(const std::string& vname) const {
        for (const auto& v : variants_) {
            if (v.name == vname) return &v;
        }
        return nullptr;
    }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::EnumDecl;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    std::vector<EnumVariantDecl> variants_;
};

// ---- ImportDecl ----
class ImportDecl : public TopLevel {
public:
    ImportDecl(SourceLocation loc, std::string path)
        : TopLevel(NodeKind::ImportDecl, std::move(loc))
        , path_(std::move(path))
    {}

    const std::string& path() const { return path_; }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ImportDecl;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string path_;
};

// ---- TraitMethodDecl - method signature in a trait ----
struct TraitMethodDecl {
    std::string name;
    std::vector<FnParam> params;
    TypeId return_type;
    TypeId error_type;
    SourceLocation loc;
};

// ---- TraitDecl ----
// Shared behavior contract: trait Name { fn method(...); ... }
// Enables zero-overhead polymorphism through compile-time monomorphization
// rather than traditional v-tables. Traits define interface contracts that
// structs can implement via `impl`.
class TraitDecl : public TopLevel {
public:
    TraitDecl(SourceLocation loc, std::string name,
              std::vector<TraitMethodDecl> methods)
        : TopLevel(NodeKind::TraitDecl, std::move(loc))
        , name_(std::move(name))
        , methods_(std::move(methods))
    {}

    const std::string& name() const { return name_; }
    const std::vector<TraitMethodDecl>& methods() const { return methods_; }
    size_t methodCount() const { return methods_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::TraitDecl;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string name_;
    std::vector<TraitMethodDecl> methods_;
};

// ---- ImplDecl ----
// Trait/behavior implementation: impl TraitName for StructName { ... }
// or: impl StructName { ... } (inherent impl)
// Cleanly couples methods and layout traits to data paradigms without
// structural coupling — keeping data declarations separated from behavior.
class ImplDecl : public TopLevel {
public:
    ImplDecl(SourceLocation loc, std::string trait_name, std::string struct_name,
             std::vector<std::unique_ptr<FnDecl>> methods)
        : TopLevel(NodeKind::ImplDecl, std::move(loc))
        , trait_name_(std::move(trait_name))
        , struct_name_(std::move(struct_name))
        , methods_(std::move(methods))
    {}

    const std::string& traitName() const { return trait_name_; }
    bool hasTrait() const { return !trait_name_.empty(); }
    const std::string& structName() const { return struct_name_; }
    const std::vector<std::unique_ptr<FnDecl>>& methods() const { return methods_; }
    size_t methodCount() const { return methods_.size(); }

    static bool classof(const ASTNode* n) {
        return n->getKind() == NodeKind::ImplDecl;
    }

    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;

private:
    std::string trait_name_;    // Empty for inherent impl (impl StructName { })
    std::string struct_name_;
    std::vector<std::unique_ptr<FnDecl>> methods_;
};

// ---- ModuleDecl ----
// Module declaration: module name;
// Declares the current file as part of a named module for namespacing.
class ModuleDecl : public TopLevel {
public:
    ModuleDecl(SourceLocation loc, std::string name)
        : TopLevel(NodeKind::ModuleDecl, std::move(loc))
        , name_(std::move(name))
    {}
    const std::string& name() const { return name_; }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::ModuleDecl; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::string name_;
};

// ---- UseDecl ----
// Selective import: use module::name or use module::name as alias
// Brings specific symbols from other modules into the current scope.
class UseDecl : public TopLevel {
public:
    UseDecl(SourceLocation loc, std::string module_path, std::string item_name,
            std::string alias = "")
        : TopLevel(NodeKind::UseDecl, std::move(loc))
        , module_path_(std::move(module_path))
        , item_name_(std::move(item_name))
        , alias_(std::move(alias))
    {}
    const std::string& modulePath() const { return module_path_; }
    const std::string& itemName() const { return item_name_; }
    const std::string& alias() const { return alias_; }
    bool hasAlias() const { return !alias_.empty(); }
    static bool classof(const ASTNode* n) { return n->getKind() == NodeKind::UseDecl; }
    void accept(ASTVisitor& visitor) override;
    void accept(ConstASTVisitor& visitor) const override;
private:
    std::string module_path_;
    std::string item_name_;
    std::string alias_;
};

// ============================================================================
// Visitor base classes (abstract, for dynamic dispatch)
// ============================================================================

class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    // Expression visitors
    virtual void visitIntLiteral(IntLiteral&) {}
    virtual void visitFloatLiteral(FloatLiteral&) {}
    virtual void visitBoolLiteral(BoolLiteral&) {}
    virtual void visitStringLiteral(StringLiteral&) {}
    virtual void visitIdentExpr(IdentExpr&) {}
    virtual void visitBinaryExpr(BinaryExpr&) {}
    virtual void visitUnaryExpr(UnaryExpr&) {}
    virtual void visitCallExpr(CallExpr&) {}
    virtual void visitMemberExpr(MemberExpr&) {}
    virtual void visitIndexExpr(IndexExpr&) {}
    virtual void visitDerefExpr(DerefExpr&) {}
    virtual void visitAddrOfExpr(AddrOfExpr&) {}
    virtual void visitCastExpr(CastExpr&) {}
    virtual void visitTypeofExpr(TypeofExpr&) {}
    virtual void visitAlignofExpr(AlignofExpr&) {}
    virtual void visitReflectExpr(ReflectExpr&) {}
    virtual void visitAwaitExpr(AwaitExpr&) {}
    virtual void visitStructInitExpr(StructInitExpr&) {}
    virtual void visitArrayInitExpr(ArrayInitExpr&) {}
    virtual void visitSliceExpr(SliceExpr&) {}
    virtual void visitSizeofExpr(SizeofExpr&) {}
    virtual void visitUnsafeExpr(UnsafeExpr&) {}
    virtual void visitPoisonExpr(PoisonExpr&) {}
    virtual void visitTryExpr(TryExpr&) {}
    virtual void visitComptimeExpr(ComptimeExpr&) {}
    virtual void visitReduceExpr(ReduceExpr&) {}

    // Statement visitors
    virtual void visitVarDeclStmt(VarDeclStmt&) {}
    virtual void visitValDeclStmt(ValDeclStmt&) {}
    virtual void visitAssignStmt(AssignStmt&) {}
    virtual void visitDeferStmt(DeferStmt&) {}
    virtual void visitIfStmt(IfStmt&) {}
    virtual void visitWhileStmt(WhileStmt&) {}
    virtual void visitReturnStmt(ReturnStmt&) {}
    virtual void visitBreakStmt(BreakStmt&) {}
    virtual void visitContinueStmt(ContinueStmt&) {}
    virtual void visitExprStmt(ExprStmt&) {}
    virtual void visitBlockStmt(BlockStmt&) {}
    virtual void visitErrdeferStmt(ErrdeferStmt&) {}
    virtual void visitAtomicStmt(AtomicStmt&) {}
    virtual void visitYieldStmt(YieldStmt&) {}
    virtual void visitMatchStmt(MatchStmt&) {}
    virtual void visitConstDeclStmt(ConstDeclStmt&) {}
    virtual void visitParallelForStmt(ParallelForStmt&) {}
    virtual void visitStaticAssertStmt(StaticAssertStmt&) {}
    virtual void visitSpawnStmt(SpawnStmt&) {}

    // Top-level visitors
    virtual void visitFnDecl(FnDecl&) {}
    virtual void visitStructDecl(StructDecl&) {}
    virtual void visitEnumDecl(EnumDecl&) {}
    virtual void visitImportDecl(ImportDecl&) {}
    virtual void visitTraitDecl(TraitDecl&) {}
    virtual void visitModuleDecl(ModuleDecl&) {}
    virtual void visitUseDecl(UseDecl&) {}
    virtual void visitImplDecl(ImplDecl&) {}
};

class ConstASTVisitor {
public:
    virtual ~ConstASTVisitor() = default;

    // Expression visitors
    virtual void visitIntLiteral(const IntLiteral&) {}
    virtual void visitFloatLiteral(const FloatLiteral&) {}
    virtual void visitBoolLiteral(const BoolLiteral&) {}
    virtual void visitStringLiteral(const StringLiteral&) {}
    virtual void visitIdentExpr(const IdentExpr&) {}
    virtual void visitBinaryExpr(const BinaryExpr&) {}
    virtual void visitUnaryExpr(const UnaryExpr&) {}
    virtual void visitCallExpr(const CallExpr&) {}
    virtual void visitMemberExpr(const MemberExpr&) {}
    virtual void visitIndexExpr(const IndexExpr&) {}
    virtual void visitDerefExpr(const DerefExpr&) {}
    virtual void visitAddrOfExpr(const AddrOfExpr&) {}
    virtual void visitCastExpr(const CastExpr&) {}
    virtual void visitTypeofExpr(const TypeofExpr&) {}
    virtual void visitAlignofExpr(const AlignofExpr&) {}
    virtual void visitReflectExpr(const ReflectExpr&) {}
    virtual void visitAwaitExpr(const AwaitExpr&) {}
    virtual void visitStructInitExpr(const StructInitExpr&) {}
    virtual void visitArrayInitExpr(const ArrayInitExpr&) {}
    virtual void visitSliceExpr(const SliceExpr&) {}
    virtual void visitSizeofExpr(const SizeofExpr&) {}
    virtual void visitUnsafeExpr(const UnsafeExpr&) {}
    virtual void visitPoisonExpr(const PoisonExpr&) {}
    virtual void visitTryExpr(const TryExpr&) {}
    virtual void visitComptimeExpr(const ComptimeExpr&) {}
    virtual void visitReduceExpr(const ReduceExpr&) {}

    // Statement visitors
    virtual void visitVarDeclStmt(const VarDeclStmt&) {}
    virtual void visitValDeclStmt(const ValDeclStmt&) {}
    virtual void visitAssignStmt(const AssignStmt&) {}
    virtual void visitDeferStmt(const DeferStmt&) {}
    virtual void visitIfStmt(const IfStmt&) {}
    virtual void visitWhileStmt(const WhileStmt&) {}
    virtual void visitReturnStmt(const ReturnStmt&) {}
    virtual void visitBreakStmt(const BreakStmt&) {}
    virtual void visitContinueStmt(const ContinueStmt&) {}
    virtual void visitExprStmt(const ExprStmt&) {}
    virtual void visitBlockStmt(const BlockStmt&) {}
    virtual void visitErrdeferStmt(const ErrdeferStmt&) {}
    virtual void visitAtomicStmt(const AtomicStmt&) {}
    virtual void visitYieldStmt(const YieldStmt&) {}
    virtual void visitMatchStmt(const MatchStmt&) {}
    virtual void visitConstDeclStmt(const ConstDeclStmt&) {}
    virtual void visitParallelForStmt(const ParallelForStmt&) {}
    virtual void visitStaticAssertStmt(const StaticAssertStmt&) {}
    virtual void visitSpawnStmt(const SpawnStmt&) {}

    // Top-level visitors
    virtual void visitFnDecl(const FnDecl&) {}
    virtual void visitStructDecl(const StructDecl&) {}
    virtual void visitEnumDecl(const EnumDecl&) {}
    virtual void visitImportDecl(const ImportDecl&) {}
    virtual void visitTraitDecl(const TraitDecl&) {}
    virtual void visitModuleDecl(const ModuleDecl&) {}
    virtual void visitUseDecl(const UseDecl&) {}
    virtual void visitImplDecl(const ImplDecl&) {}
};

// ============================================================================
// accept() implementations (inline, dispatch to visitor)
// ============================================================================

inline void IntLiteral::accept(ASTVisitor& v) { v.visitIntLiteral(*this); }
inline void IntLiteral::accept(ConstASTVisitor& v) const { v.visitIntLiteral(*this); }

inline void FloatLiteral::accept(ASTVisitor& v) { v.visitFloatLiteral(*this); }
inline void FloatLiteral::accept(ConstASTVisitor& v) const { v.visitFloatLiteral(*this); }

inline void BoolLiteral::accept(ASTVisitor& v) { v.visitBoolLiteral(*this); }
inline void BoolLiteral::accept(ConstASTVisitor& v) const { v.visitBoolLiteral(*this); }

inline void StringLiteral::accept(ASTVisitor& v) { v.visitStringLiteral(*this); }
inline void StringLiteral::accept(ConstASTVisitor& v) const { v.visitStringLiteral(*this); }

inline void IdentExpr::accept(ASTVisitor& v) { v.visitIdentExpr(*this); }
inline void IdentExpr::accept(ConstASTVisitor& v) const { v.visitIdentExpr(*this); }

inline void BinaryExpr::accept(ASTVisitor& v) { v.visitBinaryExpr(*this); }
inline void BinaryExpr::accept(ConstASTVisitor& v) const { v.visitBinaryExpr(*this); }

inline void UnaryExpr::accept(ASTVisitor& v) { v.visitUnaryExpr(*this); }
inline void UnaryExpr::accept(ConstASTVisitor& v) const { v.visitUnaryExpr(*this); }

inline void CallExpr::accept(ASTVisitor& v) { v.visitCallExpr(*this); }
inline void CallExpr::accept(ConstASTVisitor& v) const { v.visitCallExpr(*this); }

inline void MemberExpr::accept(ASTVisitor& v) { v.visitMemberExpr(*this); }
inline void MemberExpr::accept(ConstASTVisitor& v) const { v.visitMemberExpr(*this); }

inline void IndexExpr::accept(ASTVisitor& v) { v.visitIndexExpr(*this); }
inline void IndexExpr::accept(ConstASTVisitor& v) const { v.visitIndexExpr(*this); }

inline void DerefExpr::accept(ASTVisitor& v) { v.visitDerefExpr(*this); }
inline void DerefExpr::accept(ConstASTVisitor& v) const { v.visitDerefExpr(*this); }

inline void AddrOfExpr::accept(ASTVisitor& v) { v.visitAddrOfExpr(*this); }
inline void AddrOfExpr::accept(ConstASTVisitor& v) const { v.visitAddrOfExpr(*this); }

inline void CastExpr::accept(ASTVisitor& v) { v.visitCastExpr(*this); }
inline void CastExpr::accept(ConstASTVisitor& v) const { v.visitCastExpr(*this); }

inline void TypeofExpr::accept(ASTVisitor& v) { v.visitTypeofExpr(*this); }
inline void TypeofExpr::accept(ConstASTVisitor& v) const { v.visitTypeofExpr(*this); }

inline void AlignofExpr::accept(ASTVisitor& v) { v.visitAlignofExpr(*this); }
inline void AlignofExpr::accept(ConstASTVisitor& v) const { v.visitAlignofExpr(*this); }

inline void ReflectExpr::accept(ASTVisitor& v) { v.visitReflectExpr(*this); }
inline void ReflectExpr::accept(ConstASTVisitor& v) const { v.visitReflectExpr(*this); }

inline void AwaitExpr::accept(ASTVisitor& v) { v.visitAwaitExpr(*this); }
inline void AwaitExpr::accept(ConstASTVisitor& v) const { v.visitAwaitExpr(*this); }

inline void StructInitExpr::accept(ASTVisitor& v) { v.visitStructInitExpr(*this); }
inline void StructInitExpr::accept(ConstASTVisitor& v) const { v.visitStructInitExpr(*this); }

inline void ArrayInitExpr::accept(ASTVisitor& v) { v.visitArrayInitExpr(*this); }
inline void ArrayInitExpr::accept(ConstASTVisitor& v) const { v.visitArrayInitExpr(*this); }
inline void SliceExpr::accept(ASTVisitor& v) { v.visitSliceExpr(*this); }
inline void SliceExpr::accept(ConstASTVisitor& v) const { v.visitSliceExpr(*this); }

inline void SizeofExpr::accept(ASTVisitor& v) { v.visitSizeofExpr(*this); }
inline void SizeofExpr::accept(ConstASTVisitor& v) const { v.visitSizeofExpr(*this); }

inline void UnsafeExpr::accept(ASTVisitor& v) { v.visitUnsafeExpr(*this); }
inline void UnsafeExpr::accept(ConstASTVisitor& v) const { v.visitUnsafeExpr(*this); }

inline void PoisonExpr::accept(ASTVisitor& v) { v.visitPoisonExpr(*this); }
inline void PoisonExpr::accept(ConstASTVisitor& v) const { v.visitPoisonExpr(*this); }

inline void TryExpr::accept(ASTVisitor& v) { v.visitTryExpr(*this); }
inline void TryExpr::accept(ConstASTVisitor& v) const { v.visitTryExpr(*this); }

inline void ComptimeExpr::accept(ASTVisitor& v) { v.visitComptimeExpr(*this); }
inline void ComptimeExpr::accept(ConstASTVisitor& v) const { v.visitComptimeExpr(*this); }

inline void ReduceExpr::accept(ASTVisitor& v) { v.visitReduceExpr(*this); }
inline void ReduceExpr::accept(ConstASTVisitor& v) const { v.visitReduceExpr(*this); }

inline void VarDeclStmt::accept(ASTVisitor& v) { v.visitVarDeclStmt(*this); }
inline void VarDeclStmt::accept(ConstASTVisitor& v) const { v.visitVarDeclStmt(*this); }

inline void ValDeclStmt::accept(ASTVisitor& v) { v.visitValDeclStmt(*this); }
inline void ValDeclStmt::accept(ConstASTVisitor& v) const { v.visitValDeclStmt(*this); }

inline void AssignStmt::accept(ASTVisitor& v) { v.visitAssignStmt(*this); }
inline void AssignStmt::accept(ConstASTVisitor& v) const { v.visitAssignStmt(*this); }

inline void DeferStmt::accept(ASTVisitor& v) { v.visitDeferStmt(*this); }
inline void DeferStmt::accept(ConstASTVisitor& v) const { v.visitDeferStmt(*this); }

inline void ErrdeferStmt::accept(ASTVisitor& v) { v.visitErrdeferStmt(*this); }
inline void ErrdeferStmt::accept(ConstASTVisitor& v) const { v.visitErrdeferStmt(*this); }

inline void AtomicStmt::accept(ASTVisitor& v) { v.visitAtomicStmt(*this); }
inline void AtomicStmt::accept(ConstASTVisitor& v) const { v.visitAtomicStmt(*this); }

inline void YieldStmt::accept(ASTVisitor& v) { v.visitYieldStmt(*this); }
inline void YieldStmt::accept(ConstASTVisitor& v) const { v.visitYieldStmt(*this); }

inline void MatchStmt::accept(ASTVisitor& v) { v.visitMatchStmt(*this); }
inline void MatchStmt::accept(ConstASTVisitor& v) const { v.visitMatchStmt(*this); }

inline void ConstDeclStmt::accept(ASTVisitor& v) { v.visitConstDeclStmt(*this); }
inline void ConstDeclStmt::accept(ConstASTVisitor& v) const { v.visitConstDeclStmt(*this); }

inline void ParallelForStmt::accept(ASTVisitor& v) { v.visitParallelForStmt(*this); }
inline void ParallelForStmt::accept(ConstASTVisitor& v) const { v.visitParallelForStmt(*this); }

inline void StaticAssertStmt::accept(ASTVisitor& v) { v.visitStaticAssertStmt(*this); }
inline void StaticAssertStmt::accept(ConstASTVisitor& v) const { v.visitStaticAssertStmt(*this); }

inline void SpawnStmt::accept(ASTVisitor& v) { v.visitSpawnStmt(*this); }
inline void SpawnStmt::accept(ConstASTVisitor& v) const { v.visitSpawnStmt(*this); }

inline void IfStmt::accept(ASTVisitor& v) { v.visitIfStmt(*this); }
inline void IfStmt::accept(ConstASTVisitor& v) const { v.visitIfStmt(*this); }

inline void WhileStmt::accept(ASTVisitor& v) { v.visitWhileStmt(*this); }
inline void WhileStmt::accept(ConstASTVisitor& v) const { v.visitWhileStmt(*this); }

inline void ReturnStmt::accept(ASTVisitor& v) { v.visitReturnStmt(*this); }
inline void ReturnStmt::accept(ConstASTVisitor& v) const { v.visitReturnStmt(*this); }

inline void BreakStmt::accept(ASTVisitor& v) { v.visitBreakStmt(*this); }
inline void BreakStmt::accept(ConstASTVisitor& v) const { v.visitBreakStmt(*this); }

inline void ContinueStmt::accept(ASTVisitor& v) { v.visitContinueStmt(*this); }
inline void ContinueStmt::accept(ConstASTVisitor& v) const { v.visitContinueStmt(*this); }

inline void ExprStmt::accept(ASTVisitor& v) { v.visitExprStmt(*this); }
inline void ExprStmt::accept(ConstASTVisitor& v) const { v.visitExprStmt(*this); }

inline void BlockStmt::accept(ASTVisitor& v) { v.visitBlockStmt(*this); }
inline void BlockStmt::accept(ConstASTVisitor& v) const { v.visitBlockStmt(*this); }

inline void FnDecl::accept(ASTVisitor& v) { v.visitFnDecl(*this); }
inline void FnDecl::accept(ConstASTVisitor& v) const { v.visitFnDecl(*this); }

inline void StructDecl::accept(ASTVisitor& v) { v.visitStructDecl(*this); }
inline void StructDecl::accept(ConstASTVisitor& v) const { v.visitStructDecl(*this); }

inline void EnumDecl::accept(ASTVisitor& v) { v.visitEnumDecl(*this); }
inline void EnumDecl::accept(ConstASTVisitor& v) const { v.visitEnumDecl(*this); }

inline void ImportDecl::accept(ASTVisitor& v) { v.visitImportDecl(*this); }
inline void ImportDecl::accept(ConstASTVisitor& v) const { v.visitImportDecl(*this); }

inline void ModuleDecl::accept(ASTVisitor& v) { v.visitModuleDecl(*this); }
inline void ModuleDecl::accept(ConstASTVisitor& v) const { v.visitModuleDecl(*this); }

inline void UseDecl::accept(ASTVisitor& v) { v.visitUseDecl(*this); }
inline void UseDecl::accept(ConstASTVisitor& v) const { v.visitUseDecl(*this); }

inline void TraitDecl::accept(ASTVisitor& v) { v.visitTraitDecl(*this); }
inline void TraitDecl::accept(ConstASTVisitor& v) const { v.visitTraitDecl(*this); }

inline void ImplDecl::accept(ASTVisitor& v) { v.visitImplDecl(*this); }
inline void ImplDecl::accept(ConstASTVisitor& v) const { v.visitImplDecl(*this); }

} // namespace tether
