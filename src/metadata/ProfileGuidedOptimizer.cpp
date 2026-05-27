#include "metadata/ProfileGuidedOptimizer.h"
#include "parser/Parser.h"

#include <fstream>
#include <sstream>
#include <string>
#include <functional>

namespace tether {

// ============================================================================
// Anonymous namespace: AST walking helpers
// ============================================================================
namespace {

/// Recursively walk all statements in a block, invoking the callback.
void walkAllStmts(BlockStmt& block, std::function<void(Stmt&)> callback) {
    for (auto& stmt : block.stmts()) {
        if (!stmt) continue;
        callback(*stmt);

        switch (stmt->getKind()) {
            case NodeKind::IfStmt: {
                auto& is = cast<IfStmt>(*stmt);
                if (is.thenBlock()) walkAllStmts(*is.thenBlock(), callback);
                if (is.hasElse() && is.elseBlock()) walkAllStmts(*is.elseBlock(), callback);
                break;
            }
            case NodeKind::WhileStmt: {
                auto& ws = cast<WhileStmt>(*stmt);
                if (ws.body()) walkAllStmts(*ws.body(), callback);
                break;
            }
            case NodeKind::BlockStmt: {
                auto& bs = cast<BlockStmt>(*stmt);
                walkAllStmts(bs, callback);
                break;
            }
            case NodeKind::DeferStmt: {
                auto& ds = cast<DeferStmt>(*stmt);
                if (ds.stmt()) callback(*ds.stmt());
                break;
            }
            case NodeKind::ErrdeferStmt: {
                auto& es = cast<ErrdeferStmt>(*stmt);
                if (es.stmt()) callback(*es.stmt());
                break;
            }
            case NodeKind::SwitchStmt: {
                auto& ss = cast<SwitchStmt>(*stmt);
                for (auto& arm : ss.arms()) {
                    if (arm.body) walkAllStmts(*arm.body, callback);
                }
                break;
            }
            case NodeKind::AtomicStmt: {
                auto& as = cast<AtomicStmt>(*stmt);
                if (as.inner()) callback(*as.inner());
                break;
            }
            default:
                break;
        }
    }
}

} // anonymous namespace

// ============================================================================
// locKey — produce a string key from a source location
// ============================================================================

std::string ProfileGuidedOptimizer::locKey(const SourceLocation& loc) {
    return loc.filename + ":" + std::to_string(loc.line) + ":" + std::to_string(loc.col);
}

// ============================================================================
// loadProfile — parse the profile data file
// ============================================================================

bool ProfileGuidedOptimizer::loadProfile(const std::string& profile_path) {
    std::ifstream file(profile_path);
    if (!file.is_open()) {
        return false;
    }

    std::string line;
    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') continue;

        // Parse the line by splitting on ':'
        std::istringstream iss(line);
        std::string type;
        std::getline(iss, type, ':');

        if (type == "function") {
            // function:name:count
            std::string name, count_str;
            std::getline(iss, name, ':');
            std::getline(iss, count_str, ':');

            if (!name.empty() && !count_str.empty()) {
                try {
                    fn_profiles_[name].entry_count = std::stoull(count_str);
                } catch (...) {
                    // Malformed line, skip
                    continue;
                }
            }
        } else if (type == "branch") {
            // branch:file:line:col:taken:not_taken
            std::string file_str, line_str, col_str, taken_str, not_taken_str;
            std::getline(iss, file_str, ':');
            std::getline(iss, line_str, ':');
            std::getline(iss, col_str, ':');
            std::getline(iss, taken_str, ':');
            std::getline(iss, not_taken_str, ':');

            if (!file_str.empty() && !line_str.empty() &&
                !col_str.empty() && !taken_str.empty() && !not_taken_str.empty()) {
                try {
                    std::string key = file_str + ":" + line_str + ":" + col_str;
                    branch_profiles_[key].taken = std::stoull(taken_str);
                    branch_profiles_[key].not_taken = std::stoull(not_taken_str);
                } catch (...) {
                    continue;
                }
            }
        } else if (type == "loop") {
            // loop:file:line:col:avg_iters
            std::string file_str, line_str, col_str, iters_str;
            std::getline(iss, file_str, ':');
            std::getline(iss, line_str, ':');
            std::getline(iss, col_str, ':');
            std::getline(iss, iters_str, ':');

            if (!file_str.empty() && !line_str.empty() &&
                !col_str.empty() && !iters_str.empty()) {
                try {
                    std::string key = file_str + ":" + line_str + ":" + col_str;
                    loop_profiles_[key].avg_iterations = std::stoull(iters_str);
                } catch (...) {
                    continue;
                }
            }
        }
        // Unknown line types are silently ignored
    }

