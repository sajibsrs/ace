// C twin of binarytrees.ace.
#include "../awake.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define MIN_DEPTH 4
#define MAX_DEPTH 16

typedef struct Node { struct Node *left, *right; } Node;

static Node *make(int depth) {
    Node *node = malloc(sizeof(Node));
    if (!node) abort();
    if (depth > 0) {
        node->left = make(depth - 1);
        node->right = make(depth - 1);
    } else {
        node->left = NULL;
        node->right = NULL;
    }
    return node;
}

static long check(const Node *node) {
    if (!node->left) return 1;
    return 1 + check(node->left) + check(node->right);
}

static void release(Node *node) {
    if (node->left) {
        release(node->left);
        release(node->right);
    }
    free(node);
}

int main(void) {
    uint64_t started = awake();
    Node *stretch = make(MAX_DEPTH + 1);
    long total = check(stretch);
    release(stretch);

    Node *long_lived = make(MAX_DEPTH);
    for (int depth = MIN_DEPTH; depth <= MAX_DEPTH; depth += 2) {
        long iterations = 1L << (MAX_DEPTH - depth + MIN_DEPTH);
        for (long i = 0; i < iterations; i++) {
            Node *tree = make(depth);
            total += check(tree);
            release(tree);
        }
    }
    total += check(long_lived);
    release(long_lived);
    uint64_t elapsed = awake() - started;

    printf("max depth: %d\nelapsed ns: %llu\nchecksum: %ld\n", MAX_DEPTH, (unsigned long long)elapsed, total);
    return 0;
}
