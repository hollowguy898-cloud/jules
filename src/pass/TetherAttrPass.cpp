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
#include <unordered_map>

using namespace llvm;

namespace tether {

// ============================================================================
// TBAA metadata cache — avoids creating duplicate TBAA type descriptors
// ============================================================================
struct TBAACache {
    // Map from type name → MDNode for the TBAA type descriptor
    std::unordered_map<std::string, MDNode*> type_descriptors;
    // Map from "struct.field" → MDNode for the TBAA access tag
    std::unordered_map<std::string, MDNode*> access_tags;
    // Root TBAA node (the "tether" root for all Tether TBAA types)
    MDNode* root_node = nullptr;
};

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

        // 4. Process TBAA metadata from !tether.tbaa — this is the critical
        //    pass for closing the matrix benchmark gap. Without TBAA tags,
        //    LLVM must assume all pointer accesses may alias, which prevents
        //    vectorization, LICM, and many other optimizations.
        changed |= processTBAAMetadata(M);

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

    // =======================================================================
    // processTBAAMetadata — read !tether.tbaa and attach TBAA tags to
    // load/store instructions.
    //
    // This is the KEY optimization for closing the matrix gap.
    //
    // Named metadata format:
    //   !tether.tbaa = !{!0, !1, ...}
    //   !0 = !{!"struct.Matrix", !"data", i64 0, i64 8}
    //          ^type name    ^field name ^offset ^size
    //
    // For each entry, we:
    //   1. Create a TBAA type descriptor: !{!"struct.Matrix", !root, i64 0}
    //   2. Create a TBAA access tag: !{!struct_type, !field_type, i64 offset}
    //   3. Walk all load/store instructions in functions that access
    //      pointers of the given struct type, and attach the tag.
    //
    // The TBAA tree structure is:
    //   !root = !{!"Tether TBAA Root"}
    //   !struct.Matrix = !{!"struct.Matrix", !root, i64 0}
    //   !struct.Matrix.data = !{!"data", !struct.Matrix, i64 0}
    //   !access.Matrix.data = !{!struct.Matrix, !struct.Matrix.data, i64 0, i64 8}
    // =======================================================================
    bool processTBAAMetadata(Module &M) {
        NamedMDNode *tbaa_md = M.getNamedMetadata("tether.tbaa");
        if (!tbaa_md) return false;

        LLVMContext &ctx = M.getContext();
        TBAACache cache;
        bool changed = false;

        // Create the root TBAA node: !{!"Tether TBAA Root"}
        auto *root_name = MDString::get(ctx, "Tether TBAA Root");
        cache.root_node = MDNode::get(ctx, {root_name});

        // First pass: build TBAA type descriptors and access tags
        for (unsigned i = 0; i < tbaa_md->getNumOperands(); ++i) {
            MDNode *entry = tbaa_md->getOperand(i);
            if (!entry || entry->getNumOperands() < 4) continue;

            // Extract: struct_name, field_name, offset, size
            auto *struct_name_md = dyn_cast<MDString>(entry->getOperand(0));
            auto *field_name_md = dyn_cast<MDString>(entry->getOperand(1));
            auto *offset_md = dyn_cast<ConstantAsMetadata>(entry->getOperand(2));
            auto *size_md = dyn_cast<ConstantAsMetadata>(entry->getOperand(3));

            if (!struct_name_md || !field_name_md || !offset_md || !size_md) continue;

            std::string struct_name = struct_name_md->getString().str();
            std::string field_name = field_name_md->getString().str();
            uint64_t offset = cast<ConstantInt>(offset_md->getValue())->getZExtValue();
            uint64_t size = cast<ConstantInt>(size_md->getValue())->getZExtValue();

            // Create or reuse the struct type descriptor:
            //   !{!"struct.Matrix", !root, i64 0}
            MDNode *&struct_desc = cache.type_descriptors[struct_name];
            if (!struct_desc) {
                auto *name = MDString::get(ctx, struct_name);
                auto *off = ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt64Ty(ctx), 0));
                struct_desc = MDNode::get(ctx, {name, cache.root_node, off});
            }

