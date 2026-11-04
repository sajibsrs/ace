// Tree-checking benchmark paired with tree.ace and tree.c: the same generator,
// walk, and checker over the same tree, nodes in a Vec indexed by u32, names
// as byte slices, and a linear-probing hash table for functions.
use std::time::Instant;

const FUNCS: u32 = 4000;
const ROUNDS: u32 = 20;
const MAX_DEPTH: u32 = 4;
const T_INT: u32 = 1;
const T_BOOL: u32 = 2;
const T_BAD: u32 = 3;
const SLOTS: usize = 8192;

const NAMES: [&[u8]; 12] = [b"a", b"b", b"c", b"d", b"e", b"f", b"g", b"h", b"i", b"j", b"k", b"l"];

#[derive(Clone, Copy, PartialEq, Default)]
enum Kind { #[default] None, Lit, Name, Add, Less, Call, Let, Cond, Ret, Block, Func }

#[allow(dead_code)] // flags, line, col, and c are carried, as in C
#[derive(Clone, Copy, Default)]
struct Node {
    kind: Kind,
    flags: u32, line: u32, col: u32,
    text: &'static [u8],
    ival: u64,
    a: u32, b: u32, c: u32, kids: u32, nkids: u32,
}

struct Sym { ty: u32, nparams: u32 }
struct Local { name: &'static [u8], ty: u32 }
#[derive(Clone, Copy)]
struct Slot { key: &'static [u8], value: u32 }

struct Tree { nodes: Vec<Node>, kids: Vec<u32>, funcs: Vec<u32>, seed: u64 }

impl Tree {
    fn next(&mut self, n: u64) -> u64 {
        let mut x = self.seed;
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        self.seed = x;
        (x >> 11) % n
    }

    fn add(&mut self, n: Node) -> u32 {
        self.nodes.push(n);
        (self.nodes.len() - 1) as u32
    }

    fn expr(&mut self, depth: u32, nlocal: u32, f: u32) -> u32 {
        let pick = self.next(10);
        if depth >= MAX_DEPTH || pick < 3 {
            let v = self.next(1000);
            return self.add(Node { kind: Kind::Lit, ival: v, ..Node::default() });
        }
        if pick < 6 {
            let name = NAMES[self.next(nlocal as u64) as usize];
            return self.add(Node { kind: Kind::Name, text: name, ..Node::default() });
        }
        if pick < 8 {
            let a = self.expr(depth + 1, nlocal, f);
            let b = self.expr(depth + 1, nlocal, f);
            let kind = if pick == 7 { Kind::Less } else { Kind::Add };
            return self.add(Node { kind, a, b, ..Node::default() });
        }
        let callee = self.next(f as u64 + 1) as usize;
        let x = self.expr(depth + 1, nlocal, f);
        let y = self.expr(depth + 1, nlocal, f);
        let start = self.kids.len() as u32;
        self.kids.push(x);
        self.kids.push(y);
        let text = self.nodes[self.funcs[callee] as usize].text;
        self.add(Node { kind: Kind::Call, text, kids: start, nkids: 2, ..Node::default() })
    }

    fn block(&mut self, depth: u32, nlocal: u32, f: u32) -> u32 {
        let count = 2 + self.next(4) as u32;
        let mut ids = [0u32; 8];
        let mut scope = nlocal;
        for i in 0..count as usize {
            let pick = self.next(10);
            if pick < 5 && scope < 12 {
                let init = self.expr(0, scope, f);
                ids[i] = self.add(Node { kind: Kind::Let, text: NAMES[scope as usize], a: init, ..Node::default() });
                scope += 1;
            } else if pick < 8 && depth < 2 {
                let test = self.expr(0, scope, f);
                let body = self.block(depth + 1, scope, f);
                ids[i] = self.add(Node { kind: Kind::Cond, a: test, b: body, ..Node::default() });
            } else {
                let value = self.expr(0, scope, f);
                ids[i] = self.add(Node { kind: Kind::Ret, a: value, ..Node::default() });
            }
        }
        let start = self.kids.len() as u32;
        self.kids.extend_from_slice(&ids[..count as usize]);
        self.add(Node { kind: Kind::Block, kids: start, nkids: count, ..Node::default() })
    }

    fn build() -> Tree {
        let mut t = Tree { nodes: Vec::new(), kids: Vec::new(), funcs: Vec::new(), seed: 42 };
        t.add(Node::default());
        for f in 0..FUNCS {
            // The names live for the whole run, as in C's static pool.
            let name: &'static [u8] = format!("f{f}").into_bytes().leak();
            let id = t.add(Node { kind: Kind::Func, text: name, nkids: 2, ..Node::default() });
            t.funcs.push(id);
            let body = t.block(0, 2, f);
            t.nodes[id as usize].a = body;
        }
        t
    }

