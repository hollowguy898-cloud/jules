// Pure functions with LLVM readnone/readonly optimization
fn square(x: i32) i32 pure {
    return x * x;
}

fn cube(x: i32) i32 pure {
    return x * x * x;
}

fn add_pure(a: i32, b: i32) i32 pure {
    return a + b;
}

fn is_even(x: i32) bool pure {
    return x % 2 == 0;
}

fn compute() i32 {
    val a = square(4);
    val b = square(4);
    val c = cube(3);
    val d = add_pure(a, c);
    val even = is_even(42);
    return d;
}

fn main() i32 {
    val result = compute();
    return result;
}
