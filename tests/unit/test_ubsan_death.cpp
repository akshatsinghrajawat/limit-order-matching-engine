// Signed overflow must terminate a sanitized build. Registered with WILL_FAIL,
// so ctest passes only while this program exits non-zero. When UBSan is
// allowed to recover it prints its diagnostic and carries on, the program
// exits 0, and the test fails.
//
// The operands are volatile so the addition happens at run time at any
// optimization level.
#include <climits>

int main() {
  volatile int x = INT_MAX;
  volatile int y = x + 1;
  (void)y;
  return 0;
}
