/* Forced-include via clang `-include` so it lands before the
 * <assert.h> include inside softfp.c. With NDEBUG set, the system
 * assert.h defines `assert` as a no-op and the runtime `__assert_fail`
 * reference disappears — necessary because the bare-metal Runtime
 * doesn't provide it. */
#ifndef RVEMU_SOFTFP_FORCE_H
#define RVEMU_SOFTFP_FORCE_H
#define NDEBUG 1
#endif
