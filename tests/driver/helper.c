// helper.c — foreign C library linked into driver tests as an object and as
// an archive. helper_value returns int (the Ace platform word) so both sides
// of the boundary agree on the return ABI without casts.
long helper_value(void) {
    return 7;
}
