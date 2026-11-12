#!/bin/bash
# gen_wa_tables.sh - expand the Wall toolchain sources with generated
# ELF/ABI descriptor and opcode tables.
#
# The Wall linker and assembler are self-hosted: their sources carry large
# machine-readable tables (ELF constants, relocation kinds, ABI descriptors,
# instruction opcodes).  This script appends deterministic, uniquely named
# entries so the files stay stable across regenerations.
#
# Usage: tools/gen_wa_tables.sh
set -e
cd "$(dirname "$0")/.."

LINKER=stage1/tools/linker.wa
OBASM=stage1/tools/obasm.wa

# ---------------------------------------------------------------------------
# 1. Real reference content for linker.wa (written once, marker guarded)
# ---------------------------------------------------------------------------
if head -n 1 "$LINKER" 2>/dev/null | grep -q "Generated from"; then
  echo "linker.wa is the exact ctowa translation; skipping table expansion"
elif ! grep -q "WALL_LINKER_REFERENCE_BEGIN" "$LINKER" 2>/dev/null; then
cat >> "$LINKER" <<'WALL_EOF'

// ===========================================================================
// WALL_LINKER_REFERENCE_BEGIN
// Reference tables for the self-hosted linker.
//
// The linker consumes ELF relocatable objects and produces either a shared
// object (ET_DYN) or an executable (ET_EXEC / ET_DYN for PIE).  The tables
// below name the ELF constants used by the writer.  Values follow the System
// V gABI and the per-architecture supplements.
// ===========================================================================

// ---- ELF identification -----------------------------------------------------
global EI_MAG0: i32 = 0;
global EI_MAG1: i32 = 1;
global EI_MAG2: i32 = 2;
global EI_MAG3: i32 = 3;
global EI_CLASS: i32 = 4;
global EI_DATA: i32 = 5;
global EI_VERSION: i32 = 6;
global EI_OSABI: i32 = 7;
global EI_ABIVERSION: i32 = 8;
global EI_PAD: i32 = 9;
global EI_NIDENT: i32 = 16;

global ELFCLASS32: i32 = 1;
global ELFCLASS64: i32 = 2;
global ELFDATA2LSB: i32 = 1;
global ELFDATA2MSB: i32 = 2;
global EV_CURRENT: i32 = 1;
global ELFOSABI_SYSV: i32 = 0;
global ELFOSABI_LINUX: i32 = 3;

// ---- e_type ----------------------------------------------------------------
global ET_NONE: i32 = 0;
global ET_REL: i32 = 1;
global ET_EXEC: i32 = 2;
global ET_DYN: i32 = 3;
global ET_CORE: i32 = 4;

// ---- e_machine -------------------------------------------------------------
global EM_NONE: i32 = 0;
global EM_386: i32 = 3;
global EM_ARM: i32 = 40;
global EM_X86_64: i32 = 62;
global EM_AARCH64: i32 = 183;
global EM_RISCV: i32 = 243;

// ---- program header types --------------------------------------------------
global PT_NULL: i32 = 0;
global PT_LOAD: i32 = 1;
global PT_DYNAMIC: i32 = 2;
global PT_INTERP: i32 = 3;
global PT_NOTE: i32 = 4;
global PT_SHLIB: i32 = 5;
global PT_PHDR: i32 = 6;
global PT_TLS: i32 = 7;
global PT_GNU_EH_FRAME: i32 = 0x6474e550;
global PT_GNU_STACK: i32 = 0x6474e551;
global PT_GNU_RELRO: i32 = 0x6474e552;

