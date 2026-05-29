// ============================================================================
// TetherAttrPass.cpp — Custom LLVM pass plugin for Tether compiler
//
// This pass reads Tether-specific metadata from the LLVM IR module and
// injects corresponding LLVM optimization attributes. It acts as the bridge
// between Tether's rich semantic analysis (MetadataMap) and LLVM's
// optimization passes.
//
// The metadata is emitted by IRGenerator::emitTetherFnMetadata() and
// consumed here after LLVM parses the .ll file.
// ============================================================================

#include "pass/TetherAttrPass.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/Attributes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Passes/PassBuilder.h"
#include "llvm/Passes/PassPlugin.h"
#include "llvm/Support/raw_ostream.h"

#include <string>
#include <vector>

using namespace llvm;

namespace tether {

// ============================================================================
// TetherAttrPass — the actual pass implementation
//
// Uses the new LLVM pass manager (PassInfoMixin).
// Operates on the module level to read named metadata and apply attributes.
// ============================================================================
class TetherAttrPass : public PassInfoMixin<TetherAttrPass> {
public:
    // Main entry point — called by the pass manager
    PreservedAnalyses run(Module &M, ModuleAnalysisManager &AM) {
        bool changed = false;

        // 1. Process function-level metadata from !tether.fns
        changed |= processFunctionMetadata(M);

        // 2. Process parameter-level metadata from !tether.params
        changed |= processParameterMetadata(M);

        // 3. Process loop metadata (vectorize/unroll hints)
        changed |= processLoopMetadata(M);

        return changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
    }

    // Required for the pass to run even if no analyses are requested
    static bool isRequired() { return true; }

private:
    // =======================================================================
    // processFunctionMetadata — read !tether.fns and apply function attrs
    //
    // Named metadata format:
    //   !tether.fns = !{!0, !1, ...}
    //   !0 = !{!"function_name", i32 flags}
    //
    // Flags bitmask:
    //   bit0 (1)   = noalias
    //   bit1 (2)   = readonly
    //   bit2 (4)   = hot
    //   bit3 (8)   = cold
    //   bit4 (16)  = pure (memory(none))
    //   bit5 (32)  = noalloc (willreturn + nosync)
    //   bit6 (64)  = vectorize (loop)
    //   bit7 (128) = unroll (loop)
    //   bit8 (256) = invariant_load
    // =======================================================================
    bool processFunctionMetadata(Module &M) {
        NamedMDNode *fns_md = M.getNamedMetadata("tether.fns");
        if (!fns_md) return false;

        bool changed = false;

        for (unsigned i = 0; i < fns_md->getNumOperands(); ++i) {
            MDNode *fn_md = fns_md->getOperand(i);
            if (!fn_md || fn_md->getNumOperands() < 2) continue;

            // Extract function name
            auto *name_md = dyn_cast<MDString>(fn_md->getOperand(0));
            if (!name_md) continue;
            std::string fn_name = name_md->getString().str();

            // Extract flags bitmask
            auto *flags_md = dyn_cast<ConstantAsMetadata>(fn_md->getOperand(1));
            if (!flags_md) continue;
            auto *flags_const = dyn_cast<ConstantInt>(flags_md->getValue());
            if (!flags_const) continue;
            uint32_t flags = static_cast<uint32_t>(flags_const->getZExtValue());

            // Find the LLVM function by name
            Function *F = M.getFunction(fn_name);
            if (!F) {
                errs() << "TetherAttrPass: warning: function '" << fn_name
                       << "' not found in module (metadata ignored)\n";
                continue;
            }

            // Apply attributes based on flags
            changed |= applyFunctionAttrs(F, flags);
        }

        return changed;
    }

    // =======================================================================
    // processParameterMetadata — read !tether.params and apply param attrs
    //
    // Named metadata format:
    //   !tether.params = !{!0, !1, ...}
    //   !0 = !{!"function_name", i32 param_index, i32 param_flags}
    //
    // Parameter flags bitmask:
    //   bit0 (1)  = noalias
    //   bit1 (2)  = readonly
    //   bit2 (4)  = nonnull
    //   bit3 (8)  = invariant_load
    // =======================================================================
    bool processParameterMetadata(Module &M) {
        NamedMDNode *params_md = M.getNamedMetadata("tether.params");
        if (!params_md) return false;

        bool changed = false;

        for (unsigned i = 0; i < params_md->getNumOperands(); ++i) {
            MDNode *param_md = params_md->getOperand(i);
            if (!param_md || param_md->getNumOperands() < 3) continue;

            // Extract function name
            auto *name_md = dyn_cast<MDString>(param_md->getOperand(0));
            if (!name_md) continue;
            std::string fn_name = name_md->getString().str();

            // Extract parameter index
            auto *idx_md = dyn_cast<ConstantAsMetadata>(param_md->getOperand(1));
            if (!idx_md) continue;
            auto *idx_const = dyn_cast<ConstantInt>(idx_md->getValue());
            if (!idx_const) continue;
            unsigned param_idx = static_cast<unsigned>(idx_const->getZExtValue());

            // Extract flags bitmask
            auto *flags_md = dyn_cast<ConstantAsMetadata>(param_md->getOperand(2));
            if (!flags_md) continue;
            auto *flags_const = dyn_cast<ConstantInt>(flags_md->getValue());
            if (!flags_const) continue;
            uint32_t flags = static_cast<uint32_t>(flags_const->getZExtValue());

            // Find the LLVM function by name
            Function *F = M.getFunction(fn_name);
            if (!F) {
                errs() << "TetherAttrPass: warning: function '" << fn_name
                       << "' not found in module (param metadata ignored)\n";
                continue;
            }

            // Apply parameter attributes
            changed |= applyParameterAttrs(F, param_idx, flags);
        }

        return changed;
    }

