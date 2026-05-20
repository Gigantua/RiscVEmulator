#!/usr/bin/env bash
# rc6-verify — Report every instruction in an ELF that is not in the RC6
# accept set. See RC6_LLVM_PLAN.md (Phase 0).
#
# Allowed mnemonics (with --no-aliases so pseudo-instructions decode to
# their real opcode):
#   add  addi
#   xor  xori
#   rol  ror  rori                                  (Zbb)
#   bne
#   lw  lh  lhu  lb  lbu
#   sw  sh  sb
#   jal  jalr                                       (honorary RC6, per plan)
#   fence  fence.i
#   ecall  ebreak                                   (trap path)
#
# Banned (must be lowered to RC6 by the compiler):
#   lui  auipc  and  andi  or  ori  sub
#   sll  slli  srl  srli  sra  srai
#   slt  slti  sltu  sltiu
#   beq  blt  bge  bltu  bgeu
#   mul/div/rem/lr/sc/amo*  (already excluded by our -march=rv32i)
#
# Exit 0 if the ELF is RC6-clean, 1 otherwise.

set -euo pipefail

elf="${1:-}"
if [ -z "$elf" ]; then
    echo "usage: $0 <elf>" >&2
    exit 2
fi

OBJDUMP="${OBJDUMP:-$HOME/llvm-rv32i/bin/llvm-objdump}"

# -M no-aliases disables pseudo-instruction printing so we see the real
# opcode (addi vs mv, jalr vs ret, sltu vs snez, …).
ALLOWED='^(add|addi|xor|xori|rol|ror|rori|bne|lw|lh|lhu|lb|lbu|sw|sh|sb|jal|jalr|fence|ecall|ebreak|unimp)$'

"$OBJDUMP" -d -M no-aliases --no-show-raw-insn "$elf" \
  | awk -v re="$ALLOWED" '
      /^[ \t]*[0-9a-f]+:/ {
          # First whitespace-separated token after the "addr:" prefix.
          n = split($0, p, /[ \t]+/);
          mn = "";
          for (i = 1; i <= n; i++) {
              if (p[i] ~ /:$/) { mn = p[i+1]; break }
          }
          if (mn == "") next;
          if (mn ~ re) ok[mn]++; else bad[mn]++;
      }
      END {
          total_ok = 0; total_bad = 0;
          for (m in ok)  total_ok  += ok[m];
          for (m in bad) total_bad += bad[m];
          printf "RC6 ops:     %d\n", total_ok;
          printf "non-RC6 ops: %d\n", total_bad;
          if (total_bad > 0) {
              printf "\n  by mnemonic (descending):\n";
              cmd = "sort -k2 -nr";
              for (m in bad) printf "    %-12s %d\n", m, bad[m] | cmd;
              close(cmd);
          }
          exit (total_bad > 0 ? 1 : 0);
      }
  '
