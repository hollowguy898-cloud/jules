#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace tether {

// ============================================================================
// Metadata Engine Core Types
//
// These types represent the semantic information that the Metadata Engine
// collects, transforms, and emits. Each metadata category maps to a specific
// optimization capability.
// ============================================================================

// ============================================================================
// L1: Semantic metadata — ownership, aliasing, layout
// ============================================================================

enum class OwnershipKind : uint8_t {
    Owned,        // var declaration — mutable, owned
    Immutable,    // val declaration — immutable, owned
    Borrowed,     // &T — shared borrow
    MutBorrowed,  // &mut T — exclusive mutable borrow
    Moved,        // value has been moved out
};

enum class AliasingKind : uint8_t {
    NoAlias,      // restrict-qualified, unique pointer — guaranteed no alias
    MayAlias,     // general reference — may alias
    NoAccess,     // not accessed in this context
};

enum class LayoutKind : uint8_t {
    AoS,          // Array of Structs (default)
    SoA,          // Struct of Arrays (explicit or detected)
    Hybrid,       // Hot fields inline, cold fields via pointer
    Chunked,      // Chunked ECS layout (cache-friendly)
};

enum class FunctionPurity : uint8_t {
    Impure,       // May have side effects
    Pure,         // No side effects, deterministic
    NoAlloc,      // No heap allocation, suitable for HFT
};

// ============================================================================
// L2: Control-flow metadata — branch probability, select profitability
// ============================================================================

enum class BranchProbability : uint8_t {
    Unknown,
    Likely,       // > 80% probability
    Unlikely,     // < 20% probability
    Even,         // roughly 50/50
};

enum class SelectProfitability : uint8_t {
    Unknown,
    Profitable,   // Both branches cheap, no side effects, vectorizable
    Unprofitable, // One or both branches expensive or have side effects
    Neutral,      // Doesn't matter much either way
};

// ============================================================================
// L3: Memory topology — access patterns, traversal, prefetch
// ============================================================================

enum class TraversalKind : uint8_t {
    Unknown,
    Linear,       // Sequential index: data[i] with i += 1
    Strided,      // Fixed stride: data[i] with i += K
    Random,       // Random access / gather-scatter
    Sparse,       // Mostly skipping elements (conditional access)
    Chunked,      // Chunk-based iteration (ECS archetype)
};

enum class AccessKind : uint8_t {
    Read,         // Only reads
    Write,        // Only writes
    ReadWrite,    // Reads and writes
};

struct AccessPattern {
    std::string variable_name;     // Name of the variable being accessed
    TraversalKind traversal = TraversalKind::Unknown;
    AccessKind access = AccessKind::ReadWrite;
    int64_t stride = 1;            // Stride for strided traversal (1 = linear)
    bool contiguous = false;       // Accesses touch contiguous memory
    bool vectorizable = false;     // Safe to vectorize (no dependencies)
    int64_t prefetch_distance = 0; // Suggested prefetch distance in cache lines
};

// ============================================================================
// L4: Layout transformation metadata
// ============================================================================

enum class TransformKind : uint8_t {
    None,
    SoATransform,       // AoS → SoA
    HotColdSplit,       // Split hot/cold fields
    PackedBitfield,     // Pack adjacent bools into a bitfield
    FieldReorder,       // Reorder fields for minimal padding
};

struct LayoutTransform {
    TransformKind kind = TransformKind::None;
    std::string struct_name;
    std::vector<std::string> hot_fields;   // Fields in the hot path
    std::vector<std::string> cold_fields;  // Fields in the cold path
    std::vector<int> reorder_map;          // new_order[old_index] = new_index
    std::string detail;                     // Human-readable description
};

// ============================================================================
// L5: LLVM emission metadata — what LLVM hints to emit
// ============================================================================

struct LLVMMeta {
    bool noalias = false;           // Emit !noalias / !tbaa
    bool nonnull = false;           // Emit !nonnull
    uint32_t align = 0;             // Emit !align (0 = no hint)
    uint32_t range_lo = 0;          // Emit !range lower bound
    uint32_t range_hi = 0;          // Emit !range upper bound
    bool has_range = false;         // Whether range metadata applies
    BranchProbability branch_prob = BranchProbability::Unknown;
    bool vectorization_safe = false; // Loop is safe to vectorize
    bool hot_path = false;          // Emit !hot
    bool cold_path = false;         // Emit !cold / !unlikely
    int64_t prefetch_distance = 0;  // Insert prefetch at this distance
    std::string tbaa_type;          // TBAA type identifier
};

