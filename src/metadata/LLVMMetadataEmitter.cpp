#include "metadata/LLVMMetadataEmitter.h"
#include "parser/Parser.h"

#include <sstream>
#include <functional>
#include <vector>
#include <string>

namespace tether {

// ============================================================================
// Anonymous namespace: AST walking helpers
// ============================================================================
namespace {

/// Recursively walk all statements in a block and its sub-blocks,
/// invoking the callback for each statement encountered.
void walkAllStmts(BlockStmt& block, std::function<void(Stmt&)> callback) {
    for (auto& stmt : block.stmts()) {
        if (!stmt) continue;
        callback(*stmt);

        switch (stmt->getKind()) {
            case NodeKind::IfStmt: {
                auto& is = cast<IfStmt>(*stmt);
                if (is.thenBlock()) walkAllStmts(*is.thenBlock(), callback);
                if (is.hasElse() && is.elseBlock()) walkAllStmts(*is.elseBlock(), callback);
                break;
            }
            case NodeKind::WhileStmt: {
                auto& ws = cast<WhileStmt>(*stmt);
                if (ws.body()) walkAllStmts(*ws.body(), callback);
                break;
            }
            case NodeKind::BlockStmt: {
                auto& bs = cast<BlockStmt>(*stmt);
                walkAllStmts(bs, callback);
                break;
            }
            case NodeKind::VarDeclStmt: {
                auto& vd = cast<VarDeclStmt>(*stmt);
                // nothing further to walk in VarDeclStmt
                (void)vd;
                break;
            }
            case NodeKind::ValDeclStmt: {
                auto& vl = cast<ValDeclStmt>(*stmt);
                (void)vl;
                break;
            }
            case NodeKind::DeferStmt: {
                auto& ds = cast<DeferStmt>(*stmt);
                if (ds.stmt()) callback(*ds.stmt());
                break;
            }
            case NodeKind::ErrdeferStmt: {
                auto& es = cast<ErrdeferStmt>(*stmt);
                if (es.stmt()) callback(*es.stmt());
                break;
            }
            case NodeKind::SwitchStmt: {
                auto& ss = cast<SwitchStmt>(*stmt);
                for (auto& arm : ss.arms()) {
                    if (arm.body) walkAllStmts(*arm.body, callback);
                }
                break;
            }
            case NodeKind::AtomicStmt: {
                auto& as = cast<AtomicStmt>(*stmt);
                if (as.inner()) callback(*as.inner());
                break;
            }
            default:
                break;
        }
    }
}

/// Check if a TypeId refers to a reference type (shared borrow or mutable borrow).
bool isReferenceType(TypeId type) {
    if (type.isNull()) return false;
    auto kind = type->getKind();
    return kind == TypeKind::Reference || kind == TypeKind::MutReference;
}

/// Check if a TypeId refers to a pointer-like type that should be nonnull.
bool isNonNullType(TypeId type) {
    if (type.isNull()) return false;
    // Reference types (&T, &mut T) are always nonnull
    if (isReferenceType(type)) return true;
    // Smart pointers are nonnull (Box, Rc, Arc)
    if (type->getKind() == TypeKind::SmartPointer) return true;
    return false;
}

/// Join a vector of strings with a separator.
std::string joinStrings(const std::vector<std::string>& parts, const std::string& sep) {
    std::string result;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += sep;
        result += parts[i];
    }
    return result;
}

} // anonymous namespace

// ============================================================================
// L5: LLVMMetadataEmitter — main entry point
// ============================================================================

void LLVMMetadataEmitter::emit(Program& program, TypeTable& type_table, MetadataMap& meta) {
    // Reset state for a fresh emission pass
    metadata_ss_.str("");
    metadata_ss_.clear();
    meta_id_counter_ = 0;
    tbaa_type_ids_.clear();
    tbaa_root_id_ = 0;

    // Emit the metadata block components in order
    emitTBAAMetadata(type_table, meta);
    emitBranchWeights(program, meta);
    emitFunctionAttributes(program, meta);
    emitLoopMetadata(program, meta);
    emitPrefetchDeclarations(meta);
}

