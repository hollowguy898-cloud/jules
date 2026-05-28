#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"
#include "parser/Parser.h"

namespace tether {

// ============================================================================
// L4: Layout Transformer (deduplicated)
//
// After pipeline unification, L4 only handles packed bitfield detection.
// SoA and hot/cold splitting are now handled by their dedicated
// PreLLVMPass implementations (AoSToSoAPass and HotColdSplitterPass),
// which are integrated into the unified PreLLVMPipeline in the correct
// order. This eliminates the duplication that caused metadata clobbering.
//
// Packed bitfield remains here because it's a complementary, lower-priority
// transform that coexists with other layout transforms.
// ============================================================================
class LayoutTransformer {
public:
    // Run the layout transformer on the program
    // Now only applies packed bitfield transforms
    void transform(Program& program, TypeTable& type_table, MetadataMap& meta);

private:
    // Detect packed bitfield candidates
    // A struct is a bitfield candidate when:
    //   - It has >= 2 consecutive bool fields
    //   - The bools are not individually addressed (no &x.bool_field)
    bool detectPackedBitfieldCandidate(StructDecl& sd, MetadataMap& meta) const;

    // Apply packed bitfield transform by writing metadata
    void applyPackedBitfield(StructDecl& sd, MetadataMap& meta);
};

} // namespace tether