// ============================================================================
// L6: Profile-guided metadata
// ============================================================================

struct ProfileData {
    uint64_t entry_count = 0;       // How many times this function was called
    uint64_t branch_taken = 0;      // True branch taken count
    uint64_t branch_not_taken = 0;  // False branch taken count
    uint64_t loop_iteration_count = 0; // Average loop iterations
    bool has_profile = false;       // Whether real profile data is available
};

// ============================================================================
// Metadata attachment — per-node metadata bundle
//
// Each AST node can have metadata from multiple layers attached.
// The MetadataMap stores these bundles keyed by the AST node pointer.
// ============================================================================

struct NodeMeta {
    // L1: Semantic
    OwnershipKind ownership = OwnershipKind::Owned;
    AliasingKind aliasing = AliasingKind::MayAlias;
    LayoutKind layout = LayoutKind::AoS;
    uint32_t alignment = 0;            // 0 = default alignment
    FunctionPurity purity = FunctionPurity::Impure;
    bool is_restrict = false;
    bool is_inline = false;
    bool is_noalloc = false;

    // L2: Control-flow
    BranchProbability branch_prob = BranchProbability::Unknown;
    SelectProfitability select_profit = SelectProfitability::Unknown;

    // L3: Memory topology
    std::vector<AccessPattern> access_patterns;
    bool has_loop_with_linear_access = false;

    // L4: Layout
    LayoutTransform layout_transform;

    // L5: LLVM emission hints
    LLVMMeta llvm_meta;

    // L6: Profile
    ProfileData profile;

    // Pre-LLVM pass annotations (migrated from ASTAnnotationMap)
    bool yield_point = false;          // cooperative yield check needed
    bool opaque_barrier = false;       // FFI boundary, needs aliasing fence
    bool allocator_inlined = false;    // arena bump was inlined
    bool stack_allocated = false;      // smart pointer can use stack instead of heap
    bool soa_transformed = false;      // this struct was SoA-transformed
};

// ============================================================================
// MetadataMap — the central metadata store
//
// Replaces the flat ASTAnnotationMap with structured, typed metadata.
// Each layer writes into the map, and later layers / the IR generator
// read from it.
// ============================================================================

class MetadataMap {
public:
    // Get or create metadata for a node
    NodeMeta& getOrCreate(const void* node) {
        return meta_[node];
    }

    // Get metadata for a node (returns nullptr if not present)
    const NodeMeta* get(const void* node) const {
        auto it = meta_.find(node);
        if (it == meta_.end()) return nullptr;
        return &it->second;
    }

    NodeMeta* getMutable(const void* node) {
        auto it = meta_.find(node);
        if (it == meta_.end()) return nullptr;
        return &it->second;
    }

    // Check if metadata exists for a node
    bool has(const void* node) const {
        return meta_.count(node) > 0;
    }

    // Query helpers
    bool hasNoAlias(const void* node) const {
        auto* m = get(node);
        return m && m->aliasing == AliasingKind::NoAlias;
    }

    bool isHotPath(const void* node) const {
        auto* m = get(node);
        return m && m->llvm_meta.hot_path;
    }

    bool isColdPath(const void* node) const {
        auto* m = get(node);
        return m && m->llvm_meta.cold_path;
    }

    bool isVectorizableLoop(const void* node) const {
        auto* m = get(node);
        return m && m->llvm_meta.vectorization_safe;
    }

    TraversalKind getTraversalKind(const void* node) const {
        auto* m = get(node);
        if (!m || m->access_patterns.empty()) return TraversalKind::Unknown;
        return m->access_patterns[0].traversal;
    }

    // Size and clear
    size_t size() const { return meta_.size(); }
    void clear() { meta_.clear(); }

    // Dump all metadata for inspection ("show me transformed layout")
    std::string dump() const;

    // Dump metadata for a specific node
    std::string dumpNode(const void* node) const;

