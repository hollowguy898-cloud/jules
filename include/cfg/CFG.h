#pragma once

#include "ast/AST.h"
#include "sema/Type.h"

#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <optional>
#include <cstdint>
#include <cassert>

namespace jules {

// ============================================================================
// BorrowKind - classification of a borrow tracked in the CFG
// ============================================================================
enum class BorrowKind : uint8_t {
    Shared,     // &T  — shared (read-only) borrow
    MutExclusive // &mut T — exclusive (read-write) borrow
};

std::string borrowKindToString(BorrowKind kind);

// ============================================================================
// BorrowInfo - tracks a borrow within a CFG node
// ============================================================================
struct BorrowInfo {
    std::string borrowed_var;  // name of the variable being borrowed
    BorrowKind kind;           // shared or mutable borrow
    SourceLocation origin;     // where the borrow was created
    std::string borrow_var;    // name of the variable holding the borrow (if any)
    TypeId borrowed_type;      // type of the borrow (&T or &mut T)

    BorrowInfo(std::string var, BorrowKind k, SourceLocation loc,
               std::string bvar, TypeId btype)
        : borrowed_var(std::move(var))
        , kind(k)
        , origin(std::move(loc))
        , borrow_var(std::move(bvar))
        , borrowed_type(btype)
    {}
};

// ============================================================================
// CFGNode - a basic block in the control flow graph
// ============================================================================
class CFGNode {
public:
    using NodeId = uint32_t;
    static constexpr NodeId INVALID_ID = static_cast<NodeId>(-1);

    CFGNode(NodeId id, std::string label = "")
        : id_(id), label_(std::move(label)) {}

    // --- Identity ---

    NodeId id() const { return id_; }
    const std::string& label() const { return label_; }
    void setLabel(std::string label) { label_ = std::move(label); }

    // --- Statements ---

    const std::vector<Stmt*>& stmts() const { return stmts_; }
    std::vector<Stmt*>& stmts() { return stmts_; }

    void addStmt(Stmt* stmt) { stmts_.push_back(stmt); }

    // --- Successor / Predecessor edges ---

    const std::vector<CFGNode*>& successors() const { return successors_; }
    std::vector<CFGNode*>& successors() { return successors_; }

    const std::vector<CFGNode*>& predecessors() const { return predecessors_; }
    std::vector<CFGNode*>& predecessors() { return predecessors_; }

    void addSuccessor(CFGNode* succ) {
        successors_.push_back(succ);
        succ->predecessors_.push_back(this);
    }

    // --- Variable tracking ---

    // Variables defined (assigned) in this node
    const std::unordered_set<std::string>& definedVars() const { return defined_vars_; }
    void defineVar(const std::string& name) { defined_vars_.insert(name); }

    // Variables used (read) in this node
    const std::unordered_set<std::string>& usedVars() const { return used_vars_; }
    void useVar(const std::string& name) { used_vars_.insert(name); }

    // --- Borrow tracking ---

    // Borrows that originate (are created) in this node
    const std::vector<BorrowInfo>& borrowsCreated() const { return borrows_created_; }
    void addBorrowCreated(BorrowInfo info) { borrows_created_.push_back(std::move(info)); }

    // Borrows that are used (read through) in this node
    const std::vector<BorrowInfo>& borrowsUsed() const { return borrows_used_; }
    void addBorrowUsed(const BorrowInfo& info) { borrows_used_.push_back(info); }

    // Borrows that are killed (last use) in this node
    const std::vector<BorrowInfo>& borrowsKilled() const { return borrows_killed_; }
    void addBorrowKilled(const BorrowInfo& info) { borrows_killed_.push_back(info); }

    // --- Terminator ---

    // Is this a terminal node (has no successors)?
    bool isTerminal() const { return successors_.empty(); }

    // --- Debug ---

    std::string toString() const {
        return "BB" + std::to_string(id_) + (label_.empty() ? "" : ":" + label_);
    }

private:
    NodeId id_;
    std::string label_;
    std::vector<Stmt*> stmts_;
    std::vector<CFGNode*> successors_;
    std::vector<CFGNode*> predecessors_;
    std::unordered_set<std::string> defined_vars_;
    std::unordered_set<std::string> used_vars_;
    std::vector<BorrowInfo> borrows_created_;
    std::vector<BorrowInfo> borrows_used_;
    std::vector<BorrowInfo> borrows_killed_;
};

// ============================================================================
// CFG - Control Flow Graph for a single function
// ============================================================================
class CFG {
public:
    explicit CFG(std::string fn_name)
        : fn_name_(std::move(fn_name))
        , entry_(nullptr)
        , exit_(nullptr)
        , next_id_(0)
    {}

    // --- Factory methods for creating nodes ---

    CFGNode* createNode(std::string label = "") {
        auto node = std::make_unique<CFGNode>(next_id_++, std::move(label));
        CFGNode* raw = node.get();
        nodes_.push_back(std::move(node));
        return raw;
    }

    // --- Accessors ---

    const std::string& functionName() const { return fn_name_; }

    CFGNode* entry() { return entry_; }
    const CFGNode* entry() const { return entry_; }
    void setEntry(CFGNode* node) { entry_ = node; }

    CFGNode* exit() { return exit_; }
    const CFGNode* exit() const { return exit_; }
    void setExit(CFGNode* node) { exit_ = node; }

    const std::vector<std::unique_ptr<CFGNode>>& nodes() const { return nodes_; }
    std::vector<std::unique_ptr<CFGNode>>& nodes() { return nodes_; }

