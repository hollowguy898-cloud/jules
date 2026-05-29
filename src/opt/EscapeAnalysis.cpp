#include "opt/EscapeAnalysis.h"

#include <cassert>
#include <sstream>

namespace tether {

// ============================================================================
// EscapeAnalysisPass::run
//
// Iterates over every function in the program. For each function, finds
// local variables whose type is Box<T>, Rc<T>, or Arc<T>, then checks
// whether the smart pointer value escapes the function. If it does not
// escape, the allocation CallExpr is annotated with StackAllocated so
// the IR generator can emit alloca+memcpy instead of malloc/free.
// ============================================================================
bool EscapeAnalysisPass::run(Program& program, TypeTable& type_table) {
    boxes_stack_allocated_ = 0;
    rcs_stack_allocated_ = 0;
    arcs_stack_allocated_ = 0;

    bool any_changed = false;

    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (analyzeFn(fn, type_table)) {
            any_changed = true;
        }
    }

    return any_changed;
}

// ============================================================================
// analyzeFn - analyze a single function for escape opportunities
//
// Algorithm:
//   1. Collect all local variable names (from VarDeclStmt/ValDeclStmt)
//   2. For each local variable whose type is SmartPointerType:
//      a. Walk all subsequent statements to check if the variable escapes
//      b. If it doesn't escape, annotate the allocation CallExpr
// ============================================================================
bool EscapeAnalysisPass::analyzeFn(FnDecl& fn, TypeTable& /*type_table*/) {
    if (!fn.body()) return false;

    // Step 1: Collect all local variable names
    std::unordered_set<std::string> local_vars;

    // Function parameters are also local
    for (const auto& param : fn.params()) {
        local_vars.insert(param.name);
    }

    collectLocalVars(fn.body(), local_vars);

    // Step 2: Walk the function body looking for smart pointer declarations
    // that can be stack-allocated
    bool any_annotated = false;

    // We iterate over statements and for each VarDeclStmt/ValDeclStmt that
    // declares a smart pointer, we check if the variable escapes.
    auto& stmts = fn.body()->stmts();
    for (size_t i = 0; i < stmts.size(); ++i) {
        Stmt* stmt = stmts[i].get();
        if (!stmt) continue;

        // Only VarDeclStmt and ValDeclStmt can introduce new smart pointer locals
        std::string var_name;
        Expr* init_expr = nullptr;
        TypeId var_type;

        if (stmt->getKind() == NodeKind::VarDeclStmt) {
            auto& var = cast<VarDeclStmt>(*stmt);
            var_name = var.name();
            init_expr = var.hasInit() ? var.init() : nullptr;
            var_type = var.declaredType();
        } else if (stmt->getKind() == NodeKind::ValDeclStmt) {
            auto& val = cast<ValDeclStmt>(*stmt);
            var_name = val.name();
            init_expr = val.hasInit() ? val.init() : nullptr;
            var_type = val.declaredType();
        } else {
            continue;
        }

        // Check if the variable type is a smart pointer
        // We check both the declared type and the init expression type
        SmartPointerKind sp_kind;
        bool is_smart_pointer = false;

        if (var_type && isa<SmartPointerType>(var_type)) {
            auto& sp_type = cast<SmartPointerType>(var_type);
            sp_kind = sp_type.smartPointerKind();
            is_smart_pointer = true;
        } else if (init_expr && init_expr->hasType() &&
                   isa<SmartPointerType>(init_expr->getType())) {
            auto& sp_type = cast<SmartPointerType>(init_expr->getType());
            sp_kind = sp_type.smartPointerKind();
            is_smart_pointer = true;
        }

        if (!is_smart_pointer) continue;

        // Step 2a: Check if the variable escapes the function
        if (walkBlockForEscape(fn.body(), var_name)) {
            // The variable escapes — cannot stack-allocate
            continue;
        }

        // Step 2b: The variable does NOT escape — annotate the allocation
        //
        // We need to find the CallExpr that creates the smart pointer.
        // It could be:
        //   - Direct: val x = Box.new(value)     → init_expr is the CallExpr
        //   - Nested: val x = Box.new(foo())     → init_expr wraps the CallExpr
        //
        // We look for a CallExpr in the initializer whose callee names
        // Box.new / Rc.new / Arc.new.

        CallExpr* alloc_call = findAllocCall(init_expr, sp_kind);

        if (!alloc_call) {
            // If we can't find the specific allocation call, we annotate
            // the init expression itself if it's a CallExpr
            if (init_expr && init_expr->getKind() == NodeKind::CallExpr) {
                alloc_call = &cast<CallExpr>(*init_expr);
            } else {
                // Can't determine the allocation call — conservatively skip
                continue;
            }
        }

        if (meta_map_) {
            meta_map_->getOrCreate(alloc_call).stack_allocated = true;
        }

        // Update stats
        switch (sp_kind) {
            case SmartPointerKind::Box: boxes_stack_allocated_++; break;
            case SmartPointerKind::Rc:  rcs_stack_allocated_++;  break;
            case SmartPointerKind::Arc: arcs_stack_allocated_++; break;
        }

        any_annotated = true;
    }

    return any_annotated;
}

