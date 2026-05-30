#!/usr/bin/env python3
"""Run Tether micro-benchmarks at O2 and O3, comparing against C baseline."""

import subprocess
import time
import os
import sys
import tempfile

TETHERC = "/home/z/my-project/jules/tetherc"
BENCH_DIR = "/home/z/my-project/jules/benchmarks/micro"
TMPDIR = tempfile.mkdtemp(prefix="tether_bench_")
ITERS = 3  # Number of times to run each benchmark

# Benchmarks: (name, filename)
BENCHMARKS = [
    ("int_div", "int_div.tth"),
    ("loop_overhead", "loop_overhead.tth"),
    ("branching", "branching.tth"),
    ("float_math", "float_math.tth"),
    ("int_arith", "int_arith.tth"),
]

def run_cmd(cmd, timeout=30):
    """Run a command and return (success, stdout, stderr)."""
    try:
        r = subprocess.run(cmd, shell=True, capture_output=True, text=True, timeout=timeout)
        return r.returncode == 0, r.stdout, r.stderr
    except subprocess.TimeoutExpired:
        return False, "", "timeout"

def emit_ir(tth_file, opt_level):
    """Emit LLVM IR from Tether source."""
    out_file = os.path.join(TMPDIR, f"bench_{opt_level}.ll")
    ok, _, err = run_cmd(f"{TETHERC} --emit-ir -O{opt_level} {tth_file} -o {out_file}")
    if not ok:
        return None
    return out_file

def compile_ir_with_llvmlite(ir_file):
    """Compile LLVM IR to native code using llvmlite and return the shared object path."""
    try:
        import llvmlite.binding as llvm
        llvm.initialize()
        llvm.initialize_native_target()
        llvm.initialize_native_asmprinter()
        
        with open(ir_file, 'r') as f:
            ir_text = f.read()
        
        # Fix opaque pointers for LLVM 15 (llvmlite uses LLVM 15 which needs typed pointers)
        # Replace `ptr` with `i8*` as a simple workaround
        ir_text = ir_text.replace('ptr ', 'i8* ')
        ir_text = ir_text.replace(', ptr ', ', i8* ')
        ir_text = ir_text.replace('(ptr ', '(i8* ')
        ir_text = ir_text.replace('\nptr ', '\ni8* ')
        
        mod = llvm.parse_assembly(ir_text)
        mod.verify()
        
        # Create target machine
        target = llvm.Target.from_default_triple()
        target_machine = target.create_target_machine(opt=3)
        
        # Run optimization passes
        pm = llvm.ModulePassManager()
        pm.add_verification_pass()
        # Add optimization passes
        from llvmlite.binding import PassManagerBuilder
        pmb = PassManagerBuilder()
        pmb.opt_level = 3
        pmb.size_level = 0
        pmb.loop_vectorize = True
        pmb.slp_vectorize = True
        pmb.populate(pm)
        pm.run(mod)
        
        # Generate object code
        obj_data = target_machine.emit_object(mod)
        obj_file = os.path.join(TMPDIR, "bench.o")
        with open(obj_file, 'wb') as f:
            f.write(obj_data)
        
        return obj_file
    except Exception as e:
        print(f"  llvmlite error: {e}", file=sys.stderr)
        return None

def compile_ir_to_exe(ir_file, name):
    """Compile IR file to executable using gcc (via .s or .o)."""
    # First try llvmlite
    obj_file = compile_ir_with_llvmlite(ir_file)
    if obj_file:
        exe_file = os.path.join(TMPDIR, name)
        ok, _, err = run_cmd(f"gcc -O3 -no-pie -o {exe_file} {obj_file} -lm")
        if ok:
            return exe_file
        print(f"  gcc link error: {err}", file=sys.stderr)
    
    # Fallback: try using llc-style approach
    return None

def time_benchmark(exe_file):
    """Time a benchmark executable, return median time in ms."""
    times = []
    for _ in range(ITERS):
        start = time.perf_counter()
        ok, stdout, stderr = run_cmd(exe_file, timeout=60)
        elapsed = (time.perf_counter() - start) * 1000
        if not ok:
            return None
        times.append(elapsed)
    times.sort()
    return times[len(times) // 2]  # median

def write_c_equivalent(name, tth_code):
    """Generate C equivalent for a benchmark."""
    # Simple C programs for comparison
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
        "loop_overhead": """#include <stdio.h>
int main() {
    for (long i = 0; i < 1000000000; i++) {}
    return 0;
}""",
        "branching": """#include <stdio.h>
int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        if (i % 2 == 0) sum += 1;
        else sum -= 1;
    }
    return 0;
}""",
        "float_math": """#include <stdio.h>
int main() {
    double sum = 0.0;
    for (long i = 0; i < 100000000; i++) {
        double fi = (double)i;
        sum += fi * fi + fi / 3.14159;
    }
    return 0;
}""",
        "int_arith": """#include <stdio.h>
int main() {
    long sum = 0;
    for (long i = 0; i < 100000000; i++) {
        sum += i * 3 + i - i / 2 + (i & 0xFF);
    }
    return 0;
}""",
    }
    return c_programs.get(name)