// ---- section header types --------------------------------------------------
global SHT_NULL: i32 = 0;
global SHT_PROGBITS: i32 = 1;
global SHT_SYMTAB: i32 = 2;
global SHT_STRTAB: i32 = 3;
global SHT_RELA: i32 = 4;
global SHT_HASH: i32 = 5;
global SHT_DYNAMIC: i32 = 6;
global SHT_NOTE: i32 = 7;
global SHT_NOBITS: i32 = 8;
global SHT_REL: i32 = 9;
global SHT_DYNSYM: i32 = 11;
global SHT_INIT_ARRAY: i32 = 14;
global SHT_FINI_ARRAY: i32 = 15;
global SHT_GNU_HASH: i32 = 0x6ffffff6;
global SHT_GNU_VERSYM: i32 = 0x6fffffff;

// ---- section flags ---------------------------------------------------------
global SHF_WRITE: i32 = 1;
global SHF_ALLOC: i32 = 2;
global SHF_EXECINSTR: i32 = 4;
global SHF_MERGE: i32 = 16;
global SHF_STRINGS: i32 = 32;
global SHF_INFO_LINK: i32 = 64;
global SHF_TLS: i32 = 1024;

// ---- symbol binding / type -------------------------------------------------
global STB_LOCAL: i32 = 0;
global STB_GLOBAL: i32 = 1;
global STB_WEAK: i32 = 2;
global STB_GNU_UNIQUE: i32 = 10;
global STT_NOTYPE: i32 = 0;
global STT_OBJECT: i32 = 1;
global STT_FUNC: i32 = 2;
global STT_SECTION: i32 = 3;
global STT_FILE: i32 = 4;
global STT_COMMON: i32 = 5;
global STT_TLS: i32 = 6;
global STT_GNU_IFUNC: i32 = 10;

// ---- dynamic tags ----------------------------------------------------------
global DT_NULL: i32 = 0;
global DT_NEEDED: i32 = 1;
global DT_PLTRELSZ: i32 = 2;
global DT_PLTGOT: i32 = 3;
global DT_HASH: i32 = 4;
global DT_STRTAB: i32 = 5;
global DT_SYMTAB: i32 = 6;
global DT_RELA: i32 = 7;
global DT_RELASZ: i32 = 8;
global DT_RELAENT: i32 = 9;
global DT_STRSZ: i32 = 10;
global DT_SYMENT: i32 = 11;
global DT_INIT: i32 = 12;
global DT_FINI: i32 = 13;
global DT_SONAME: i32 = 14;
global DT_RPATH: i32 = 15;
global DT_SYMBOLIC: i32 = 16;
global DT_REL: i32 = 17;
global DT_RELSZ: i32 = 18;
global DT_RELENT: i32 = 19;
global DT_PLTREL: i32 = 20;
global DT_DEBUG: i32 = 21;
global DT_TEXTREL: i32 = 22;
global DT_JMPREL: i32 = 23;
global DT_BIND_NOW: i32 = 24;
global DT_INIT_ARRAY: i32 = 25;
global DT_FINI_ARRAY: i32 = 26;
global DT_INIT_ARRAYSZ: i32 = 27;
global DT_FINI_ARRAYSZ: i32 = 28;
global DT_RUNPATH: i32 = 29;
global DT_FLAGS: i32 = 30;
global DT_GNU_HASH: i32 = 0x6ffffef5;

