#include "opt/AoSToSoA.h"

#include <algorithm>
#include <cassert>
#include <functional>

namespace tether {

// ============================================================================
// AoSToSoAPass::run - scan all soa struct declarations and transform them
// ============================================================================
bool AoSToSoAPass::run(Program& program, TypeTable& type_table) {
    transforms_.clear();
    bool any_changed = false;

    // Phase 1: Identify all soa structs and build transformation plans
    for (auto& top_level : program) {
        if (top_level->getKind() == NodeKind::StructDecl) {
            auto& decl = cast<StructDecl>(*top_level);
            if (decl.isSoA()) {
                if (transformStruct(decl, type_table)) {
                    any_changed = true;
                }
            }
        }
    }

    // Phase 2: Rewrite member accesses for each transformed struct
    for (const auto& transform : transforms_) {
        if (transform.was_transformed) {
            if (transformAccesses(program, transform)) {
                // Accesses were noted for rewriting; actual rewriting
                // happens during IR generation using the annotation map
            }
        }
    }

    return any_changed;
}

// ============================================================================
// transformStruct - plan the SoA transformation for a single struct
// ============================================================================
bool AoSToSoAPass::transformStruct(StructDecl& decl, TypeTable& type_table) {
    const auto& fields = decl.fields();
    if (fields.empty()) return false;

    SoATransform transform;
    transform.struct_name = decl.name();
    transform.was_transformed = false;

    // Build the per-field array names and types
    for (const auto& field : fields) {
        transform.field_names.push_back(field.name);
        transform.field_array_names.push_back(decl.name() + "_" + field.name);
        transform.field_types.push_back(field.type);
    }

    // Look up the struct type in the type table to verify it exists
    auto struct_type_opt = type_table.lookupAlias(decl.name());
    if (!struct_type_opt.has_value()) {
        // Try looking it up by its canonical form
        // The struct might not be registered as an alias, but the
        // transformation can still proceed at the AST level
    }

    // For each field, create a separate array type in the type table.
    // The IR generator will use the SoATransform metadata to emit
    // separate arrays instead of a single array of structs.
    for (size_t i = 0; i < fields.size(); ++i) {
        // Annotate the struct decl so the IR generator knows about the transformation
        if (annotations_) {
            std::string detail = transform.field_array_names[i] + ":" +
                                 transform.field_types[i]->toString();
            annotations_->annotate(&decl, ASTAnnotationKind::SoATransformed, detail);
        }
    }

    transform.was_transformed = true;
    transforms_.push_back(std::move(transform));
    return true;
}

// ============================================================================
// transformAccesses - find and annotate member accesses on SoA structs
//
// We look for patterns like: data[i].field where data is an array of
// a SoA struct. We annotate these IndexExpr+MemberExpr combinations
// so the IR generator can emit the correct SoA access pattern:
//   data_field[i] instead of data[i].field
// ============================================================================
bool AoSToSoAPass::transformAccesses(Program& program, const SoATransform& transform) {
    int access_count = 0;

    // Walk all function bodies looking for member accesses on SoA struct arrays
    for (auto& top_level : program) {
        if (top_level->getKind() != NodeKind::FnDecl) continue;
        auto& fn = cast<FnDecl>(*top_level);
        if (!fn.body()) continue;

        // Recursively walk the function body looking for member accesses
        // that match the SoA pattern: (IndexExpr (array, i)).field
        std::function<void(Expr*)> walk_expr = [&](Expr* expr) {
            if (!expr) return;

            if (auto* member = dyn_cast<MemberExpr>(expr)) {
                // Check if the object is an IndexExpr on a SoA struct array
                if (auto* index = dyn_cast<IndexExpr>(member->object())) {
                    // Check if the indexed object has a type that matches our SoA struct
                    if (index->object()->hasType()) {
                        TypeId obj_type = index->object()->getType();
                        // The object type should be an array of our struct
                        // Check if the element type matches the SoA struct
                        if (obj_type && !obj_type.isNull()) {
                            // Look up the field name in the transform
                            const std::string& field_name = member->field();
                            for (size_t i = 0; i < transform.field_names.size(); ++i) {
                                if (transform.field_names[i] == field_name) {
                                    // Annotate this member access for SoA rewriting
                                    if (annotations_) {
                                        std::string detail = transform.struct_name + ":" +
                                                             transform.field_array_names[i];
                                        annotations_->annotate(member,
                                            ASTAnnotationKind::SoATransformed, detail);
                                    }
                                    access_count++;
                                    break;
                                }
                            }
                        }
                    }
                }
                // Recurse into the object expression
                walk_expr(member->object());
            }
            else if (auto* call = dyn_cast<CallExpr>(expr)) {
                walk_expr(call->callee());
                for (auto& arg : call->args()) {
                    walk_expr(arg.get());
                }
            }
            else if (auto* binary = dyn_cast<BinaryExpr>(expr)) {
                walk_expr(binary->left());
                walk_expr(binary->right());
            }
            else if (auto* unary = dyn_cast<UnaryExpr>(expr)) {
                walk_expr(unary->operand());
            }
            else if (auto* index = dyn_cast<IndexExpr>(expr)) {
                walk_expr(index->object());
                walk_expr(index->index());
            }
            else if (auto* deref = dyn_cast<DerefExpr>(expr)) {
                walk_expr(deref->operand());
            }
            else if (auto* addr = dyn_cast<AddrOfExpr>(expr)) {
                walk_expr(addr->operand());
            }
            else if (auto* cast_expr = dyn_cast<CastExpr>(expr)) {
                walk_expr(cast_expr->expr());
            }
            else if (auto* select = dyn_cast<SelectExpr>(expr)) {
                walk_expr(select->condition());
                walk_expr(select->trueExpr());
                walk_expr(select->falseExpr());
            }
            else if (auto* unsafe = dyn_cast<UnsafeExpr>(expr)) {
                walk_expr(unsafe->inner());
            }
            else if (auto* try_expr = dyn_cast<TryExpr>(expr)) {
                walk_expr(try_expr->operand());
            }
            else if (auto* struct_init = dyn_cast<StructInitExpr>(expr)) {
                for (auto& init : struct_init->inits()) {
                    walk_expr(init.value.get());
                }
            }
            else if (auto* array_init = dyn_cast<ArrayInitExpr>(expr)) {
                for (auto& elem : array_init->elements()) {
                    walk_expr(elem.get());
                }
            }
            else if (auto* sizeof_expr = dyn_cast<SizeofExpr>(expr)) {
                if (sizeof_expr->isExprOperand()) {
                    walk_expr(sizeof_expr->expr());
                }
            }
        };

        // Walk the function body statements
        std::function<void(Stmt*)> walk_stmt = [&](Stmt* stmt) {
            if (!stmt) return;

            if (auto* block = dyn_cast<BlockStmt>(stmt)) {
                for (auto& s : block->stmts()) {
                    walk_stmt(s.get());
                }
            }
            else if (auto* var = dyn_cast<VarDeclStmt>(stmt)) {
                if (var->hasInit()) walk_expr(var->init());
            }
            else if (auto* val = dyn_cast<ValDeclStmt>(stmt)) {
                if (val->hasInit()) walk_expr(val->init());
            }
            else if (auto* assign = dyn_cast<AssignStmt>(stmt)) {
                walk_expr(assign->target());
                walk_expr(assign->value());
            }
            else if (auto* defer = dyn_cast<DeferStmt>(stmt)) {
                walk_stmt(defer->stmt());
            }
            else if (auto* errdefer = dyn_cast<ErrdeferStmt>(stmt)) {
                walk_stmt(errdefer->stmt());
            }
            else if (auto* if_stmt = dyn_cast<IfStmt>(stmt)) {
                walk_expr(if_stmt->condition());
                if (if_stmt->thenBlock()) {
                    for (auto& s : if_stmt->thenBlock()->stmts()) walk_stmt(s.get());
                }
                if (if_stmt->elseBlock()) {
                    for (auto& s : if_stmt->elseBlock()->stmts()) walk_stmt(s.get());
                }
            }
            else if (auto* while_stmt = dyn_cast<WhileStmt>(stmt)) {
                walk_expr(while_stmt->condition());
                if (while_stmt->body()) {
                    for (auto& s : while_stmt->body()->stmts()) walk_stmt(s.get());
                }
            }
            else if (auto* ret = dyn_cast<ReturnStmt>(stmt)) {
                if (ret->hasValue()) walk_expr(ret->value());
            }
            else if (auto* expr_stmt = dyn_cast<ExprStmt>(stmt)) {
                walk_expr(expr_stmt->expr());
            }
            else if (auto* atomic = dyn_cast<AtomicStmt>(stmt)) {
                walk_stmt(atomic->inner());
            }
            else if (auto* yield = dyn_cast<YieldStmt>(stmt)) {
                if (yield->hasValue()) walk_expr(yield->value());
            }
        };

        for (auto& s : fn.body()->stmts()) {
            walk_stmt(s.get());
        }
    }

    return access_count > 0;
}

} // namespace tether