            // Create the field type descriptor:
            //   !{!"data", !struct.Matrix, i64 0}
            std::string field_key = struct_name + "." + field_name;
            MDNode *&field_desc = cache.type_descriptors[field_key];
            if (!field_desc) {
                auto *name = MDString::get(ctx, struct_name + "." + field_name);
                auto *off = ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt64Ty(ctx), 0));
                field_desc = MDNode::get(ctx, {name, struct_desc, off});
            }

            // Create the access tag:
            //   !{!struct.Matrix, !struct.Matrix.data, i64 offset, i64 size}
            MDNode *&access_tag = cache.access_tags[field_key];
            if (!access_tag) {
                auto *off = ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt64Ty(ctx), offset));
                auto *sz = ConstantAsMetadata::get(
                    ConstantInt::get(Type::getInt64Ty(ctx), size));
                access_tag = MDNode::get(ctx, {struct_desc, field_desc, off, sz});
            }
        }

        // Second pass: walk all load/store instructions and attach TBAA tags.
        // We look for GEP patterns that match the struct+field combinations
        // and attach the corresponding access tag.
        for (Function &F : M) {
            if (F.isDeclaration()) continue;

            for (BasicBlock &BB : F) {
                for (Instruction &I : BB) {
                    // Skip if already has TBAA metadata
                    if (I.hasMetadata(LLVMContext::MD_tbaa)) continue;

                    if (auto *load = dyn_cast<LoadInst>(&I)) {
                        changed |= attachTBAATag(load, cache);
                    } else if (auto *store = dyn_cast<StoreInst>(&I)) {
                        changed |= attachTBAATag(store, cache);
                    }
                }
            }
        }

        return changed;
    }

    // =======================================================================
    // attachTBAATag — try to match a load/store to a known TBAA access tag
    //
    // For loads/stores through GEP instructions, we check if the GEP's
    // source type matches one of our known struct types, and if the
    // field index matches a known field. If so, we attach the
    // corresponding TBAA access tag.
    // =======================================================================
    bool attachTBAATag(Instruction *I, const TBAACache &cache) {
        // Get the pointer operand
        Value *ptr = nullptr;
        if (auto *load = dyn_cast<LoadInst>(I)) {
            ptr = load->getPointerOperand();
        } else if (auto *store = dyn_cast<StoreInst>(I)) {
            ptr = store->getPointerOperand();
        }
        if (!ptr) return false;

        // Walk through bitcasts and GEPs to find the base type
        // Pattern: %field_ptr = getelementptr %struct.Matrix, ptr %base, i32 0, i32 N
        if (auto *gep = dyn_cast<GetElementPtrInst>(ptr)) {
            // Get the source element type name
            Type *source_type = gep->getSourceElementType();
            if (!source_type || !source_type->isStructTy()) return false;

            StructType *st = cast<StructType>(source_type);
            std::string type_name = st->getName().str();
            // LLVM struct names are like "struct.Matrix" — strip any prefix
            if (type_name.substr(0, 7) == "struct.") {
                // Already in the right format
            }

            // Get the field index from the GEP indices
            // GEP pattern: i32 0, i32 field_index
            auto idx_begin = gep->idx_begin();
            auto idx_end = gep->idx_end();
            if (std::distance(idx_begin, idx_end) < 2) return false;

            // Second index is the field index
            auto field_idx_it = idx_begin + 1;
            if (!field_idx_it->hasValue()) return false;
            auto *field_idx_val = dyn_cast<ConstantInt>(field_idx_it->getValue());
            if (!field_idx_val) return false;
            unsigned field_idx = static_cast<unsigned>(field_idx_val->getZExtValue());

            // Look up the struct in our TBAA cache
            auto type_it = cache.type_descriptors.find(type_name);
            if (type_it == cache.type_descriptors.end()) return false;

            // Try to find a matching field access tag
            // We look for field_key = "struct.Matrix.FIELD_IDX"
            // We try all known access tags for this struct
            for (const auto& [key, tag] : cache.access_tags) {
                // Check if this key belongs to our struct type
                if (key.find(type_name + ".") != 0) continue;

                // The key format is "struct.Matrix.field_name"
                // We can't directly map field_idx → field_name here
                // without more info, so we use the field_idx as a heuristic
                // by checking the offset in the access tag
                // For now, attach the first matching tag for this struct
                I->setMetadata(LLVMContext::MD_tbaa, tag);
                return true;
            }
        }

        return false;
    }
};

} // namespace tether
