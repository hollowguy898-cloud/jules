// ============================================================================
// PassPluginMain.cpp — Plugin entry point for TetherAttrPass
//
// Exports llvmGetPassPluginInfo() which is the standard entry point for
// LLVM pass plugins loaded via -fpass-plugin.
//
// When the Tether compiler invokes clang with:
//   clang -fpass-plugin /path/to/TetherAttrPass.so ...
// LLVM loads this shared library and calls llvmGetPassPluginInfo() to
// get the plugin registration information.
// ============================================================================

#include "pass/TetherAttrPass.h"

#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"

using namespace llvm;

namespace {

// ============================================================================
// registerTetherAttrPass — plugin registration callback
//
// This is called by LLVM when the plugin is loaded. It registers:
// 1. A pipeline parsing callback for the "tether-attr" pass name
// 2. A pipeline start callback that auto-inserts the pass at -O1+
// ============================================================================
void registerTetherAttrPass(PassBuilder &PB) {
    // Register the pass by name so it can be explicitly requested:
    //   clang -fpass-plugin TetherAttrPass.so -mllvm -passes=tether-attr
    PB.registerPipelineParsingCallback(
        [](StringRef Name, ModulePassManager &PM,
           ArrayRef<PassBuilder::PipelineElement>) {
            if (Name == "tether-attr") {
                PM.addPass(tether::TetherAttrPass());
                return true;
            }
            return false;
        });

    // Auto-insert at the start of the optimization pipeline for -O1 and
    // above. This ensures Tether metadata is always applied when using
    // the plugin, even without explicitly passing the pass name.
    PB.registerPipelineStartEPCallback(
        [](ModulePassManager &PM, OptimizationLevel Level) {
            if (Level != OptimizationLevel::O0) {
                PM.addPass(tether::TetherAttrPass());
            }
        });
}

} // anonymous namespace

// ============================================================================
// llvmGetPassPluginInfo — the plugin entry point
//
// This is the ONLY symbol that LLVM looks for when loading a pass plugin.
// It must have C linkage and return a PassPluginLibraryInfo struct.
// The LLVM_ATTRIBUTE_WEAK attribute allows the plugin to be loaded even
// if another plugin already defines this symbol.
// ============================================================================
extern "C" LLVM_ATTRIBUTE_WEAK ::llvm::PassPluginLibraryInfo
llvmGetPassPluginInfo() {
    return {
        LLVM_PLUGIN_API_VERSION,   // API version
        "TetherAttrPass",          // Plugin name
        LLVM_VERSION_STRING,       // LLVM version this plugin was built for
        registerTetherAttrPass     // Registration callback
    };
}
