/* mem_widths.c — tests all load/store widths and sign extension.
 * NOTE: all string literals use only plain ASCII (no multi-byte Unicode).
 */

#include "libc.h"

static void check(const char *name, int got, int expected)
{
    printf("%s: %d", name, got);
    if (got != expected)
        printf(" FAIL (expected %d)", expected);
    else
        printf(" OK");
    printf("\n");
}

void _start(void)
{
    unsigned int buf[4];
    unsigned char  *bp = (unsigned char *)buf;
    unsigned short *hp = (unsigned short *)buf;

    /* ── SB / LB / LBU ─────────────────────────────────────── */
    bp[0] = 0xFF;
    check("lb 0xFF sign=-1",    (int)(signed char)*bp,         -1);
    check("lbu 0xFF zero=255",  (int)(unsigned char)*bp,       255);

    bp[0] = 0x7F;
    check("lb 0x7F sign=127",   (int)(signed char)*bp,         127);

    /* ── SH / LH / LHU ──────────────────────────────────────── */
    hp[0] = 0xFFFF;
    check("lh 0xFFFF sign=-1",    (int)(short)hp[0],           -1);
    check("lhu 0xFFFF zero=65535",(int)(unsigned short)hp[0],  65535);

    hp[0] = 0x8000;
    check("lh 0x8000 sign=-32768",(int)(short)hp[0],           -32768);

    /* ── SW / LW ─────────────────────────────────────────────── */
    buf[0] = 0xDEADBEEFu;
    check("lw 0xDEADBEEF", (int)buf[0], (int)0xDEADBEEFu);

    /* ── Little-endian byte order ─────────────────────────────── */
    /* 0x01020304 stored LE: byte[0]=0x04, byte[1]=0x03, byte[2]=0x02, byte[3]=0x01 */
    buf[0] = 0x01020304u;
    check("le byte0=4",  (int)(unsigned char)bp[0], 4);
    check("le byte1=3",  (int)(unsigned char)bp[1], 3);
    check("le byte2=2",  (int)(unsigned char)bp[2], 2);
    check("le byte3=1",  (int)(unsigned char)bp[3], 1);

    /* ── SH stores correct bytes ─────────────────────────────── */
    hp[0] = 0x1234;
    check("sh byte0=0x34", (int)(unsigned char)bp[0], 0x34);
    check("sh byte1=0x12", (int)(unsigned char)bp[1], 0x12);

    exit(0);
}