    // Node count
    size_t nodeCount() const { return nodes_.size(); }

    // Lookup a node by its ID
    CFGNode* nodeById(CFGNode::NodeId id) {
        for (auto& n : nodes_) {
            if (n->id() == id) return n.get();
        }
        return nullptr;
    }

    const CFGNode* nodeById(CFGNode::NodeId id) const {
        for (auto& n : nodes_) {
            if (n->id() == id) return n.get();
        }
        return nullptr;
    }

    // --- Topological ordering (reverse postorder) for dataflow ---

    std::vector<CFGNode*> reversePostorder() {
        std::vector<CFGNode*> order;
        std::unordered_set<CFGNode::NodeId> visited;

        if (entry_) {
            dfsPostorder(entry_, visited, order);
        }

        // Reverse to get reverse postorder
        std::reverse(order.begin(), order.end());
        return order;
    }

    // --- Debug ---

    std::string toDot() const {
        std::string result = "digraph \"" + fn_name_ + "\" {\n";
        for (const auto& node : nodes_) {
            std::string label = node->toString();
            // Add statement summary
            label += "\\n";
            for (auto* stmt : node->stmts()) {
                label += "[" + std::to_string(static_cast<int>(stmt->getKind())) + "] ";
            }
            result += "  \"" + std::to_string(node->id()) + "\" [label=\"" + label + "\"];\n";
        }
        for (const auto& node : nodes_) {
            for (auto* succ : node->successors()) {
                result += "  \"" + std::to_string(node->id()) + "\" -> \"" +
                          std::to_string(succ->id()) + "\";\n";
            }
        }
        result += "}\n";
        return result;
    }

private:
    void dfsPostorder(CFGNode* node,
                      std::unordered_set<CFGNode::NodeId>& visited,
                      std::vector<CFGNode*>& order) {
        if (!node || visited.count(node->id())) return;
        visited.insert(node->id());

        for (auto* succ : node->successors()) {
            dfsPostorder(succ, visited, order);
        }

        order.push_back(node);
    }

    std::string fn_name_;
    CFGNode* entry_;
    CFGNode* exit_;
    CFGNode::NodeId next_id_;
    std::vector<std::unique_ptr<CFGNode>> nodes_;
};

// ============================================================================
// CFGBuilder - constructs a CFG from an FnDecl's AST
// ============================================================================
class CFGBuilder {
public:
    CFGBuilder() = default;

    // Build a CFG from a function declaration.
    // The function must have already been semantically analyzed
    // (all types resolved, all Expr nodes have TypeId set).
    std::unique_ptr<CFG> build(FnDecl& fn);

private:
    // --- Internal state ---

    // The CFG being constructed
    CFG* cfg_ = nullptr;

    // Break targets (stack of loop header nodes for break)
    std::vector<CFGNode*> break_targets_;

    // Continue targets (stack of loop condition nodes for continue)
    std::vector<CFGNode*> continue_targets_;

    // Defer statements collected during traversal (to be emitted at exit)
    std::vector<Stmt*> deferred_stmts_;

    // --- Node creation ---

    CFGNode* newBlock(std::string label = "") {
        return cfg_->createNode(std::move(label));
    }

    // --- Block building ---

    // Build a block statement, appending statements to the current node.
    // Returns the exit node of the block.
    // `current` is the node to append to; may be split into multiple nodes.
    CFGNode* buildBlock(BlockStmt& block, CFGNode* current);

    // Build a single statement, appending to the current node.
    // Returns the exit node (may be different from `current` if the statement
    // splits the CFG into multiple nodes).
    CFGNode* buildStmt(Stmt& stmt, CFGNode* current);

    // Build specific statement types
    CFGNode* buildVarDeclStmt(VarDeclStmt& vd, CFGNode* current);
    CFGNode* buildValDeclStmt(ValDeclStmt& vd, CFGNode* current);
    CFGNode* buildAssignStmt(AssignStmt& as, CFGNode* current);
    CFGNode* buildDeferStmt(DeferStmt& ds, CFGNode* current);
    CFGNode* buildIfStmt(IfStmt& is, CFGNode* current);
    CFGNode* buildWhileStmt(WhileStmt& ws, CFGNode* current);
    CFGNode* buildReturnStmt(ReturnStmt& rs, CFGNode* current);
    CFGNode* buildBreakStmt(BreakStmt& bs, CFGNode* current);
    CFGNode* buildContinueStmt(ContinueStmt& cs, CFGNode* current);
    CFGNode* buildExprStmt(ExprStmt& es, CFGNode* current);

    // --- Expression analysis for variable/borrow tracking ---

    // Extract which variables are read (used) by an expression
    void collectUsedVars(Expr& expr, std::unordered_set<std::string>& used);

    // Extract which variables are defined (written) by an expression
    void collectDefinedVars(Expr& expr, std::unordered_set<std::string>& defined);

    // Extract borrow information from an expression
    void collectBorrows(Expr& expr, std::vector<BorrowInfo>& borrows);

    // Track variable uses/defs and borrows for a statement in a node
    void trackStatement(Stmt& stmt, CFGNode* node);
};

// ============================================================================
// Inline implementations
// ============================================================================

inline std::string borrowKindToString(BorrowKind kind) {
    switch (kind) {
        case BorrowKind::Shared:      return "&";
        case BorrowKind::MutExclusive: return "&mut";
        default:                       return "unknown_borrow";
    }
}

} // namespace jules
