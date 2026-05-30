#pragma once

// ============================================================================
// TetherAttrPass.h — Custom LLVM pass plugin header for Tether compiler
//
// This header declares the TetherAttrPass class and the flag constants
// used in the Tether metadata format. It is included by both
// TetherAttrPass.cpp (the pass implementation) and PassPluginMain.cpp
// (the plugin entry point).
//
// NOTE: This header requires LLVM headers to be available at compile time.
// If LLVM is not available, the pass plugin cannot be built, but the
// IRGenerator metadata emission still works (it just emits text metadata
// nodes in the .ll file).
// ============================================================================

#include "llvm/IR/PassManager.h"
#include "llvm/IR/Module.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

#include <cstdint>

namespace tether {

// ============================================================================
// Flag constants for function metadata
// ============================================================================
constexpr uint32_t kFnNoAlias        = 1u << 0;  // 1
constexpr uint32_t kFnReadOnly       = 1u << 1;  // 2
constexpr uint32_t kFnHot            = 1u << 2;  // 4
constexpr uint32_t kFnCold           = 1u << 3;  // 8
constexpr uint32_t kFnPure           = 1u << 4;  // 16
constexpr uint32_t kFnNoAlloc        = 1u << 5;  // 32
constexpr uint32_t kFnVectorize      = 1u << 6;  // 64
constexpr uint32_t kFnUnroll         = 1u << 7;  // 128
constexpr uint32_t kFnInvariantLoad  = 1u << 8;  // 256

// ============================================================================
// Flag constants for parameter metadata
// ============================================================================
constexpr uint32_t kParamNoAlias       = 1u << 0;  // 1
constexpr uint32_t kParamReadOnly      = 1u << 1;  // 2
constexpr uint32_t kParamNonNull       = 1u << 2;  // 4
constexpr uint32_t kParamInvariantLoad = 1u << 3;  // 8
constexpr uint32_t kParamDereferenceable = 1u << 4;  // 16
constexpr uint32_t kParamNoCapture      = 1u << 5;  // 32
constexpr uint32_t kParamWriteOnly      = 1u << 6;  // 64

// ============================================================================
// TetherAttrPass — LLVM module pass that reads Tether metadata and applies
// LLVM optimization attributes.
//
// Uses the new LLVM pass manager (PassInfoMixin).
// Reads named metadata from the module:
//   !tether.fns    — function name + flags bitmask
//   !tether.params — function name + param index + param flags
//   !tether.loops  — function name + loop flags (vectorize/unroll)
//
// Then injects the corresponding LLVM attributes (noalias, readonly, hot,
// cold, memory(none), willreturn, nosync, etc.) and loop metadata
// (!llvm.loop with vectorize.enable / unroll.enable).
// ============================================================================
class TetherAttrPass : public llvm::PassInfoMixin<TetherAttrPass> {
public:
    // Main entry point — called by the LLVM pass manager
    llvm::PreservedAnalyses run(llvm::Module &M,
                                llvm::ModuleAnalysisManager &AM);

    // Required for the pass to run even without analyses
    static bool isRequired() { return true; }
};

} // namespace tether
