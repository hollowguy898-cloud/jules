#include "metadata/MemoryTopologyAnalyzer.h"
#include "parser/Parser.h"  // for Program = vector<unique_ptr<TopLevel>>

#include <algorithm>
#include <unordered_set>

namespace tether {

// ============================================================================
// Constants
// ============================================================================
static constexpr uint64_t CACHE_LINE_SIZE = 64;

// ============================================================================
// Internal helpers (file-scoped)
// ============================================================================

// Build a human-readable name for the object being indexed.
// IdentExpr -> "x", MemberExpr -> "obj.field"
static std::string getObjectName(Expr* obj) {
    if (!obj) return "<unknown>";
    if (auto* ident = dyn_cast<IdentExpr>(obj)) {
        return ident->name();
    }
    if (auto* member = dyn_cast<MemberExpr>(obj)) {
        return getObjectName(member->object()) + "." + member->field();
    }
    // For DerefExpr, peel through
    if (auto* deref = dyn_cast<DerefExpr>(obj)) {
        return "*" + getObjectName(deref->operand());
    }
    return "<unknown>";
}

// Check whether an index expression is simply the loop variable (IdentExpr with that name)
static bool isLoopVarExpr(Expr* expr, const std::string& loop_var) {
    if (!expr) return false;
    if (auto* ident = dyn_cast<IdentExpr>(expr)) {
        return ident->name() == loop_var;
    }
    return false;
}

// Recursively collect all IndexExpr nodes within an expression tree.
// is_write tracks whether we are in a write (LHS of assignment) context.
static void collectIndexExprsInExpr(
    Expr* expr,
    bool is_write,
    std::vector<MemoryTopologyAnalyzer::IndexAccessInfo>& result)
{
    if (!expr) return;

    // IndexExpr — the primary target
    if (auto* idx = dyn_cast<IndexExpr>(expr)) {
        MemoryTopologyAnalyzer::IndexAccessInfo info;
        info.variable_name   = getObjectName(idx->object());
        info.index_expr      = idx->index();
        info.access_kind     = is_write ? AccessKind::Write : AccessKind::Read;
        info.is_assignment_target = is_write;
        result.push_back(info);
        // Also recurse into the object and index sub-expressions for nested accesses
        collectIndexExprsInExpr(idx->object(), false, result);
        collectIndexExprsInExpr(idx->index(), false, result);
        return;
    }

    // BinaryExpr — recurse both sides (LHS is not a write context unless
    // the whole BinaryExpr is the target of an AssignStmt, which is handled
    // at the statement level)
    if (auto* bin = dyn_cast<BinaryExpr>(expr)) {
        collectIndexExprsInExpr(bin->left(), false, result);
        collectIndexExprsInExpr(bin->right(), false, result);
        return;
    }

    // UnaryExpr
    if (auto* unary = dyn_cast<UnaryExpr>(expr)) {
        collectIndexExprsInExpr(unary->operand(), false, result);
        return;
    }

    // CallExpr — callee and all args are reads
    if (auto* call = dyn_cast<CallExpr>(expr)) {
        collectIndexExprsInExpr(call->callee(), false, result);
        for (const auto& arg : call->args()) {
            collectIndexExprsInExpr(arg.get(), false, result);
        }
        return;
    }

    // MemberExpr
    if (auto* member = dyn_cast<MemberExpr>(expr)) {
        collectIndexExprsInExpr(member->object(), false, result);
        return;
    }

    // SelectExpr
    if (auto* sel = dyn_cast<SelectExpr>(expr)) {
        collectIndexExprsInExpr(sel->condition(), false, result);
        collectIndexExprsInExpr(sel->trueExpr(), false, result);
        collectIndexExprsInExpr(sel->falseExpr(), false, result);
        return;
    }

    // CastExpr
    if (auto* cast_e = dyn_cast<CastExpr>(expr)) {
        collectIndexExprsInExpr(cast_e->expr(), false, result);
        return;
    }

    // DerefExpr
    if (auto* deref = dyn_cast<DerefExpr>(expr)) {
        collectIndexExprsInExpr(deref->operand(), false, result);
        return;
    }

    // AddrOfExpr
    if (auto* addr = dyn_cast<AddrOfExpr>(expr)) {
        collectIndexExprsInExpr(addr->operand(), false, result);
        return;
    }

    // TryExpr
    if (auto* try_e = dyn_cast<TryExpr>(expr)) {
        collectIndexExprsInExpr(try_e->operand(), false, result);
        return;
    }

    // UnsafeExpr
    if (auto* unsafe = dyn_cast<UnsafeExpr>(expr)) {
        collectIndexExprsInExpr(unsafe->inner(), false, result);
        return;
    }

    // ComptimeExpr
    if (auto* ct = dyn_cast<ComptimeExpr>(expr)) {
        collectIndexExprsInExpr(ct->inner(), false, result);
        return;
    }

    // ReduceExpr
    if (auto* red = dyn_cast<ReduceExpr>(expr)) {
        collectIndexExprsInExpr(red->iterable(), false, result);
        if (red->axis()) {
            collectIndexExprsInExpr(red->axis(), false, result);
        }
        return;
    }

    // StructInitExpr
    if (auto* si = dyn_cast<StructInitExpr>(expr)) {
        for (const auto& init : si->inits()) {
            if (init.value) {
                collectIndexExprsInExpr(init.value.get(), false, result);
            }
        }
        return;
    }

    // ArrayInitExpr
    if (auto* ai = dyn_cast<ArrayInitExpr>(expr)) {
        for (const auto& elem : ai->elements()) {
            collectIndexExprsInExpr(elem.get(), false, result);
        }
        return;
    }

    // SizeofExpr (may have expr operand)
    if (auto* sz = dyn_cast<SizeofExpr>(expr)) {
        if (sz->isExprOperand() && sz->expr()) {
            collectIndexExprsInExpr(sz->expr(), false, result);
        }
        return;
    }

    // PoisonExpr, IntLiteral, FloatLiteral, BoolLiteral, StringLiteral, IdentExpr
    // — leaf nodes, nothing to recurse into.
}

// Collect IndexExprs from a statement, properly tracking write context.
static void collectIndexExprsInStmt(
    Stmt* stmt,
    std::vector<MemoryTopologyAnalyzer::IndexAccessInfo>& result)
{
    if (!stmt) return;

    // AssignStmt — target is a write, value is a read
    if (auto* assign = dyn_cast<AssignStmt>(stmt)) {
        collectIndexExprsInExpr(assign->target(), true, result);
        collectIndexExprsInExpr(assign->value(), false, result);
        return;
    }

    // VarDeclStmt — initializer is a read
    if (auto* var = dyn_cast<VarDeclStmt>(stmt)) {
        if (var->hasInit()) {
            collectIndexExprsInExpr(var->init(), false, result);
        }
        return;
    }

    // ValDeclStmt — initializer is a read
    if (auto* val = dyn_cast<ValDeclStmt>(stmt)) {
        if (val->hasInit()) {
            collectIndexExprsInExpr(val->init(), false, result);
        }
        return;
    }

    // ExprStmt
    if (auto* es = dyn_cast<ExprStmt>(stmt)) {
        collectIndexExprsInExpr(es->expr(), false, result);
        return;
    }

    // IfStmt — recurse condition + both branches
    if (auto* ifstmt = dyn_cast<IfStmt>(stmt)) {
        collectIndexExprsInExpr(ifstmt->condition(), false, result);
        if (ifstmt->thenBlock()) {
            for (const auto& s : ifstmt->thenBlock()->stmts()) {
                collectIndexExprsInStmt(s.get(), result);
            }
        }
        if (ifstmt->elseBlock()) {
            for (const auto& s : ifstmt->elseBlock()->stmts()) {
                collectIndexExprsInStmt(s.get(), result);
            }
        }
        return;
    }

    // WhileStmt — recurse condition + body
    if (auto* ws = dyn_cast<WhileStmt>(stmt)) {
        collectIndexExprsInExpr(ws->condition(), false, result);
        if (ws->body()) {
            for (const auto& s : ws->body()->stmts()) {
                collectIndexExprsInStmt(s.get(), result);
            }
        }
        return;
    }

    // BlockStmt
    if (auto* block = dyn_cast<BlockStmt>(stmt)) {
        for (const auto& s : block->stmts()) {
            collectIndexExprsInStmt(s.get(), result);
        }
        return;
    }

    // DeferStmt
    if (auto* defer = dyn_cast<DeferStmt>(stmt)) {
        collectIndexExprsInStmt(defer->stmt(), result);
        return;
    }

    // ErrdeferStmt
    if (auto* errdefer = dyn_cast<ErrdeferStmt>(stmt)) {
        collectIndexExprsInStmt(errdefer->stmt(), result);
        return;
    }

    // ReturnStmt
    if (auto* ret = dyn_cast<ReturnStmt>(stmt)) {
        if (ret->hasValue()) {
            collectIndexExprsInExpr(ret->value(), false, result);
        }
        return;
    }

    // AtomicStmt — treat inner as normal stmt
    if (auto* atomic = dyn_cast<AtomicStmt>(stmt)) {
        collectIndexExprsInStmt(atomic->inner(), result);
        return;
    }

    // SwitchStmt
    if (auto* sw = dyn_cast<SwitchStmt>(stmt)) {
        collectIndexExprsInExpr(sw->subject(), false, result);
        for (const auto& arm : sw->arms()) {
            if (arm.pattern) {
                collectIndexExprsInExpr(arm.pattern.get(), false, result);
            }
            if (arm.body) {
                for (const auto& s : arm.body->stmts()) {
                    collectIndexExprsInStmt(s.get(), result);
                }
            }
        }
        return;
    }

    // BreakStmt, ContinueStmt, YieldStmt — no index expressions
    if (auto* yield = dyn_cast<YieldStmt>(stmt)) {
        if (yield->hasValue()) {
            collectIndexExprsInExpr(yield->value(), false, result);
        }
        return;
    }

    // SpawnStmt
    if (auto* spawn = dyn_cast<SpawnStmt>(stmt)) {
        collectIndexExprsInExpr(spawn->task(), false, result);
        return;
    }
}

// Check whether a block (recursively) contains an IfStmt, which indicates
// conditional/sparse access patterns.
static bool containsIfStmt(BlockStmt& block) {
    for (const auto& stmt : block.stmts()) {
        if (!stmt) continue;
        if (isa<IfStmt>(stmt.get())) return true;

        // Recurse into nested blocks
        if (auto* inner = dyn_cast<BlockStmt>(stmt.get())) {
            if (containsIfStmt(*inner)) return true;
        }
        // Recurse into while loop bodies
        if (auto* ws = dyn_cast<WhileStmt>(stmt.get())) {
            if (ws->body() && containsIfStmt(*ws->body())) return true;
        }
        // Recurse into if-else branches
        if (auto* ifstmt = dyn_cast<IfStmt>(stmt.get())) {
            if (ifstmt->thenBlock() && containsIfStmt(*ifstmt->thenBlock())) return true;
            if (ifstmt->elseBlock() && containsIfStmt(*ifstmt->elseBlock())) return true;
        }
        // Recurse into defer
        if (auto* defer = dyn_cast<DeferStmt>(stmt.get())) {
            if (defer->stmt()) {
                // Wrap the single stmt in a temporary check
                if (auto* inner_block = dyn_cast<BlockStmt>(defer->stmt())) {
                    if (containsIfStmt(*inner_block)) return true;
                }
                if (isa<IfStmt>(defer->stmt())) return true;
            }
        }
    }
    return false;
}

// Try to extract the element TypeId from the object of an IndexExpr.
// For []T -> T, *T -> T, &T -> T, etc.
static TypeId getElementTypeId(Expr* obj) {
    if (!obj || !obj->hasType()) return TypeId();
    TypeId obj_type = obj->getType();

    // Slice []T -> element is T
    if (auto* slice = dyn_cast<SliceType>(obj_type)) {
        return slice->element();
    }
    // Pointer *T -> element is T
    if (auto* ptr = dyn_cast<PointerType>(obj_type)) {
        return ptr->pointee();
    }
    // Reference &T -> element is T
    if (auto* ref = dyn_cast<ReferenceType>(obj_type)) {
        return ref->referent();
    }
    // Mutable reference &mut T -> element is T
    if (auto* mref = dyn_cast<MutReferenceType>(obj_type)) {
        return mref->referent();
    }
    // Tensor tensor<E, S, St> -> element is E
    if (auto* tensor = dyn_cast<TensorType>(obj_type)) {
        return tensor->elementType();
    }

    return TypeId();
}

// Compute the byte size of a type. Returns 0 if unknown.
static uint64_t typeSizeBytes(TypeId tid) {
    if (tid.isNull()) return 0;
    uint64_t bits = tid->bitWidth();
    if (bits == 0) return 0;
    return (bits + 7) / 8;
}

// Compute the alignment of a type (in bytes). Simplified heuristic:
// - Primitives: natural alignment (size, capped at 8)
// - Pointers/references: 8 bytes
// - Structs: max of field alignments
// - Others: 8 bytes (conservative)
static uint64_t typeAlignment(TypeId tid) {
    if (tid.isNull()) return 1;
    if (auto* prim = dyn_cast<PrimitiveType>(tid)) {
        uint64_t sz = typeSizeBytes(tid);
        return std::min(sz, uint64_t(8));
    }
    if (tid->isPointerLike()) return 8;
    if (auto* st = dyn_cast<StructType>(tid)) {
        uint64_t max_align = 1;
        for (const auto& field : st->fields()) {
            max_align = std::max(max_align, typeAlignment(field.type));
        }
        return max_align;
    }
    if (auto* aligned = dyn_cast<AlignedType>(tid)) {
        return aligned->alignment();
    }
    return 8;
}

// Round up `value` to the next multiple of `alignment`.
static uint64_t alignUp(uint64_t value, uint64_t alignment) {
    if (alignment == 0) return value;
    return (value + alignment - 1) / alignment * alignment;
}

// Try to evaluate a simple integer expression to a constant.
// Only handles IntLiteral and simple BinaryExpr(Add/Sub, IdentExpr, IntLiteral).
static bool tryEvalConstInt(Expr* expr, int64_t& out) {
    if (!expr) return false;
    if (auto* lit = dyn_cast<IntLiteral>(expr)) {
        out = static_cast<int64_t>(lit->value());
        return true;
    }
    if (auto* neg = dyn_cast<UnaryExpr>(expr)) {
        if (neg->op() == UnaryOp::Neg) {
            int64_t val;
            if (tryEvalConstInt(neg->operand(), val)) {
                out = -val;
                return true;
            }
        }
    }
    return false;
}

// ============================================================================
// MemoryTopologyAnalyzer — public API
// ============================================================================

void MemoryTopologyAnalyzer::analyze(
    Program& program, TypeTable& type_table, MetadataMap& meta)
{
    for (const auto& decl : program) {
        if (!decl) continue;

        if (auto* fn = dyn_cast<FnDecl>(decl.get())) {
            analyzeFn(*fn, type_table, meta);
        } else if (auto* sd = dyn_cast<StructDecl>(decl.get())) {
            analyzeStructWaste(*sd, type_table, meta);
        }
    }
}

// ============================================================================
// analyzeFn
// ============================================================================

void MemoryTopologyAnalyzer::analyzeFn(
    FnDecl& fn, TypeTable& type_table, MetadataMap& meta)
{
    if (!fn.body()) return;
    walkStmts(*fn.body(), type_table, meta);
}

// ============================================================================
// walkStmts — recursive walk looking for while loops
// ============================================================================

void MemoryTopologyAnalyzer::walkStmts(
    BlockStmt& block, TypeTable& type_table, MetadataMap& meta)
{
    for (const auto& stmt : block.stmts()) {
        if (!stmt) continue;

        if (auto* ws = dyn_cast<WhileStmt>(stmt.get())) {
            analyzeWhileLoop(*ws, type_table, meta);
            // Also recurse into the while loop's body for nested loops
            if (ws->body()) {
                walkStmts(*ws->body(), type_table, meta);
            }
        } else if (auto* ifstmt = dyn_cast<IfStmt>(stmt.get())) {
            // Recurse into if branches
            if (ifstmt->thenBlock()) {
                walkStmts(*ifstmt->thenBlock(), type_table, meta);
            }
            if (ifstmt->elseBlock()) {
                walkStmts(*ifstmt->elseBlock(), type_table, meta);
            }
        } else if (auto* inner = dyn_cast<BlockStmt>(stmt.get())) {
            walkStmts(*inner, type_table, meta);
        } else if (auto* defer = dyn_cast<DeferStmt>(stmt.get())) {
            if (defer->stmt()) {
                if (auto* inner_block = dyn_cast<BlockStmt>(defer->stmt())) {
                    walkStmts(*inner_block, type_table, meta);
                } else if (auto* ws = dyn_cast<WhileStmt>(defer->stmt())) {
                    analyzeWhileLoop(*ws, type_table, meta);
                }
            }
        } else if (auto* errdefer = dyn_cast<ErrdeferStmt>(stmt.get())) {
            if (errdefer->stmt()) {
                if (auto* inner_block = dyn_cast<BlockStmt>(errdefer->stmt())) {
                    walkStmts(*inner_block, type_table, meta);
                }
            }
        } else if (auto* atomic = dyn_cast<AtomicStmt>(stmt.get())) {
            if (atomic->inner()) {
                if (auto* inner_block = dyn_cast<BlockStmt>(atomic->inner())) {
                    walkStmts(*inner_block, type_table, meta);
                } else if (auto* ws = dyn_cast<WhileStmt>(atomic->inner())) {
                    analyzeWhileLoop(*ws, type_table, meta);
                }
            }
        } else if (auto* sw = dyn_cast<SwitchStmt>(stmt.get())) {
            for (const auto& arm : sw->arms()) {
                if (arm.body) {
                    walkStmts(*arm.body, type_table, meta);
                }
            }
        }
    }
}

// ============================================================================
// analyzeWhileLoop — the core method
// ============================================================================

void MemoryTopologyAnalyzer::analyzeWhileLoop(
    WhileStmt& ws, TypeTable& type_table, MetadataMap& meta)
{
    // 1. Get the loop variable name
    std::string loop_var = getLoopVarName(ws);
    if (loop_var.empty()) {
        // Cannot determine loop variable — still record basic metadata
        NodeMeta& nm = meta.getOrCreate(&ws);
        nm.has_loop_with_linear_access = false;
        return;
    }

    // 2. Collect all index accesses in the loop body
    auto accesses = collectIndexAccesses(*ws.body());

    if (accesses.empty()) {
        // No array accesses in this loop — nothing to optimize
        NodeMeta& nm = meta.getOrCreate(&ws);
        nm.has_loop_with_linear_access = false;
        return;
    }

    // 3. Determine traversal kind
    TraversalKind traversal = determineTraversal(accesses, ws);

    // 4. Determine stride
    int64_t stride = 1;
    if (traversal == TraversalKind::Strided) {
        int64_t s = 0;
        isStridedIncrement(ws, s);
        stride = s;
    } else if (traversal == TraversalKind::Linear) {
        stride = 1;
    }

    // 5. Check contiguous access
    bool contiguous = isContiguousAccess(accesses, loop_var);

    // 6. Separate reads and writes for dependency analysis
    std::vector<IndexAccessInfo> reads, writes;
    for (const auto& acc : accesses) {
        if (acc.is_assignment_target) {
            writes.push_back(acc);
        } else {
            reads.push_back(acc);
        }
    }

    // 7. Check for loop-carried dependencies
    bool has_dep = hasLoopCarriedDependency(reads, writes, loop_var);

    // 8. Determine overall access kind
    bool has_reads  = !reads.empty();
    bool has_writes = !writes.empty();
    AccessKind overall_access;
    if (has_reads && has_writes) {
        overall_access = AccessKind::ReadWrite;
    } else if (has_writes) {
        overall_access = AccessKind::Write;
    } else {
        overall_access = AccessKind::Read;
    }

    // 9. Compute element size for prefetch distance
    uint64_t element_size_bytes = 8; // default: u64/f64
    std::string tbaa_type;

    // Try to get the element type by walking the body looking for IndexExpr
    // and checking its object's type
    for (const auto& stmt : ws.body()->stmts()) {
        if (!stmt) continue;
        // Quick scan for ExprStmt or AssignStmt containing IndexExpr
        Expr* candidate = nullptr;
        if (auto* es = dyn_cast<ExprStmt>(stmt.get())) {
            candidate = es->expr();
        } else if (auto* assign = dyn_cast<AssignStmt>(stmt.get())) {
            candidate = assign->target();
        }

        if (candidate) {
            if (auto* idx = dyn_cast<IndexExpr>(candidate)) {
                TypeId elem_type = getElementTypeId(idx->object());
                if (!elem_type.isNull()) {
                    uint64_t sz = typeSizeBytes(elem_type);
                    if (sz > 0) {
                        element_size_bytes = sz;
                    }
                    tbaa_type = elem_type->toString();
                    break;
                }
            }
        }
    }

    // If we still don't have a tbaa_type, use a fallback from the first access
    if (tbaa_type.empty() && !accesses.empty()) {
        tbaa_type = accesses[0].variable_name;
    }

    // 10. Compute prefetch distance
    int64_t prefetch_dist = computePrefetchDistance(traversal, element_size_bytes);

    // 11. Determine vectorizability
    bool vectorizable = !has_dep &&
                        (traversal == TraversalKind::Linear ||
                         traversal == TraversalKind::Strided);

    // 12. Build AccessPattern entries — one per unique variable
    // Group accesses by variable name
    std::unordered_map<std::string, std::vector<IndexAccessInfo>> by_var;
    for (const auto& acc : accesses) {
        by_var[acc.variable_name].push_back(acc);
    }

    NodeMeta& nm = meta.getOrCreate(&ws);
    nm.has_loop_with_linear_access = (traversal == TraversalKind::Linear && contiguous);

    for (auto& [var_name, var_accesses] : by_var) {
        AccessPattern pat;
        pat.variable_name    = var_name;
        pat.traversal        = traversal;
        pat.stride           = stride;
        pat.contiguous       = contiguous;
        pat.prefetch_distance = prefetch_dist;

        // Determine this variable's access kind
        bool var_read = false, var_write = false;
        for (const auto& a : var_accesses) {
            if (a.is_assignment_target) var_write = true;
            else var_read = true;
        }
        if (var_read && var_write)       pat.access = AccessKind::ReadWrite;
        else if (var_write)              pat.access = AccessKind::Write;
        else                             pat.access = AccessKind::Read;

        pat.vectorizable = vectorizable;

        nm.access_patterns.push_back(std::move(pat));
    }

    // 13. Set LLVM metadata flags
    nm.llvm_meta.vectorization_safe = (traversal == TraversalKind::Linear) && !has_dep;
    nm.llvm_meta.prefetch_distance  = prefetch_dist;
    nm.llvm_meta.tbaa_type          = tbaa_type;
}

// ============================================================================
// collectIndexAccesses
// ============================================================================

std::vector<MemoryTopologyAnalyzer::IndexAccessInfo>
MemoryTopologyAnalyzer::collectIndexAccesses(BlockStmt& body) {
    std::vector<IndexAccessInfo> result;
    for (const auto& stmt : body.stmts()) {
        if (stmt) {
            collectIndexExprsInStmt(stmt.get(), result);
        }
    }
    return result;
}

// ============================================================================
// determineTraversal
// ============================================================================

TraversalKind MemoryTopologyAnalyzer::determineTraversal(
    const std::vector<IndexAccessInfo>& accesses,
    WhileStmt& ws) const
{
    // Check for linear increment first (i += 1)
    if (isLinearIncrement(ws)) {
        // If there are conditional accesses, it's sparse
        if (containsIfStmt(*ws.body())) {
            return TraversalKind::Sparse;
        }
        return TraversalKind::Linear;
    }

    // Check for strided increment (i += K)
    int64_t stride = 0;
    if (isStridedIncrement(ws, stride)) {
        if (containsIfStmt(*ws.body())) {
            return TraversalKind::Sparse;
        }
        return TraversalKind::Strided;
    }

    // Check if any index expression is NOT the loop variable
    std::string loop_var = getLoopVarName(ws);
    for (const auto& acc : accesses) {
        if (acc.index_expr && !isLoopVarExpr(acc.index_expr, loop_var)) {
            // The index is a computed expression, not just the loop var
            // This could be random/gather-scatter
            return TraversalKind::Random;
        }
    }

    // If there are conditional accesses
    if (containsIfStmt(*ws.body())) {
        return TraversalKind::Sparse;
    }

    return TraversalKind::Unknown;
}

// ============================================================================
// isLinearIncrement — check if the loop variable increments by 1
// ============================================================================

bool MemoryTopologyAnalyzer::isLinearIncrement(WhileStmt& ws) const {
    if (!ws.hasIncrement()) return false;
    Expr* incr = ws.increment();
    if (!incr) return false;

    // Pattern 1: i += 1 (BinaryExpr with AddAssign)
    if (auto* bin = dyn_cast<BinaryExpr>(incr)) {
        if (bin->op() == BinaryOp::AddAssign) {
            int64_t val = 0;
            if (tryEvalConstInt(bin->right(), val) && val == 1) {
                return true;
            }
        }
        // Pattern 2: i = i + 1 (BinaryExpr Assign with Add on right)
        if (bin->op() == BinaryOp::Assign) {
            if (auto* rhs = dyn_cast<BinaryExpr>(bin->right())) {
                if (rhs->op() == BinaryOp::Add) {
                    int64_t val = 0;
                    if (tryEvalConstInt(rhs->right(), val) && val == 1) {
                        // Also check that lhs of Add matches the target
                        if (auto* lhs_ident = dyn_cast<IdentExpr>(bin->left())) {
                            if (auto* add_lhs = dyn_cast<IdentExpr>(rhs->left())) {
                                if (lhs_ident->name() == add_lhs->name()) {
                                    return true;
                                }
                            }
                        }
                    }
                    // Also try the other order: i = 1 + i
                    if (tryEvalConstInt(rhs->left(), val) && val == 1) {
                        if (auto* lhs_ident = dyn_cast<IdentExpr>(bin->left())) {
                            if (auto* add_rhs = dyn_cast<IdentExpr>(rhs->right())) {
                                if (lhs_ident->name() == add_rhs->name()) {
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

// ============================================================================
// isStridedIncrement — check if the loop variable increments by K > 1
// ============================================================================

bool MemoryTopologyAnalyzer::isStridedIncrement(WhileStmt& ws, int64_t& stride) const {
    if (!ws.hasIncrement()) return false;
    Expr* incr = ws.increment();
    if (!incr) return false;

    // Pattern 1: i += K (BinaryExpr with AddAssign)
    if (auto* bin = dyn_cast<BinaryExpr>(incr)) {
        if (bin->op() == BinaryOp::AddAssign) {
            int64_t val = 0;
            if (tryEvalConstInt(bin->right(), val) && val > 1) {
                stride = val;
                return true;
            }
        }
        // Pattern 2: i = i + K
        if (bin->op() == BinaryOp::Assign) {
            if (auto* rhs = dyn_cast<BinaryExpr>(bin->right())) {
                if (rhs->op() == BinaryOp::Add) {
                    int64_t val = 0;
                    // i = i + K
                    if (tryEvalConstInt(rhs->right(), val) && val > 1) {
                        if (auto* lhs_ident = dyn_cast<IdentExpr>(bin->left())) {
                            if (auto* add_lhs = dyn_cast<IdentExpr>(rhs->left())) {
                                if (lhs_ident->name() == add_lhs->name()) {
                                    stride = val;
                                    return true;
                                }
                            }
                        }
                    }
                    // i = K + i
                    if (tryEvalConstInt(rhs->left(), val) && val > 1) {
                        if (auto* lhs_ident = dyn_cast<IdentExpr>(bin->left())) {
                            if (auto* add_rhs = dyn_cast<IdentExpr>(rhs->right())) {
                                if (lhs_ident->name() == add_rhs->name()) {
                                    stride = val;
                                    return true;
                                }
                            }
                        }
                    }
                }
            }
        }
    }

    return false;
}

// ============================================================================
// getLoopVarName — extract the loop variable from the while condition
// ============================================================================

std::string MemoryTopologyAnalyzer::getLoopVarName(WhileStmt& ws) const {
    Expr* cond = ws.condition();
    if (!cond) return "";

    // Typical pattern: i < N or i <= N
    if (auto* bin = dyn_cast<BinaryExpr>(cond)) {
        if (bin->op() == BinaryOp::Lt || bin->op() == BinaryOp::Le ||
            bin->op() == BinaryOp::Ne) {
            if (auto* ident = dyn_cast<IdentExpr>(bin->left())) {
                return ident->name();
            }
        }
        // Handle N > i or N >= i
        if (bin->op() == BinaryOp::Gt || bin->op() == BinaryOp::Ge) {
            if (auto* ident = dyn_cast<IdentExpr>(bin->right())) {
                return ident->name();
            }
        }
    }

    // Fallback: if the increment exists, try to extract the variable name from it
    if (ws.hasIncrement()) {
        Expr* incr = ws.increment();
        if (!incr) return "";

        if (auto* bin = dyn_cast<BinaryExpr>(incr)) {
            // i += K or i = i + K
            if (bin->op() == BinaryOp::AddAssign ||
                bin->op() == BinaryOp::SubAssign ||
                bin->op() == BinaryOp::Assign) {
                if (auto* ident = dyn_cast<IdentExpr>(bin->left())) {
                    return ident->name();
                }
            }
        }
    }

    return "";
}

// ============================================================================
// computePrefetchDistance
// ============================================================================

int64_t MemoryTopologyAnalyzer::computePrefetchDistance(
    TraversalKind traversal,
    uint64_t element_size_bytes) const
{
    if (element_size_bytes == 0) element_size_bytes = 8;

    switch (traversal) {
        case TraversalKind::Linear:
            // Prefetch 2 cache lines ahead
            return static_cast<int64_t>(2 * CACHE_LINE_SIZE / element_size_bytes);

        case TraversalKind::Strided: {
            // Strided: prefetch = stride * 2 cache lines / element_size
            // We don't have stride here, so use a conservative estimate:
            // assume stride of 2 (worst reasonable case) → 2 * 2 * 64 / element_size
            // Actually the caller should factor in stride separately. Here we
            // compute elements per 2 cache lines.
            return static_cast<int64_t>(2 * CACHE_LINE_SIZE / element_size_bytes);
        }

        case TraversalKind::Random:
            // Prefetching doesn't help for random access
            return 0;

        case TraversalKind::Sparse:
            // 1 cache line ahead
            return static_cast<int64_t>(CACHE_LINE_SIZE / element_size_bytes);

        case TraversalKind::Chunked:
            // Chunked access: prefetch 2 cache lines ahead like linear
            return static_cast<int64_t>(2 * CACHE_LINE_SIZE / element_size_bytes);

        case TraversalKind::Unknown:
        default:
            // Conservative: 1 cache line ahead
            return static_cast<int64_t>(CACHE_LINE_SIZE / element_size_bytes);
    }
}

// ============================================================================
// isContiguousAccess
// ============================================================================

bool MemoryTopologyAnalyzer::isContiguousAccess(
    const std::vector<IndexAccessInfo>& accesses,
    const std::string& loop_var) const
{
    if (accesses.empty()) return false;
    if (loop_var.empty()) return false;

    // All index expressions must be directly the loop variable
    for (const auto& acc : accesses) {
        if (!acc.index_expr) return false;
        if (!isLoopVarExpr(acc.index_expr, loop_var)) {
            return false;
        }
    }
    return true;
}

// ============================================================================
// hasLoopCarriedDependency
// ============================================================================

bool MemoryTopologyAnalyzer::hasLoopCarriedDependency(
    const std::vector<IndexAccessInfo>& reads,
    const std::vector<IndexAccessInfo>& writes,
    const std::string& loop_var) const
{
    if (writes.empty() || reads.empty()) return false;
    if (loop_var.empty()) return true; // conservative

    // Build set of variable names that are written
    std::unordered_set<std::string> written_vars;
    // Track whether each written variable uses a non-loop-var index
    std::unordered_set<std::string> written_vars_nontrivial_index;
    // Track whether each written variable uses just the loop var
    std::unordered_set<std::string> written_vars_trivial_index;

    for (const auto& w : writes) {
        written_vars.insert(w.variable_name);
        if (w.index_expr && isLoopVarExpr(w.index_expr, loop_var)) {
            written_vars_trivial_index.insert(w.variable_name);
        } else {
            written_vars_nontrivial_index.insert(w.variable_name);
        }
    }

    // Check each read against writes
    for (const auto& r : reads) {
        if (written_vars.count(r.variable_name) == 0) continue;

        // Same variable is both read and written.

        // Case 1: Write uses a non-loop-var index expression → dependency.
        // e.g., data[i+1] = ... and read data[i]
        if (written_vars_nontrivial_index.count(r.variable_name) > 0) {
            return true;
        }

        // Case 2: Both read and write use the loop var directly, BUT
        // the read uses a non-trivial index → dependency.
        // e.g., data[i] = data[i-1] + 1 — write at i, read at i-1
        if (r.index_expr && !isLoopVarExpr(r.index_expr, loop_var)) {
            return true;
        }

        // Case 3: Both read and write are at data[loop_var].
        // e.g., data[i] = data[i] + 1 — LLVM can handle this (read-before-write
        // at the same index within one iteration is not a cross-iteration dep).
        // No dependency → fall through.
    }

    return false;
}

// ============================================================================
// analyzeStructWaste
// ============================================================================

void MemoryTopologyAnalyzer::analyzeStructWaste(
    StructDecl& sd, TypeTable& type_table, MetadataMap& meta)
{
    // Compute struct size with alignment padding for metadata
    uint64_t struct_size = 0;
    uint64_t max_align = 1;
    for (const auto& field : sd.fields()) {
        uint64_t field_size = typeSizeBytes(field.type);
        uint64_t field_align = typeAlignment(field.type);
        struct_size = alignUp(struct_size, field_align);
        struct_size += field_size;
        max_align = std::max(max_align, field_align);
    }
    // Pad to alignment
    struct_size = alignUp(struct_size, max_align);

    // Compute the fraction of waste in a cache line
    uint64_t useful_per_line = (struct_size == 0) ? CACHE_LINE_SIZE :
        (CACHE_LINE_SIZE / struct_size) * struct_size;
    // If the struct is larger than a cache line, waste is the remainder
    // of the last cache line
    uint64_t waste = 0;
    if (struct_size > CACHE_LINE_SIZE) {
        waste = struct_size % CACHE_LINE_SIZE;
        if (waste != 0) {
            waste = CACHE_LINE_SIZE - waste;
        }
    } else {
        waste = CACHE_LINE_SIZE - useful_per_line;
    }

    // Set alignment on the StructDecl
    uint32_t struct_alignment = static_cast<uint32_t>(max_align);
    sd.setAlignment(struct_alignment);

    // Register struct metadata
    MetadataMap::StructMeta smeta;
    smeta.name      = sd.name();
    smeta.alignment = struct_alignment;

    // Determine layout kind
    if (sd.isSoA()) {
        smeta.layout = LayoutKind::SoA;
    } else {
        smeta.layout = LayoutKind::AoS;
    }

    // Collect field names
    for (const auto& field : sd.fields()) {
        smeta.field_names.push_back(field.name);
        smeta.field_is_hot.push_back(false); // hot/cold not determined here
    }

    // If waste > 25% of a cache line, flag as layout optimization candidate
    uint64_t threshold = CACHE_LINE_SIZE / 4; // 16 bytes
    if (waste > threshold) {
        // Create a layout transform suggestion
        LayoutTransform transform;
        transform.kind        = TransformKind::FieldReorder;
        transform.struct_name = sd.name();

        // Sort fields by size descending (simple heuristic for reordering)
        // to minimize padding
        struct FieldInfo {
            std::string name;
            uint64_t size;
            uint64_t alignment;
            int original_index;
        };
        std::vector<FieldInfo> fields_info;
        for (int i = 0; i < static_cast<int>(sd.fields().size()); ++i) {
            const auto& f = sd.fields()[i];
            fields_info.push_back({
                f.name,
                typeSizeBytes(f.type),
                typeAlignment(f.type),
                i
            });
        }
        std::sort(fields_info.begin(), fields_info.end(),
            [](const FieldInfo& a, const FieldInfo& b) {
                // Sort by alignment descending, then size descending
                if (a.alignment != b.alignment) return a.alignment > b.alignment;
                return a.size > b.size;
            });

        transform.reorder_map.resize(sd.fields().size());
        for (int new_idx = 0; new_idx < static_cast<int>(fields_info.size()); ++new_idx) {
            transform.reorder_map[fields_info[new_idx].original_index] = new_idx;
        }

        // Build description
        transform.detail = "Cache-line waste: " + std::to_string(waste) +
            " bytes (" + std::to_string(waste * 100 / CACHE_LINE_SIZE) +
            "% of 64B line). Consider reordering fields to reduce padding.";

        // Classify hot/cold: fields with pointer-like types are often hot
        for (const auto& fi : fields_info) {
            bool is_hot = false;
            for (const auto& field : sd.fields()) {
                if (field.name == fi.name && field.type) {
                    is_hot = field.type->isPointerLike() || field.type->isNumeric();
                    break;
                }
            }
            if (is_hot) {
                transform.hot_fields.push_back(fi.name);
            } else {
                transform.cold_fields.push_back(fi.name);
            }
        }

        // If there are clear hot/cold splits and enough waste, suggest SoA transform
        if (!transform.hot_fields.empty() && !transform.cold_fields.empty() &&
            waste > CACHE_LINE_SIZE / 2) {
            transform.kind = TransformKind::SoATransform;
            transform.detail += " SoA transform recommended.";
            smeta.layout = LayoutKind::SoA;
        }

        smeta.transform = std::move(transform);
    }

    meta.registerStruct(sd.name(), std::move(smeta));

    // Also attach metadata to the StructDecl node itself
    NodeMeta& nm = meta.getOrCreate(&sd);
    nm.alignment = struct_alignment;
    nm.layout    = sd.isSoA() ? LayoutKind::SoA : LayoutKind::AoS;
}

// ============================================================================
// computeCacheLineWaste
// ============================================================================

uint64_t MemoryTopologyAnalyzer::computeCacheLineWaste(
    StructDecl& sd, TypeTable& type_table) const
{
    // Compute the struct's total size with proper alignment padding
    uint64_t struct_size = 0;
    uint64_t max_align = 1;

    for (const auto& field : sd.fields()) {
        uint64_t field_size  = typeSizeBytes(field.type);
        uint64_t field_align = typeAlignment(field.type);
        if (field_size == 0) field_size = 1; // minimum size for unknown types

        // Align current offset to field alignment
        struct_size = alignUp(struct_size, field_align);
        struct_size += field_size;
        max_align = std::max(max_align, field_align);
    }

    // Pad struct to its own alignment
    struct_size = alignUp(struct_size, max_align);

    if (struct_size == 0) return 0;
    if (struct_size >= CACHE_LINE_SIZE) {
        // Struct is at least one cache line; waste is the trailing padding
        // in the last cache line
        uint64_t remainder = struct_size % CACHE_LINE_SIZE;
        return (remainder == 0) ? 0 : (CACHE_LINE_SIZE - remainder);
    }

    // How many complete structs fit in one cache line
    uint64_t count_per_line = CACHE_LINE_SIZE / struct_size;
    uint64_t used = count_per_line * struct_size;
    return CACHE_LINE_SIZE - used;
}

} // namespace tether
