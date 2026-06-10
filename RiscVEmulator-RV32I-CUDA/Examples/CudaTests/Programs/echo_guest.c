/* echo_guest.c — console text IO on the CUDA core, written as plain C against
 * the bare-metal runtime: printf/puts ride the UART output ring via _write,
 * fgets/getchar ride the keyboard mailbox via _read (one key staged per kernel
 * launch). Echoes each line back doubled in quotes; empty line or "exit" quits. */

#include <stdio.h>
#include <string.h>

int main(void)
{
    puts("GPU echo ready - type a line (empty line or 'exit' quits)");
    char line[128];
    for (;;) {
        printf("echo> ");
        if (!fgets(line, sizeof line, stdin)) break;
        char *nl = strchr(line, '\n');
        if (nl) *nl = '\0';
        if (!line[0] || !strcmp(line, "exit")) break;
        printf("Cuda answer: \"%s %s\"\n", line, line);
    }
    puts("bye");
    return 0;
}
