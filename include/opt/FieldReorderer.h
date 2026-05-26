#pragma once

#include "ast/AST.h"
#include "sema/Type.h"
#include "parser/Parser.h"

#include <vector>
#include <string>

namespace tether {

struct ReorderResult {
    std::string struct_name;
    std::vector<int> original_order;   // indices in declaration order
    std::vector<int> reordered_order;  // indices after reordering (largest alignment first)
    uint64_t original_size;            // bytes with original layout + padding
    uint64_t reordered_size;           // bytes after reordering
    bool was_improved;                 // true if reordered_size < original_size
};

class FieldReorderer {
public:
    explicit FieldReorderer(TypeTable& type_table);

    // Analyze a single struct and return the reordering result
    ReorderResult analyze(StructDecl& decl);

    // Analyze all structs in the program and return results
    std::vector<ReorderResult> analyzeAll(Program& program);

    // Apply the reordering to a StructDecl (modifies field order in-place)
    void apply(StructDecl& decl, const ReorderResult& result);

    // Return the size in bytes for a given type
    uint64_t fieldSizeBytes(TypeId type) const;
    // Return the alignment requirement for a given type
    uint64_t fieldAlignment(TypeId type) const;

private:
    uint64_t computeLayoutSize(const std::vector<StructFieldDecl>& fields,
                               const std::vector<int>& order) const;

    TypeTable& type_table_;
};

} // namespace tether
