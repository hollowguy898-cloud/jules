#pragma once

#include "sema/Type.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cassert>
#include <utility>

namespace jules {

// ============================================================================
// SymbolKind - classification of symbols
// ============================================================================
enum class SymbolKind : uint8_t {
    Var,      // mutable variable (var)
    Val,      // immutable variable (val)
    Param,    // function parameter
    Fn,       // function
    Struct,   // struct type
    Enum,     // enum type
    Field,    // struct field
    Variant,  // enum variant
    Import,   // imported module
    TypeParam // type parameter (for future generics)
};

std::string symbolKindToString(SymbolKind kind);

// ============================================================================
// Symbol - represents a named entity in a scope
// ============================================================================
class Symbol {
public:
    Symbol(std::string name, TypeId type, SymbolKind kind,
           bool is_mutable = false, SourceLocation loc = SourceLocation())
        : name_(std::move(name))
        , type_(type)
        , kind_(kind)
        , is_mutable_(is_mutable)
        , loc_(std::move(loc))
    {}

    // --- Accessors ---

    const std::string& name() const { return name_; }
    TypeId type() const { return type_; }
    SymbolKind kind() const { return kind_; }
    bool isMutable() const { return is_mutable_; }
    const SourceLocation& sourceLoc() const { return loc_; }

    // --- Mutability (for var declarations) ---

    void setMutable(bool m) { is_mutable_ = m; }

    // --- Type update (for type inference) ---

    void setType(TypeId t) { type_ = t; }

    // --- Kind queries ---

    bool isVar() const { return kind_ == SymbolKind::Var; }
    bool isVal() const { return kind_ == SymbolKind::Val; }
    bool isParam() const { return kind_ == SymbolKind::Param; }
    bool isFn() const { return kind_ == SymbolKind::Fn; }
    bool isStruct() const { return kind_ == SymbolKind::Struct; }
    bool isEnum() const { return kind_ == SymbolKind::Enum; }
    bool isField() const { return kind_ == SymbolKind::Field; }
    bool isVariant() const { return kind_ == SymbolKind::Variant; }
    bool isImport() const { return kind_ == SymbolKind::Import; }

    // --- Check if this symbol can be reassigned ---

    bool isAssignable() const {
        return is_mutable_ && (kind_ == SymbolKind::Var || kind_ == SymbolKind::Param);
    }

    // --- Check if this is a type symbol (struct, enum) ---

    bool isTypeSymbol() const {
        return kind_ == SymbolKind::Struct || kind_ == SymbolKind::Enum;
    }

private:
    std::string name_;
    TypeId type_;
    SymbolKind kind_;
    bool is_mutable_;
    SourceLocation loc_;
};

// ============================================================================
// Scope - represents a lexical scope with nested scoping
// ============================================================================
class Scope {
public:
    // Scope kind for diagnostic purposes
    enum class ScopeKind : uint8_t {
        Global,     // file-level scope
        Fn,         // function body scope
        Block,      // block scope { }
        Loop,       // while/for loop scope
        Struct,     // struct definition scope (fields)
        Enum,       // enum definition scope (variants)
        If,         // if-else scope
    };

    explicit Scope(ScopeKind kind, Scope* parent = nullptr)
        : kind_(kind), parent_(parent), depth_(parent ? parent->depth_ + 1 : 0)
    {}

    // Prevent copying
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    // --- Accessors ---

    ScopeKind scopeKind() const { return kind_; }
    Scope* parent() { return parent_; }
    const Scope* parent() const { return parent_; }
    uint32_t depth() const { return depth_; }

    // --- Symbol management ---

    // Insert a symbol into this scope. Returns true if successful,
    // false if a symbol with the same name already exists in THIS scope.
    bool insert(std::unique_ptr<Symbol> symbol) {
        if (!symbol) return false;
        const std::string& name = symbol->name();
        if (symbols_.count(name) > 0) {
            return false; // duplicate in this scope
        }
        Symbol* raw = symbol.get();
        symbols_[name] = std::move(symbol);
        ordered_symbols_.push_back(raw);
        return true;
    }

    // Look up a symbol in this scope only (does NOT search parents).
    Symbol* lookupLocal(const std::string& name) {
        auto it = symbols_.find(name);
        return it != symbols_.end() ? it->second.get() : nullptr;
    }

    const Symbol* lookupLocal(const std::string& name) const {
        auto it = symbols_.find(name);
        return it != symbols_.end() ? it->second.get() : nullptr;
    }