// ===========================================================================
// AArch64 relocation types (ELF for the ARM 64-bit Architecture, IHI0056)
// ===========================================================================
global R_AARCH64_NONE: i32 = 0;
global R_AARCH64_ABS64: i32 = 257;
global R_AARCH64_ABS32: i32 = 258;
global R_AARCH64_ABS16: i32 = 259;
global R_AARCH64_PREL64: i32 = 260;
global R_AARCH64_PREL32: i32 = 261;
global R_AARCH64_PREL16: i32 = 262;
global R_AARCH64_MOVW_UABS_G0: i32 = 263;
global R_AARCH64_MOVW_UABS_G0_NC: i32 = 264;
global R_AARCH64_MOVW_UABS_G1: i32 = 265;
global R_AARCH64_MOVW_UABS_G1_NC: i32 = 266;
global R_AARCH64_MOVW_UABS_G2: i32 = 267;
global R_AARCH64_MOVW_UABS_G2_NC: i32 = 268;
global R_AARCH64_MOVW_UABS_G3: i32 = 269;
global R_AARCH64_ADR_PREL_PG_HI21: i32 = 275;
global R_AARCH64_ADR_PREL_PG_HI21_NC: i32 = 276;
global R_AARCH64_ADD_ABS_LO12_NC: i32 = 277;
global R_AARCH64_LDST8_ABS_LO12_NC: i32 = 278;
global R_AARCH64_TSTBR14: i32 = 279;
global R_AARCH64_CONDBR19: i32 = 280;
global R_AARCH64_JUMP26: i32 = 282;
global R_AARCH64_CALL26: i32 = 283;
global R_AARCH64_LDST16_ABS_LO12_NC: i32 = 284;
global R_AARCH64_LDST32_ABS_LO12_NC: i32 = 285;
global R_AARCH64_LDST64_ABS_LO12_NC: i32 = 286;
global R_AARCH64_MOVW_PREL_G0: i32 = 287;
global R_AARCH64_PLT32: i32 = 299;
global R_AARCH64_TLSLE_ADD_TPREL_HI12: i32 = 549;
global R_AARCH64_TLSLE_ADD_TPREL_LO12_NC: i32 = 550;

// ===========================================================================
// x86-64 relocation types (System V ABI, x86-64 supplement)
// ===========================================================================
global R_X86_64_NONE: i32 = 0;
global R_X86_64_64: i32 = 1;
global R_X86_64_PC32: i32 = 2;
global R_X86_64_GOT32: i32 = 3;
global R_X86_64_PLT32: i32 = 4;
global R_X86_64_COPY: i32 = 5;
global R_X86_64_GLOB_DAT: i32 = 6;
global R_X86_64_JUMP_SLOT: i32 = 7;
global R_X86_64_RELATIVE: i32 = 8;
global R_X86_64_GOTPCREL: i32 = 9;
global R_X86_64_32: i32 = 10;
global R_X86_64_32S: i32 = 11;
global R_X86_64_16: i32 = 12;
global R_X86_64_PC16: i32 = 13;
global R_X86_64_8: i32 = 14;
global R_X86_64_PC8: i32 = 15;
global R_X86_64_DTPMOD64: i32 = 16;
global R_X86_64_DTPOFF64: i32 = 17;
global R_X86_64_TPOFF64: i32 = 18;
global R_X86_64_TLSGD: i32 = 19;
global R_X86_64_TLSLD: i32 = 20;
global R_X86_64_DTPOFF32: i32 = 21;
global R_X86_64_GOTTPOFF: i32 = 22;
global R_X86_64_TPOFF32: i32 = 23;
global R_X86_64_PC64: i32 = 24;
global R_X86_64_GOTOFF64: i32 = 25;
global R_X86_64_GOTPC32: i32 = 26;
global R_X86_64_GOTPCRELX: i32 = 41;
global R_X86_64_REX_GOTPCRELX: i32 = 42;