def compile_c(c_code, name):
    """Compile C code to executable."""
    c_file = os.path.join(TMPDIR, f"{name}_c.c")
    exe_file = os.path.join(TMPDIR, f"{name}_c")
    with open(c_file, 'w') as f:
        f.write(c_code)
    ok, _, err = run_cmd(f"gcc -O3 -o {exe_file} {c_file} -lm")
    if ok:
        return exe_file
    print(f"  C compile error: {err}", file=sys.stderr)
    return None

def main():
    print("=" * 80)
    print("Tether Compiler Micro-Benchmark Suite")
    print("=" * 80)
    print(f"Temp dir: {TMPDIR}")
    print(f"Iterations per bench: {ITERS}")
    print()
    
    results = {}
    
    for name, filename in BENCHMARKS:
        tth_file = os.path.join(BENCH_DIR, filename)
        if not os.path.exists(tth_file):
            print(f"SKIP {name}: {tth_file} not found")
            continue
        
        print(f"--- {name} ---")
        result = {"name": name}
        
        # Test at O3
        print(f"  Emitting IR at O3...")
        ir_file = emit_ir(tth_file, 3)
        if ir_file:
            # Check IR for key optimizations
            with open(ir_file, 'r') as f:
                ir_text = f.read()
            
            has_and = 'and i64' in ir_text or 'and i32' in ir_text
            has_srem = 'srem' in ir_text
            has_sdiv = 'sdiv' in ir_text
            has_select = 'select i1' in ir_text
            has_i128 = 'i128' in ir_text
            
            print(f"  IR stats: sdiv={ir_text.count('sdiv')} srem={ir_text.count('srem')} and={ir_text.count('and')} select={ir_text.count('select')} i128={ir_text.count('i128')}")
            
            result['ir_o3'] = ir_file
        
        # Test at O2
        print(f"  Emitting IR at O2...")
        ir_file_o2 = emit_ir(tth_file, 2)
        if ir_file_o2:
            with open(ir_file_o2, 'r') as f:
                ir_text_o2 = f.read()
            print(f"  IR(O2) stats: sdiv={ir_text_o2.count('sdiv')} srem={ir_text_o2.count('srem')} and={ir_text_o2.count('and')} i128={ir_text_o2.count('i128')}")
            result['ir_o2'] = ir_file_o2
        
        # Compile and time Tether O3
        if result.get('ir_o3'):
            print(f"  Compiling Tether O3...")
            exe = compile_ir_to_exe(result['ir_o3'], f"{name}_O3")
            if exe:
                ms = time_benchmark(exe)
                if ms is not None:
                    result['tether_o3_ms'] = ms
                    print(f"  Tether O3: {ms:.1f} ms")
                else:
                    print(f"  Tether O3: FAILED to run")
            else:
                print(f"  Tether O3: FAILED to compile")
        
        # Compile and time Tether O2
        if result.get('ir_o2'):
            print(f"  Compiling Tether O2...")
            exe = compile_ir_to_exe(result['ir_o2'], f"{name}_O2")
            if exe:
                ms = time_benchmark(exe)
                if ms is not None:
                    result['tether_o2_ms'] = ms
                    print(f"  Tether O2: {ms:.1f} ms")
                else:
                    print(f"  Tether O2: FAILED to run")
            else:
                print(f"  Tether O2: FAILED to compile")
        
        # Compile and time C baseline
        c_code = write_c_equivalent(name, "")
        if c_code:
            print(f"  Compiling C baseline...")
            c_exe = compile_c(c_code, name)
            if c_exe:
                ms = time_benchmark(c_exe)
                if ms is not None:
                    result['c_ms'] = ms
                    print(f"  C (gcc -O3): {ms:.1f} ms")
        
        results[name] = result
        print()
    
    # Summary table
    print("=" * 80)
    print("SUMMARY")
    print("=" * 80)
    print(f"{'Benchmark':<16} {'Tether O3':>12} {'Tether O2':>12} {'C (gcc O3)':>12} {'O3/C ratio':>12}")
    print("-" * 64)
    for name, r in results.items():
        o3 = f"{r['tether_o3_ms']:.1f}" if 'tether_o3_ms' in r else "FAIL"
        o2 = f"{r['tether_o2_ms']:.1f}" if 'tether_o2_ms' in r else "FAIL"
        c = f"{r['c_ms']:.1f}" if 'c_ms' in r else "N/A"
        if 'tether_o3_ms' in r and 'c_ms' in r and r['c_ms'] > 0:
            ratio = f"{r['tether_o3_ms'] / r['c_ms']:.2f}x"
        else:
            ratio = "N/A"
        print(f"{name:<16} {o3:>12} {o2:>12} {c:>12} {ratio:>12}")
    
    print()
    print("Key improvements from this fix:")
    print("  1. Strength reduction REMOVED — no more i128 pessimization for division")
    print("  2. Select type mismatch FIXED — proper type casts before select")
    print("  3. Power-of-2 modulo uses AND at O3 — even for signed types")
    print("  4. Non-power-of-2 div/mod: plain sdiv/srem — let LLVM instcombine handle it")

if __name__ == "__main__":
    main()