    // Look up a symbol in this scope and all parent scopes.
    // Returns the first match found (nearest scope wins).
    Symbol* lookup(const std::string& name) {
        // Search this scope
        if (auto* sym = lookupLocal(name)) {
            return sym;
        }
        // Search parent scopes
        if (parent_) {
            return parent_->lookup(name);
        }
        return nullptr;
    }

    const Symbol* lookup(const std::string& name) const {
        if (auto* sym = lookupLocal(name)) {
            return sym;
        }
        if (parent_) {
            return parent_->lookup(name);
        }
        return nullptr;
    }

    // Check if a symbol exists in this scope only
    bool hasLocal(const std::string& name) const {
        return symbols_.count(name) > 0;
    }

    // Check if a symbol exists in this scope or any parent scope
    bool has(const std::string& name) const {
        return lookup(name) != nullptr;
    }

    // --- Iteration over symbols in insertion order ---

    const std::vector<Symbol*>& orderedSymbols() const {
        return ordered_symbols_;
    }

    // Total number of symbols in this scope (not including parent scopes)
    size_t symbolCount() const { return symbols_.size(); }

    // --- Child scope management ---

    // Create a new child scope and return a pointer to it.
    // The child scope is owned by this scope.
    Scope* createChild(ScopeKind kind) {
        auto child = std::make_unique<Scope>(kind, this);
        Scope* raw = child.get();
        children_.push_back(std::move(child));
        return raw;
    }

    const std::vector<std::unique_ptr<Scope>>& children() const {
        return children_;
    }

    // --- Scope kind queries ---

    bool isGlobal() const { return kind_ == ScopeKind::Global; }
    bool isFn() const { return kind_ == ScopeKind::Fn; }
    bool isLoop() const { return kind_ == ScopeKind::Loop; }
    bool isBlock() const { return kind_ == ScopeKind::Block; }

    // Check if this scope or any ancestor is a loop scope
    bool isInLoop() const {
        const Scope* s = this;
        while (s) {
            if (s->kind_ == ScopeKind::Loop) return true;
            s = s->parent_;
        }
        return false;
    }

    // Check if this scope or any ancestor is a function scope
    bool isInFn() const {
        const Scope* s = this;
        while (s) {
            if (s->kind_ == ScopeKind::Fn) return true;
            s = s->parent_;
        }
        return false;
    }

    // Find the nearest enclosing function scope, or nullptr if not in a function
    Scope* enclosingFn() {
        Scope* s = this;
        while (s) {
            if (s->kind_ == ScopeKind::Fn) return s;
            s = s->parent_;
        }
        return nullptr;
    }

    const Scope* enclosingFn() const {
        const Scope* s = this;
        while (s) {
            if (s->kind_ == ScopeKind::Fn) return s;
            s = s->parent_;
        }
        return nullptr;
    }

private:
    ScopeKind kind_;
    Scope* parent_;
    uint32_t depth_;
    std::unordered_map<std::string, std::unique_ptr<Symbol>> symbols_;
    std::vector<Symbol*> ordered_symbols_; // for ordered iteration
    std::vector<std::unique_ptr<Scope>> children_;
};

// ============================================================================
// SymbolTable - manages scopes and symbol resolution
// ============================================================================
class SymbolTable {
public:
    SymbolTable() {
        // Start with a global scope
        auto global = std::make_unique<Scope>(Scope::ScopeKind::Global);
        global_scope_ = global.get();
        current_scope_ = global_scope_;
        owned_scopes_.push_back(std::move(global));
    }

    ~SymbolTable() = default;

    // Prevent copying
    SymbolTable(const SymbolTable&) = delete;
    SymbolTable& operator=(const SymbolTable&) = delete;

    // -----------------------------------------------------------------------
    // Scope management
    // -----------------------------------------------------------------------

    // Enter a new scope of the given kind. The new scope becomes current.
    Scope* pushScope(Scope::ScopeKind kind) {
        Scope* child = current_scope_->createChild(kind);
        current_scope_ = child;
        scope_stack_.push_back(child);
        return child;
    }

    // Leave the current scope, returning to the parent scope.
    // Returns the scope that was left, or nullptr if already at global scope.
    Scope* popScope() {
        if (current_scope_ == global_scope_) {
            return nullptr; // can't pop global scope
        }
        Scope* leaving = current_scope_;
        scope_stack_.pop_back();
        if (!scope_stack_.empty()) {
            current_scope_ = scope_stack_.back();
        } else {
            current_scope_ = global_scope_;
        }
        return leaving;
    }