// ============================================================================
// fnAttributes — produce LLVM function attribute string
// ============================================================================

std::string LLVMMetadataEmitter::fnAttributes(FnDecl& fn, const MetadataMap& meta) const {
    std::vector<std::string> attrs;
    auto* nm = meta.get(&fn);

    // If is_inline -> "alwaysinline"
    bool is_inline = fn.isInline() || (nm && nm->is_inline);
    if (is_inline) attrs.push_back("alwaysinline");

    // If is_noalloc -> "nounwind" (noalloc: no heap allocation;
    // nounwind is a conservative attribute hint — the function won't
    // unwind exceptions, which is consistent with no-alloc guarantees
    // in HFT contexts where exceptions are forbidden)
    bool is_noalloc = fn.isNoalloc() || (nm && nm->is_noalloc);
    if (is_noalloc) attrs.push_back("nounwind");

    // If purity == Pure -> "readonly"
    bool is_pure = fn.isPure() || (nm && nm->purity == FunctionPurity::Pure);
    if (is_pure) attrs.push_back("readonly");

    // If llvm_meta.hot_path -> "hot"
    if (nm && nm->llvm_meta.hot_path) attrs.push_back("hot");

    // If llvm_meta.cold_path -> "cold"
    if (nm && nm->llvm_meta.cold_path) attrs.push_back("cold");

    // Note: restrict params get noalias on the parameters themselves,
    // not on the function. We note it here for documentation but don't
    // emit a function-level attribute.

    return joinStrings(attrs, " ");
}

// ============================================================================
// paramAttributes — produce LLVM parameter attribute string
// ============================================================================

std::string LLVMMetadataEmitter::paramAttributes(FnParam& param, const MetadataMap& meta) const {
    std::vector<std::string> attrs;
    auto* nm = meta.get(&param);

    // If is_restrict -> "noalias"
    bool is_restrict = param.is_restrict || (nm && nm->is_restrict);
    if (is_restrict) attrs.push_back("noalias");

    // If the type is a reference type -> "nonnull"
    if (isNonNullType(param.type)) attrs.push_back("nonnull");

    // If llvm_meta.align > 0 -> "align N"
    if (nm && nm->llvm_meta.align > 0) {
        attrs.push_back("align " + std::to_string(nm->llvm_meta.align));
    } else if (nm && nm->alignment > 0) {
        attrs.push_back("align " + std::to_string(nm->alignment));
    }

    return joinStrings(attrs, " ");
}

// ============================================================================
// memoryMetadata — produce LLVM metadata string for load/store instructions
// ============================================================================

std::string LLVMMetadataEmitter::memoryMetadata(Expr& expr, const MetadataMap& meta) const {
    auto* nm = meta.get(&expr);
    if (!nm) return "";

    std::vector<std::string> parts;

    // If noalias -> !noalias !N
    if (nm->llvm_meta.noalias || nm->aliasing == AliasingKind::NoAlias) {
        auto it = tbaa_type_ids_.find("__noalias_scope");
        if (it != tbaa_type_ids_.end()) {
            parts.push_back("!noalias !" + std::to_string(it->second));
        }
    }

    // If tbaa_type is set -> !tbaa !N
    if (!nm->llvm_meta.tbaa_type.empty()) {
        // Try the struct-level TBAA first
        auto it = tbaa_type_ids_.find(nm->llvm_meta.tbaa_type);
        if (it != tbaa_type_ids_.end()) {
            parts.push_back("!tbaa !" + std::to_string(it->second));
        } else {
            // Try field-level TBAA
            it = tbaa_type_ids_.find("field." + nm->llvm_meta.tbaa_type);
            if (it != tbaa_type_ids_.end()) {
                parts.push_back("!tbaa !" + std::to_string(it->second));
            }
        }
    }

    // If align -> !align N
    if (nm->llvm_meta.align > 0) {
        parts.push_back("!align " + std::to_string(nm->llvm_meta.align));
    }

    return joinStrings(parts, ", ");
}

