#include "metadata/SemanticCollector.h"

namespace tether {

// ============================================================================
// L1: Semantic Collector — walks the AST and attaches semantic metadata
//
// This is the "understand the program" layer. It classifies ownership,
// aliasing, purity, layout, and related properties that later layers
// depend on.
// ============================================================================

// ============================================================================
// Top-level entry point
// ============================================================================

void SemanticCollector::collect(Program& program, TypeTable& type_table, MetadataMap& meta) {
    // Phase 1: Collect metadata from top-level declarations
    for (auto& decl : program) {
        if (!decl) continue;

        switch (decl->getKind()) {
            case NodeKind::FnDecl:
                collectFnDecl(static_cast<FnDecl&>(*decl), meta);
                break;
            case NodeKind::StructDecl:
                collectStructDecl(static_cast<StructDecl&>(*decl), type_table, meta);
                break;
            case NodeKind::EnumDecl:
                collectEnumDecl(static_cast<EnumDecl&>(*decl), meta);
                break;
            default:
                // ImportDecl, TraitDecl, ImplDecl — no L1 metadata to collect
                break;
        }
    }

    // Phase 2: Walk function bodies for statement-level metadata
    for (auto& decl : program) {
        if (!decl) continue;
        if (decl->getKind() == NodeKind::FnDecl) {
            auto& fn = static_cast<FnDecl&>(*decl);
            if (fn.body()) {
                collectBlockStmt(*fn.body(), meta);
            }
        }
    }
}

// ============================================================================
// Function declaration collection
// ============================================================================

void SemanticCollector::collectFnDecl(FnDecl& fn, MetadataMap& meta) {
    NodeMeta& nm = meta.getOrCreate(&fn);

    // Classify function purity
    if (fn.isNoalloc()) {
        nm.purity = FunctionPurity::NoAlloc;
    } else if (fn.isPure()) {
        nm.purity = FunctionPurity::Pure;
    } else {
        nm.purity = FunctionPurity::Impure;
    }

    nm.is_inline = fn.isInline();
    nm.is_noalloc = fn.isNoalloc();

    // Classify each parameter's aliasing and LLVM attributes
    for (const auto& param : fn.params()) {
        if (param.type.isNull()) continue;

        NodeMeta& pm = meta.getOrCreate(&param);

        // Classify aliasing based on parameter type
        if (isa<MutReferenceType>(param.type)) {
            // &mut T → MayAlias, unless restrict → NoAlias
            if (param.is_restrict) {
                pm.aliasing = AliasingKind::NoAlias;
                pm.is_restrict = true;
                pm.llvm_meta.noalias = true;
            } else {
                pm.aliasing = AliasingKind::MayAlias;
            }
            // &mut references are nonnull
            pm.llvm_meta.nonnull = true;
        } else if (isa<ReferenceType>(param.type)) {
            // &T → MayAlias
            pm.aliasing = AliasingKind::MayAlias;
            // References are nonnull
            pm.llvm_meta.nonnull = true;
            if (param.is_restrict) {
                pm.is_restrict = true;
                pm.llvm_meta.noalias = true;
            }
        } else if (isa<PointerType>(param.type)) {
            // *T → MayAlias, unless restrict → NoAlias
            if (param.is_restrict) {
                pm.aliasing = AliasingKind::NoAlias;
                pm.is_restrict = true;
                pm.llvm_meta.noalias = true;
            } else {
                pm.aliasing = AliasingKind::MayAlias;
            }
        } else {
            // Owned values — no aliasing
            pm.aliasing = AliasingKind::NoAccess;
        }
    }
}

// ============================================================================
// Struct declaration collection
// ============================================================================

void SemanticCollector::collectStructDecl(StructDecl& sd, TypeTable& type_table, MetadataMap& meta) {
    // Create StructMeta entry
    MetadataMap::StructMeta smeta;
    smeta.name = sd.name();
    smeta.layout = sd.isSoA() ? LayoutKind::SoA : LayoutKind::AoS;
    smeta.alignment = sd.alignment();

    // Collect field names and classify hot/cold
    for (const auto& field : sd.fields()) {
        smeta.field_names.push_back(field.name);

        // Classify: small numeric types are hot, indirect/large types are cold
        bool is_hot = false;
        if (!field.type.isNull()) {
            if (isa<PrimitiveType>(field.type)) {
                // Small numeric primitives (u8..u64, i8..i64, f32, f64, bool) are hot
                is_hot = true;
            } else if (isa<PointerType>(field.type) || isa<ReferenceType>(field.type) ||
                       isa<MutReferenceType>(field.type)) {
                // Pointer/reference fields: treat as hot (they are small and commonly accessed)
                is_hot = true;
            } else if (isa<EnumType>(field.type)) {
                // Enums are small (integer-sized), hot
                is_hot = true;
            } else {
                // Struct, slice, smart pointer, etc. — indirect/large → cold
                is_hot = false;
            }
        } else {
            // Unresolved type — conservatively cold
            is_hot = false;
        }
        smeta.field_is_hot.push_back(is_hot);
    }

    // Check for packed bitfield candidates
    if (hasPackedBitfieldCandidate(sd)) {
        smeta.transform.kind = TransformKind::PackedBitfield;
        smeta.transform.struct_name = sd.name();
        smeta.transform.detail = "Consecutive bool fields can be packed into a bitfield";

        // Identify which bool fields form the bitfield group
        const auto& fields = sd.fields();
        for (int i = 0; i < static_cast<int>(fields.size()); ) {
            if (!fields[i].type.isNull() && isa<PrimitiveType>(fields[i].type) &&
                cast<PrimitiveType>(fields[i].type).isBool()) {
                int count = countConsecutiveBools(fields, i);
                if (count >= 2) {
                    for (int j = i; j < i + count; ++j) {
                        smeta.transform.cold_fields.push_back(fields[j].name);
                    }
                    i += count;
                } else {
                    ++i;
                }
            } else {
                ++i;
            }
        }
    }

    // Register the struct metadata
    meta.registerStruct(sd.name(), std::move(smeta));

    // Annotate the struct node itself
    NodeMeta& nm = meta.getOrCreate(&sd);
    nm.layout = sd.isSoA() ? LayoutKind::SoA : LayoutKind::AoS;
    nm.alignment = sd.alignment();

    // Set LLVM alignment hint
    if (sd.alignment() > 0) {
        nm.llvm_meta.align = sd.alignment();
    }
}

// ============================================================================
// Enum declaration collection
// ============================================================================

void SemanticCollector::collectEnumDecl(EnumDecl& ed, MetadataMap& meta) {
    NodeMeta& nm = meta.getOrCreate(&ed);

    // Enum values have a known range: [0, number_of_variants)
    nm.llvm_meta.has_range = true;
    nm.llvm_meta.range_lo = 0;
    nm.llvm_meta.range_hi = static_cast<uint32_t>(ed.variantCount());
}

// ============================================================================
// Statement collection
// ============================================================================

void SemanticCollector::collectStmt(Stmt& stmt, MetadataMap& meta) {
    switch (stmt.getKind()) {
        case NodeKind::VarDeclStmt:
            collectVarDeclStmt(static_cast<VarDeclStmt&>(stmt), meta);
            break;
        case NodeKind::ValDeclStmt:
            collectValDeclStmt(static_cast<ValDeclStmt&>(stmt), meta);
            break;
        case NodeKind::AssignStmt:
            collectAssignStmt(static_cast<AssignStmt&>(stmt), meta);
            break;
        case NodeKind::IfStmt:
            collectIfStmt(static_cast<IfStmt&>(stmt), meta);
            break;
        case NodeKind::WhileStmt:
            collectWhileStmt(static_cast<WhileStmt&>(stmt), meta);
            break;
        case NodeKind::BlockStmt:
            collectBlockStmt(static_cast<BlockStmt&>(stmt), meta);
            break;
        case NodeKind::DeferStmt: {
            auto& ds = static_cast<DeferStmt&>(stmt);
            if (ds.stmt()) collectStmt(*ds.stmt(), meta);
            break;
        }
        case NodeKind::ErrdeferStmt: {
            auto& es = static_cast<ErrdeferStmt&>(stmt);
            if (es.stmt()) collectStmt(*es.stmt(), meta);
            break;
        }
        case NodeKind::AtomicStmt: {
            auto& as = static_cast<AtomicStmt&>(stmt);
            if (as.inner()) collectStmt(*as.inner(), meta);
            break;
        }
        case NodeKind::ReturnStmt:
        case NodeKind::BreakStmt:
        case NodeKind::ContinueStmt:
        case NodeKind::YieldStmt:
            // No L1 metadata to collect for these
            break;
        case NodeKind::ExprStmt: {
            auto& es = static_cast<ExprStmt&>(stmt);
            // Walk expression for aliasing info
            if (es.expr()) collectExpr(*es.expr(), meta);
            break;
        }
        case NodeKind::MatchStmt: {
            auto& ms = static_cast<MatchStmt&>(stmt);
            if (ms.subject()) collectExpr(*ms.subject(), meta);
            for (const auto& arm : ms.arms()) {
                if (arm.body) collectBlockStmt(*arm.body, meta);
            }
            break;
        }
        case NodeKind::SpawnStmt: {
            auto& sp = static_cast<SpawnStmt&>(stmt);
            if (sp.task()) collectExpr(*sp.task(), meta);
            break;
        }
        default:
            break;
    }
}

void SemanticCollector::collectBlockStmt(BlockStmt& block, MetadataMap& meta) {
    for (auto& stmt : block.stmts()) {
        if (stmt) collectStmt(*stmt, meta);
    }
}

void SemanticCollector::collectVarDeclStmt(VarDeclStmt& vd, MetadataMap& meta) {
    NodeMeta& nm = meta.getOrCreate(&vd);

    // var declarations are mutable and owned
    nm.ownership = OwnershipKind::Owned;

    // If the init expression is a reference, set aliasing accordingly
    if (vd.hasInit() && vd.init()) {
        TypeId init_type = vd.init()->getType();
        if (!init_type.isNull()) {
            nm.aliasing = classifyAliasing(init_type);
        }
    }

    // Also check the declared type for reference types
    if (vd.hasType()) {
        TypeId decl_type = vd.declaredType();
        if (!decl_type.isNull()) {
            if (isa<ReferenceType>(decl_type)) {
                nm.ownership = OwnershipKind::Borrowed;
            } else if (isa<MutReferenceType>(decl_type)) {
                nm.ownership = OwnershipKind::MutBorrowed;
            }
        }
    }
}

void SemanticCollector::collectValDeclStmt(ValDeclStmt& vd, MetadataMap& meta) {
    NodeMeta& nm = meta.getOrCreate(&vd);

    // val declarations are immutable and owned
    nm.ownership = OwnershipKind::Immutable;

    // If the init expression is a reference, set aliasing accordingly
    if (vd.hasInit() && vd.init()) {
        TypeId init_type = vd.init()->getType();
        if (!init_type.isNull()) {
            nm.aliasing = classifyAliasing(init_type);
        }
    }

    // Also check the declared type for reference types
    if (vd.hasType()) {
        TypeId decl_type = vd.declaredType();
        if (!decl_type.isNull()) {
            if (isa<ReferenceType>(decl_type)) {
                nm.ownership = OwnershipKind::Borrowed;
            } else if (isa<MutReferenceType>(decl_type)) {
                nm.ownership = OwnershipKind::MutBorrowed;
            }
        }
    }
}

void SemanticCollector::collectAssignStmt(AssignStmt& as, MetadataMap& meta) {
    // Track write access patterns for the target expression
    Expr* target = as.target();
    if (target) {
        NodeMeta& nm = meta.getOrCreate(target);

        // Determine the access pattern based on the target type
        AccessPattern ap;
        ap.access = AccessKind::Write;

        // If the target is an index expression, we can identify stride info
        if (isa<IndexExpr>(target)) {
            auto& idx = static_cast<IndexExpr&>(*target);
            ap.variable_name = "indexed_write";
            ap.traversal = TraversalKind::Unknown; // L3 refines this
        } else if (isa<IdentExpr>(target)) {
            ap.variable_name = static_cast<IdentExpr&>(*target).name();
            ap.traversal = TraversalKind::Unknown;
        } else if (isa<MemberExpr>(target)) {
            ap.variable_name = "member_write";
            ap.traversal = TraversalKind::Unknown;
        }

        nm.access_patterns.push_back(std::move(ap));

        // Walk expressions for deeper metadata
        collectExpr(*target, meta);
    }

    if (as.value()) {
        collectExpr(*as.value(), meta);
    }
}

void SemanticCollector::collectIfStmt(IfStmt& is, MetadataMap& meta) {
    NodeMeta& nm = meta.getOrCreate(&is);

    Expr* cond = is.condition();

    // Set branch probability based on condition patterns
    if (cond) {
        // Pattern 1: UnaryExpr with Not operator — `!expr`
        // The positive (then) branch is unlikely when the condition is negated
        if (isa<UnaryExpr>(cond)) {
            auto& unary = static_cast<UnaryExpr&>(*cond);
            if (unary.op() == UnaryOp::Not) {
                // `if (!expr) { A } else { B }` — A (then-block) is less likely
                nm.branch_prob = BranchProbability::Unlikely;
                nm.llvm_meta.branch_prob = BranchProbability::Unlikely;
            }
        }
        // Pattern 2: Comparison against 0 or null
        else if (isa<BinaryExpr>(cond)) {
            auto& bin = static_cast<BinaryExpr&>(*cond);
            // Check if comparing against zero/null on the right side
            bool compares_against_zero = false;
            if (bin.right() && isa<IntLiteral>(bin.right())) {
                auto& lit = static_cast<IntLiteral&>(*bin.right());
                if (lit.value() == 0) {
                    compares_against_zero = true;
                }
            }
            // Also check if comparing against a BoolLiteral false
            if (bin.right() && isa<BoolLiteral>(bin.right())) {
                auto& lit = static_cast<BoolLiteral&>(*bin.right());
                if (!lit.value()) {
                    compares_against_zero = true;
                }
            }

            if (compares_against_zero) {
                switch (bin.op()) {
                    case BinaryOp::Eq:
                        // x == 0 means the non-zero branch (else) is likely
                        // (the then-branch runs when x is 0, which is less likely
                        //  for most pointer/flag checks)
                        nm.branch_prob = BranchProbability::Unlikely;
                        nm.llvm_meta.branch_prob = BranchProbability::Unlikely;
                        break;
                    case BinaryOp::Ne:
                        // x != 0 means the then-branch is likely (non-zero)
                        nm.branch_prob = BranchProbability::Likely;
                        nm.llvm_meta.branch_prob = BranchProbability::Likely;
                        break;
                    default:
                        break;
                }
            }
        }
    }

    // Walk the condition and branches
    if (cond) collectExpr(*cond, meta);
    if (is.thenBlock()) collectBlockStmt(*is.thenBlock(), meta);
    if (is.elseBlock()) collectBlockStmt(*is.elseBlock(), meta);
}

void SemanticCollector::collectWhileStmt(WhileStmt& ws, MetadataMap& meta) {
    // L3 does the heavy loop analysis; here we just walk the body
    if (ws.condition()) collectExpr(*ws.condition(), meta);
    if (ws.body()) collectBlockStmt(*ws.body(), meta);
    if (ws.increment()) collectExpr(*ws.increment(), meta);
}

// ============================================================================
// Expression collection (for aliasing info)
// ============================================================================

void SemanticCollector::collectExpr(Expr& expr, MetadataMap& meta) {
    // Classify aliasing for the expression based on its type
    TypeId expr_type = expr.getType();
    if (!expr_type.isNull()) {
        NodeMeta& nm = meta.getOrCreate(&expr);
        nm.aliasing = classifyAliasing(expr_type);
    }

    // Recursively walk sub-expressions
    switch (expr.getKind()) {
        case NodeKind::BinaryExpr: {
            auto& bin = static_cast<BinaryExpr&>(expr);
            if (bin.left()) collectExpr(*bin.left(), meta);
            if (bin.right()) collectExpr(*bin.right(), meta);
            break;
        }
        case NodeKind::UnaryExpr: {
            auto& unary = static_cast<UnaryExpr&>(expr);
            if (unary.operand()) collectExpr(*unary.operand(), meta);
            break;
        }
        case NodeKind::CallExpr: {
            auto& call = static_cast<CallExpr&>(expr);
            if (call.callee()) collectExpr(*call.callee(), meta);
            for (auto& arg : call.args()) {
                if (arg) collectExpr(*arg, meta);
            }
            break;
        }
        case NodeKind::MemberExpr: {
            auto& mem = static_cast<MemberExpr&>(expr);
            if (mem.object()) collectExpr(*mem.object(), meta);
            break;
        }
        case NodeKind::IndexExpr: {
            auto& idx = static_cast<IndexExpr&>(expr);
            if (idx.object()) collectExpr(*idx.object(), meta);
            if (idx.index()) collectExpr(*idx.index(), meta);
            break;
        }
        case NodeKind::DerefExpr: {
            auto& deref = static_cast<DerefExpr&>(expr);
            if (deref.operand()) collectExpr(*deref.operand(), meta);
            break;
        }
        case NodeKind::AddrOfExpr: {
            auto& addr = static_cast<AddrOfExpr&>(expr);
            NodeMeta& nm = meta.getOrCreate(&expr);
            if (addr.isMutable()) {
                nm.ownership = OwnershipKind::MutBorrowed;
            } else {
                nm.ownership = OwnershipKind::Borrowed;
            }
            if (addr.operand()) collectExpr(*addr.operand(), meta);
            break;
        }
        case NodeKind::CastExpr: {
            auto& cast_e = static_cast<CastExpr&>(expr);
            if (cast_e.expr()) collectExpr(*cast_e.expr(), meta);
            break;
        }
        case NodeKind::StructInitExpr: {
            auto& si = static_cast<StructInitExpr&>(expr);
            for (auto& init : si.inits()) {
                if (init.value) collectExpr(*init.value, meta);
            }
            break;
        }
        case NodeKind::ArrayInitExpr: {
            auto& ai = static_cast<ArrayInitExpr&>(expr);
            for (auto& elem : ai.elements()) {
                if (elem) collectExpr(*elem, meta);
            }
            break;
        }
        case NodeKind::UnsafeExpr: {
            auto& u = static_cast<UnsafeExpr&>(expr);
            if (u.inner()) collectExpr(*u.inner(), meta);
            break;
        }
        case NodeKind::TryExpr: {
            auto& t = static_cast<TryExpr&>(expr);
            if (t.operand()) collectExpr(*t.operand(), meta);
            break;
        }
        case NodeKind::ComptimeExpr: {
            auto& ct = static_cast<ComptimeExpr&>(expr);
            if (ct.inner()) collectExpr(*ct.inner(), meta);
            break;
        }
        case NodeKind::ReduceExpr: {
            auto& r = static_cast<ReduceExpr&>(expr);
            if (r.iterable()) collectExpr(*r.iterable(), meta);
            if (r.axis()) collectExpr(*r.axis(), meta);
            break;
        }
        // Leaf expressions — no children to walk
        case NodeKind::IntLiteral:
        case NodeKind::FloatLiteral:
        case NodeKind::BoolLiteral:
        case NodeKind::StringLiteral:
        case NodeKind::IdentExpr:
        case NodeKind::SizeofExpr:
        case NodeKind::PoisonExpr:
            break;
        default:
            break;
    }
}

// ============================================================================
// Aliasing classification
// ============================================================================

AliasingKind SemanticCollector::classifyAliasing(TypeId type) const {
    if (type.isNull()) return AliasingKind::NoAlias;

    // Pointer and reference types may alias
    if (isa<PointerType>(type)) return AliasingKind::MayAlias;
    if (isa<ReferenceType>(type)) return AliasingKind::MayAlias;
    if (isa<MutReferenceType>(type)) return AliasingKind::MayAlias;

    // Owned values don't alias
    return AliasingKind::NoAlias;
}

// ============================================================================
// Packed bitfield candidate detection
// ============================================================================

bool SemanticCollector::hasPackedBitfieldCandidate(StructDecl& sd) const {
    const auto& fields = sd.fields();
    for (int i = 0; i < static_cast<int>(fields.size()); ++i) {
        if (!fields[i].type.isNull() && isa<PrimitiveType>(fields[i].type) &&
            cast<PrimitiveType>(fields[i].type).isBool()) {
            // Found a bool; check if there are 2+ consecutive
            int count = countConsecutiveBools(fields, i);
            if (count >= 2) return true;
            i += count - 1; // skip past the bools we already counted
        }
    }
    return false;
}

int SemanticCollector::countConsecutiveBools(const std::vector<StructFieldDecl>& fields, int start) const {
    int count = 0;
    for (int i = start; i < static_cast<int>(fields.size()); ++i) {
        if (!fields[i].type.isNull() && isa<PrimitiveType>(fields[i].type) &&
            cast<PrimitiveType>(fields[i].type).isBool()) {
            ++count;
        } else {
            break;
        }
    }
    return count;
}

} // namespace tether
