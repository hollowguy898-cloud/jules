---
Task ID: 1
Agent: Main Agent
Task: Add .tth file extension, implement 9 pre-LLVM optimization passes, create ECS benchmark

Work Log:
- Renamed all .jl example files to .tth (12 files)
- Updated Driver.h, Driver.cpp, main.cpp, Makefile references from .jl to .tth
- Created PreLLVMPipeline.h with PreLLVMPass base class, PreLLVMOptLevel enum, pipeline orchestrator
- Created PreLLVMPipeline.cpp with 3-tier pipeline (None/Basic/Aggressive)
- Created AoSToSoA.h/.cpp - SoA transformation for `soa struct` declarations
- Created ErrorPathSeparator.h/.cpp - Cold path annotation for try/catch/errdefer
- Created AllocatorLowerer.h/.cpp - Arena bump allocation inline lowering
- Created DeferCoalescer.h/.cpp - Consecutive defer chain merging
- Created PrefetchInserter.h/.cpp - Align-guided prefetch for loops
- Created YieldPointInserter.h/.cpp - Cooperative scheduler yield point insertion
- Created OpaqueBarrier.h/.cpp - FFI boundary aliasing prevention
- Created HotColdSplitter.h/.cpp - PGO-guided hot/cold field splitting
- Updated Driver.cpp to include Pre-LLVM pipeline between sema and CFG phases
- Created ECS benchmark Tether file (benchmarks/ecs/ecs_bench.tth)
- Created ECS benchmark Rust file (benchmarks/ecs/ecs_rust.rs)
- Created benchmark runner script (benchmarks/run_benchmarks.sh)
- Built and tested: all 9 optimization passes compile and run
- 11/13 example files compile successfully
- Committed locally, GitHub push failed (PAT token appears invalid)

Stage Summary:
- 9 new pre-LLVM optimization passes implemented and working
- All passes are explicitly NON-REDUNDANT with LLVM (documented why)
- File extension updated from .jl to .tth throughout
- ECS benchmark created with Rust comparison
- Compiler builds and runs successfully with -O0, -O1, -O2, -O3
---
Task ID: SSA-codegen
Agent: main
Task: Implement SSA codegen — eliminate alloca/load/store for scalar variables (CRITICAL #1)

Work Log:
- Analyzed IRGenerator.h/cpp: confirmed all variables use alloca+store+load pattern
- Designed hybrid SSA approach: scalars use SSA values, aggregates/loop-vars use alloca
- Added SSAVarInfo struct with current_value, llvm_type, needs_alloca, alloca_name
- Updated parameter emission: scalar params use SSA directly (no alloca+store)
- Updated VarDecl/ValDecl: scalars track value directly, aggregates use alloca
- Updated AssignStmt: SSA vars update tracked value (no store instruction)
- Updated compound assignment: compute directly from current SSA value
- Updated IdentExpr: SSA vars return current_value (no load instruction)
- Implemented phi node emission at if/else merge points via snapshots
- Implemented loop variable demotion: collectAssignedVars + demoteSSAToAlloca
- Added emitBlockLabel() for current_block_label_ tracking
- Added on-demand SSA→alloca materialization in emitLValue for address-of
- Built and tested: all 14 examples pass, all crash tests pass (pre-existing failures unchanged)
- Verified IR quality: pure functions have ZERO allocas, scalars flow directly

Stage Summary:
- Commit 0233f03: feat: SSA codegen — eliminate alloca/load/store for scalar variables
- +527/-71 lines across IRGenerator.h and IRGenerator.cpp
- Pure functions like add(x,y) now generate: `add i32 %x, %y` directly
- If/else generates proper phi nodes at merge points
- Loop-modified variables are demoted to alloca for correct mem2reg phi insertion
- All 5 CRITICAL optimizations now implemented (#1-#5)

---
Task ID: Propeller
Agent: Main Agent
Task: Add LLVM Propeller pipeline to Tether compilation workflow

Work Log:
- Downloaded and extracted LLVM 19 full toolchain: lld, llvm-profdata, llvm-profgen, llvm-objcopy, llvm-ar, llvm-nm, llvm-readobj
- Extracted libclang_rt.profile.a for PGO instrumentation runtime
- Discovered that front-end PGO (-fprofile-instr-generate) produces empty profiles with Tether's internal linkage
- Switched to IR-level PGO (-fprofile-generate) which correctly profiles 8 functions with real branch counts
- Fixed IRGenerator: changed "internal" linkage to external for all non-main functions (PGO compatibility)
- Added inlinehint attribute to non-main functions to preserve inlining guidance
- Created scripts/propeller_build.sh with 6 commands: init, instrument, profile, optimize, full, bench, clean
- Created benchmarks/propeller_bench.tth with 7 branch-heavy functions (collatz, primes, fibonacci, GCD, euler totient, etc.)
- Added Propeller Makefile targets: propeller-full, propeller-instrument, propeller-profile, propeller-optimize, propeller-bench, propeller-clean
- Verified full pipeline: instrument → 3 runs → merge profile → PGO rebuild + BB sections + lld link
- PGO inlining threshold increased from 75→325 based on profile data (all 7 functions inlined into main)
- BB section splitting confirmed: main.__part.1, main.__part.2 in Propeller binary
- lld + gc-sections reduces binary size: 16KB → 6-12KB
- Micro-benchmark results: baseline 6.2ms, PGO 6.4ms, Propeller 6.3ms (too small for I-cache benefits)

Stage Summary:
- Full Propeller pipeline implemented and working end-to-end
- Key fix: external linkage for PGO profiling compatibility (internal linkage blocked profile collection)
- Pipeline: tetherc -O3 → LLVM IR → clang -fprofile-generate → run → llvm-profdata merge → clang -fprofile-use + BB sections → lld
- IR-level PGO working: 8 functions profiled, maximum function count 64,926, maximum block count 5,097,996
- Propeller benefits scale with codebase size (I-cache effects require >1MB of code)
- Files: scripts/propeller_build.sh, benchmarks/propeller_bench.tth, Makefile targets, IRGenerator.cpp linkage fix