    fn walk(&self, id: u32) -> usize {
        let n = &self.nodes[id as usize];
        match n.kind {
            Kind::Lit => n.ival as usize,
            Kind::Name => n.text.len(),
            Kind::Add | Kind::Less | Kind::Cond => self.walk(n.a) + self.walk(n.b) + 1,
            Kind::Let | Kind::Ret => self.walk(n.a) + 2,
            Kind::Call | Kind::Block => {
                let kids = &self.kids[n.kids as usize..(n.kids + n.nkids) as usize];
                n.nkids as usize + kids.iter().map(|&k| self.walk(k)).sum::<usize>()
            }
            _ => 0,
        }
    }
}

fn hash(s: &[u8]) -> u64 {
    let mut h: u64 = 1469598103934665603;
    for &b in s { h = (h ^ b as u64).wrapping_mul(1099511628211); }
    h
}

struct Checker<'t> {
    tree: &'t Tree,
    syms: Vec<Sym>,
    slots: Vec<Option<Slot>>,
    locals: Vec<Local>,
    nty: Vec<u32>,
    errors: usize,
}

impl Checker<'_> {
    fn global_set(&mut self, key: &'static [u8], value: u32) {
        let mut at = hash(key) as usize & (SLOTS - 1);
        while let Some(s) = self.slots[at] {
            if s.key == key { break; }
            at = (at + 1) & (SLOTS - 1);
        }
        self.slots[at] = Some(Slot { key, value });
    }

    fn global_get(&self, key: &[u8]) -> Option<u32> {
        let mut at = hash(key) as usize & (SLOTS - 1);
        while let Some(s) = self.slots[at] {
            if s.key == key { return Some(s.value); }
            at = (at + 1) & (SLOTS - 1);
        }
        None
    }

    fn find_local(&self, name: &[u8]) -> u32 {
        self.locals.iter().rev().find(|l| l.name == name).map_or(0, |l| l.ty)
    }

    fn check_expr(&mut self, id: u32) -> u32 {
        let n = &self.tree.nodes[id as usize];
        let mut t = T_BAD;
        match n.kind {
            Kind::Lit => t = T_INT,
            Kind::Name => {
                t = self.find_local(n.text);
                if t == 0 { self.errors += 1; t = T_BAD; }
            }
            Kind::Add | Kind::Less => {
                let x = self.check_expr(n.a);
                let y = self.check_expr(n.b);
                if x != y { self.errors += 1; }
                t = if n.kind == Kind::Less { T_BOOL } else { T_INT };
            }
            Kind::Call => {
                let s = self.global_get(n.text);
                if s.map_or(true, |s| self.syms[s as usize].nparams != n.nkids) { self.errors += 1; }
                for i in 0..n.nkids {
                    if self.check_expr(self.tree.kids[(n.kids + i) as usize]) != T_INT { self.errors += 1; }
                }
                if let Some(s) = s { t = self.syms[s as usize].ty; }
            }
            _ => {}
        }
        self.nty[id as usize] = t;
        t
    }

    fn check_block(&mut self, id: u32) {
        let mark = self.locals.len();
        let n = &self.tree.nodes[id as usize];
        for i in 0..n.nkids {
            let x = &self.tree.nodes[self.tree.kids[(n.kids + i) as usize] as usize];
            match x.kind {
                Kind::Let => {
                    let ty = self.check_expr(x.a);
                    self.locals.push(Local { name: x.text, ty });
                }
                Kind::Cond => {
                    if self.check_expr(x.a) != T_BOOL { self.errors += 1; }
                    self.check_block(x.b);
                }
                Kind::Ret => {
                    if self.check_expr(x.a) != T_INT { self.errors += 1; }
                }
                _ => {}
            }
        }
        self.locals.truncate(mark);
    }

    fn check(&mut self) {
        for f in 0..FUNCS as usize {
            self.locals.clear();
            self.locals.push(Local { name: NAMES[0], ty: T_INT });
            self.locals.push(Local { name: NAMES[1], ty: T_INT });
            self.check_block(self.tree.nodes[self.tree.funcs[f] as usize].a);
        }
        self.locals.clear();
    }
}

fn report(name: &str, started: Instant, operations: usize, checksum: usize) {
    println!("{name} {} {operations} {checksum}", started.elapsed().as_nanos());
}

fn main() {
    let tree = Tree::build();
    let nnodes = tree.nodes.len();
    let mut c = Checker {
        tree: &tree,
        syms: vec![Sym { ty: 0, nparams: 0 }],
        slots: vec![None; SLOTS],
        locals: Vec::with_capacity(64),
        nty: vec![0; nnodes],
        errors: 0,
    };
    for f in 0..FUNCS {
        let text = tree.nodes[tree.funcs[f as usize] as usize].text;
        c.syms.push(Sym { ty: T_INT, nparams: 2 });
        c.global_set(text, f + 1);
    }

    let started = Instant::now();
    let mut sum = 0;
    for _ in 0..ROUNDS {
        for &f in &tree.funcs { sum += tree.walk(tree.nodes[f as usize].a); }
    }
    report("walk", started, ROUNDS as usize * nnodes, sum);

    let started = Instant::now();
    for _ in 0..ROUNDS {
        c.errors = 0;
        c.check();
    }
    let typed: usize = c.nty.iter().map(|&t| t as usize).sum();
    report("check", started, ROUNDS as usize * nnodes, c.errors * 1000003 + typed);
}
