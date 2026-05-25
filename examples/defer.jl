// Defer statement - explicit cleanup like Zig/Go
fn create_and_cleanup() i32 {
    var result: i32 = 0;
    var counter: i32 = 10;
    defer counter = 0;

    counter += 5;
    result = counter;

    return result;
}

fn early_return_defer() i32 {
    var resource: i32 = 100;
    defer resource = 0;

    if (resource > 50) {
        return 1;
    }

    resource += 10;
    return 0;
}

fn main() i32 {
    val r1 = create_and_cleanup();
    val r3 = early_return_defer();
    return r1 + r3;
}