// ============================================================================
// findAllocCall - find the Box.new/Rc.new/Arc.new CallExpr within an
// initialization expression
// ============================================================================
CallExpr* EscapeAnalysisPass::findAllocCall(Expr* expr, SmartPointerKind kind) {
    if (!expr) return nullptr;

    // If it's directly a CallExpr, check the callee
    if (expr->getKind() == NodeKind::CallExpr) {
        auto& call = cast<CallExpr>(*expr);
        if (isSmartPointerNew(call, kind)) {
            return &call;
        }
        // Check arguments recursively (the new() call might be nested)
        for (auto& arg : call.args()) {
            CallExpr* found = findAllocCall(arg.get(), kind);
            if (found) return found;
        }
        return nullptr;
    }

    // For other expression types, try to find a CallExpr inside
    if (expr->getKind() == NodeKind::MemberExpr) {
        auto& member = cast<MemberExpr>(*expr);
        return findAllocCall(member.object(), kind);
    }

    return nullptr;
}

// ============================================================================
// isSmartPointerNew - check if a CallExpr is Box.new(), Rc.new(), or Arc.new()
// ============================================================================
bool EscapeAnalysisPass::isSmartPointerNew(CallExpr& call, SmartPointerKind kind) {
    Expr* callee = call.callee();
    if (!callee) return false;

    // Expected pattern: Box.new(...), Rc.new(...), Arc.new(...)
    // The callee is a MemberExpr with object "Box"/"Rc"/"Arc" and field "new"
    if (callee->getKind() != NodeKind::MemberExpr) return false;

    auto& member = cast<MemberExpr>(*callee);
    if (member.field() != "new") return false;

    if (member.object()->getKind() != NodeKind::IdentExpr) return false;

    auto& ident = cast<IdentExpr>(*member.object());
    switch (kind) {
        case SmartPointerKind::Box: return ident.name() == "Box";
        case SmartPointerKind::Rc:  return ident.name() == "Rc";
        case SmartPointerKind::Arc: return ident.name() == "Arc";
    }
    return false;
}

// ============================================================================
// collectLocalVars - walk the function body to collect all local variable names
// ============================================================================
void EscapeAnalysisPass::collectLocalVars(BlockStmt* block,
                                          std::unordered_set<std::string>& locals) {
    if (!block) return;

    for (auto& stmt : block->stmts()) {
        if (!stmt) continue;

        switch (stmt->getKind()) {
            case NodeKind::VarDeclStmt: {
                auto& var = cast<VarDeclStmt>(*stmt);
                locals.insert(var.name());
                break;
            }
            case NodeKind::ValDeclStmt: {
                auto& val = cast<ValDeclStmt>(*stmt);
                locals.insert(val.name());
                break;
            }
            case NodeKind::BlockStmt: {
                collectLocalVars(&cast<BlockStmt>(*stmt), locals);
                break;
            }
            case NodeKind::IfStmt: {
                auto& if_stmt = cast<IfStmt>(*stmt);
                if (if_stmt.thenBlock()) {
                    collectLocalVars(if_stmt.thenBlock(), locals);
                }
                if (if_stmt.elseBlock()) {
                    collectLocalVars(if_stmt.elseBlock(), locals);
                }
                break;
            }
            case NodeKind::WhileStmt: {
                auto& while_stmt = cast<WhileStmt>(*stmt);
                if (while_stmt.body()) {
                    collectLocalVars(while_stmt.body(), locals);
                }
                break;
            }
            default:
                break;
        }
    }
}