    has_profile_ = true;
    return true;
}

// ============================================================================
// apply — walk the program and apply profile data to metadata
// ============================================================================

void ProfileGuidedOptimizer::apply(Program& program, MetadataMap& meta) {
    if (!has_profile_) return;

    for (auto& toplevel : program) {
        // Process function declarations
        auto* fn = dyn_cast<FnDecl>(toplevel.get());
        if (!fn) continue;

        // --- Apply function-level profile data ---

        // Look up entry count for this function
        uint64_t entry_count = getFunctionEntryCount(fn->name());

        if (fn_profiles_.count(fn->name()) > 0) {
            auto& nm = meta.getOrCreate(fn);
            nm.profile.entry_count = entry_count;
            nm.profile.has_profile = true;

            // If entry_count > 10000 -> hot path
            if (entry_count > 10000) {
                nm.llvm_meta.hot_path = true;
            }

            // If entry_count == 0 -> cold path
            if (entry_count == 0) {
                nm.llvm_meta.cold_path = true;
            }
        }

        // --- Apply branch-level profile data ---

        if (!fn->body()) continue;

        walkAllStmts(*fn->body(), [&](Stmt& stmt) {
            if (stmt.getKind() == NodeKind::IfStmt) {
                auto& is = cast<IfStmt>(stmt);

                // Look up branch counts from profile data
                uint64_t taken = 0, not_taken = 0;
                if (!getBranchCounts(is.sourceLoc(), taken, not_taken)) {
                    return; // No profile data for this branch
                }

                auto& nm = meta.getOrCreate(&is);
                nm.profile.branch_taken = taken;
                nm.profile.branch_not_taken = not_taken;
                nm.profile.has_profile = true;

                // Compute the branch probability
                uint64_t total = taken + not_taken;
                if (total == 0) {
                    // No data, don't change branch_prob
                    return;
                }

                double probability = static_cast<double>(taken) / static_cast<double>(total);

                if (probability > 0.8) {
                    nm.branch_prob = BranchProbability::Likely;
                } else if (probability < 0.2) {
                    nm.branch_prob = BranchProbability::Unlikely;
                } else {
                    nm.branch_prob = BranchProbability::Even;
                }

            } else if (stmt.getKind() == NodeKind::WhileStmt) {
                auto& ws = cast<WhileStmt>(stmt);

                // Look up loop iteration count from profile data
                const auto& loc = ws.sourceLoc();
                std::string key = locKey(loc);

                if (loop_profiles_.count(key) == 0) {
                    return; // No profile data for this loop
                }

                auto& nm = meta.getOrCreate(&ws);
                nm.profile.loop_iteration_count = loop_profiles_[key].avg_iterations;
                nm.profile.has_profile = true;
            }
        });
    }
}

// ============================================================================
// getFunctionEntryCount — lookup a function's entry count
// ============================================================================

uint64_t ProfileGuidedOptimizer::getFunctionEntryCount(const std::string& fn_name) const {
    auto it = fn_profiles_.find(fn_name);
    if (it == fn_profiles_.end()) return 0;
    return it->second.entry_count;
}

// ============================================================================
// getBranchCounts — lookup a branch's taken/not_taken counts
// ============================================================================

bool ProfileGuidedOptimizer::getBranchCounts(const SourceLocation& loc,
                                              uint64_t& taken,
                                              uint64_t& not_taken) const {
    std::string key = locKey(loc);
    auto it = branch_profiles_.find(key);
    if (it == branch_profiles_.end()) return false;

    taken = it->second.taken;
    not_taken = it->second.not_taken;
    return true;
}

// ============================================================================
// getLoopIterationCount — lookup a loop's average iteration count
// ============================================================================

uint64_t ProfileGuidedOptimizer::getLoopIterationCount(const SourceLocation& loc) const {
    std::string key = locKey(loc);
    auto it = loop_profiles_.find(key);
    if (it == loop_profiles_.end()) return 0;
    return it->second.avg_iterations;
}

} // namespace tether
