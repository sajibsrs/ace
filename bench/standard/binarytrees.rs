// Rust twin of binarytrees.ace: nodes are Boxes, released by drop.
use std::time::Instant;

const MIN_DEPTH: i32 = 4;
const MAX_DEPTH: i32 = 16;

struct Node {
    left: Option<Box<Node>>,
    right: Option<Box<Node>>,
}

fn make(depth: i32) -> Box<Node> {
    if depth > 0 {
        Box::new(Node { left: Some(make(depth - 1)), right: Some(make(depth - 1)) })
    } else {
        Box::new(Node { left: None, right: None })
    }
}

fn check(node: &Node) -> i64 {
    match (&node.left, &node.right) {
        (Some(left), Some(right)) => 1 + check(left) + check(right),
        _ => 1,
    }
}

fn main() {
    let started = Instant::now();
    let stretch = make(MAX_DEPTH + 1);
    let mut total = check(&stretch);
    drop(stretch);

    let long_lived = make(MAX_DEPTH);
    for depth in (MIN_DEPTH..=MAX_DEPTH).step_by(2) {
        let iterations = 1i64 << (MAX_DEPTH - depth + MIN_DEPTH);
        for _ in 0..iterations {
            let tree = make(depth);
            total += check(&tree);
        }
    }
    total += check(&long_lived);
    drop(long_lived);
    let elapsed = started.elapsed().as_nanos();

    println!("max depth: {MAX_DEPTH}\nelapsed ns: {elapsed}\nchecksum: {total}");
}
