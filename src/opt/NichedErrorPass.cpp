#include "opt/NichedErrorPass.h"

#include "ast/AST.h"
#include "sema/Type.h"

namespace tether {

// ============================================================================
// checkNiche — detailed niche analysis for a success type
//
// Returns a NicheInfo struct describing whether the success type has unused
// bit patterns that can encode the error discriminant, what bit pattern to
// use, and the resulting niched size.
// ============================================================================
NichedErrorPass::NicheInfo NichedErrorPass::checkNiche(TypeId type) const {
    NicheInfo info;
    if (!type) return info;

    // Pointer types: null pointer is the niche
    if (isa<PointerType>(type)) {
        info.has_niche = true;
        info.niche_bit_pattern = 0;  // null pointer = error
        info.original_size = 8;      // pointer is 8 bytes
        info.niched_size = 8;        // no extra byte needed
        info.description = "null pointer niche";
        return info;
    }

    // Reference types: references are non-null, so null is a niche
    if (isa<ReferenceType>(type) || isa<MutReferenceType>(type)) {
        info.has_niche = true;
        info.niche_bit_pattern = 0;
        info.original_size = 8;
        info.niched_size = 8;
        info.description = "null reference niche";
        return info;
    }

    // Rc/Arc smart pointers: these are aggregate structs (no pointer niche)
    if (isa<SmartPointerType>(type)) {
        auto& sp = cast<SmartPointerType>(type);
        if (sp.smartPointerKind() == SmartPointerKind::Box) {
            info.has_niche = true;
            info.niche_bit_pattern = 0;  // null pointer = error
            info.original_size = 8;
            info.niched_size = 8;
            info.description = "null Box pointer niche";
            return info;
        }
        // Rc/Arc are aggregate structs — no niche available
        return info;
    }

    // Bool: i8 has 254 unused bit patterns (only 0 and 1 are valid)
    if (type->isBool()) {
        info.has_niche = true;
        info.niche_bit_pattern = 2;  // 0b00000010 = error
        info.original_size = 1;
        info.niched_size = 1;
        info.description = "bool niche (bit pattern 2)";
        return info;
    }

    // Small integer types: use wider integer with high bit as error flag
    if (isa<PrimitiveType>(type)) {
        auto& prim = cast<PrimitiveType>(type);
        if (prim.isInteger() && prim.bitWidth() <= 32) {
            info.has_niche = true;
            info.niche_bit_pattern = 1ULL << 63;  // high bit of i64 = error
            info.original_size = prim.bitWidth() / 8;
            info.niched_size = 8;  // i64
            info.description = "integer niche (high bit of i64)";
            return info;
        }
    }

    // AlignedType delegates to its inner type
    if (isa<AlignedType>(type)) {
        auto& at = cast<AlignedType>(type);
        return checkNiche(at.inner());
    }

    // Option types: if the inner type has a niche, the outer Option can use it too
    // (Future: handle Option<T> when added to the type system)

    return info;
}

// ============================================================================
// classifyNiche — determine which niche strategy applies to a success type
// ============================================================================
NodeMeta::NichedErrorKind NichedErrorPass::classifyNiche(TypeId success_type) const {
    if (!success_type) return NodeMeta::NichedErrorKind::StructFallback;

    // Pointer-like types: pointer, reference, mutable reference, smart pointers
    // These are always aligned, so the lowest bit is always 0 in valid values.
    // We can use the lowest bit set as an error sentinel.
    if (isa<PointerType>(success_type) ||
        isa<ReferenceType>(success_type) ||
        isa<MutReferenceType>(success_type)) {
        return NodeMeta::NichedErrorKind::PointerNiche;
    }

    // Smart pointers: Box<T> is just a ptr, Rc<T>/Arc<T> are aggregate structs
    if (isa<SmartPointerType>(success_type)) {
        auto& sp = cast<SmartPointerType>(success_type);
        if (sp.smartPointerKind() == SmartPointerKind::Box) {
            return NodeMeta::NichedErrorKind::PointerNiche;
        }
        // Rc and Arc are aggregate structs — can't use pointer niche
        return NodeMeta::NichedErrorKind::StructFallback;
    }

    // Small integer types: use wider integer with high bit as error flag
    // Only applies to integer types with bitWidth <= 32 (fit in i64 with room for flag)
    if (isa<PrimitiveType>(success_type)) {
        auto& prim = cast<PrimitiveType>(success_type);
        if (prim.isInteger() && prim.bitWidth() <= 32) {
            return NodeMeta::NichedErrorKind::IntegerNiche;
        }
    }

    // Bool can use integer niche (i8 with high bit as flag)
    if (success_type->isBool()) {
        return NodeMeta::NichedErrorKind::IntegerNiche;
    }

    // AlignedType delegates to its inner type
    if (isa<AlignedType>(success_type)) {
        auto& at = cast<AlignedType>(success_type);
        return classifyNiche(at.inner());
    }

    // All other types (struct, slice, enum with large variants, etc.) fall back
    return NodeMeta::NichedErrorKind::StructFallback;
}

// ============================================================================
// annotateErrorType — write niche metadata for a given ErrorType
// ============================================================================
void NichedErrorPass::annotateErrorType(TypeId error_type) {
    if (!error_type || !isa<ErrorType>(error_type)) return;

    auto& err = cast<ErrorType>(error_type);
    TypeId succ = err.successType();
    auto kind = classifyNiche(succ);

    // Perform detailed niche analysis via checkNiche
    NicheInfo niche = checkNiche(succ);

    // Write to metadata map keyed by the TypeId's raw pointer
    // (types are interned, so the same TypeId always has the same pointer)
    if (meta_map_) {
        auto& nm = meta_map_->getOrCreate(succ.raw());
        nm.niched_error = (kind != NodeMeta::NichedErrorKind::StructFallback);
        nm.niched_error_kind = kind;

        // Store detailed niche info for the IRGenerator
        nm.has_niche = niche.has_niche;
        nm.niche_bit_pattern = niche.niche_bit_pattern;
        nm.niched_size = niche.niched_size;
        nm.niche_description = niche.description;
    }

    switch (kind) {
        case NodeMeta::NichedErrorKind::PointerNiche:  pointer_niches_++;  break;
        case NodeMeta::NichedErrorKind::IntegerNiche:   integer_niches_++;  break;
        case NodeMeta::NichedErrorKind::StructFallback: struct_fallbacks_++; break;
        default: break;
    }
}

// ============================================================================
// AST walking — find all ErrorType usages
// ============================================================================
void NichedErrorPass::walkExpr(Expr* expr) {
    if (!expr) return;

    // If this expression has an ErrorType, annotate it
    TypeId t = expr->getType();
    if (t && isa<ErrorType>(t)) {
        annotateErrorType(t);
    }

    // Recurse into sub-expressions
    switch (expr->getKind()) {
        case NodeKind::BinaryExpr: {
            auto& bin = cast<BinaryExpr>(*expr);
            walkExpr(bin.left());
            walkExpr(bin.right());
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& un = cast<UnaryExpr>(*expr);
            walkExpr(un.operand());
            break;
        }
        case NodeKind::CallExpr: {
            auto& call = cast<CallExpr>(*expr);
            walkExpr(call.callee());
            for (const auto& arg : call.args())
                walkExpr(arg.get());
            break;
        }
        case NodeKind::MemberExpr: {
            auto& mem = cast<MemberExpr>(*expr);
            walkExpr(mem.object());
            break;
        }
        case NodeKind::IndexExpr: {
            auto& idx = cast<IndexExpr>(*expr);
            walkExpr(idx.object());
            walkExpr(idx.index());
            break;
        }
        case NodeKind::DerefExpr: {
            auto& d = cast<DerefExpr>(*expr);
            walkExpr(d.operand());
            break;
        }
        case NodeKind::AddrOfExpr: {
            auto& a = cast<AddrOfExpr>(*expr);
            walkExpr(a.operand());
            break;
        }
        case NodeKind::CastExpr: {
            auto& c = cast<CastExpr>(*expr);
            walkExpr(c.expr());
            break;
        }
        case NodeKind::TryExpr: {
            auto& t = cast<TryExpr>(*expr);
            walkExpr(t.operand());
            break;
        }
        case NodeKind::ComptimeExpr: {
            auto& ce = cast<ComptimeExpr>(*expr);
            walkExpr(ce.inner());
            break;
        }
        case NodeKind::ReduceExpr: {
            auto& re = cast<ReduceExpr>(*expr);
            walkExpr(re.iterable());
            if (re.hasAxis()) walkExpr(re.axis());
            break;
        }
        default: break;
    }
}

void NichedErrorPass::walkStmt(Stmt* stmt) {
    if (!stmt) return;
    switch (stmt->getKind()) {
        case NodeKind::VarDeclStmt: {
            auto& vd = cast<VarDeclStmt>(*stmt);
            // Check if the declared type is an ErrorType
            if (vd.hasType()) annotateErrorType(vd.declaredType());
            if (vd.hasInit()) walkExpr(vd.init());
            break;
        }
        case NodeKind::ValDeclStmt: {
            auto& vd = cast<ValDeclStmt>(*stmt);
            if (vd.hasType()) annotateErrorType(vd.declaredType());
            if (vd.hasInit()) walkExpr(vd.init());
            break;
        }
        case NodeKind::AssignStmt: {
            auto& as = cast<AssignStmt>(*stmt);
            walkExpr(as.target());
            walkExpr(as.value());
            break;
        }
        case NodeKind::BlockStmt: {
            auto& block = cast<BlockStmt>(*stmt);
            for (const auto& s : block.stmts())
                walkStmt(s.get());
            break;
        }
        case NodeKind::IfStmt: {
            auto& is = cast<IfStmt>(*stmt);
            walkExpr(is.condition());
            walkBlock(is.thenBlock());
            if (is.hasElse()) walkBlock(is.elseBlock());
            break;
        }
        case NodeKind::WhileStmt: {
            auto& ws = cast<WhileStmt>(*stmt);
            walkExpr(ws.condition());
            walkBlock(ws.body());
            break;
        }
        case NodeKind::ReturnStmt: {
            auto& rs = cast<ReturnStmt>(*stmt);
            if (rs.hasValue()) walkExpr(rs.value());
            break;
        }
        case NodeKind::ExprStmt: {
            auto& es = cast<ExprStmt>(*stmt);
            walkExpr(es.expr());
            break;
        }
        case NodeKind::DeferStmt: {
            auto& ds = cast<DeferStmt>(*stmt);
            walkStmt(ds.stmt());
            break;
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = static_cast<ErrdeferStmt&>(*stmt);
            walkStmt(es.stmt());
            break;
        }
        case NodeKind::MatchStmt: {
            auto& ms = cast<MatchStmt>(*stmt);
            walkExpr(ms.subject());
            for (const auto& arm : ms.arms()) {
                if (arm.pattern) walkExpr(arm.pattern.get());
                if (arm.body) walkBlock(arm.body.get());
            }
            break;
        }
        default: break;
    }
}

void NichedErrorPass::walkBlock(BlockStmt* block) {
    if (!block) return;
    for (const auto& s : block->stmts())
        walkStmt(s.get());
}

void NichedErrorPass::walkFn(FnDecl& fn) {
    // Annotate the function's error type if it can error
    if (fn.canError()) {
        annotateErrorType(fn.errorType());
    }
    // Annotate parameter types
    for (const auto& p : fn.params()) {
        annotateErrorType(p.type);
    }
    // Walk the body
    if (fn.body()) walkBlock(fn.body());
}

// ============================================================================
// run — execute the pass
// ============================================================================
bool NichedErrorPass::run(Program& program, TypeTable& type_table) {
    pointer_niches_ = 0;
    integer_niches_ = 0;
    struct_fallbacks_ = 0;

    for (auto& top_level : program) {
        if (top_level->getKind() == NodeKind::FnDecl) {
            auto& fn = cast<FnDecl>(*top_level);
            walkFn(fn);
        }
    }

    // Return true if we found any nicheable error types
    return (pointer_niches_ + integer_niches_) > 0;
}

} // namespace tether
