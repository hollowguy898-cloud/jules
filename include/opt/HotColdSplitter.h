#pragma once

#include "opt/PreLLVMPipeline.h"

namespace tether {

// Hot/Cold Field Splitting: With PGO data, splits struct fields into
// "hot" (frequently accessed) and "cold" (rarely accessed) groups.
// The hot fields stay in the main struct for cache locality, while
// cold fields are moved to a separate allocation.
//
// LLVM cannot do this because it doesn't know which fields are hot
// vs cold (that requires semantic/profile knowledge).
//
// Example with PGO data:
//   struct Entity { pos: Vec3, vel: Vec3, flags: u32, debug_name: string }
//   // pos and vel are hot (accessed every frame), flags and debug_name are cold
// Becomes:
//   struct Entity { pos: Vec3, vel: Vec3, __cold: *EntityCold }
//   struct EntityCold { flags: u32, debug_name: string }
//
// Currently, this pass uses heuristics when PGO data is unavailable:
// - Pointer and smart pointer fields are assumed cold (indirection)
// - String/slice fields are assumed cold (typically debug/formatting)
// - Small numeric fields accessed in loops are assumed hot
//
// The pass sets metadata on the StructDecl for the IR generator to
// emit the split layout.
class HotColdSplitterPass : public PreLLVMPass {
public:
    std::string name() const override { return "HotColdFieldSplitting"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::LayoutTransform; }

    int structsSplit() const { return structs_split_; }

private:
    int structs_split_ = 0;

    // Minimum number of fields to consider splitting
    static constexpr size_t MIN_FIELDS_FOR_SPLIT = 4;
    // Minimum ratio of cold fields to consider splitting
    static constexpr double MIN_COLD_RATIO = 0.3;

    bool processStructDecl(StructDecl& decl, TypeTable& type_table);

    // Heuristic: classify a field as hot or cold based on its type
    bool isLikelyColdField(const StructFieldDecl& field, TypeTable& type_table) const;

    // Check if a type is pointer-like (indirection = likely cold)
    bool isIndirectType(TypeId type) const;

    // Check if a type is small and numeric (likely hot)
    bool isSmallNumeric(TypeId type) const;
};

} // namespace tether