    // =======================================================================
    // processLoopMetadata — read !tether.loops and apply loop hints
    //
    // Named metadata format:
    //   !tether.loops = !{!0, !1, ...}
    //   !0 = !{!"function_name", i32 loop_flags}
    //
    // Loop flags:
    //   bit6 (64)  = vectorize
    //   bit7 (128) = unroll
    // =======================================================================
    bool processLoopMetadata(Module &M) {
        NamedMDNode *loops_md = M.getNamedMetadata("tether.loops");
        if (!loops_md) return false;

        bool changed = false;

        for (unsigned i = 0; i < loops_md->getNumOperands(); ++i) {
            MDNode *loop_md = loops_md->getOperand(i);
            if (!loop_md || loop_md->getNumOperands() < 2) continue;

            // Extract function name
            auto *name_md = dyn_cast<MDString>(loop_md->getOperand(0));
            if (!name_md) continue;
            std::string fn_name = name_md->getString().str();

            // Extract loop flags
            auto *flags_md = dyn_cast<ConstantAsMetadata>(loop_md->getOperand(1));
            if (!flags_md) continue;
            auto *flags_const = dyn_cast<ConstantInt>(flags_md->getValue());
            if (!flags_const) continue;
            uint32_t flags = static_cast<uint32_t>(flags_const->getZExtValue());

            bool want_vectorize = (flags & kFnVectorize) != 0;
            bool want_unroll = (flags & kFnUnroll) != 0;

            if (!want_vectorize && !want_unroll) continue;

            // Find the LLVM function by name
            Function *F = M.getFunction(fn_name);
            if (!F) continue;

            // Find backedge branch instructions (loop latches) and attach
            // !llvm.loop metadata. We look for conditional branches that
            // jump backward (to a lower-numbered basic block), which is
            // the typical loop backedge pattern in Tether-generated IR.
            for (BasicBlock &BB : *F) {
                Instruction *term = BB.getTerminator();
                auto *br = dyn_cast<BranchInst>(term);
                if (!br || !br->isConditional()) continue;

                // Check if this branch is a loop backedge
                // Heuristic: one of the successors is a block that appears
                // before the current block in function layout order.
                bool is_backedge = false;
                for (unsigned s = 0; s < br->getNumSuccessors(); ++s) {
                    BasicBlock *succ = br->getSuccessor(s);
                    // Check if successor comes before current block
                    // (typical for loop headers)
                    if (&*F->begin() == succ ||
                        succ->comesBefore(&BB)) {
                        is_backedge = true;
                        break;
                    }
                }

                if (!is_backedge) continue;

                // Don't overwrite existing !llvm.loop metadata
                if (br->hasMetadata(LLVMContext::MD_loop)) continue;

                // Create the !llvm.loop metadata node
                LLVMContext &ctx = M.getContext();
                MDNode *loop_id = createLoopMetadata(ctx, want_vectorize,
                                                     want_unroll);
                if (loop_id) {
                    br->setMetadata(LLVMContext::MD_loop, loop_id);
                    changed = true;
                }

                // Only annotate the first backedge per function.
                // More sophisticated analysis could annotate each loop
                // separately using loop info.
                break;
            }
        }

        return changed;
    }

