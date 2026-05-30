#!/usr/bin/env python3
"""Compile and run Tether micro-benchmarks, fixing IR for llvmlite compatibility."""

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
    """Fix LLVM IR for llvmlite (LLVM 15, typed pointers)."""
    
    # 1. Collect all struct type definitions
    struct_types = {}
    for m in re.finditer(r'(%struct\.\w+)\s*=\s*type\s*\{([^}]+)\}', ir_text):
        struct_types[m.group(1)] = m.group(0)
    
    # 2. Replace `ptr` with appropriate typed pointers
    # For alloca: ptr %name -> <type>* %name (need to know the alloca type)
    # For function params: ptr %name -> i8* %name (safe default)
    # For stores/loads: we need to track the type
    
    # Simple approach: replace all `ptr` with `i8*` first
    # Then fix specific patterns where we know the type
    
    # Replace `ptr ` with `i8* ` in most contexts
    # But we need to be more careful with GEP, alloca, etc.
    
    # Actually, let's do a more targeted replacement:
    # - `alloca ptr` -> keep alloca but replace the ptr with type
    # - For alloca: `alloca <type>` -> `alloca <type>` (no ptr change needed for alloca itself)
    # - For store/load: `store <type> %val, ptr %addr` -> `store <type> %val, <type>* %addr`
    # - For function signatures: `ptr %name` -> `i8* %name`
    # - For GEP: `getelementptr ptr` -> `getelementptr i8*`  (complex)
    
    # Strategy: Use a regex-based approach to fix the most common patterns
    
    lines = ir_text.split('\n')
    fixed_lines = []
    alloca_types = {}  # name -> type for alloca'd variables
    
    for line in lines:
        # Track alloca types: "  %name = alloca i64" -> alloca_types["%name"] = "i64*"
        m = re.match(r'\s+(%\w+)\s*=\s*alloca\s+(\w+)', line)
        if m:
            alloca_types[m.group(1)] = m.group(2) + '*'
        
        # Track struct alloca types
        m = re.match(r'\s+(%\w+)\s*=\s*alloca\s+(%struct\.\w+)', line)
        if m:
            alloca_types[m.group(1)] = m.group(2) + '*'
        
        fixed_lines.append(line)
    
    ir_text = '\n'.join(fixed_lines)
    
    # Now replace `ptr` with typed pointers
    # Pattern: `ptr %name` in store/load/GEP -> use alloca type if known, else i8*
    def replace_ptr_with_type(match):
        prefix = match.group(1)
        name = match.group(2)
        suffix = match.group(3)
        if name in alloca_types:
            return prefix + alloca_types[name] + ' ' + name + suffix
        return prefix + 'i8* ' + name + suffix
    
    # Replace `ptr %name` patterns
    # But NOT in type definitions or struct definitions
    ir_text = re.sub(r'(\s+|,\s*)ptr\s+(%\w+)([,\s\n])', replace_ptr_with_type, ir_text)
    
    # Replace `ptr ` in function return types and parameter types
    # define i32 @main(ptr %err_slot) -> define i32 @main(i8* %err_slot)
    ir_text = re.sub(r'define\s+(\w+)\s+@(\w+)\s*\(([^)]*)\)', 
                     lambda m: m.group(0).replace('ptr ', 'i8* '), 
                     ir_text)
    
    # Replace `ptr` in call arguments
    ir_text = re.sub(r'call\s+(\w+)\s+@(\w+)\s*\(([^)]*)\)',
                     lambda m: m.group(0).replace('ptr ', 'i8* '),
                     ir_text)
    
    # Replace remaining `ptr ` with `i8* `
    ir_text = re.sub(r'\bptr\b', 'i8*', ir_text)
    
    # Fix double pointers: i8** is fine
    # Fix: store i64 %val, i64* %addr (not i8* %addr)
    # This is handled by the alloca_types mapping above
    
    return ir_text

