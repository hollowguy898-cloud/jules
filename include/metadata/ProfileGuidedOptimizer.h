#pragma once

#include "ast/AST.h"
#include "parser/Parser.h"
#include "sema/Type.h"
#include "metadata/MetaTypes.h"

#include <string>

namespace tether {

// ============================================================================
// L6: Profile-Guided Optimizer
//
// Feeds runtime profiling back into:
//   - Function ordering (instruction cache locality)
//   - Branch weighting (branch prediction)
//   - Code layout (hot-path specialization)
//   - Hot/cold path classification
//
// Profile data format: simple text file with per-function and per-branch
// counters. The format is:
//   function:<name>:<entry_count>
//   branch:<file>:<line>:<col>:<taken>:<not_taken>
//   loop:<file>:<line>:<col>:<avg_iterations>
//
// When no profile data is available, this layer does nothing — it does
// NOT fall back to heuristics (that's L1's job).
// ============================================================================
class ProfileGuidedOptimizer {
public:
    // Load profile data from a file. Returns true on success.
    bool loadProfile(const std::string& profile_path);

    // Apply profile data to the metadata map
    void apply(Program& program, MetadataMap& meta);

    // Check if profile data is loaded
    bool hasProfile() const { return has_profile_; }

    // Get a function's entry count from profile data
    uint64_t getFunctionEntryCount(const std::string& fn_name) const;

    // Get a branch's taken/not_taken counts
    bool getBranchCounts(const SourceLocation& loc,
                         uint64_t& taken, uint64_t& not_taken) const;

    // Get a loop's average iteration count
    uint64_t getLoopIterationCount(const SourceLocation& loc) const;

private:
    bool has_profile_ = false;

    // Profile data storage
    struct FnProfile {
        uint64_t entry_count = 0;
    };

    struct BranchProfile {
        uint64_t taken = 0;
        uint64_t not_taken = 0;
    };

    struct LoopProfile {
        uint64_t avg_iterations = 0;
    };

    std::unordered_map<std::string, FnProfile> fn_profiles_;
    std::unordered_map<std::string, BranchProfile> branch_profiles_;
    std::unordered_map<std::string, LoopProfile> loop_profiles_;

    // Make a key from a source location for branch/loop lookup
    static std::string locKey(const SourceLocation& loc);
};

} // namespace tether
