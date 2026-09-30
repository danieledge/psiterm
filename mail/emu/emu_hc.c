/* the harness hooks this function's address (its own file, so never inlined) */
int emu_hc(int op, int a, int b, int c, int d) { return op + a + b + c + d; }
