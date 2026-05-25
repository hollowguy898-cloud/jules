// While loops with increment clause and control flow
fn fibonacci(n: i32) i32 {
    if (n <= 1) {
        return n;
    }
    var a: i32 = 0;
    var b: i32 = 1;
    var i: i32 = 2;
    while (i <= n) : (i += 1) {
        val temp = b;
        b = a + b;
        a = temp;
    }
    return b;
}

fn sum_range(start: i32, end: i32) i32 {
    var total: i32 = 0;
    var i: i32 = start;
    while (i <= end) : (i += 1) {
        total += i;
    }
    return total;
}

fn count_down(n: i32) i32 {
    var count: i32 = 0;
    var i: i32 = n;
    while (i > 0) : (i -= 1) {
        if (i == 5) {
            break;
        }
        count += 1;
    }
    return count;
}

fn main() i32 {
    val fib10 = fibonacci(10);
    val sum = sum_range(1, 100);
    val cd = count_down(10);
    return fib10;
}
