// Branchless select expression - zero pipeline flushes
fn clamp(x: i32, lo: i32, hi: i32) i32 pure {
    // Branchless: if x < lo, return lo; if x > hi, return hi; else x
    val clamped_low = select(x < lo, lo, x);
    val result = select(clamped_low > hi, hi, clamped_low);
    return result;
}

fn abs_val(x: i32) i32 pure {
    // Branchless absolute value
    val mask = x >> 31;
    return (x + mask) ^ mask;
}

fn min_val(a: i32, b: i32) i32 pure {
    return select(a < b, a, b);
}

fn max_val(a: i32, b: i32) i32 pure {
    return select(a > b, a, b);
}

fn signum(x: i32) i32 pure {
    val is_neg = select(x < 0, 1, 0);
    val is_pos = select(x > 0, 1, 0);
    return is_pos - is_neg;
}

fn main() i32 {
    val clamped = clamp(150, 0, 100);
    val absolute = abs_val(-42);
    val minimum = min_val(10, 20);
    val maximum = max_val(10, 20);
    val sign = signum(-5);
    return clamped + absolute + minimum + maximum + sign;
}