    // =======================================================================
    // applyFunctionAttrs — apply LLVM function attributes based on flags
    //
    // Only adds attributes that don't already exist on the function.
    // This avoids duplicate attribute warnings from LLVM.
    // =======================================================================
    bool applyFunctionAttrs(Function *F, uint32_t flags) {
        bool changed = false;
        AttrBuilder builder(F->getContext());

        // noalias — function return value doesn't alias any other pointer
        if (flags & kFnNoAlias) {
            if (!F->hasFnAttribute(Attribute::NoAlias)) {
                builder.addAttribute(Attribute::NoAlias);
            }
        }

        // readonly — function only reads from memory
        if (flags & kFnReadOnly) {
            if (!F->hasFnAttribute(Attribute::ReadOnly)) {
                builder.addAttribute(Attribute::ReadOnly);
            }
        }

        // hot — function is on a hot execution path
        if (flags & kFnHot) {
            if (!F->hasFnAttribute(Attribute::Hot)) {
                builder.addAttribute(Attribute::Hot);
            }
        }

        // cold — function is on a cold execution path
        if (flags & kFnCold) {
            if (!F->hasFnAttribute(Attribute::Cold)) {
                builder.addAttribute(Attribute::Cold);
            }
        }

        // pure — function has no side effects (memory(none))
        if (flags & kFnPure) {
            if (!F->hasFnAttribute(Attribute::ReadNone)) {
                // memory(none) is the modern way to express purity
                builder.addAttribute(Attribute::Memory,
                                     MemoryEffects::none().encode());
            }
        }

        // noalloc — function doesn't allocate and will return
        // Expressed as willreturn + nosync in LLVM
        if (flags & kFnNoAlloc) {
            if (!F->hasFnAttribute(Attribute::WillReturn)) {
                builder.addAttribute(Attribute::WillReturn);
            }
            if (!F->hasFnAttribute(Attribute::NoSync)) {
                builder.addAttribute(Attribute::NoSync);
            }
        }

        if (builder.hasAttributes()) {
            F->addFnAttrs(builder);
            changed = true;
        }

        return changed;
    }

    // =======================================================================
    // applyParameterAttrs — apply LLVM parameter attributes based on flags
    //
    // Only adds attributes that don't already exist on the parameter.
    // =======================================================================
    bool applyParameterAttrs(Function *F, unsigned param_idx, uint32_t flags) {
        if (param_idx >= F->arg_size()) return false;

        bool changed = false;
        AttrBuilder builder(F->getContext());

        // noalias — parameter pointer doesn't alias other pointers
        if (flags & kParamNoAlias) {
            if (!F->hasParamAttribute(param_idx, Attribute::NoAlias)) {
                builder.addAttribute(Attribute::NoAlias);
            }
        }

        // readonly — parameter pointer is only read through
        if (flags & kParamReadOnly) {
            if (!F->hasParamAttribute(param_idx, Attribute::ReadOnly)) {
                builder.addAttribute(Attribute::ReadOnly);
            }
        }

        // nonnull — parameter pointer is never null
        if (flags & kParamNonNull) {
            if (!F->hasParamAttribute(param_idx, Attribute::NonNull)) {
                builder.addAttribute(Attribute::NonNull);
            }
        }

        // invariant_load — loads from this parameter are invariant
        if (flags & kParamInvariantLoad) {
            if (!F->hasParamAttribute(param_idx, Attribute::InvariantLoad)) {
                builder.addAttribute(Attribute::InvariantLoad);
            }
        }

        if (builder.hasAttributes()) {
            F->addParamAttrs(param_idx, builder);
            changed = true;
        }

        return changed;
    }

    // =======================================================================
    // createLoopMetadata — create !llvm.loop metadata for vectorize/unroll
    //
    // Creates metadata like:
    //   !N = distinct !{!N, !{!"llvm.loop.vectorize.enable", i1 true}}
    // or:
    //   !N = distinct !{!N, !{!"llvm.loop.vectorize.enable", i1 true},
    //                    !{!"llvm.loop.unroll.enable", i1 true}}
    // =======================================================================
    MDNode *createLoopMetadata(LLVMContext &ctx, bool vectorize, bool unroll) {
        SmallVector<Metadata *, 4> operands;

        // Placeholder for self-reference (standard LLVM loop metadata idiom)
        operands.push_back(nullptr);

        if (vectorize) {
            // !{!"llvm.loop.vectorize.enable", i1 true}
            auto *key = MDString::get(ctx, "llvm.loop.vectorize.enable");
            auto *val = ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt1Ty(ctx), 1));
            operands.push_back(MDNode::get(ctx, {key, val}));
        }

        if (unroll) {
            // !{!"llvm.loop.unroll.enable", i1 true}
            auto *key = MDString::get(ctx, "llvm.loop.unroll.enable");
            auto *val = ConstantAsMetadata::get(
                ConstantInt::get(Type::getInt1Ty(ctx), 1));
            operands.push_back(MDNode::get(ctx, {key, val}));
        }

        // Create the distinct loop metadata node
        auto *loop_md = MDNode::getDistinct(ctx, operands);
        // Set the self-reference (standard idiom for !llvm.loop)
        loop_md->replaceOperandWith(0, loop_md);

        return loop_md;
    }
};

} // namespace tether