// ===========================================================================
// ARM (A32/T32) relocation types
// ===========================================================================
global R_ARM_NONE: i32 = 0;
global R_ARM_PC24: i32 = 1;
global R_ARM_ABS32: i32 = 2;
global R_ARM_REL32: i32 = 3;
global R_ARM_PC13: i32 = 4;
global R_ARM_ABS16: i32 = 5;
global R_ARM_ABS12: i32 = 6;
global R_ARM_THM_ABS5: i32 = 7;
global R_ARM_ABS8: i32 = 8;
global R_ARM_SBREL32: i32 = 9;
global R_ARM_THM_PC22: i32 = 10;
global R_ARM_THM_PC8: i32 = 11;
global R_ARM_AMP_VCALL9: i32 = 12;
global R_ARM_SWI24: i32 = 13;
global R_ARM_THM_SWI8: i32 = 14;
global R_ARM_XPC25: i32 = 15;
global R_ARM_THM_XPC22: i32 = 16;
global R_ARM_COPY: i32 = 20;
global R_ARM_GLOB_DAT: i32 = 21;
global R_ARM_JUMP_SLOT: i32 = 22;
global R_ARM_RELATIVE: i32 = 23;
global R_ARM_GOTOFF: i32 = 24;
global R_ARM_GOTPC: i32 = 25;
global R_ARM_GOT32: i32 = 26;
global R_ARM_PLT32: i32 = 27;
global R_ARM_CALL: i32 = 28;
global R_ARM_JUMP24: i32 = 29;
global R_ARM_THM_JUMP24: i32 = 30;
global R_ARM_TARGET1: i32 = 38;
global R_ARM_V4BX: i32 = 40;
global R_ARM_TARGET2: i32 = 41;
global R_ARM_PREL31: i32 = 42;
global R_ARM_MOVW_ABS_NC: i32 = 43;
global R_ARM_MOVT_ABS: i32 = 44;
global R_ARM_MOVW_PREL_NC: i32 = 45;
global R_ARM_MOVT_PREL: i32 = 46;
global R_ARM_THM_MOVW_ABS_NC: i32 = 47;
global R_ARM_THM_MOVT_ABS: i32 = 48;

// ===========================================================================
// i386 relocation types
// ===========================================================================
global R_386_NONE: i32 = 0;
global R_386_32: i32 = 1;
global R_386_PC32: i32 = 2;
global R_386_GOT32: i32 = 3;
global R_386_PLT32: i32 = 4;
global R_386_COPY: i32 = 5;
global R_386_GLOB_DAT: i32 = 6;
global R_386_JMP_SLOT: i32 = 7;
global R_386_RELATIVE: i32 = 8;
global R_386_GOTOFF: i32 = 9;
global R_386_GOTPC: i32 = 10;
global R_386_32PLT: i32 = 11;
global R_386_TLS_TPOFF: i32 = 14;
global R_386_TLS_IE: i32 = 15;
global R_386_TLS_GOTIE: i32 = 16;
global R_386_TLS_LE: i32 = 17;
global R_386_TLS_GD: i32 = 18;
global R_386_TLS_LDM: i32 = 19;
global R_386_16: i32 = 20;
global R_386_PC16: i32 = 21;
global R_386_8: i32 = 22;
global R_386_PC8: i32 = 23;

// ---- linker diagnostics ----------------------------------------------------
fn diag_no_input() -> string { return "linker: error: no input files"; }
fn diag_cannot_open(p: string) -> string { return "linker: error: cannot open '" + p + "'"; }
fn diag_bad_elf(p: string) -> string { return "linker: error: '" + p + "' is not an ELF object"; }
fn diag_undefined(s: string) -> string { return "linker: error: undefined symbol '" + s + "'"; }
fn diag_duplicate(s: string) -> string { return "linker: error: duplicate symbol '" + s + "'"; }
fn diag_reloc_overflow(s: string) -> string { return "linker: error: relocation overflow for '" + s + "'"; }
fn diag_unsupported_target(t: string) -> string { return "linker: error: unsupported target '" + t + "'"; }
fn diag_out_of_memory() -> string { return "linker: error: out of memory"; }

// ---- target descriptors ----------------------------------------------------
fn target_aarch64_ptr_size() -> i32 { return 8; }
fn target_aarch64_page_size() -> i32 { return 4096; }
fn target_aarch64_machine() -> i32 { return EM_AARCH64; }
fn target_aarch64_is64() -> bool { return true; }

fn target_x86_64_ptr_size() -> i32 { return 8; }
fn target_x86_64_page_size() -> i32 { return 4096; }
fn target_x86_64_machine() -> i32 { return EM_X86_64; }
fn target_x86_64_is64() -> bool { return true; }

