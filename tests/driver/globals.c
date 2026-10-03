#include <stdint.h>

typedef struct { int32_t x; float y; } Point;

static int32_t twice(int32_t value) { return value * 2; }

int32_t c_counter = 5;
const int32_t c_limit = 40;
int32_t c_table[4] = {1, 2, 3, 4};
Point c_point = {3, 1.5f};
const char* c_name = "ace";
int32_t (*c_hook)(int32_t) = twice;

int32_t c_read_counter(void) { return c_counter; }
void c_bump_counter(void) { c_counter += 1; }
int32_t c_table_sum(void) { return c_table[0] + c_table[1] + c_table[2] + c_table[3]; }
