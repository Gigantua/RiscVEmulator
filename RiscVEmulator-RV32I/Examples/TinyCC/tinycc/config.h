/* TinyCC bare-metal config for RV32I emulator */
#pragma once

/* No TCC installation directories — all paths empty */
#define CONFIG_TCCDIR ""
#define CONFIG_TCC_SYSINCLUDEPATHS ""
#define CONFIG_TCC_LIBPATHS ""
#define CONFIG_TCC_CRTPREFIX ""
#define CONFIG_TCC_ELFINTERP ""
#define CONFIG_TCC_SWITCHES ""
#define CONFIG_LDDIR "lib"
#define CONFIG_TRIPLET "riscv32-unknown-elf"
#define CONFIG_USR_INCLUDE ""

/* Disable features that require OS support */
#define CONFIG_TCC_BCHECK    0
#define CONFIG_TCC_BACKTRACE 0
#define CONFIG_TCC_SEMLOCK   0
#define CONFIG_TCC_STATIC    1   /* prevents dlfcn.h include */

/* Pretend we're on Linux so TCC uses ELF and not PE/Macho */
#define TARGETOS_Linux 1

/* Version string */
#define TCC_VERSION "mob"

/* Skip the TCCSYM stdio symbol table (no fopen/fprintf/fclose in bare metal) */
#define CONFIG_TCCBOOT 1

/* Embed tccdefs.h as strings at compile time (no filesystem access at runtime) */
#define CONFIG_TCC_PREDEFS 1

/* mprotect stub: emulator RAM is always executable */
#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4
static inline int mprotect(void *p, unsigned int n, int prot) { (void)p; (void)n; (void)prot; return 0; }

/* getenv stub: no environment on bare metal */
static inline char *getenv(const char *n) { (void)n; return 0; }
