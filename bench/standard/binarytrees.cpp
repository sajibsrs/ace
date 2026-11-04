// C++ twin of binarytrees.ace: nodes by new and delete, as C++ idiom builds them.
#include "../awake.h"
#include <cstdint>
#include <cstdio>

constexpr int MIN_DEPTH = 4;
constexpr int MAX_DEPTH = 16;

struct Node {
    Node *left = nullptr, *right = nullptr;
    ~Node() {
        delete left;
        delete right;
    }
};

static Node *make(int depth) {
    Node *node = new Node;
    if (depth > 0) {
        node->left = make(depth - 1);
        node->right = make(depth - 1);
    }
    return node;
}

static long check(const Node *node) {
    if (!node->left) return 1;
    return 1 + check(node->left) + check(node->right);
}

int main() {
    std::uint64_t started = awake();
    Node *stretch = make(MAX_DEPTH + 1);
    long total = check(stretch);
    delete stretch;

    Node *long_lived = make(MAX_DEPTH);
    for (int depth = MIN_DEPTH; depth <= MAX_DEPTH; depth += 2) {
        long iterations = 1L << (MAX_DEPTH - depth + MIN_DEPTH);
        for (long i = 0; i < iterations; i++) {
            Node *tree = make(depth);
            total += check(tree);
            delete tree;
        }
    }
    total += check(long_lived);
    delete long_lived;
    std::uint64_t elapsed = awake() - started;

    std::printf("max depth: %d\nelapsed ns: %llu\nchecksum: %ld\n", MAX_DEPTH, (unsigned long long)elapsed, total);
    return 0;
}