// ============================================================================
// walkBlockForEscape - walk a block of statements to check if a variable
// escapes anywhere within it
// ============================================================================
bool EscapeAnalysisPass::walkBlockForEscape(BlockStmt* block,
                                            const std::string& var_name) {
    if (!block) return false;

    for (auto& stmt : block->stmts()) {
        if (walkStmtForEscape(stmt.get(), var_name)) {
            return true;
        }
    }
    return false;
}

// ============================================================================
// walkStmtForEscape - check if a statement causes the variable to escape
// ============================================================================
bool EscapeAnalysisPass::walkStmtForEscape(Stmt* stmt,
                                           const std::string& var_name) {
    if (!stmt) return false;

    switch (stmt->getKind()) {
        case NodeKind::ReturnStmt: {
            // If the returned expression references our variable, it escapes
            auto& ret = cast<ReturnStmt>(*stmt);
            if (ret.hasValue() && exprReferencesVar(ret.value(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::AssignStmt: {
            auto& assign = cast<AssignStmt>(*stmt);

            // If the target is a MemberExpr and the value references our
            // variable, it escapes (e.g., global.field = my_box)
            if (assign.target()->getKind() == NodeKind::MemberExpr) {
                if (exprReferencesVar(assign.value(), var_name)) {
                    return true;
                }
            }

            // If the target is an IndexExpr and the value references our
            // variable, it escapes (e.g., array[i] = my_box)
            if (assign.target()->getKind() == NodeKind::IndexExpr) {
                if (exprReferencesVar(assign.value(), var_name)) {
                    return true;
                }
            }

            // If the target is a DerefExpr and the value references our
            // variable, it escapes (e.g., *ptr = my_box)
            if (assign.target()->getKind() == NodeKind::DerefExpr) {
                if (exprReferencesVar(assign.value(), var_name)) {
                    return true;
                }
            }

            // Also check if the target itself references our variable in a
            // way that causes escape (e.g., my_box.field = ... is fine,
            // but if someone assigns TO our variable from non-local, that's
            // a reassignment which we track conservatively)
            break;
        }

        case NodeKind::ExprStmt: {
            auto& expr_stmt = cast<ExprStmt>(*stmt);
            // An expression statement that contains a call passing our
            // variable as an argument causes escape (unless the callee is pure)
            if (checkEscape(expr_stmt.expr(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::VarDeclStmt: {
            auto& var = cast<VarDeclStmt>(*stmt);
            // If the initializer of a new variable references our smart pointer,
            // that's an escape (aliased into a new binding)
            if (var.hasInit() && exprReferencesVar(var.init(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::ValDeclStmt: {
            auto& val = cast<ValDeclStmt>(*stmt);
            // Same as VarDeclStmt — aliasing into a new binding escapes
            if (val.hasInit() && exprReferencesVar(val.init(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::BlockStmt: {
            return walkBlockForEscape(&cast<BlockStmt>(*stmt), var_name);
        }

        case NodeKind::IfStmt: {
            auto& if_stmt = cast<IfStmt>(*stmt);
            // Check condition
            if (exprReferencesVar(if_stmt.condition(), var_name)) {
                // Using in a condition doesn't escape by itself, but check
                // for call expressions in the condition
                if (checkEscape(if_stmt.condition(), var_name)) {
                    return true;
                }
            }
            if (if_stmt.thenBlock() &&
                walkBlockForEscape(if_stmt.thenBlock(), var_name)) {
                return true;
            }
            if (if_stmt.elseBlock() &&
                walkBlockForEscape(if_stmt.elseBlock(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::WhileStmt: {
            auto& while_stmt = cast<WhileStmt>(*stmt);
            // Check condition for call-based escape
            if (checkEscape(while_stmt.condition(), var_name)) {
                return true;
            }
            if (while_stmt.body() &&
                walkBlockForEscape(while_stmt.body(), var_name)) {
                return true;
            }
            if (while_stmt.hasIncrement() &&
                checkEscape(while_stmt.increment(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::DeferStmt: {
            auto& defer = cast<DeferStmt>(*stmt);
            if (walkStmtForEscape(defer.stmt(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::ErrdeferStmt: {
            auto& errdefer = cast<ErrdeferStmt>(*stmt);
            if (walkStmtForEscape(errdefer.stmt(), var_name)) {
                return true;
            }
            break;
        }

        case NodeKind::AtomicStmt: {
            auto& atomic = cast<AtomicStmt>(*stmt);
            if (walkStmtForEscape(atomic.inner(), var_name)) {
                return true;
            }
            break;
        }

        default:
            break;
    }

    return false;
}

// ============================================================================
// checkEscape - check if an expression causes the variable to escape
//
// This specifically checks for CallExpr nodes where the variable is
// passed as an argument. If the callee is not a pure function, passing
// the smart pointer as an argument means it may escape.
// ============================================================================
bool EscapeAnalysisPass::checkEscape(Expr* expr,
                                     const std::unordered_set<std::string>& /*local_vars*/) {
    if (!expr) return false;
    return checkEscape(expr, std::string());
}

bool EscapeAnalysisPass::checkEscape(Expr* expr, const std::string& var_name) {
    if (!expr) return false;

    switch (expr->getKind()) {
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);

            // Check if the callee expression itself references our variable
            // (e.g., var.method() — this passes &var or var by value)
            // We don't consider method calls on the smart pointer itself
            // as escaping, since the smart pointer is the receiver.
            // But if the variable is passed as a regular argument, it escapes.

            for (auto& arg : call.args()) {
                if (exprReferencesVar(arg.get(), var_name)) {
                    // The variable is passed as an argument.
                    // Check if the callee is a pure function — if so, it
                    // cannot store the value, so no escape.
                    //
                    // We conservatively assume non-pure for any call where
                    // we can't determine the callee. If the callee is an
                    // IdentExpr or MemberExpr, we check if it's a known pure
                    // function.
                    //
                    // For now: if we can identify the callee and it's NOT
                    // a known pure function, it escapes.
                    //
                    // We check if the callee is a simple function name or
                    // method call. Method calls on the smart pointer itself
                    // (like box.deref()) are safe because they return inner
                    // values, not store the pointer.

                    // If the argument IS our variable directly (not a
                    // member access on it), it definitely escapes.
                    if (arg->getKind() == NodeKind::IdentExpr) {
                        auto& ident = cast<IdentExpr>(*arg);
                        if (ident.name() == var_name) {
                            // Direct pass as argument — escapes unless callee is pure
                            // For now, conservatively assume escape
                            // TODO: Check if callee is a known pure function
                            return true;
                        }
                    }

                    // If the variable is used inside the argument expression
                    // (e.g., foo(&my_box) or foo(my_box.field)), it may escape
                    // We're conservative: any use in an argument escapes.
                    return true;
                }
            }

            // Recursively check the callee expression
            if (checkEscape(call.callee(), var_name)) {
                return true;
            }

            break;
        }

        case NodeKind::BinaryExpr: {
            auto& binary = cast<BinaryExpr>(*expr);
            if (checkEscape(binary.left(), var_name)) return true;
            if (checkEscape(binary.right(), var_name)) return true;
            break;
        }

        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            // Taking the address of our variable (&my_box) causes escape
            if (unary.op() == UnaryOp::Addr &&
                exprReferencesVar(unary.operand(), var_name)) {
                return true;
            }
            if (checkEscape(unary.operand(), var_name)) return true;
            break;
        }

        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            // Accessing a field on the smart pointer (my_box.field) is OK
            // by itself — the escape happens when the result is used in
            // a return/assign/call. But we need to recurse in case the
            // object is not our variable but something containing it.
            if (checkEscape(member.object(), var_name)) return true;
            break;
        }

        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            if (checkEscape(index.object(), var_name)) return true;
            if (checkEscape(index.index(), var_name)) return true;
            break;
        }

        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            if (checkEscape(deref.operand(), var_name)) return true;
            break;
        }

        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            // Taking the address of something containing our variable escapes
            if (exprReferencesVar(addr.operand(), var_name)) {
                return true;
            }
            if (checkEscape(addr.operand(), var_name)) return true;
            break;
        }

        case NodeKind::CastExpr: {
            auto& cast_expr = cast<CastExpr>(*expr);
            if (checkEscape(cast_expr.expr(), var_name)) return true;
            break;
        }

        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            // Storing our variable into a struct initializer escapes it
            for (auto& field : init.inits()) {
                if (exprReferencesVar(field.value.get(), var_name)) {
                    return true;
                }
            }
            break;
        }

        case NodeKind::ArrayInitExpr: {
            auto& init = cast<ArrayInitExpr>(*expr);
            // Storing our variable into an array initializer escapes it
            for (auto& elem : init.elements()) {
                if (exprReferencesVar(elem.get(), var_name)) {
                    return true;
                }
            }
            break;
        }

        case NodeKind::TryExpr: {
            auto& try_expr = cast<TryExpr>(*expr);
            if (checkEscape(try_expr.operand(), var_name)) return true;
            break;
        }

        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            if (checkEscape(unsafe.inner(), var_name)) return true;
            break;
        }

        default:
            break;
    }

    return false;
}

// ============================================================================
// exprReferencesVar - check if an expression directly references a variable
//
// This is a syntactic check: does the expression tree contain an IdentExpr
// with the given variable name? This is used to detect when a smart pointer
// variable appears in an escape-relevant context.
// ============================================================================
bool EscapeAnalysisPass::exprReferencesVar(Expr* expr,
                                           const std::string& var_name) {
    if (!expr) return false;

    switch (expr->getKind()) {
        case NodeKind::IdentExpr: {
            auto& ident = cast<IdentExpr>(*expr);
            return ident.name() == var_name;
        }

        case NodeKind::BinaryExpr: {
            auto& binary = cast<BinaryExpr>(*expr);
            return exprReferencesVar(binary.left(), var_name) ||
                   exprReferencesVar(binary.right(), var_name);
        }

        case NodeKind::UnaryExpr: {
            auto& unary = cast<UnaryExpr>(*expr);
            return exprReferencesVar(unary.operand(), var_name);
        }

        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            if (exprReferencesVar(call.callee(), var_name)) return true;
            for (auto& arg : call.args()) {
                if (exprReferencesVar(arg.get(), var_name)) return true;
            }
            return false;
        }

        case NodeKind::MemberExpr: {
            auto& member = cast<MemberExpr>(*expr);
            return exprReferencesVar(member.object(), var_name);
        }

        case NodeKind::IndexExpr: {
            auto& index = cast<IndexExpr>(*expr);
            return exprReferencesVar(index.object(), var_name) ||
                   exprReferencesVar(index.index(), var_name);
        }

        case NodeKind::DerefExpr: {
            auto& deref = cast<DerefExpr>(*expr);
            return exprReferencesVar(deref.operand(), var_name);
        }

        case NodeKind::AddrOfExpr: {
            auto& addr = cast<AddrOfExpr>(*expr);
            return exprReferencesVar(addr.operand(), var_name);
        }

        case NodeKind::CastExpr: {
            auto& cast_expr = cast<CastExpr>(*expr);
            return exprReferencesVar(cast_expr.expr(), var_name);
        }

        case NodeKind::StructInitExpr: {
            auto& init = cast<StructInitExpr>(*expr);
            for (auto& field : init.inits()) {
                if (exprReferencesVar(field.value.get(), var_name)) return true;
            }
            return false;
        }

        case NodeKind::ArrayInitExpr: {
            auto& init = cast<ArrayInitExpr>(*expr);
            for (auto& elem : init.elements()) {
                if (exprReferencesVar(elem.get(), var_name)) return true;
            }
            return false;
        }

        case NodeKind::TryExpr: {
            auto& try_expr = cast<TryExpr>(*expr);
            return exprReferencesVar(try_expr.operand(), var_name);
        }

        case NodeKind::UnsafeExpr: {
            auto& unsafe = cast<UnsafeExpr>(*expr);
            return exprReferencesVar(unsafe.inner(), var_name);
        }

        default:
            return false;
    }
}

} // namespace tether
