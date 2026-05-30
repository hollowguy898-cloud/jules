#!/usr/bin/env python3
"""Compile and run Tether micro-benchmarks with proper IR fixup for llvmlite."""

import subprocess
import time
import os
import sys
import tempfile
import re

TETHERC = "/home/z/my-project/jules/tetherc"
BENCH_DIR = "/home/z/my-project/jules/benchmarks/micro"
TMPDIR = tempfile.mkdtemp(prefix="tether_bench_")
ITERS = 3

BENCHMARKS = [
    ("int_div", "int_div.tth"),
    ("loop_overhead", "loop_overhead.tth"),
    ("branching", "branching.tth"),
    ("float_math", "float_math.tth"),
    ("int_arith", "int_arith.tth"),
]

def run_cmd(cmd, timeout=30):
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return r.returncode == 0, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return False, "", "timeout"

def fix_ir_for_llvmlite(ir_text):
    """Fix LLVM IR for llvmlite (LLVM 15, typed pointers, no opaque ptr)."""
    
    lines = ir_text.split('\n')
    fixed_lines = []
    alloca_types = {}  # %name -> pointee_type (e.g., "i64", "%struct.Point")
    
    # First pass: collect alloca types
    for line in lines:
        # Match: %name = alloca <type>
        m = re.match(r'\s+(%\w+)\s*=\s*alloca\s+(.+?)\s*$', line.strip())
        if m:
            name = m.group(1)
            alloca_type = m.group(2).strip()
            alloca_types[name] = alloca_type
    
    # Second pass: fix `ptr` references
    for line in lines:
        fixed = line
        
        # Fix alloca lines: alloca ptr -> alloca <type> (shouldn't happen but just in case)
        # These are already correct since Tether emits `alloca i64` etc.
        
        # Fix store: store <type> <val>, ptr <addr> -> store <type> <val>, <type>* <addr>
        m = re.match(r'(\s+)store\s+(\S+)\s+(%\w+),\s*ptr\s+(%\w+)(.*)', line)
        if m:
            indent = m.group(1)
            val_type = m.group(2)
            val = m.group(3)
            addr = m.group(4)
            rest = m.group(5)
            ptr_type = alloca_types.get(addr, 'i8')
            fixed = f'{indent}store {val_type} {val}, {ptr_type}* {addr}{rest}'
        
        # Fix load: <reg> = load <type>, ptr <addr> -> <reg> = load <type>, <type>* <addr>
        m = re.match(r'(\s+)(%\w+)\s*=\s*load\s+(\S+),\s*ptr\s+(%\w+)(.*)', line)
        if m:
            indent = m.group(1)
            reg = m.group(2)
            load_type = m.group(3)
            addr = m.group(4)
            rest = m.group(5)
            ptr_type = alloca_types.get(addr, 'i8')
            fixed = f'{indent}{reg} = load {load_type}, {ptr_type}* {addr}{rest}'
        
        # Fix GEP: getelementptr ptr <addr>, ... -> getelementptr <type>* <addr>, ...
        m = re.match(r'(\s+)(%\w+)\s*=\s*getelementptr\s+ptr\s+(%\w+)(.*)', line)
        if m:
            indent = m.group(1)
            reg = m.group(2)
            addr = m.group(3)
            rest = m.group(4)
            # Determine the base type for GEP
            if addr in alloca_types:
                base_type = alloca_types[addr]
                # If it's a struct, use the struct type for GEP
                if base_type.startswith('%struct.'):
                    fixed = f'{indent}{reg} = getelementptr {base_type}, {base_type}* {addr}{rest}'
                else:
                    fixed = f'{indent}{reg} = getelementptr {base_type}, {base_type}* {addr}{rest}'
            else:
                fixed = f'{indent}{reg} = getelementptr i8, i8* {addr}{rest}'
        
        # Fix function declarations with ptr parameters
        # declare void @tether_yield(i64) nounwind willreturn memory(none)  — no ptr, OK
        # define dso_local i32 @main(ptr %err_slot) -> define dso_local i32 @main(i8* %err_slot)
        
        # Fix remaining `ptr ` that aren't part of alloca/store/load/GEP
        # These are typically in function parameters
        if 'define ' in fixed and 'ptr ' in fixed:
            fixed = fixed.replace('ptr ', 'i8* ')
        if 'declare ' in fixed and 'ptr ' in fixed:
            fixed = fixed.replace('ptr ', 'i8* ')
        if 'call ' in fixed and 'ptr ' in fixed:
            fixed = fixed.replace('ptr ', 'i8* ')
        
        # Fix memcpy/memset intrinsic calls
        if 'llvm.memcpy' in fixed or 'llvm.memset' in fixed:
            fixed = fixed.replace('ptr ', 'i8* ')
        
        # Remove any remaining standalone `ptr` that we might have missed
        # (Be careful not to break struct type definitions)
        if not fixed.strip().startswith('%struct.'):
            # Replace `ptr` at word boundaries only
            fixed = re.sub(r'\bptr\b', 'i8*', fixed)
        
        fixed_lines.append(fixed)
    
    return '\n'.join(fixed_lines)

