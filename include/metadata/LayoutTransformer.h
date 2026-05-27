#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// L4: Layout Transformer
//
// Tether's killer feature area. Transforms:
//   - AoS → SoA (automatic detection from L3 traversal patterns)
//   - Hybrid layouts (hot fields inline, cold via pointer)
//   - Packed bitfields for consecutive bool fields
//   - Hot/cold field splitting
//
// IMPORTANT CONSTRAINT: Transforms must remain:
//   - Explicit (developer can opt in/out)
//   - Inspectable ("show me transformed layout" must work)
//   - Deterministic (same input → same output every time)
// ============================================================================
class LayoutTransformer {
public:
    // Run the layout transformer on the program
    void transform(Program& program, TypeTable& type_table, MetadataMap& meta);

private:
    // Detect AoS→SoA candidates from memory topology metadata
    // A struct is a SoA candidate when:
    //   - It is accessed via linear traversal in a hot loop
    //   - Its fields are accessed independently (not always all together)
    //   - It has >= 2 fields of the same primitive type
    bool detectSoACandidate(StructDecl& sd, MetadataMap& meta) const;

    // Detect hot/cold split candidates
    // A struct is a split candidate when:
    //   - It has >= 4 fields
    //   - Some fields are accessed in hot loops, others aren't
    //   - The struct would span > 1 cache line after splitting
    bool detectHotColdSplitCandidate(StructDecl& sd, MetadataMap& meta) const;

    // Detect packed bitfield candidates
    // A struct is a bitfield candidate when:
    //   - It has >= 2 consecutive bool fields
    //   - The bools are not individually addressed (no &x.bool_field)
    bool detectPackedBitfieldCandidate(StructDecl& sd, MetadataMap& meta) const;

    // Apply detected transforms by writing metadata
    void applySoATransform(StructDecl& sd, MetadataMap& meta);
    void applyHotColdSplit(StructDecl& sd, MetadataMap& meta);
    void applyPackedBitfield(StructDecl& sd, MetadataMap& meta);

    // Compute which fields are accessed in hot loops
    // Returns a vector of booleans, one per field, true = hot
    std::vector<bool> computeFieldHotness(
        StructDecl& sd, MetadataMap& meta) const;

    // Count how many fields of a struct are accessed in a given loop
    // (via member access patterns)
    int countFieldsAccessedInLoops(
        const std::string& struct_name, MetadataMap& meta) const;
};

} // namespace tether
