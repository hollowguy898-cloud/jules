// Pointers, references, and unsafe operations
fn main() i32 {
    var x: i32 = 10;
    var y: i32 = 20;

    // Raw pointer operations
    var ptr: *i32 = &x;
    unsafe(*ptr = 42);

    // Mutable borrow
    var counter: i32 = 0;
    counter += 5;
    counter += 10;

    return counter + y;
}