// ============================================================================
// branchWeightMetadata — produce LLVM !prof metadata for branch weights
// ============================================================================

std::string LLVMMetadataEmitter::branchWeightMetadata(IfStmt& is, const MetadataMap& meta) const {
    auto* nm = meta.get(&is);
    if (!nm) return "";

    switch (nm->branch_prob) {
        case BranchProbability::Likely:
            return "!prof !{!\"branch_weights\", i32 80, i32 20}";
        case BranchProbability::Unlikely:
            return "!prof !{!\"branch_weights\", i32 20, i32 80}";
        case BranchProbability::Even:
            return "!prof !{!\"branch_weights\", i32 50, i32 50}";
        case BranchProbability::Unknown:
        default:
            return "";
    }
}

// ============================================================================
// loopMetadata — produce LLVM loop metadata string
// ============================================================================

std::string LLVMMetadataEmitter::loopMetadata(WhileStmt& ws, const MetadataMap& meta) const {
    auto* nm = meta.get(&ws);
    if (!nm) return "";

    std::vector<std::string> parts;

    // If vectorization_safe -> emit vectorization hint
    if (nm->llvm_meta.vectorization_safe) {
        parts.push_back("!llvm.loop.vectorize.enable, i1 true");
    }

    // If prefetch_distance > 0 -> emit Tether-specific annotation
    // (LLVM doesn't have standard prefetch loop metadata, so we emit
    // a Tether-specific annotation that the prefetch inserter can use)
    if (nm->llvm_meta.prefetch_distance > 0) {
        parts.push_back("!\"tether.prefetch_distance\", i32 " +
                        std::to_string(static_cast<int32_t>(nm->llvm_meta.prefetch_distance)));
    }

    // Combine with unrolling hints from profile data
    if (nm->profile.has_profile && nm->profile.loop_iteration_count > 0) {
        // Suggest unroll count based on average iteration count.
        // Clamp to a reasonable maximum to avoid code bloat.
        uint64_t unroll = nm->profile.loop_iteration_count;
        if (unroll > 8) unroll = 8;
        if (unroll >= 2) {
            parts.push_back("!llvm.loop.unroll.count, i32 " + std::to_string(unroll));
        }
    }
    // Also check access patterns for vectorization hints from L3
    if (!nm->access_patterns.empty()) {
        for (const auto& ap : nm->access_patterns) {
            if (ap.vectorizable && !nm->llvm_meta.vectorization_safe) {
                // If L3 says vectorizable but L5 hasn't set it, add it here
                parts.push_back("!llvm.loop.vectorize.enable, i1 true");
                break;
            }
        }
    }

    if (parts.empty()) return "";

    // Construct the combined loop metadata
    std::string result = "!llvm.loop !{";
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) result += ", ";
        result += parts[i];
    }
    result += "}";
    return result;
}

// ============================================================================
// emitTBAAMetadata — generate TBAA type descriptors
// ============================================================================

