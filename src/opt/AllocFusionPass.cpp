#include "opt/AllocFusionPass.h"

#include <cassert>
#include <algorithm>

namespace tether {

// ============================================================================
// AllocFusionPass::run
//
// Walk all functions looking for consecutive Box.new() calls in the same
// block. When 2+ consecutive Box allocations are found, annotate them
// with alloc_fused metadata so the IR generator can emit batch allocation.
// ============================================================================
bool AllocFusionPass::run(Program& program, TypeTable& type_table) {
    batches_formed_ = 0;
    allocations_fused_ = 0;

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
// analyzeFn - process a function for allocation fusion opportunities
// ============================================================================
bool AllocFusionPass::analyzeFn(FnDecl& fn, TypeTable& type_table) {
    if (!fn.body()) return false;
    return analyzeBlock(fn.body(), type_table);
}

// ============================================================================
// analyzeBlock - look for consecutive Box.new() calls within a block
//
// Algorithm:
//   1. Iterate over statements in the block
//   2. For each statement, check if it's a Box.new() call (in VarDecl/ValDecl/ExprStmt)
//   3. Collect runs of consecutive Box.new() calls
//   4. When a run ends (non-Box statement or end of block), if the run
//      has 2+ allocations, fuse them
// ============================================================================
bool AllocFusionPass::analyzeBlock(BlockStmt* block, TypeTable& type_table) {
    if (!block) return false;

    bool changed = false;
    auto& stmts = block->stmts();

    // Collect runs of consecutive Box.new() calls
    std::vector<CallExpr*> current_run;

    for (size_t i = 0; i < stmts.size(); ++i) {
        Stmt* stmt = stmts[i].get();
        if (!stmt) {
            // End of run — fuse if we have 2+ allocations
            if (current_run.size() >= 2) {
                fuseAllocations(current_run, type_table);
                changed = true;
            }
            current_run.clear();
            continue;
        }

        // Try to extract a Box.new() CallExpr from this statement
        CallExpr* box_call = nullptr;

        switch (stmt->getKind()) {
            case NodeKind::VarDeclStmt: {
                auto& var = cast<VarDeclStmt>(*stmt);
                if (var.hasInit()) {
                    // The init could be a direct Box.new() call or wrap one
                    if (var.init()->getKind() == NodeKind::CallExpr) {
                        auto& call = cast<CallExpr>(*var.init());
                        if (isBoxNew(call)) {
                            box_call = &call;
                        }
                    }
                }
                break;
            }

            case NodeKind::ValDeclStmt: {
                auto& val = cast<ValDeclStmt>(*stmt);
                if (val.hasInit()) {
                    if (val.init()->getKind() == NodeKind::CallExpr) {
                        auto& call = cast<CallExpr>(*val.init());
                        if (isBoxNew(call)) {
                            box_call = &call;
                        }
                    }
                }
                break;
            }

            case NodeKind::ExprStmt: {
                auto& expr_stmt = cast<ExprStmt>(*stmt);
                if (expr_stmt.expr() &&
                    expr_stmt.expr()->getKind() == NodeKind::CallExpr) {
                    auto& call = cast<CallExpr>(*expr_stmt.expr());
                    if (isBoxNew(call)) {
                        box_call = &call;
                    }
                }
                break;
            }

            default:
                break;
        }

        if (box_call) {
            // Check if EscapeAnalysis has already determined this allocation
            // can be stack-allocated. If so, skip fusion — the IR generator
            // will emit alloca instead.
            if (meta_map_) {
                const NodeMeta* existing = meta_map_->get(box_call);
                if (existing && existing->stack_allocated) {
                    // This allocation is already going to be stack-allocated.
                    // End the current run and skip this allocation.
                    if (current_run.size() >= 2) {
                        fuseAllocations(current_run, type_table);
                        changed = true;
                    }
                    current_run.clear();
                    continue;
                }
            }
            current_run.push_back(box_call);
        } else {
            // Not a Box.new() call — end the current run
            if (current_run.size() >= 2) {
                fuseAllocations(current_run, type_table);
                changed = true;
            }
            current_run.clear();

            // Recurse into sub-blocks (if, while, etc.)
            switch (stmt->getKind()) {
                case NodeKind::BlockStmt:
                    if (analyzeBlock(&cast<BlockStmt>(*stmt), type_table))
                        changed = true;
                    break;

                case NodeKind::IfStmt: {
                    auto& if_stmt = cast<IfStmt>(*stmt);
                    if (if_stmt.thenBlock() &&
                        analyzeBlock(if_stmt.thenBlock(), type_table))
                        changed = true;
                    if (if_stmt.elseBlock() &&
                        analyzeBlock(if_stmt.elseBlock(), type_table))
                        changed = true;
                    break;
                }

                case NodeKind::WhileStmt: {
                    auto& while_stmt = cast<WhileStmt>(*stmt);
                    if (while_stmt.body() &&
                        analyzeBlock(while_stmt.body(), type_table))
                        changed = true;
                    break;
                }

                default:
                    break;
            }
        }
    }

    // Handle end-of-block run
    if (current_run.size() >= 2) {
        fuseAllocations(current_run, type_table);
        changed = true;
    }

    return changed;
}

// ============================================================================
// isBoxNew - check if a CallExpr is Box.new()
// ============================================================================
bool AllocFusionPass::isBoxNew(CallExpr& call) {
    Expr* callee = call.callee();
    if (!callee) return false;

    // Expected pattern: Box.new(...)
    // The callee is a MemberExpr with object "Box" and field "new"
    if (callee->getKind() != NodeKind::MemberExpr) return false;

    auto& member = cast<MemberExpr>(*callee);
    if (member.field() != "new") return false;

    if (member.object()->getKind() != NodeKind::IdentExpr) return false;

    auto& ident = cast<IdentExpr>(*member.object());
    return ident.name() == "Box";
}

// ============================================================================
// computeBoxAllocSize - compute the allocation size for a Box.new() call
//
// The size is determined by the type of the first argument (the value being
// boxed). If we can't determine it, we use a default of 8 bytes.
// ============================================================================
int64_t AllocFusionPass::computeBoxAllocSize(CallExpr& call, TypeTable& type_table) {
    // The allocation size depends on the type of the value being boxed.
    // Box.new(value) — the type of 'value' determines the Box's size.
    // We look at the return type of the call (which should be Box<T>) and
    // extract T's size. If we can't determine it, default to 8 bytes.

    if (call.hasType()) {
        TypeId call_type = call.getType();
        if (call_type && isa<SmartPointerType>(call_type)) {
            auto& sp_type = cast<SmartPointerType>(call_type);
            TypeId pointee = sp_type.pointee();
            if (pointee && !pointee.isNull()) {
                uint64_t bw = pointee->bitWidth();
                if (bw > 0) {
                    int64_t size = (int64_t)(bw / 8);
                    if (size == 0) size = 1;  // minimum 1 byte
                    return size;
                }
            }
        }
    }

    // Try the first argument's type
    if (call.argCount() >= 1 && call.args()[0]->hasType()) {
        TypeId arg_type = call.args()[0]->getType();
        if (arg_type && !arg_type.isNull()) {
            uint64_t bw = arg_type->bitWidth();
            if (bw > 0) {
                int64_t size = (int64_t)(bw / 8);
                if (size == 0) size = 1;
                return size;
            }
        }
    }

    // Default: 8 bytes (reasonable for common types)
    return 8;
}

// ============================================================================
// fuseAllocations - annotate a run of Box.new() calls with fusion metadata
//
// For each Box.new() in the run:
//   - Set alloc_fused = true
//   - Set alloc_batch_size = total batch size (only on the first allocation)
//   - Set alloc_batch_offset = offset within the batch
//
// The IR generator can then emit:
//   %batch = call tether_batch_alloc(total_size)
//   %ptr0  = call tether_batch_carve(%batch, size0)
//   %ptr1  = call tether_batch_carve(%batch, size1)
//   ...
//   call tether_batch_free(%batch)   ; at the end of the scope
// ============================================================================
void AllocFusionPass::fuseAllocations(const std::vector<CallExpr*>& calls,
                                      TypeTable& type_table) {
    if (!meta_map_ || calls.size() < 2) return;

    // Compute sizes and total
    std::vector<int64_t> sizes;
    int64_t total_size = 0;
    int64_t current_offset = 0;

    for (auto* call : calls) {
        int64_t size = computeBoxAllocSize(*call, type_table);
        // Align each allocation to 16 bytes (matching tether_batch_carve)
        int64_t aligned_offset = (current_offset + 15) & ~15;
        sizes.push_back(size);
        total_size = aligned_offset + size;
        current_offset = total_size;
    }

    // Annotate each allocation
    current_offset = 0;
    for (size_t i = 0; i < calls.size(); ++i) {
        int64_t aligned_offset = (current_offset + 15) & ~15;
        auto& nm = meta_map_->getOrCreate(calls[i]);
        nm.alloc_fused = true;
        nm.alloc_batch_offset = aligned_offset;

        if (i == 0) {
            // First allocation in the batch carries the total size
            nm.alloc_batch_size = total_size;
        }

        current_offset = aligned_offset + sizes[i];
    }

    batches_formed_++;
    allocations_fused_ += (int)calls.size();
}

} // namespace tether
