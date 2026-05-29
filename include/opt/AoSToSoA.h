#pragma once

#include "opt/PreLLVMPipeline.h"
#include "ast/AST.h"
#include "sema/Type.h"

namespace tether {

// AoS->SoA transformation: Transforms arrays of structs marked with `soa`
// into separate arrays per field. This is something LLVM fundamentally
// cannot do because it changes the semantic meaning of data access.
//
// Example:
//   soa struct Vec3 { x: f32, y: f32, z: f32 }
//   var data: [1000]Vec3
// Becomes (internally):
//   var data_x: [1000]f32
//   var data_y: [1000]f32
//   var data_z: [1000]f32
//
// This enables SIMD vectorization and cache-friendly access patterns.
//
// LLVM CANNOT do this: struct layout is fixed by the time LLVM sees IR,
// and LLVM cannot change the semantic layout of data structures.
class AoSToSoAPass : public PreLLVMPass {
public:
    std::string name() const override { return "AoS->SoA"; }
    bool run(Program& program, TypeTable& type_table) override;
    bool isRedundantWithLLVM() const override { return false; }
    PassCategory category() const override { return PassCategory::LayoutTransform; }

    struct SoATransform {
        std::string struct_name;
        std::vector<std::string> field_names;     // original field names
        std::vector<std::string> field_array_names; // e.g., data_x, data_y, data_z
        std::vector<TypeId> field_types;           // type of each field
        bool was_transformed = false;
    };

    const std::vector<SoATransform>& transforms() const { return transforms_; }

private:
    std::vector<SoATransform> transforms_;

    bool transformStruct(StructDecl& decl, TypeTable& type_table);
    bool transformAccesses(Program& program, const SoATransform& transform);
};

} // namespace tether