    // Get the current scope
    Scope* currentScope() { return current_scope_; }
    const Scope* currentScope() const { return current_scope_; }

    // Get the global scope
    Scope* globalScope() { return global_scope_; }
    const Scope* globalScope() const { return global_scope_; }

    // Get the current scope depth (0 = global)
    uint32_t currentDepth() const { return current_scope_->depth(); }

    // -----------------------------------------------------------------------
    // Symbol insertion
    // -----------------------------------------------------------------------

    // Insert a symbol into the current scope.
    // Returns true on success, false if the name is already taken in this scope.
    bool insert(std::unique_ptr<Symbol> symbol) {
        return current_scope_->insert(std::move(symbol));
    }

    // Insert a symbol into the global scope regardless of current scope.
    // Used for top-level declarations (functions, structs, enums).
    bool insertGlobal(std::unique_ptr<Symbol> symbol) {
        return global_scope_->insert(std::move(symbol));
    }

    // Convenience: create and insert a var symbol
    Symbol* declareVar(const std::string& name, TypeId type,
                       const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, type, SymbolKind::Var,
                                            true /* mutable */, loc);
        Symbol* raw = sym.get();
        if (!insert(std::move(sym))) {
            return nullptr; // name collision
        }
        return raw;
    }

    // Convenience: create and insert a val symbol
    Symbol* declareVal(const std::string& name, TypeId type,
                       const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, type, SymbolKind::Val,
                                            false /* immutable */, loc);
        Symbol* raw = sym.get();
        if (!insert(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert a function parameter symbol
    Symbol* declareParam(const std::string& name, TypeId type, bool is_mutable = false,
                         const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, type, SymbolKind::Param,
                                            is_mutable, loc);
        Symbol* raw = sym.get();
        if (!insert(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert a function symbol in global scope
    Symbol* declareFn(const std::string& name, TypeId fn_type,
                      const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, fn_type, SymbolKind::Fn,
                                            false /* immutable */, loc);
        Symbol* raw = sym.get();
        if (!insertGlobal(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert a struct type symbol in global scope
    Symbol* declareStruct(const std::string& name, TypeId struct_type,
                          const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, struct_type, SymbolKind::Struct,
                                            false, loc);
        Symbol* raw = sym.get();
        if (!insertGlobal(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert an enum type symbol in global scope
    Symbol* declareEnum(const std::string& name, TypeId enum_type,
                        const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, enum_type, SymbolKind::Enum,
                                            false, loc);
        Symbol* raw = sym.get();
        if (!insertGlobal(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert a struct field symbol
    Symbol* declareField(const std::string& name, TypeId type,
                         const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, type, SymbolKind::Field,
                                            false, loc);
        Symbol* raw = sym.get();
        if (!insert(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // Convenience: create and insert an enum variant symbol
    Symbol* declareVariant(const std::string& name, TypeId enum_type,
                           const SourceLocation& loc = SourceLocation()) {
        auto sym = std::make_unique<Symbol>(name, enum_type, SymbolKind::Variant,
                                            false, loc);
        Symbol* raw = sym.get();
        if (!insert(std::move(sym))) {
            return nullptr;
        }
        return raw;
    }

    // -----------------------------------------------------------------------
    // Symbol lookup
    // -----------------------------------------------------------------------

    // Look up a symbol in the current scope and all parent scopes.
    Symbol* lookup(const std::string& name) {
        return current_scope_->lookup(name);
    }

    const Symbol* lookup(const std::string& name) const {
        return current_scope_->lookup(name);
    }

    // Look up a symbol in the current scope only (no parent scopes).
    Symbol* lookupLocal(const std::string& name) {
        return current_scope_->lookupLocal(name);
    }

    const Symbol* lookupLocal(const std::string& name) const {
        return current_scope_->lookupLocal(name);
    }

    // Look up a symbol in the global scope.
    Symbol* lookupGlobal(const std::string& name) {
        return global_scope_->lookupLocal(name);
    }

    const Symbol* lookupGlobal(const std::string& name) const {
        return global_scope_->lookupLocal(name);
    }

    // Check if a name exists in the current scope
    bool hasName(const std::string& name) const {
        return current_scope_->has(name);
    }

    // Check if a name exists in the current scope only
    bool hasLocalName(const std::string& name) const {
        return current_scope_->hasLocal(name);
    }

    // -----------------------------------------------------------------------
    // Shadowing support
    // -----------------------------------------------------------------------

    // Check if shadowing a name is allowed.
    // Rules:
    //   - val/var can shadow variables in outer scopes (allowed with warning)
    //   - val/var CANNOT shadow names in the SAME scope (error)
    //   - val/var cannot shadow function/struct/enum names in global scope
    //   - Parameters cannot be shadowed in the same function

    enum class ShadowResult : uint8_t {
        Allowed,          // shadowing is fine
        SameScope,        // name already exists in this scope (error)
        ShadowsGlobal,    // shadows a global type/function (warning)
        ShadowsParam,     // shadows a parameter (warning)
        ShadowsOuter,     // shadows an outer scope variable (warning)
    };

    ShadowResult checkShadowing(const std::string& name) const {
        // Check same scope first
        if (current_scope_->hasLocal(name)) {
            return ShadowResult::SameScope;
        }

        // Check if shadowing a global type/function
        if (global_scope_->hasLocal(name)) {
            const Symbol* global_sym = global_scope_->lookupLocal(name);
            if (global_sym && (global_sym->isTypeSymbol() || global_sym->isFn())) {
                return ShadowResult::ShadowsGlobal;
            }
        }

        // Check if shadowing a parameter
        if (current_scope_->parent()) {
            const Symbol* outer = current_scope_->parent()->lookupLocal(name);
            if (outer && outer->isParam()) {
                return ShadowResult::ShadowsParam;
            }
        }

        // Check if shadowing any outer variable
        if (current_scope_->parent() && current_scope_->parent()->lookup(name)) {
            return ShadowResult::ShadowsOuter;
        }

        return ShadowResult::Allowed;
    }

    // -----------------------------------------------------------------------
    // Scope queries
    // -----------------------------------------------------------------------

    // Check if we're in a function scope
    bool isInFn() const { return current_scope_->isInFn(); }

    // Check if we're in a loop scope
    bool isInLoop() const { return current_scope_->isInLoop(); }

    // Check if we're at global scope
    bool isGlobalScope() const { return current_scope_ == global_scope_; }

    // Get the enclosing function scope
    Scope* enclosingFn() { return current_scope_->enclosingFn(); }
    const Scope* enclosingFn() const { return current_scope_->enclosingFn(); }

    // -----------------------------------------------------------------------
    // RAII scope guard for automatic push/pop
    // -----------------------------------------------------------------------
    class ScopeGuard {
    public:
        explicit ScopeGuard(SymbolTable& symtab, Scope::ScopeKind kind)
            : symtab_(symtab), active_(true)
        {
            symtab_.pushScope(kind);
        }

        ~ScopeGuard() {
            if (active_) {
                symtab_.popScope();
            }
        }

        // Prevent copying
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;

        // Allow moving
        ScopeGuard(ScopeGuard&& other) noexcept
            : symtab_(other.symtab_), active_(other.active_)
        {
            other.active_ = false;
        }

        void dismiss() { active_ = false; }

    private:
        SymbolTable& symtab_;
        bool active_;
    };

    // Convenience: create a scope guard that auto-pops on destruction
    ScopeGuard scopedScope(Scope::ScopeKind kind) {
        return ScopeGuard(*this, kind);
    }

    // -----------------------------------------------------------------------
    // Diagnostics / debugging
    // -----------------------------------------------------------------------

    // Count total symbols across all scopes
    size_t totalSymbolCount() const {
        return countSymbols(global_scope_);
    }

    // Get all global symbols
    const std::vector<Symbol*>& globalSymbols() const {
        return global_scope_->orderedSymbols();
    }

private:
    // Recursively count symbols in a scope and all children
    size_t countSymbols(const Scope* scope) const {
        size_t count = scope->symbolCount();
        for (const auto& child : scope->children()) {
            count += countSymbols(child.get());
        }
        return count;
    }

    Scope* global_scope_;
    Scope* current_scope_;
    std::vector<Scope*> scope_stack_;
    std::vector<std::unique_ptr<Scope>> owned_scopes_;
};

// ============================================================================
// Inline implementations
// ============================================================================

inline std::string symbolKindToString(SymbolKind kind) {
    switch (kind) {
        case SymbolKind::Var:      return "var";
        case SymbolKind::Val:      return "val";
        case SymbolKind::Param:    return "param";
        case SymbolKind::Fn:       return "fn";
        case SymbolKind::Struct:   return "struct";
        case SymbolKind::Enum:     return "enum";
        case SymbolKind::Field:    return "field";
        case SymbolKind::Variant:  return "variant";
        case SymbolKind::Import:   return "import";
        case SymbolKind::TypeParam: return "type_param";
        default:                   return "unknown";
    }
}

} // namespace jules
