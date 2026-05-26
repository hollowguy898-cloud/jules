#pragma once

#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"

#include <string>
#include <vector>
#include <cstdint>

namespace tether {

struct LayoutWarning {
    SourceLocation loc;
    std::string struct_name;
    uint64_t current_size;
    uint64_t optimal_size;
    std::string message;
};

class StructLayoutChecker {
public:
    StructLayoutChecker(TypeTable& type_table);
    
    void check(Program& program);
    
    const std::vector<LayoutWarning>& warnings() const { return warnings_; }
    bool hasWarnings() const { return !warnings_.empty(); }

private:
    uint64_t fieldSizeBytes(TypeId type) const;
    uint64_t fieldAlignment(TypeId type) const;
    uint64_t computePaddedSize(const std::vector<StructFieldDecl>& fields) const;
    uint64_t computeOptimalSize(const std::vector<StructFieldDecl>& fields) const;
    
    TypeTable& type_table_;
    std::vector<LayoutWarning> warnings_;
};

} // namespace tether