def emit_ir(tth_file, opt_level):
    out_file = os.path.join(TMPDIR, f"bench_O{opt_level}.ll")
    ok, _, err = run_cmd(f"{TETHERC} --emit-ir -O{opt_level} {tth_file} -o {out_file}")
    if not ok:
        print(f"  Emit IR error: {err.strip()[:200]}")
        return None
    return out_file

def compile_ir_with_llvmlite(ir_file):
    try:
        import llvmlite.binding as llvm
        llvm.initialize()
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        
        with open(ir_file, 'r') as f:
            ir_text = f.read()
        
        # Fix IR for llvmlite
        ir_text = fix_ir_for_llvmlite(ir_text)
        
        # Write fixed IR for debugging
        fixed_file = ir_file.replace('.ll', '_fixed.ll')
        with open(fixed_file, 'w') as f:
            f.write(ir_text)
        
        mod = llvm.parse_assembly(ir_text)
        mod.verify()
        
        # Create target machine
        target = llvm.Target.from_default_triple()
        target_machine = target.create_target_machine(opt=3)
        
        # Run optimization passes
        pm = llvm.ModulePassManager()
        pm.add_constant_merge_pass()
        pm.add_instruction_combining_pass()
        pm.add_reassociate_expressions_pass()
        pm.add_gvn_pass()
        pm.add_cfg_simplification_pass()
        pm.add_licm_pass()
        pm.add_loop_unroll_pass()
        pm.add_sroa_pass()
        pm.add_dead_code_elimination_pass()
        pm.add_function_inlining_pass(275)  # threshold
        pm.add_global_dce_pass()
        pm.add_aggressive_dead_code_elimination_pass()
        pm.add_tail_call_elimination_pass()
        pm.add_loop_deletion_pass()
        pm.add_loop_rotate_pass()
        pm.add_loop_simplification_pass()
        pm.add_instruction_combining_pass()
        pm.add_reassociate_expressions_pass()
        pm.add_gvn_pass()
        pm.add_cfg_simplification_pass()
        pm.add_dead_code_elimination_pass()
        pm.run(mod)
        
        # Generate object code
        obj_data = target_machine.emit_object(mod)
        obj_file = os.path.join(TMPDIR, "bench.o")
        with open(obj_file, 'wb') as f:
            f.write(obj_data)
        
        return obj_file
    except Exception as e:
        err_str = str(e)
        if len(err_str) > 300:
            err_str = err_str[:300] + "..."
        print(f"  llvmlite error: {err_str}")
        # Show first few lines of fixed IR for debugging
        fixed_file = ir_file.replace('.ll', '_fixed.ll')
        if os.path.exists(fixed_file):
            with open(fixed_file) as f:
                lines = f.readlines()
                for i, line in enumerate(lines[:40]):
                    print(f"  {i+1:4d}: {line.rstrip()}")
        return None

def compile_to_exe(obj_file, name):
    exe_file = os.path.join(TMPDIR, name)
    # Provide a stub for tether_yield
    yield_stub = os.path.join(TMPDIR, "yield_stub.c")
    if not os.path.exists(yield_stub):
        with open(yield_stub, 'w') as f:
            f.write('void tether_yield(unsigned long x) { (void)x; }\n')
    ok, out, err = run_cmd(f"gcc -O3 -no-pie -o {exe_file} {obj_file} {yield_stub} -lm")
    if ok:
        return exe_file
    print(f"  gcc link error: {err.strip()[:200]}")
    return None