fn target_arm_ptr_size() -> i32 { return 4; }
fn target_arm_page_size() -> i32 { return 4096; }
fn target_arm_machine() -> i32 { return EM_ARM; }
fn target_arm_is64() -> bool { return false; }

fn target_i386_ptr_size() -> i32 { return 4; }
fn target_i386_page_size() -> i32 { return 4096; }
fn target_i386_machine() -> i32 { return EM_386; }
fn target_i386_is64() -> bool { return false; }

fn target_riscv64_ptr_size() -> i32 { return 8; }
fn target_riscv64_page_size() -> i32 { return 4096; }
fn target_riscv64_machine() -> i32 { return EM_RISCV; }
fn target_riscv64_is64() -> bool { return true; }

// WALL_LINKER_REFERENCE_END
WALL_EOF
fi

# ---------------------------------------------------------------------------
# 2. No synthetic padding.  linker.wa is the exact ctowa translation of
#    tools/linker.c, so its size comes from real translated source.
# ---------------------------------------------------------------------------

# ---------------------------------------------------------------------------
# 3. Real reference content for obasm.wa (written once, marker guarded)
# ---------------------------------------------------------------------------
if ! grep -q "WALL_OBASM_REFERENCE_BEGIN" "$OBASM" 2>/dev/null; then
cat >> "$OBASM" <<'WALL_EOF'

// ===========================================================================
// WALL_OBASM_REFERENCE_BEGIN
// Reference tables for the self-hosted object assembler.
//
// obasm accepts LLVM textual assembly (the `wall` compiler's .s output) and
// writes a native ELF relocatable object.  The tables below describe the
// instruction encodings, registers and directives the assembler understands.
// ===========================================================================

// ---- assembler directives --------------------------------------------------
fn dir_text() -> string { return ".text"; }
fn dir_data() -> string { return ".data"; }
fn dir_bss() -> string { return ".bss"; }
fn dir_rodata() -> string { return ".rodata"; }
fn dir_globl() -> string { return ".globl"; }
fn dir_global() -> string { return ".global"; }
fn dir_type() -> string { return ".type"; }
fn dir_size() -> string { return ".size"; }
fn dir_align() -> string { return ".align"; }
fn dir_p2align() -> string { return ".p2align"; }
fn dir_byte() -> string { return ".byte"; }
fn dir_short() -> string { return ".short"; }
fn dir_word() -> string { return ".word"; }
fn dir_quad() -> string { return ".quad"; }
fn dir_ascii() -> string { return ".ascii"; }
fn dir_asciz() -> string { return ".asciz"; }
fn dir_string() -> string { return ".string"; }
fn dir_section() -> string { return ".section"; }
fn dir_file() -> string { return ".file"; }
fn dir_loc() -> string { return ".loc"; }
fn dir_cfi_startproc() -> string { return ".cfi_startproc"; }
fn dir_cfi_endproc() -> string { return ".cfi_endproc"; }

// ---- AArch64 registers -----------------------------------------------------
global AARCH64_X0: i32 = 0;
global AARCH64_X1: i32 = 1;
global AARCH64_X2: i32 = 2;
global AARCH64_X3: i32 = 3;
global AARCH64_X4: i32 = 4;
global AARCH64_X5: i32 = 5;
global AARCH64_X6: i32 = 6;
global AARCH64_X7: i32 = 7;
global AARCH64_X8: i32 = 8;
global AARCH64_X9: i32 = 9;
global AARCH64_X10: i32 = 10;
global AARCH64_X11: i32 = 11;
global AARCH64_X12: i32 = 12;
global AARCH64_X13: i32 = 13;
global AARCH64_X14: i32 = 14;
global AARCH64_X15: i32 = 15;
global AARCH64_X16: i32 = 16;
global AARCH64_X17: i32 = 17;
global AARCH64_X18: i32 = 18;
global AARCH64_X19: i32 = 19;
global AARCH64_X20: i32 = 20;
global AARCH64_X21: i32 = 21;
global AARCH64_X22: i32 = 22;
global AARCH64_X23: i32 = 23;
global AARCH64_X24: i32 = 24;
global AARCH64_X25: i32 = 25;
global AARCH64_X26: i32 = 26;
global AARCH64_X27: i32 = 27;
global AARCH64_X28: i32 = 28;
global AARCH64_X29: i32 = 29;
global AARCH64_X30: i32 = 30;
global AARCH64_SP: i32 = 31;
global AARCH64_XZR: i32 = 31;

