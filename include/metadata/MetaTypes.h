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
// Speculative optimization assumption kinds
//
// These are the types of assumptions the speculative optimizer can make
// about program behavior. Each assumption carries a confidence level and
// results in a deoptimization guard in the generated LLVM IR.
// ============================================================================
enum class AssumptionKind : uint8_t {
    NeverNull,        // Pointer is never null
    TypeExact,        // Type is exactly T (not a subtype)
    BranchNeverTaken, // A branch is never taken (cold path)
    NoOverflow,       // Arithmetic doesn't overflow
    BoundsInRange,    // Array index is within bounds
    NoAlias,          // Pointers don't alias
    PureCall,         // Function call has no side effects
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
    uint64_t violation_count = 0;   // How many times a speculative assumption was violated
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

    // Speculative optimization (from SpeculativeOptimizerPass)
    bool has_speculative_assumption = false;
    AssumptionKind speculative_kind;   // Only valid if has_speculative_assumption
    float speculative_confidence = 0.0f;

    // Strict borrow checker results
    bool borrow_proven_safe = false;     // Borrow checker proved this access is safe
    bool bounds_proven_safe = false;     // Bounds check can be eliminated
    bool in_unsafe_block = false;        // This node is inside an unsafe block
    bool moved_out = false;              // This variable has been moved out
    uint32_t lifetime_id = static_cast<uint32_t>(-1);  // Assigned lifetime for this reference (INVALID_LIFETIME = -1)

    // Compile-time evaluation results (from ComptimeEvalPass)
    bool comptime_evaluated = false;     // This expression was fully evaluated at compile time
    int64_t comptime_int_value = 0;      // Valid when comptime_evaluated && kind == Int
    double comptime_float_value = 0.0;   // Valid when comptime_evaluated && kind == Float
    bool comptime_bool_value = false;    // Valid when comptime_evaluated && kind == Bool
    std::string comptime_string_value;   // Valid when comptime_evaluated && kind == String

    // SROA (Scalar Replacement of Aggregates) — set by SROAPass
    bool sroa_eligible = false;              // This struct variable can be SROA'd
    std::vector<std::string> sroa_field_names;  // Names for the decomposed fields (e.g. "p.x", "p.y")

    // Niched error type optimization (from NichedErrorPass)
    // When niched_error = true, the ErrorType uses pointer niche or integer
    // niche encoding instead of a separate i1 error flag, eliminating
    // the overhead of error handling in the common (success) path.
    enum class NichedErrorKind : uint8_t {
        PointerNiche,    // ptr with low bit as error flag
        IntegerNiche,    // i64 with high bit as error flag
        StructFallback,  // Traditional { T, i1 } struct
    };
    bool niched_error = false;              // This ErrorType uses niche encoding
    NichedErrorKind niched_error_kind = NichedErrorKind::StructFallback;

    // Niche optimization info (from NichedErrorPass::checkNiche)
    // Detailed bit-pattern and size information for the IRGenerator to
    // emit compact error representations without consulting type sizes.
    bool has_niche = false;                 // True if the success type has a usable niche
    uint64_t niche_bit_pattern = 0;         // The bit pattern that encodes "error"
    uint64_t niched_size = 0;               // Size after niche optimization (bytes)
    std::string niche_description;          // Human-readable niche description

    // Allocation fusion (from AllocFusionPass)
    bool alloc_fused = false;            // This Box allocation is part of a fused batch
    int64_t alloc_batch_size = 0;        // Total batch size (only set on the first allocation in a batch)
    int64_t alloc_batch_offset = 0;      // Offset within the batch for this allocation

    // Monomorphization (from MonomorphizationPass)
    // When set on a CallExpr, indicates that the call should be emitted
    // using the mangled monomorphized function name instead of the generic name.
    // This is the Phase 6 bridge: the pass records which calls should use
    // which concrete instantiation, and the IRGenerator reads this field.
    std::string monomorphized_target;    // e.g., "max_i32" (empty = not monomorphized)
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
