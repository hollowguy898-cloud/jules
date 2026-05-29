#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "opt/PreLLVMPassBase.h"
#include <unordered_map>
#include <string>
#include <cstdint>
#include <variant>
#include <vector>

namespace tether {

// ============================================================================
// A compile-time value — the result of comptime evaluation
// ============================================================================
struct ComptimeValue {
    enum class Kind : uint8_t {
        Int, Float, Bool, String, Struct, Array, Poison
    };

    Kind kind;
    union {
        int64_t int_val;
        double float_val;
        bool bool_val;
    };
    std::string string_val;

    // For struct/array values: field/element values
    std::vector<ComptimeValue> elements;
    std::vector<std::string> field_names; // For struct

    TypeId type; // The Tether type of this value

    static ComptimeValue makeInt(int64_t v, TypeId t = TypeId()) {
        ComptimeValue val;
        val.kind = Kind::Int;
        val.int_val = v;
        val.type = t;
        return val;
    }
    static ComptimeValue makeFloat(double v, TypeId t = TypeId()) {
        ComptimeValue val;
        val.kind = Kind::Float;
        val.float_val = v;
        val.type = t;
        return val;
    }
    static ComptimeValue makeBool(bool v, TypeId t = TypeId()) {
        ComptimeValue val;
        val.kind = Kind::Bool;
        val.bool_val = v;
        val.type = t;
        return val;
    }
    static ComptimeValue makeString(std::string v, TypeId t = TypeId()) {
        ComptimeValue val;
        val.kind = Kind::String;
        val.string_val = std::move(v);
        val.type = t;
        return val;
    }
    static ComptimeValue makePoison() {
        ComptimeValue val;
        val.kind = Kind::Poison;
        val.int_val = 0;
        val.type = TypeId();
        return val;
    }

    bool isPoison() const { return kind == Kind::Poison; }
    bool isInt() const { return kind == Kind::Int; }
    bool isFloat() const { return kind == Kind::Float; }
    bool isBool() const { return kind == Kind::Bool; }
    bool isString() const { return kind == Kind::String; }
    bool isStruct() const { return kind == Kind::Struct; }
    bool isArray() const { return kind == Kind::Array; }

    int64_t asInt() const { return int_val; }
    double asFloat() const { return float_val; }
    bool asBool() const { return bool_val; }
    const std::string& asString() const { return string_val; }

    // Default constructor needed for unordered_map — creates a Poison value
    ComptimeValue() : kind(Kind::Poison), int_val(0) {}
};

// ============================================================================
// Compile-time evaluation context — tracks variable bindings
// ============================================================================
class ComptimeEvalContext {
public:
    // Bind a variable name to a compile-time value
    void bind(const std::string& name, ComptimeValue value) {
        bindings_[name] = std::move(value);
    }

    // Look up a variable binding
    const ComptimeValue* lookup(const std::string& name) const {
        auto it = bindings_.find(name);
        return it != bindings_.end() ? &it->second : nullptr;
    }

    // Push a new scope
    void pushScope() { scope_stack_.push_back({}); }

    // Pop a scope, removing any bindings added in that scope
    void popScope() {
        if (!scope_stack_.empty()) {
            for (const auto& name : scope_stack_.back()) {
                bindings_.erase(name);
            }
            scope_stack_.pop_back();
        }
    }

    // Bind in current scope (tracks for popScope)
    void bindInScope(const std::string& name, ComptimeValue value) {
        bindings_[name] = std::move(value);
        if (!scope_stack_.empty()) {
            scope_stack_.back().push_back(name);
        }
    }

private:
    std::unordered_map<std::string, ComptimeValue> bindings_;
    std::vector<std::vector<std::string>> scope_stack_;
};

// ============================================================================
// The comptime evaluator — evaluates expressions at compile time
// ============================================================================
class ComptimeEvaluator {
public:
    // Evaluate an expression at compile time. Returns a Poison value
    // if the expression cannot be evaluated (depends on runtime values).
    ComptimeValue evaluate(Expr* expr, TypeTable& type_table, ComptimeEvalContext& ctx);

    // Evaluate a statement at compile time (for const declarations, static_assert, etc.)
    // Returns true if the statement was fully evaluated at compile time.
    bool evaluateStmt(Stmt* stmt, TypeTable& type_table, ComptimeEvalContext& ctx);

    // Check if an expression is comptime-evaluable (pure, no side effects,
    // only references comptime-known values)
    bool isComptimeEvaluatable(Expr* expr, const ComptimeEvalContext& ctx) const;

private:
    ComptimeValue evalBinary(BinaryExpr* bin, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalUnary(UnaryExpr* un, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalCall(CallExpr* call, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalStructInit(StructInitExpr* init, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalArrayInit(ArrayInitExpr* init, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalMember(MemberExpr* member, TypeTable& tt, ComptimeEvalContext& ctx);
    ComptimeValue evalIndex(IndexExpr* index, TypeTable& tt, ComptimeEvalContext& ctx);

    // Recursively walk a block evaluating statements
    bool evalBlock(BlockStmt* block, TypeTable& tt, ComptimeEvalContext& ctx);
};

} // namespace tether
