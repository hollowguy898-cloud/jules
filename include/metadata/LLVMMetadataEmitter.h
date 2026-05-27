#pragma once

#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"

#include <sstream>
#include <unordered_map>

namespace tether {

// ============================================================================
// L5: LLVM Metadata Emitter
//
// Translates Tether semantics into LLVM-friendly hints:
//   !noalias       — from restrict parameters
//   !range         — for integer types with known bounds
//   !align         — from struct alignment
//   branch weights — from branch probability metadata
//   loop metadata  — vectorization, unroll hints
//   prefetch hints — from memory topology analysis
//   hot/cold       — function and basic block annotations
//
// This layer can stay surprisingly small (1k-3k LOC).
// ============================================================================
class LLVMMetadataEmitter {
public:
    // Run the emitter — generates LLVM metadata strings that the
    // IRGenerator can insert into the .ll output
    void emit(Program& program, TypeTable& type_table, MetadataMap& meta);

    // Get the generated LLVM IR metadata block
    std::string metadataBlock() const { return metadata_ss_.str(); }

    // Get the function-level attributes for a specific function
    std::string fnAttributes(FnDecl& fn, const MetadataMap& meta) const;

    // Get the parameter attributes for a specific parameter
    std::string paramAttributes(FnParam& param, const MetadataMap& meta) const;

    // Get the load/store metadata string for a memory access
    std::string memoryMetadata(Expr& expr, const MetadataMap& meta) const;

    // Get the branch weight metadata string
    std::string branchWeightMetadata(IfStmt& is, const MetadataMap& meta) const;

    // Get the loop metadata string (vectorization, prefetch)
    std::string loopMetadata(WhileStmt& ws, const MetadataMap& meta) const;

private:
    // Generate TBAA (Type-Based Alias Analysis) metadata
    void emitTBAAMetadata(TypeTable& type_table, MetadataMap& meta);

    // Generate branch weight metadata entries
    void emitBranchWeights(Program& program, MetadataMap& meta);

    // Generate function hot/cold attributes
    void emitFunctionAttributes(Program& program, MetadataMap& meta);

    // Generate loop vectorization metadata
    void emitLoopMetadata(Program& program, MetadataMap& meta);

    // Generate prefetch intrinsic declarations
    void emitPrefetchDeclarations(MetadataMap& meta);

    // Get the next metadata ID
    int nextMetaId() { return meta_id_counter_++; }

    std::ostringstream metadata_ss_;
    int meta_id_counter_ = 0;

    // Collected TBAA type descriptors
    std::unordered_map<std::string, int> tbaa_type_ids_;
    int tbaa_root_id_ = 0;
};

} // namespace tether