void LLVMMetadataEmitter::emitTBAAMetadata(TypeTable& type_table, MetadataMap& meta) {
    // Create the root TBAA node
    tbaa_root_id_ = nextMetaId();
    metadata_ss_ << "!" << tbaa_root_id_ << " = !{!\"Tether TBAA\"}\n";

    // Create alias scope domain and scope for noalias metadata
    int domain_id = nextMetaId();
    metadata_ss_ << "!" << domain_id << " = !{!\"tether-alias-domain\"}\n";

    int scope_id = nextMetaId();
    metadata_ss_ << "!" << scope_id << " = !{!\"tether-noalias-scope\", !"
                 << domain_id << "}\n";
    tbaa_type_ids_["__noalias_scope"] = scope_id;
    tbaa_type_ids_["__noalias_domain"] = domain_id;

    // For each struct registered in the metadata map, create TBAA nodes
    for (const auto& [name, sm] : meta.structs()) {
        // Create struct-level TBAA node
        int struct_id = nextMetaId();
        metadata_ss_ << "!" << struct_id << " = !{!\"struct." << name
                     << "\", !" << tbaa_root_id_ << ", 0}\n";
        tbaa_type_ids_["struct." + name] = struct_id;

        // Create per-field TBAA nodes with computed offsets
        // Offsets are computed based on typical field sizes:
        // we use 8-byte alignment per field as a conservative default.
        // For precise offsets, the code generator should compute from the
        // actual struct layout.
        uint64_t offset = 0;
        for (size_t i = 0; i < sm.field_names.size(); ++i) {
            int field_id = nextMetaId();
            metadata_ss_ << "!" << field_id << " = !{!\"field."
                         << sm.field_names[i] << "\", !" << struct_id
                         << ", " << offset << "}\n";
            tbaa_type_ids_["field." + name + "." + sm.field_names[i]] = field_id;

            // Advance offset: default to 8 bytes per field (covers u64/f64/ptr)
            // This is a conservative estimate; the real code generator would
            // use the actual type sizes from the StructType.
            offset += 8;
        }
    }
}

// ============================================================================
// emitBranchWeights — walk all if statements and emit !prof metadata nodes
// ============================================================================

void LLVMMetadataEmitter::emitBranchWeights(Program& program, MetadataMap& meta) {
    for (auto& toplevel : program) {
        auto* fn = dyn_cast<FnDecl>(toplevel.get());
        if (!fn || !fn->body()) continue;

        walkAllStmts(*fn->body(), [&](Stmt& stmt) {
            if (stmt.getKind() != NodeKind::IfStmt) return;
            auto& is = cast<IfStmt>(stmt);
            auto* nm = meta.get(&is);
            if (!nm || nm->branch_prob == BranchProbability::Unknown) return;

            int prof_id = nextMetaId();
            switch (nm->branch_prob) {
                case BranchProbability::Likely:
                    metadata_ss_ << "!" << prof_id
                                 << " = !{!\"branch_weights\", i32 80, i32 20}\n";
                    break;
                case BranchProbability::Unlikely:
                    metadata_ss_ << "!" << prof_id
                                 << " = !{!\"branch_weights\", i32 20, i32 80}\n";
                    break;
                case BranchProbability::Even:
                    metadata_ss_ << "!" << prof_id
                                 << " = !{!\"branch_weights\", i32 50, i32 50}\n";
                    break;
                default:
                    break;
            }
        });
    }
}

// ============================================================================
// emitFunctionAttributes — walk all FnDecls and emit hot/cold metadata
// ============================================================================

void LLVMMetadataEmitter::emitFunctionAttributes(Program& program, MetadataMap& meta) {
    for (auto& toplevel : program) {
        auto* fn = dyn_cast<FnDecl>(toplevel.get());
        if (!fn) continue;

        auto* nm = meta.get(fn);
        if (!nm) continue;

        // Hot functions get function entry count metadata
        if (nm->llvm_meta.hot_path) {
            int hot_id = nextMetaId();
            metadata_ss_ << "!" << hot_id
                         << " = !{!\"function_entry_count\", i64 "
                         << nm->profile.entry_count << "}\n";
        }

        // Cold functions get a cold attribute metadata node
        if (nm->llvm_meta.cold_path) {
            int cold_id = nextMetaId();
            metadata_ss_ << "!" << cold_id << " = !{!\"cold\"}\n";
        }
    }
}

// ============================================================================
// emitLoopMetadata — walk all WhileStmts and emit loop optimization metadata
// ============================================================================

