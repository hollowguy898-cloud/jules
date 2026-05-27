#include "metadata/MetaTypes.h"

#include <sstream>
#include <algorithm>

namespace tether {

// ============================================================================
// String conversion helpers
// ============================================================================

const char* toString(OwnershipKind k) {
    switch (k) {
        case OwnershipKind::Owned:       return "owned";
        case OwnershipKind::Immutable:   return "immutable";
        case OwnershipKind::Borrowed:    return "borrowed";
        case OwnershipKind::MutBorrowed: return "mut_borrowed";
        case OwnershipKind::Moved:       return "moved";
    }
    return "unknown";
}

const char* toString(AliasingKind k) {
    switch (k) {
        case AliasingKind::NoAlias:  return "noalias";
        case AliasingKind::MayAlias: return "may_alias";
        case AliasingKind::NoAccess: return "no_access";
    }
    return "unknown";
}

const char* toString(LayoutKind k) {
    switch (k) {
        case LayoutKind::AoS:     return "aos";
        case LayoutKind::SoA:     return "soa";
        case LayoutKind::Hybrid:  return "hybrid";
        case LayoutKind::Chunked: return "chunked";
    }
    return "unknown";
}

const char* toString(FunctionPurity k) {
    switch (k) {
        case FunctionPurity::Impure: return "impure";
        case FunctionPurity::Pure:   return "pure";
        case FunctionPurity::NoAlloc: return "noalloc";
    }
    return "unknown";
}

const char* toString(TraversalKind k) {
    switch (k) {
        case TraversalKind::Unknown: return "unknown";
        case TraversalKind::Linear:  return "linear";
        case TraversalKind::Strided: return "strided";
        case TraversalKind::Random:  return "random";
        case TraversalKind::Sparse:  return "sparse";
        case TraversalKind::Chunked: return "chunked";
    }
    return "unknown";
}

const char* toString(AccessKind k) {
    switch (k) {
        case AccessKind::Read:      return "read";
        case AccessKind::Write:     return "write";
        case AccessKind::ReadWrite: return "readwrite";
    }
    return "unknown";
}

const char* toString(TransformKind k) {
    switch (k) {
        case TransformKind::None:          return "none";
        case TransformKind::SoATransform:  return "soa_transform";
        case TransformKind::HotColdSplit:  return "hot_cold_split";
        case TransformKind::PackedBitfield: return "packed_bitfield";
        case TransformKind::FieldReorder:  return "field_reorder";
    }
    return "unknown";
}

const char* toString(BranchProbability k) {
    switch (k) {
        case BranchProbability::Unknown:  return "unknown";
        case BranchProbability::Likely:   return "likely";
        case BranchProbability::Unlikely: return "unlikely";
        case BranchProbability::Even:     return "even";
    }
    return "unknown";
}

const char* toString(SelectProfitability k) {
    switch (k) {
        case SelectProfitability::Unknown:     return "unknown";
        case SelectProfitability::Profitable:  return "profitable";
        case SelectProfitability::Unprofitable: return "unprofitable";
        case SelectProfitability::Neutral:     return "neutral";
    }
    return "unknown";
}

// ============================================================================
// MetadataMap::dumpNode — dump metadata for a single node
// ============================================================================
std::string MetadataMap::dumpNode(const void* node) const {
    auto* m = get(node);
    if (!m) return "  (no metadata)\n";

    std::ostringstream ss;

    // L1: Semantic
    ss << "  ownership=" << toString(m->ownership);
    ss << " aliasing=" << toString(m->aliasing);
    ss << " layout=" << toString(m->layout);
    if (m->alignment > 0) ss << " align=" << m->alignment;
    ss << " purity=" << toString(m->purity);
    if (m->is_restrict) ss << " restrict";
    if (m->is_inline) ss << " inline";
    if (m->is_noalloc) ss << " noalloc";
    ss << "\n";

    // L2: Control-flow
    if (m->branch_prob != BranchProbability::Unknown) {
        ss << "  branch=" << toString(m->branch_prob);
        ss << " select=" << toString(m->select_profit);
        ss << "\n";
    }

    // L3: Memory topology
    if (!m->access_patterns.empty()) {
        ss << "  access_patterns:\n";
        for (const auto& ap : m->access_patterns) {
            ss << "    " << ap.variable_name
               << ": traversal=" << toString(ap.traversal)
               << " access=" << toString(ap.access)
               << " stride=" << ap.stride
               << " contiguous=" << (ap.contiguous ? "true" : "false")
               << " vectorizable=" << (ap.vectorizable ? "true" : "false")
               << " prefetch_distance=" << ap.prefetch_distance
               << "\n";
        }
    }

    // L4: Layout
    if (m->layout_transform.kind != TransformKind::None) {
        ss << "  transform=" << toString(m->layout_transform.kind);
        if (!m->layout_transform.detail.empty()) {
            ss << " detail=\"" << m->layout_transform.detail << "\"";
        }
        ss << "\n";
    }

    // L5: LLVM hints
    auto& lm = m->llvm_meta;
    if (lm.noalias || lm.nonnull || lm.align > 0 || lm.has_range ||
        lm.hot_path || lm.cold_path || lm.vectorization_safe ||
        lm.prefetch_distance > 0 || !lm.tbaa_type.empty()) {
        ss << "  llvm:";
        if (lm.noalias) ss << " noalias";
        if (lm.nonnull) ss << " nonnull";
        if (lm.align > 0) ss << " align=" << lm.align;
        if (lm.has_range) ss << " range=[" << lm.range_lo << "," << lm.range_hi << ")";
        if (lm.vectorization_safe) ss << " vectorizable";
        if (lm.hot_path) ss << " hot";
        if (lm.cold_path) ss << " cold";
        if (lm.prefetch_distance > 0) ss << " prefetch=" << lm.prefetch_distance;
        if (!lm.tbaa_type.empty()) ss << " tbaa=" << lm.tbaa_type;
        ss << "\n";
    }

    // L6: Profile
    if (m->profile.has_profile) {
        ss << "  profile: entries=" << m->profile.entry_count
           << " branch_taken=" << m->profile.branch_taken
           << " branch_not_taken=" << m->profile.branch_not_taken
           << " avg_loop_iters=" << m->profile.loop_iteration_count
           << "\n";
    }

    return ss.str();
}

// ============================================================================
// MetadataMap::dump — dump all metadata for inspection
// ============================================================================
std::string MetadataMap::dump() const {
    std::ostringstream ss;

    ss << "=== Tether Metadata Engine Report ===\n\n";

    // Struct-level metadata
    if (!structs_.empty()) {
        ss << "--- Struct Layouts ---\n";
        for (const auto& [name, sm] : structs_) {
            ss << "struct " << name << ":\n";
            ss << "  layout=" << toString(sm.layout);
            if (sm.alignment > 0) ss << " align=" << sm.alignment;
            ss << "\n";
            if (sm.transform.kind != TransformKind::None) {
                ss << "  transform=" << toString(sm.transform.kind);
                if (!sm.transform.detail.empty()) {
                    ss << " \"" << sm.transform.detail << "\"";
                }
                ss << "\n";
                if (!sm.transform.hot_fields.empty()) {
                    ss << "  hot_fields: ";
                    for (size_t i = 0; i < sm.transform.hot_fields.size(); i++) {
                        if (i > 0) ss << ", ";
                        ss << sm.transform.hot_fields[i];
                    }
                    ss << "\n";
                }
                if (!sm.transform.cold_fields.empty()) {
                    ss << "  cold_fields: ";
                    for (size_t i = 0; i < sm.transform.cold_fields.size(); i++) {
                        if (i > 0) ss << ", ";
                        ss << sm.transform.cold_fields[i];
                    }
                    ss << "\n";
                }
            }
            if (!sm.field_names.empty()) {
                ss << "  fields:\n";
                for (size_t i = 0; i < sm.field_names.size(); i++) {
                    ss << "    " << sm.field_names[i];
                    if (i < sm.field_is_hot.size()) {
                        ss << (sm.field_is_hot[i] ? " [hot]" : " [cold]");
                    }
                    ss << "\n";
                }
            }
        }
        ss << "\n";
    }

    // Per-node metadata summary
    ss << "--- Node Metadata (" << meta_.size() << " nodes) ---\n";
    for (const auto& [node, m] : meta_) {
        // Only dump nodes with interesting metadata
        bool interesting = false;
        if (m.aliasing == AliasingKind::NoAlias) interesting = true;
        if (m.is_restrict) interesting = true;
        if (m.layout != LayoutKind::AoS) interesting = true;
        if (m.alignment > 0) interesting = true;
        if (!m.access_patterns.empty()) interesting = true;
        if (m.layout_transform.kind != TransformKind::None) interesting = true;
        if (m.llvm_meta.noalias || m.llvm_meta.hot_path || m.llvm_meta.cold_path) interesting = true;
        if (m.llvm_meta.vectorization_safe) interesting = true;
        if (m.profile.has_profile) interesting = true;
        if (m.branch_prob != BranchProbability::Unknown) interesting = true;

        if (interesting) {
            ss << "  node@" << node << ":\n";
            ss << dumpNode(node);
        }
    }

    ss << "\n=== End Report ===\n";
    return ss.str();
}

} // namespace tether