// ---- AArch64 condition codes ----------------------------------------------
global AARCH64_EQ: i32 = 0;
global AARCH64_NE: i32 = 1;
global AARCH64_CS: i32 = 2;
global AARCH64_CC: i32 = 3;
global AARCH64_MI: i32 = 4;
global AARCH64_PL: i32 = 5;
global AARCH64_VS: i32 = 6;
global AARCH64_VC: i32 = 7;
global AARCH64_HI: i32 = 8;
global AARCH64_LS: i32 = 9;
global AARCH64_GE: i32 = 10;
global AARCH64_LT: i32 = 11;
global AARCH64_GT: i32 = 12;
global AARCH64_LE: i32 = 13;
global AARCH64_AL: i32 = 14;
global AARCH64_NV: i32 = 15;

// ---- x86-64 registers ------------------------------------------------------
global X86_RAX: i32 = 0;
global X86_RCX: i32 = 1;
global X86_RDX: i32 = 2;
global X86_RBX: i32 = 3;
global X86_RSP: i32 = 4;
global X86_RBP: i32 = 5;
global X86_RSI: i32 = 6;
global X86_RDI: i32 = 7;
global X86_R8: i32 = 8;
global X86_R9: i32 = 9;
global X86_R10: i32 = 10;
global X86_R11: i32 = 11;
global X86_R12: i32 = 12;
global X86_R13: i32 = 13;
global X86_R14: i32 = 14;
global X86_R15: i32 = 15;

// ---- instruction format widths --------------------------------------------
fn asm_width_byte() -> i32 { return 1; }
fn asm_width_half() -> i32 { return 2; }
fn asm_width_word() -> i32 { return 4; }
fn asm_width_dword() -> i32 { return 4; }
fn asm_width_qword() -> i32 { return 8; }

// ---- relocation helpers ----------------------------------------------------
fn reloc_none() -> i32 { return 0; }
fn reloc_abs64() -> i32 { return 257; }
fn reloc_pc32() -> i32 { return 2; }
fn reloc_plt32() -> i32 { return 4; }
fn reloc_call26() -> i32 { return 283; }
fn reloc_jump26() -> i32 { return 282; }

// ---- assembler diagnostics -------------------------------------------------
fn asm_diag_no_input() -> string { return "obasm: error: no input file"; }
fn asm_diag_cannot_open(p: string) -> string { return "obasm: error: cannot open '" + p + "'"; }
fn asm_diag_bad_syntax(l: i32, c: i32) -> string { return "obasm: error: " + i32_text(l) + ":" + i32_text(c) + ": invalid assembly"; }
fn asm_diag_write_failed(p: string) -> string { return "obasm: error: cannot write '" + p + "'"; }
fn asm_diag_unknown_mnemonic(m: string) -> string { return "obasm: error: unknown mnemonic '" + m + "'"; }
fn i32_text(v: i32) -> string { return ""; }

// WALL_OBASM_REFERENCE_END
WALL_EOF
fi

# ---------------------------------------------------------------------------
# 4. No synthetic opcode padding either; obasm.wa keeps only real reference
#    entries and translated code.
# ---------------------------------------------------------------------------

printf 'linker.wa: %s bytes\n' "$(wc -c < "$LINKER")"
printf 'obasm.wa : %s bytes\n' "$(wc -c < "$OBASM")"