def time_benchmark(exe_file):
    times = []
    for _ in range(ITERS):
        start = time.perf_counter()
        ok, stdout, stderr = run_cmd(exe_file, timeout=60)
        elapsed = (time.perf_counter() - start) * 1000
        if not ok:
            return None
        times.append(elapsed)
    times.sort()
    return times[len(times) // 2]

def write_c_program(name):
    c_programs = {
        "int_div": """int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        sum += i / 7;
        sum += i / 100;
        sum += i % 256;
    }
    return 0;
}""",
        "loop_overhead": """int main() {
    long i = 0;
    while (i < 1000000000) { i++; }
    return 0;
}""",
        "branching": """int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        if (i % 2 == 0) sum += 1;
        else sum -= 1;
    }
    return 0;
}""",
        "float_math": """int main() {
    double sum = 0.0;
    for (long i = 0; i < 100000000; i++) {
        double fi = (double)i;
        sum += fi * fi + fi / 3.14159;
    }
    return 0;
}""",
        "int_arith": """int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        sum += i * 3 + i - i / 2 + (i & 0xFF);
    }
    return 0;
}""",
    }
    return c_programs.get(name)

def compile_c(c_code, name):
    c_file = os.path.join(TMPDIR, f"{name}_c.c")
    exe_file = os.path.join(TMPDIR, f"{name}_c")
    with open(c_file, 'w') as f:
        f.write(c_code)
    ok, _, err = run_cmd(f"gcc -O3 -o {exe_file} {c_file} -lm")
    if ok:
        return exe_file
    print(f"  C compile error: {err}")
    return None

def main():
    print("=" * 80)
    print("Tether Micro-Benchmark Suite (Post-Fix Validation)")
    print("=" * 80)
    print()
    
    results = {}
    
    for name, filename in BENCHMARKS:
        tth_file = os.path.join(BENCH_DIR, filename)
        if not os.path.exists(tth_file):
            print(f"SKIP {name}: not found")
            continue
        
        print(f"--- {name} ---")
        result = {"name": name}
        
        for opt in [2, 3]:
            ir_file = emit_ir(tth_file, opt)
            if ir_file:
                with open(ir_file) as f:
                    ir = f.read()
                key = f"ir_o{opt}"
                print(f"  IR(O{opt}): sdiv={ir.count('sdiv')} srem={ir.count('srem')} and={ir.count('and')} select={ir.count('select')} i128={ir.count('i128')}")
                result[key] = ir_file
                
                print(f"  Compiling Tether O{opt} via llvmlite...")
                obj = compile_ir_with_llvmlite(ir_file)
                if obj:
                    exe = compile_to_exe(obj, f"{name}_O{opt}")
                    if exe:
                        ms = time_benchmark(exe)
                        if ms is not None:
                            result[f'tether_o{opt}_ms'] = ms
                            print(f"  Tether O{opt}: {ms:.1f} ms")
                        else:
                            print(f"  Tether O{opt}: FAILED to run")
                    else:
                        print(f"  Tether O{opt}: FAILED to link")
                else:
                    print(f"  Tether O{opt}: FAILED to compile")
        
        # Compile and time C baseline
        c_code = write_c_program(name)
        if c_code:
            c_exe = compile_c(c_code, name)
            if c_exe:
                ms = time_benchmark(c_exe)
                if ms is not None:
                    result['c_ms'] = ms
                    print(f"  C (gcc -O3): {ms:.1f} ms")
        
        results[name] = result
        print()
    
    # Summary
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"{'Benchmark':<16} {'Tether O3':>12} {'Tether O2':>12} {'C (gcc O3)':>12} {'O3/C':>8} {'O2/C':>8}")
    print("-" * 72)
    for name, r in results.items():
        o3 = f"{r['tether_o3_ms']:.1f}" if 'tether_o3_ms' in r else "FAIL"
        o2 = f"{r['tether_o2_ms']:.1f}" if 'tether_o2_ms' in r else "FAIL"
        c = f"{r['c_ms']:.1f}" if 'c_ms' in r else "N/A"
        o3c = f"{r['tether_o3_ms'] / r['c_ms']:.2f}x" if 'tether_o3_ms' in r and 'c_ms' in r and r['c_ms'] > 0 else "N/A"
        o2c = f"{r['tether_o2_ms'] / r['c_ms']:.2f}x" if 'tether_o2_ms' in r and 'c_ms' in r and r['c_ms'] > 0 else "N/A"
        print(f"{name:<16} {o3:>12} {o2:>12} {c:>12} {o3c:>8} {o2c:>8}")
    
    print()
    print("Fixes applied:")
    print("  1. Strength reduction REMOVED for non-power-of-2 div/mod")
    print("  2. Power-of-2 modulo uses AND at O3 (even for signed)")
    print("  3. Select type mismatch FIXED — proper type casts")

if __name__ == "__main__":
    main()