def emit_ir(tth_file, opt_level):
    out_file = os.path.join(TMPDIR, f"bench_O{opt_level}.ll")
    ok, _, err = run_cmd(f"{TETHERC} --emit-ir -O{opt_level} {tth_file} -o {out_file}")
    if not ok:
        print(f"  Emit IR error: {err}")
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
        # Add standard O3 passes manually
        pm.add_constant_merge_pass()
        pm.add_instruction_combining_pass()
        pm.add_reassociate_expressions_pass()
        pm.add_gvn_pass()
        pm.add_cfg_simplification_pass()
        pm.add_licm_pass()
        pm.add_loop_unroll_pass()
        pm.add_sroa_pass()
        pm.add_dead_code_elimination_pass()
        pm.add_function_inlining_pass()
        pm.add_global_dce_pass()
        pm.add_aggressive_dead_code_elimination_pass()
        pm.add_tail_call_elimination_pass()
        pm.add_scalar_evolution_aa_pass()
        pm.add_loop_deletion_pass()
        pm.add_loop_rotate_pass()
        pm.add_loop_simplification_pass()
        pm.add_scalar_evolution_aa_pass()
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
        print(f"  llvmlite error: {e}")
        # Try to show the fixed IR for debugging
        if 'parse_assembly' in str(e) or 'parsing error' in str(e).lower():
            fixed_file = ir_file.replace('.ll', '_fixed.ll')
            if os.path.exists(fixed_file):
                with open(fixed_file) as f:
                    lines = f.readlines()
                    # Show first 50 lines
                    for i, line in enumerate(lines[:50]):
                        print(f"  {i+1:4d}: {line.rstrip()}")
        return None

def compile_to_exe(obj_file, name):
    exe_file = os.path.join(TMPDIR, name)
    ok, _, err = run_cmd(f"gcc -O3 -no-pie -o {exe_file} {obj_file} -lm")
    if ok:
        return exe_file
    print(f"  gcc link error: {err}")
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
        "int_div": """#include <stdio.h>
int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        sum += i / 7;
        sum += i / 100;
        sum += i % 256;
    }
    return 0;
}""",
        "loop_overhead": """int main() {
    volatile long i = 0;
    while (i < 1000000000) { i++; }
    return 0;
}""",
        "branching": """int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        if (i % 2 == 0) sum += 1;
        else sum -= 1;
    }
    return (int)sum;
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
        
        # Emit IR at O3
        ir_file_o3 = emit_ir(tth_file, 3)
        if ir_file_o3:
            with open(ir_file_o3) as f:
                ir_o3 = f.read()
            print(f"  IR(O3): sdiv={ir_o3.count('sdiv')} srem={ir_o3.count('srem')} and={ir_o3.count('and')} select={ir_o3.count('select')} i128={ir_o3.count('i128')}")
            result['ir_o3'] = ir_file_o3
        
        # Emit IR at O2
        ir_file_o2 = emit_ir(tth_file, 2)
        if ir_file_o2:
            with open(ir_file_o2) as f:
                ir_o2 = f.read()
            print(f"  IR(O2): sdiv={ir_o2.count('sdiv')} srem={ir_o2.count('srem')} and={ir_o2.count('and')} i128={ir_o2.count('i128')}")
            result['ir_o2'] = ir_file_o2
        
        # Compile and time Tether O3
        if result.get('ir_o3'):
            print(f"  Compiling Tether O3 via llvmlite...")
            obj = compile_ir_with_llvmlite(result['ir_o3'])
            if obj:
                exe = compile_to_exe(obj, f"{name}_O3")
                if exe:
                    ms = time_benchmark(exe)
                    if ms is not None:
                        result['tether_o3_ms'] = ms
                        print(f"  Tether O3: {ms:.1f} ms")
        
        # Compile and time Tether O2
        if result.get('ir_o2'):
            print(f"  Compiling Tether O2 via llvmlite...")
            obj = compile_ir_with_llvmlite(result['ir_o2'])
            if obj:
                exe = compile_to_exe(obj, f"{name}_O2")
                if exe:
                    ms = time_benchmark(exe)
                    if ms is not None:
                        result['tether_o2_ms'] = ms
                        print(f"  Tether O2: {ms:.1f} ms")
        
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
        if 'tether_o3_ms' in r and 'c_ms' in r and r['c_ms'] > 0:
            o3c = f"{r['tether_o3_ms'] / r['c_ms']:.2f}x"
        else:
            o3c = "N/A"
        if 'tether_o2_ms' in r and 'c_ms' in r and r['c_ms'] > 0:
            o2c = f"{r['tether_o2_ms'] / r['c_ms']:.2f}x"
        else:
            o2c = "N/A"
        print(f"{name:<16} {o3:>12} {o2:>12} {c:>12} {o3c:>8} {o2c:>8}")

if __name__ == "__main__":
    main()