    // Struct-level metadata: which structs have layout transforms
    struct StructMeta {
        std::string name;
        LayoutKind layout = LayoutKind::AoS;
        uint32_t alignment = 0;
        LayoutTransform transform;
        std::vector<std::string> field_names;
        std::vector<bool> field_is_hot;  // per-field hot classification
    };

    // Register struct metadata (initial registration — fails if already exists)
    void registerStruct(const std::string& name, StructMeta meta) {
        structs_[name] = std::move(meta);
    }

    // Register or merge struct metadata — does NOT clobber existing data.
    // Merges field_is_hot (OR), preserves higher-priority transforms,
    // and accumulates field_names if not already present.
    void mergeStructMeta(const std::string& name, StructMeta meta) {
        auto it = structs_.find(name);
        if (it == structs_.end()) {
            // Not present — just register
            structs_[name] = std::move(meta);
            return;
        }

        // Merge into existing entry
        StructMeta& existing = it->second;

        // Merge alignment: take the larger value
        if (meta.alignment > existing.alignment) {
            existing.alignment = meta.alignment;
        }

        // Merge layout: take the more specific one
        // Priority: SoA > Hybrid > Chunked > AoS
        if (meta.layout == LayoutKind::SoA && existing.layout != LayoutKind::SoA) {
            existing.layout = LayoutKind::SoA;
        } else if (meta.layout == LayoutKind::Hybrid && existing.layout == LayoutKind::AoS) {
            existing.layout = LayoutKind::Hybrid;
        }

        // Merge field_is_hot: OR (a field is hot if ANY pass says it's hot)
        if (meta.field_is_hot.size() > existing.field_is_hot.size()) {
            existing.field_is_hot.resize(meta.field_is_hot.size(), false);
        }
        for (size_t i = 0; i < meta.field_is_hot.size(); ++i) {
            if (meta.field_is_hot[i]) {
                existing.field_is_hot[i] = true;
            }
        }

        // Merge field_names: only if existing is empty
        if (existing.field_names.empty() && !meta.field_names.empty()) {
            existing.field_names = std::move(meta.field_names);
        }

        // Merge transform: take the higher-priority one
        // Priority: SoATransform > HotColdSplit > FieldReorder > PackedBitfield > None
        auto transformPriority = [](TransformKind k) -> int {
            switch (k) {
                case TransformKind::SoATransform:  return 4;
                case TransformKind::HotColdSplit:  return 3;
                case TransformKind::FieldReorder:  return 2;
                case TransformKind::PackedBitfield: return 1;
                case TransformKind::None:           return 0;
            }
            return 0;
        };

        if (transformPriority(meta.transform.kind) > transformPriority(existing.transform.kind)) {
            existing.transform = std::move(meta.transform);
        } else if (meta.transform.kind == existing.transform.kind &&
                   meta.transform.kind != TransformKind::None) {
            // Same transform kind — merge details
            if (!meta.transform.hot_fields.empty()) {
                existing.transform.hot_fields = std::move(meta.transform.hot_fields);
            }
            if (!meta.transform.cold_fields.empty()) {
                existing.transform.cold_fields = std::move(meta.transform.cold_fields);
            }
            if (!meta.transform.reorder_map.empty()) {
                existing.transform.reorder_map = std::move(meta.transform.reorder_map);
            }
            if (!meta.transform.detail.empty()) {
                existing.transform.detail = std::move(meta.transform.detail);
            }
        }
    }

    const StructMeta* getStructMeta(const std::string& name) const {
        auto it = structs_.find(name);
        if (it == structs_.end()) return nullptr;
        return &it->second;
    }

    StructMeta* getStructMetaMutable(const std::string& name) {
        auto it = structs_.find(name);
        if (it == structs_.end()) return nullptr;
        return &it->second;
    }

    const std::unordered_map<std::string, StructMeta>& structs() const {
        return structs_;
    }

private:
    std::unordered_map<const void*, NodeMeta> meta_;
    std::unordered_map<std::string, StructMeta> structs_;
};

// ============================================================================
// String conversion helpers
// ============================================================================

const char* toString(OwnershipKind k);
const char* toString(AliasingKind k);
const char* toString(LayoutKind k);
const char* toString(FunctionPurity k);
const char* toString(TraversalKind k);
const char* toString(AccessKind k);
const char* toString(TransformKind k);
const char* toString(BranchProbability k);
const char* toString(SelectProfitability k);

} // namespace tether