void LLVMMetadataEmitter::emitLoopMetadata(Program& program, MetadataMap& meta) {
    bool has_prefetch = false;

    for (auto& toplevel : program) {
        auto* fn = dyn_cast<FnDecl>(toplevel.get());
        if (!fn || !fn->body()) continue;

        walkAllStmts(*fn->body(), [&](Stmt& stmt) {
            if (stmt.getKind() != NodeKind::WhileStmt) return;
            auto& ws = cast<WhileStmt>(stmt);
            auto* nm = meta.get(&ws);
            if (!nm) return;

            // Track if any loop needs prefetch
            if (nm->llvm_meta.prefetch_distance > 0) {
                has_prefetch = true;
            }

            std::vector<int> entry_ids;

            // Vectorizable loops get !llvm.loop.vectorize.enable
            if (nm->llvm_meta.vectorization_safe) {
                int vec_id = nextMetaId();
                metadata_ss_ << "!" << vec_id
                             << " = !{!\"llvm.loop.vectorize.enable\", i1 true}\n";
                entry_ids.push_back(vec_id);
            }

            // Loops with known trip counts from profile get !llvm.loop.unroll.count
            if (nm->profile.has_profile && nm->profile.loop_iteration_count > 0) {
                uint64_t unroll = nm->profile.loop_iteration_count;
                if (unroll > 8) unroll = 8;
                if (unroll >= 2) {
                    int unroll_id = nextMetaId();
                    metadata_ss_ << "!" << unroll_id
                                 << " = !{!\"llvm.loop.unroll.count\", i32 "
                                 << unroll << "}\n";
                    entry_ids.push_back(unroll_id);
                }
            }

            // Loops with prefetch distance get Tether-specific annotation
            if (nm->llvm_meta.prefetch_distance > 0) {
                int prefetch_id = nextMetaId();
                metadata_ss_ << "!" << prefetch_id
                             << " = !{!\"tether.prefetch_distance\", i32 "
                             << static_cast<int32_t>(nm->llvm_meta.prefetch_distance)
                             << "}\n";
                entry_ids.push_back(prefetch_id);
            }

            // Loops with linear/contiguous access from L3 analysis
            if (!nm->access_patterns.empty()) {
                bool any_contiguous = false;
                for (const auto& ap : nm->access_patterns) {
                    if (ap.contiguous) {
                        any_contiguous = true;
                        break;
                    }
                }
                if (any_contiguous && !nm->llvm_meta.vectorization_safe) {
                    int vec_id = nextMetaId();
                    metadata_ss_ << "!" << vec_id
                                 << " = !{!\"llvm.loop.vectorize.enable\", i1 true}\n";
                    entry_ids.push_back(vec_id);
                }
            }

            // If we have any loop metadata entries, create the combined
            // loop metadata node
            if (!entry_ids.empty()) {
                int loop_id = nextMetaId();
                metadata_ss_ << "!" << loop_id << " = distinct !{";
                for (size_t i = 0; i < entry_ids.size(); ++i) {
                    if (i > 0) metadata_ss_ << ", ";
                    metadata_ss_ << "!" << entry_ids[i];
                }
                metadata_ss_ << "}\n";
            }
        });
    }

    // Store the prefetch flag for emitPrefetchDeclarations
    if (has_prefetch) {
        tbaa_type_ids_["__has_prefetch"] = 1;
    }
}

// ============================================================================
// emitPrefetchDeclarations — emit llvm.prefetch intrinsic if needed
// ============================================================================

void LLVMMetadataEmitter::emitPrefetchDeclarations(MetadataMap& meta) {
    auto it = tbaa_type_ids_.find("__has_prefetch");
    if (it != tbaa_type_ids_.end() && it->second > 0) {
        metadata_ss_ << "declare void @llvm.prefetch(ptr, i32, i32, i32)\n";
    }
}

} // namespace tether
