/* linker.c - Wall self-contained ELF linker.
 *
 * Links AArch64 ELF64 relocatable objects into a static ELF executable or a
 * shared object *without* invoking any external tool and *without* linking a
 * C library.  The Wall freestanding runtime (runtime/wallrt.o + runtime/start.o)
 * is embedded in this binary and merged into every link, supplying _start,
 * printf, write, exit and the memory/string primitives through raw Linux
 * system calls.
 *
 * Pipeline:
 *   1. parse every input ELF64 relocatable object,
 *   2. classify allocatable sections into text / rodata / data / bss,
 *   3. assign addresses and lay the blobs out into ELF segments,
 *   4. build a global symbol table,
 *   5. apply AArch64 relocations (ABS/CALL/JUMP/ADR_PREL/LO12/MOVW/...),
 *   6. write the ELF header, program headers, sections and .comment.
 *
 * There is no exec(), no system(), no popen() and no library dependency in the
 * produced binary.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <sys/stat.h>
#include "runtime_blob.h"
#include "wall_cli.h"
#include "wall_log.h"

#ifndef WALL_DEFAULT_COMMENT
#define WALL_DEFAULT_COMMENT "wall version 1.0.0"
#endif

#define MAXSEC 4096
/* First byte of .text.  64-byte ELF header + 3*56-byte program headers = 232,
 * rounded to 16.  Keeping text in the first page avoids a 4 KiB pad and does
 * not violate p_align because p_offset and p_vaddr share the same residue. */
#define WALL_TEXT_OFF 0xF0u
#define SHF_WRITE 0x1
#define SHF_ALLOC 0x2
#define SHF_EXECINSTR 0x4
#define SHT_PROGBITS 1
#define SHT_SYMTAB 2
#define SHT_STRTAB 3
#define SHT_RELA 4
#define SHT_NOBITS 8
#define SHT_NOTE 7
#define PT_NOTE 4
#define SHN_UNDEF 0
#define SHN_ABS 0xfff1
#define SHN_COMMON 0xfff2

/* ------------------------------------------------------------------------- */
/* Little-endian readers / writers                                            */
/* ------------------------------------------------------------------------- */
static uint16_t r16(const unsigned char *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t r32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t r64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void w16(unsigned char *p, uint16_t v) { memcpy(p, &v, 2); }
static void w32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void w64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }
static uint64_t align_up(uint64_t v, uint64_t a) { return a ? (v + a - 1) / a * a : v; }

/* ------------------------------------------------------------------------- */
/* Growable byte buffer                                                       */
/* ------------------------------------------------------------------------- */
typedef struct { unsigned char *p; size_t len, cap; } Buf;

static void b_reserve(Buf *b, size_t need) {
  if (b->len + need <= b->cap) return;
  size_t c = b->cap ? b->cap : 4096;
  while (c < b->len + need) c *= 2;
  unsigned char *np = (unsigned char *)realloc(b->p, c);
  if (!np) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  memset(np + b->cap, 0, c - b->cap);
  b->p = np;
  b->cap = c;
}
static void b_append(Buf *b, const void *d, size_t n) {
  if (!n) return;
  b_reserve(b, n);
  memcpy(b->p + b->len, d, n);
  b->len += n;
}
static void b_byte(Buf *b, unsigned char c) { b_reserve(b, 1); b->p[b->len++] = c; }
static void b_align(Buf *b, size_t a) {
  if (!a) a = 1;
  while (b->len % a) b_byte(b, 0);
}
static int b_stradd(Buf *b, const char *s) {
  int off = (int)b->len;
  b_append(b, s, strlen(s) + 1);
  return off;
}

/* ------------------------------------------------------------------------- */
/* Input objects                                                              */
/* ------------------------------------------------------------------------- */
typedef struct {
  const char *name;
  const unsigned char *buf;
  size_t size;
  int owned;
  uint64_t shoff;
  uint16_t shentsize, shnum, shstrndx;
  uint64_t symoff, symsz, stroff, strsz;
  int sec_kind[MAXSEC];    /* 0 skip, 1 text, 2 rodata, 3 data, 4 bss */
  size_t sec_off[MAXSEC];
  uint64_t sec_addr[MAXSEC];
  int execstack;           /* .note.GNU-stack has SHF_EXECINSTR */
} Obj;

static Obj *g_objs;
static int g_nobjs, g_capobjs;

/* Undefined symbols drive archive extraction, just like ld. */
static char **g_undef;
static int g_nundef, g_capundef;

static int undef_has(const char *name) {
  if (!name || !name[0]) return 0;
  for (int i = 0; i < g_nundef; i++) if (!strcmp(g_undef[i], name)) return 1;
  return 0;
}
static void undef_add(const char *name) {
  if (!name || !name[0] || undef_has(name)) return;
  if (g_nundef == g_capundef) {
    g_capundef = g_capundef ? g_capundef * 2 : 64;
    g_undef = (char **)realloc(g_undef, (size_t)g_capundef * sizeof(char *));
    if (!g_undef) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  }
  g_undef[g_nundef++] = strdup(name);
}
static void undef_del(const char *name) {
  for (int i = 0; i < g_nundef; i++) {
    if (!strcmp(g_undef[i], name)) {
      free(g_undef[i]);
      g_undef[i] = g_undef[--g_nundef];
      return;
    }
  }
}

/* Archives are loaded lazily: only members that satisfy an undefined symbol
 * (or all members inside --whole-archive) become objects. */
typedef struct { char *name; const unsigned char *data; size_t size; uint64_t hdr_off; int loaded; } ArMember;
typedef struct {
  const unsigned char *buf;
  size_t size;
  ArMember *mem;
  int nmem, capmem;
  char **syms;
  int *symmem;
  int nsyms, capsyms;
  int whole;
} ArInfo;
static ArInfo *g_ars;
static int g_nars, g_capars;
static int g_whole_archive;

static Buf g_text, g_ro, g_data;
static Buf g_got;
static uint64_t g_bss;
static uint64_t g_text_addr, g_ro_addr, g_data_addr, g_bss_addr, g_got_addr;
static int g_shared;
static int g_strip = 1;   /* small binaries by default; --symbols keeps symtab */
static int g_build_id;    /* --build-id */

/* Option state for real GNU ld compatible behaviour. */
static int g_allow_multiple_definition;
static int g_define_common = 1;
static int g_fatal_warnings;
static int g_no_warnings;
static int g_trace;
static int g_stats;
static int g_print_memory_usage;
static int g_require_defined;
static int g_error_execstack;
static int g_warn_execstack;
static int g_execstack;
static int g_no_undefined = 1;
static int g_warn_unresolved;
static int g_sort_common = 1;
static int g_warn_common;
static int g_print_output_format;
static int g_had_error;
static int g_nwarn;
static char **g_allow_undef;
static int g_nallow_undef;
static char **g_required;
static int g_nrequired;
static const char *g_oformat;
static const char *g_sysroot;
static char **g_libdirs;
static int g_nlibdirs;
static const char *g_retain_file;
static char **g_retain;
static int g_nretain;
static char **g_maplines;
static int g_nmaplines;
static int g_map_stdout;
static struct { int whole; int static_mode; int execstack; } g_state_stack[32];
static int g_nstate_stack;

/* --build-id needs a fourth program header (PT_NOTE): 64 + 4*56 = 288. */
static uint64_t text_file_off(void) { return g_build_id ? 0x120u : WALL_TEXT_OFF; }
static const char *g_map_path;

typedef struct {
  char name[256];
  uint64_t addr;
  int bind;
  uint64_t got_addr;
  int has_got;
  int is_common;
  int defined;
  uint64_t common_size;
  uint64_t common_align;
} SymEnt;
static SymEnt *g_syms;
static int g_nsym, g_capsym;

static const unsigned char *sh_ptr(Obj *o, int i) {
  return o->buf + o->shoff + (uint64_t)o->shentsize * (uint64_t)i;
}
static const char *sec_name(Obj *o, int i) {
  const unsigned char *shstr = o->buf + r64(sh_ptr(o, (int)o->shstrndx) + 24);
  uint32_t no = r32(sh_ptr(o, i));
  return (const char *)(shstr + no);
}

static int obj_parse(Obj *o) {
  if (o->size < 64) return -1;
  if (o->buf[0] != 0x7f || o->buf[1] != 'E' || o->buf[2] != 'L' || o->buf[3] != 'F') return -1;
  if (o->buf[4] != 2 || o->buf[5] != 1) {
    WALL_LOGE("linker", "'%s' is not a 64-bit little-endian ELF object\n", o->name);
    return -1;
  }
  if (r16(o->buf + 16) != 1) {
    WALL_LOGE("linker", "'%s' is not a relocatable object\n", o->name);
    return -1;
  }
  if (r16(o->buf + 18) != 183) {
    WALL_LOGE("linker", "'%s' is not an AArch64 object\n", o->name);
    return -1;
  }
  o->shoff = r64(o->buf + 40);
  o->shentsize = r16(o->buf + 58);
  o->shnum = r16(o->buf + 60);
  o->shstrndx = r16(o->buf + 62);
  if (o->shentsize < 64 || o->shnum == 0 || o->shnum > MAXSEC || o->shstrndx >= o->shnum) return -1;
  if (o->shoff + (uint64_t)o->shentsize * o->shnum > o->size) return -1;

  o->symoff = o->symsz = o->stroff = o->strsz = 0;
  for (int i = 1; i < o->shnum; i++) {
    const unsigned char *sh = sh_ptr(o, i);
    const char *nm = sec_name(o, i);
    uint32_t type = r32(sh + 4);
    uint64_t flags = r64(sh + 8);
    int kind = 0;
    if (flags & SHF_ALLOC) {
      if (type == SHT_PROGBITS) {
        if (flags & SHF_EXECINSTR) kind = 1;
        else if (flags & SHF_WRITE) kind = 3;
        else kind = 2;
      } else if (type == SHT_NOBITS && (flags & SHF_WRITE)) {
        kind = 4;
      }
    }
    if (!strcmp(nm, ".eh_frame") || !strncmp(nm, ".note", 5) ||
        !strcmp(nm, ".comment") || !strcmp(nm, ".llvm_addrsig") ||
        !strncmp(nm, ".ARM.attributes", 15)) {
      if (!strcmp(nm, ".note.GNU-stack") && (flags & SHF_EXECINSTR)) o->execstack = 1;
      kind = 0;
    }
    o->sec_kind[i] = kind;
    o->sec_off[i] = 0;
    o->sec_addr[i] = 0;
  }
  for (int i = 1; i < o->shnum; i++) {
    const unsigned char *sh = sh_ptr(o, i);
    if (r32(sh + 4) == SHT_SYMTAB) {
      o->symoff = r64(sh + 24);
      o->symsz = r64(sh + 32);
      uint32_t link = r32(sh + 40);
      if (link < o->shnum) {
        const unsigned char *st = sh_ptr(o, (int)link);
        o->stroff = r64(st + 24);
        o->strsz = r64(st + 32);
      }
      break;
    }
  }
  return 0;
}

static void scan_object_symbols(Obj *o) {
  if (!o->symsz) return;
  uint64_t n = o->symsz / 24;
  for (uint64_t j = 0; j < n; j++) {
    const unsigned char *s = o->buf + o->symoff + j * 24;
    uint32_t nm = r32(s + 0);
    unsigned char info = s[4];
    uint16_t shndx = r16(s + 6);
    int bind = info >> 4;
    if (bind != 1 && bind != 2) continue;
    const char *name = (nm < o->strsz) ? (const char *)(o->buf + o->stroff + nm) : "";
    if (!name[0]) continue;
    if (shndx == SHN_UNDEF) undef_add(name);
    else undef_del(name);
  }
}

static int add_object(const char *name, const unsigned char *buf, size_t size, int owned) {
  if (g_nobjs == g_capobjs) {
    g_capobjs = g_capobjs ? g_capobjs * 2 : 16;
    g_objs = (Obj *)realloc(g_objs, (size_t)g_capobjs * sizeof(Obj));
    if (!g_objs) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  }
  Obj *o = &g_objs[g_nobjs];
  memset(o, 0, sizeof(*o));
  o->name = name;
  o->buf = buf;
  o->size = size;
  o->owned = owned;
  if (obj_parse(o) != 0) return -1;
  g_nobjs++;
  scan_object_symbols(o);
  return 0;
}

/* ------------------------------------------------------------------------- */
/* GNU/BSD ar archives                                                        */
/* ------------------------------------------------------------------------- */
static int is_ar(const unsigned char *b, size_t n) {
  return n >= 8 && !memcmp(b, "!<arch>\n", 8);
}

static uint32_t be32(const unsigned char *p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}
static uint64_t be64(const unsigned char *p) {
  uint64_t v = 0;
  for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
  return v;
}

static int ar_add_member(ArInfo *a, char *name, const unsigned char *data,
                         size_t size, uint64_t hdr_off) {
  if (a->nmem == a->capmem) {
    a->capmem = a->capmem ? a->capmem * 2 : 16;
    a->mem = (ArMember *)realloc(a->mem, (size_t)a->capmem * sizeof(ArMember));
    if (!a->mem) return -1;
  }
  ArMember *m = &a->mem[a->nmem++];
  m->name = name;
  m->data = data;
  m->size = size;
  m->hdr_off = hdr_off;
  m->loaded = 0;
  return 0;
}

static int ar_add_sym(ArInfo *a, const char *name, int memidx) {
  if (a->nsyms == a->capsyms) {
    a->capsyms = a->capsyms ? a->capsyms * 2 : 64;
    a->syms = (char **)realloc(a->syms, (size_t)a->capsyms * sizeof(char *));
    a->symmem = (int *)realloc(a->symmem, (size_t)a->capsyms * sizeof(int));
    if (!a->syms || !a->symmem) return -1;
  }
  a->syms[a->nsyms] = strdup(name);
  a->symmem[a->nsyms] = memidx;
  a->nsyms++;
  return 0;
}

static int add_archive(const char *name, const unsigned char *buf, size_t size) {
  if (!is_ar(buf, size)) return -1;
  (void)name;
  ArInfo a;
  memset(&a, 0, sizeof(a));
  a.buf = buf;
  a.size = size;
  a.whole = g_whole_archive;

  const unsigned char *longnames = NULL;
  size_t longsz = 0;
  const unsigned char *symtab = NULL;
  size_t symsz = 0;
  int sym64 = 0;

  size_t pos = 8;
  while (pos + 60 <= size) {
    const unsigned char *h = buf + pos;
    char nm[17];
    memcpy(nm, h, 16);
    nm[16] = 0;
    for (int i = 15; i >= 0 && (nm[i] == ' ' || nm[i] == 0); i--) nm[i] = 0;

    uint64_t msz = 0;
    int got = 0;
    for (int i = 48; i < 58; i++) {
      unsigned char c = h[i];
      if (c == ' ') { if (got) break; else continue; }
      if (c < '0' || c > '9') break;
      msz = msz * 10 + (uint64_t)(c - '0');
      got = 1;
    }
    if (!got || pos + 60 + msz > size) return -1;
    const unsigned char *data = buf + pos + 60;

    if (!strcmp(nm, "/")) {
      symtab = data;
      symsz = (size_t)msz;
      sym64 = 0;
    } else if (!strcmp(nm, "/SYM64/")) {
      symtab = data;
      symsz = (size_t)msz;
      sym64 = 1;
    } else if (!strcmp(nm, "//")) {
      longnames = data;
      longsz = (size_t)msz;
    } else {
      char real[256];
      memset(real, 0, sizeof(real));
      if (!strncmp(nm, "#1/", 3)) {
        unsigned long nl = strtoul(nm + 3, NULL, 10);
        if (nl >= msz || nl + 1 > sizeof(real)) { pos += 60 + msz; if (pos & 1) pos++; continue; }
        memcpy(real, data, nl);
        real[nl] = 0;
        data += nl;
        msz -= nl;
      } else {
        snprintf(real, sizeof(real), "%s", nm);
      }
      char *keep = (char *)malloc(strlen(real) + 1);
      if (!keep) return -1;
      strcpy(keep, real);
      if (ar_add_member(&a, keep, data, (size_t)msz, pos) != 0) return -1;
    }
    pos += 60 + (size_t)msz;
    if (pos & 1) pos++;
  }

  /* Resolve the "/<offset>" long names now that "//" has been seen. */
  for (int m = 0; m < a.nmem; m++) {
    char *nm = a.mem[m].name;
    if (nm[0] == '/' && nm[1] >= '0' && nm[1] <= '9' && longnames) {
      unsigned long off = strtoul(nm + 1, NULL, 10);
      if (off < longsz) {
        size_t k = 0;
        char real[256];
        while (off + k < longsz && longnames[off + k] != '/' &&
               longnames[off + k] != '\n' && k + 1 < sizeof(real)) {
          real[k] = (char)longnames[off + k];
          k++;
        }
        real[k] = 0;
        free(nm);
        a.mem[m].name = strdup(real);
      }
    }
  }

  /* Parse the archive symbol index: count, member offsets, then names. */
  if (symtab && symsz) {
    uint64_t cnt = sym64 ? be64(symtab) : be32(symtab);
    size_t esz = sym64 ? 8 : 4;
    size_t off = sym64 ? 8 : 4;
    if (off + cnt * esz > symsz) return -1;
    size_t nameoff = off + (size_t)cnt * esz;
    for (uint64_t s = 0; s < cnt; s++) {
      uint64_t mo = sym64 ? be64(symtab + off + s * 8) : be32(symtab + off + s * 4);
      int mi = -1;
      for (int m = 0; m < a.nmem; m++) if (a.mem[m].hdr_off == mo) { mi = (int)m; break; }
      size_t nl = strlen((const char *)(symtab + nameoff));
      if (mi >= 0 && nameoff + nl <= symsz) ar_add_sym(&a, (const char *)(symtab + nameoff), mi);
      nameoff += nl + 1;
      if (nameoff > symsz) break;
    }
  }

  if (g_nars == g_capars) {
    g_capars = g_capars ? g_capars * 2 : 8;
    g_ars = (ArInfo *)realloc(g_ars, (size_t)g_capars * sizeof(ArInfo));
    if (!g_ars) return -1;
  }
  g_ars[g_nars++] = a;
  return 0;
}

static int ar_load_member(ArInfo *a, int idx) {
  if (idx < 0 || idx >= a->nmem || a->mem[idx].loaded) return 0;
  a->mem[idx].loaded = 1;
  return add_object(a->mem[idx].name, a->mem[idx].data, a->mem[idx].size, 0);
}

/* Pull members until every archive-provided undefined symbol is satisfied. */
static int resolve_archives(void) {
  int changed = 1;
  while (changed) {
    changed = 0;
    for (int k = 0; k < g_nars; k++) {
      ArInfo *a = &g_ars[k];
      for (int s = 0; s < a->nsyms; s++) {
        int mi = a->symmem[s];
        if (mi < 0 || a->mem[mi].loaded) continue;
        if (a->whole || undef_has(a->syms[s])) {
          if (ar_load_member(a, mi) != 0) return -1;
          changed = 1;
        }
      }
    }
  }
  for (int k = 0; k < g_nars; k++) {
    ArInfo *a = &g_ars[k];
    if (!a->whole) continue;
    for (int m = 0; m < a->nmem; m++) {
      if (!a->mem[m].loaded && ar_load_member(a, m) != 0) return -1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Layout                                                                     */
/* ------------------------------------------------------------------------- */
static int layout_objects(void) {
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      int kind = o->sec_kind[i];
      if (!kind) continue;
      const unsigned char *sh = sh_ptr(o, i);
      uint64_t off = r64(sh + 24);
      uint64_t sz = r64(sh + 32);
      uint64_t al = r64(sh + 48);
      if (al < 1) al = 1;
      if (off + sz > o->size && kind != 4) {
        WALL_LOGE("linker", "section out of bounds in '%s'\n", o->name);
        return -1;
      }
      if (kind == 4) {
        g_bss = align_up(g_bss, al);
        o->sec_off[i] = (size_t)g_bss;
        g_bss += sz;
      } else {
        Buf *b = kind == 1 ? &g_text : kind == 2 ? &g_ro : &g_data;
        b_align(b, (size_t)al);
        o->sec_off[i] = b->len;
        b_append(b, o->buf + off, (size_t)sz);
      }
    }
  }
  return 0;
}

static int assign_addresses(void) {
  uint64_t base = g_shared ? 0 : 0x400000;
  uint64_t text_off = text_file_off();
  g_text_addr = base + text_off;
  uint64_t ro_off = align_up(text_off + g_text.len, 16);
  g_ro_addr = base + ro_off;
  uint64_t data_off = align_up(ro_off + g_ro.len, 0x1000);
  g_data_addr = base + data_off;
  g_bss_addr = base + align_up(data_off + g_data.len, 16);
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      int kind = o->sec_kind[i];
      uint64_t b = kind == 1 ? g_text_addr : kind == 2 ? g_ro_addr : kind == 3 ? g_data_addr : kind == 4 ? g_bss_addr : 0;
      o->sec_addr[i] = b + (uint64_t)o->sec_off[i];
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Symbols                                                                    */
/* ------------------------------------------------------------------------- */
static SymEnt *sym_find(const char *name) {
  for (int i = 0; i < g_nsym; i++)
    if (!strcmp(g_syms[i].name, name)) return &g_syms[i];
  return NULL;
}

static void lwarn(const char *fmt, ...) {
  if (g_no_warnings) return;
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  WALL_LOGW("linker", "%s\n", buf);
  g_nwarn++;
  if (g_fatal_warnings) g_had_error = 1;
}
static void lerror(const char *fmt, ...) {
  char buf[1024];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  WALL_LOGE("linker", "%s\n", buf);
  g_had_error = 1;
}
static int allow_undef_name(const char *name) {
  for (int i = 0; i < g_nallow_undef; i++) if (!strcmp(g_allow_undef[i], name)) return 1;
  return 0;
}
static int retain_has(const char *name) {
  if (g_nretain == 0) return 1;
  for (int i = 0; i < g_nretain; i++) if (!strcmp(g_retain[i], name)) return 1;
  return 0;
}

static void sym_add(const char *name, uint64_t addr, int bind) {
  if (!name || !name[0]) return;
  SymEnt *e = sym_find(name);
  if (e) {
    if (bind == 1) {
      if (e->bind == 1 && e->defined && !e->is_common && !g_allow_multiple_definition) {
        lerror("multiple definition of '%s'", name);
        return;
      }
      e->addr = addr; e->bind = 1; e->defined = 1; e->is_common = 0;
    } else if (e->bind != 1) {
      e->addr = addr; e->bind = 2; e->defined = 1;
    }
    return;
  }
  if (g_nsym == g_capsym) {
    g_capsym = g_capsym ? g_capsym * 2 : 64;
    g_syms = (SymEnt *)realloc(g_syms, (size_t)g_capsym * sizeof(SymEnt));
    if (!g_syms) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  }
  SymEnt *n = &g_syms[g_nsym++];
  memset(n, 0, sizeof(*n));
  strncpy(n->name, name, sizeof(n->name) - 1);
  n->addr = addr;
  n->bind = bind;
  n->defined = 1;
}

static SymEnt *sym_ensure(const char *name, int bind) {
  SymEnt *e = sym_find(name);
  if (e) return e;
  if (g_nsym == g_capsym) {
    g_capsym = g_capsym ? g_capsym * 2 : 64;
    g_syms = (SymEnt *)realloc(g_syms, (size_t)g_capsym * sizeof(SymEnt));
    if (!g_syms) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  }
  e = &g_syms[g_nsym++];
  memset(e, 0, sizeof(*e));
  strncpy(e->name, name, sizeof(e->name) - 1);
  e->bind = bind;
  return e;
}

static int sym_addr(Obj *o, uint32_t idx, uint64_t *out, const char **name_out) {
  if (!o->symsz || (uint64_t)(idx + 1) * 24 > o->symsz) return -1;
  const unsigned char *s = o->buf + o->symoff + (uint64_t)idx * 24;
  uint32_t nm = r32(s + 0);
  unsigned char info = s[4];
  uint16_t shndx = r16(s + 6);
  uint64_t val = r64(s + 8);
  int bind = info >> 4;
  const char *name = (nm < o->strsz) ? (const char *)(o->buf + o->stroff + nm) : "";
  if (name_out) *name_out = name;
  if (shndx == SHN_UNDEF) {
    SymEnt *e = sym_find(name);
    if (!e) {
      if (bind == 2) { *out = 0; return 0; }
      return -2;
    }
    *out = e->addr;
    return 0;
  }
  if (shndx == SHN_ABS) { *out = val; return 0; }
  if (shndx == SHN_COMMON) {
    SymEnt *e = sym_find(name);
    if (!e) return -2;
    if (!g_define_common) return -2;
    *out = e->addr;
    return 0;
  }
  if (shndx < o->shnum && o->sec_kind[shndx]) { *out = o->sec_addr[shndx] + val; return 0; }
  *out = val;
  return 0;
}

static int register_symbols(void) {
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    if (!o->symsz) continue;
    uint64_t n = o->symsz / 24;
    for (uint64_t j = 0; j < n; j++) {
      const unsigned char *s = o->buf + o->symoff + j * 24;
      uint32_t nm = r32(s + 0);
      unsigned char info = s[4];
      uint16_t shndx = r16(s + 6);
      uint64_t val = r64(s + 8);
      int bind = info >> 4;
      if (bind != 1 && bind != 2) continue;
      const char *name = (nm < o->strsz) ? (const char *)(o->buf + o->stroff + nm) : "";
      if (!name[0]) continue;
      uint64_t size = r64(s + 16);
      uint64_t addr;
      if (shndx == SHN_UNDEF) continue;
      else if (shndx == SHN_ABS) addr = val;
      else if (shndx == SHN_COMMON) {
        SymEnt *e = sym_find(name);
        if (e && e->defined && !e->is_common) continue;
        e = sym_ensure(name, bind);
        e->is_common = 1;
        e->common_size = size;
        e->common_align = val ? val : 1;
        continue;
      } else if (shndx < o->shnum && o->sec_kind[shndx]) addr = o->sec_addr[shndx] + val;
      else continue;
      sym_add(name, addr, bind);
    }
  }
  return 0;
}

/* Allocate tentative definitions (SHN_COMMON) in .bss, after any GOT shift. */
static void allocate_commons(void) {
  if (!g_define_common) return;
  /* --sort-common: largest alignment first, matching GNU ld's default. */
  int n = 0;
  for (int i = 0; i < g_nsym; i++) if (g_syms[i].is_common) n++;
  if (g_warn_common) {
    for (int i = 0; i < g_nsym; i++)
      if (g_syms[i].is_common) lwarn("common symbol '%s' is being allocated", g_syms[i].name);
  }
  if (n > 1 && g_sort_common) {
    SymEnt **v = (SymEnt **)malloc((size_t)n * sizeof(SymEnt *));
    int k = 0;
    for (int i = 0; i < g_nsym; i++) if (g_syms[i].is_common) v[k++] = &g_syms[i];
    for (int i = 1; i < n; i++) {
      SymEnt *x = v[i]; int j = i - 1;
      while (j >= 0 && v[j]->common_align < x->common_align) { v[j + 1] = v[j]; j--; }
      v[j + 1] = x;
    }
    for (int i = 0; i < n; i++) {
      SymEnt *e = v[i];
      uint64_t a = e->common_align ? e->common_align : 1;
      g_bss = align_up(g_bss, a);
      e->addr = g_bss_addr + g_bss;
      g_bss += e->common_size;
      e->is_common = 0;
      e->defined = 1;
    }
    free(v);
    return;
  }
  for (int i = 0; i < g_nsym; i++) {
    SymEnt *e = &g_syms[i];
    if (!e->is_common) continue;
    uint64_t a = e->common_align ? e->common_align : 1;
    g_bss = align_up(g_bss, a);
    e->addr = g_bss_addr + g_bss;
    g_bss += e->common_size;
    e->is_common = 0;
    e->defined = 1;
  }
}

/* ------------------------------------------------------------------------- */
/* Global offset table (for PIC/shared links)                                 */
/* ------------------------------------------------------------------------- */
static int got_for(const char *name) {
  SymEnt *e = sym_find(name);
  if (!e) {
    /* A weak undefined symbol legitimately resolves to address zero.  Give it
     * an entry so the GOT slot can hold that zero. */
    e = sym_ensure(name, 2);
    e->addr = 0;
    e->defined = 0;
  }
  if (!e->has_got) {
    b_align(&g_got, 8);
    e->got_addr = (uint64_t)g_got.len;   /* temporary: byte offset */
    unsigned char zero[8] = {0, 0, 0, 0, 0, 0, 0, 0};
    b_append(&g_got, zero, 8);
    e->has_got = 1;
  }
  return 0;
}

static int got_scan(void) {
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      const unsigned char *sh = sh_ptr(o, i);
      if (r32(sh + 4) != SHT_RELA) continue;
      int target = (int)r32(sh + 44);
      if (target <= 0 || target >= o->shnum || o->sec_kind[target] == 0) continue;
      uint64_t roff = r64(sh + 24);
      uint64_t rsz = r64(sh + 32);
      if (roff + rsz > o->size) return -1;
      uint64_t n = rsz / 24;
      for (uint64_t j = 0; j < n; j++) {
        const unsigned char *e = o->buf + roff + j * 24;
        uint64_t r_info = r64(e + 8);
        uint32_t symidx = (uint32_t)(r_info >> 32);
        uint32_t type = (uint32_t)(r_info & 0xffffffffu);
        if (type != 311 && type != 312) continue;   /* ADR_GOT_PAGE / LD64_GOT_LO12_NC */
        uint64_t S = 0;
        const char *name = "";
        int rc = sym_addr(o, symidx, &S, &name);
        if (rc != 0) {
          WALL_LOGE("linker", "unresolved GOT symbol '%s' in '%s'\n", name, o->name);
          return -1;
        }
        if (got_for(name) != 0) {
          WALL_LOGE("linker", "cannot allocate GOT entry for '%s'\n", name);
          return -1;
        }
      }
    }
  }
  return 0;
}

static void build_got(void) {
  if (!g_got.len) return;
  uint64_t base = g_shared ? 0 : 0x400000;
  uint64_t text_off = text_file_off();
  uint64_t ro_off = align_up(text_off + g_text.len, 16);
  uint64_t data_off = align_up(ro_off + g_ro.len, 0x1000);
  uint64_t got_off = align_up(data_off + g_data.len, 8);
  g_got_addr = base + got_off;
  g_bss_addr = base + align_up(got_off + g_got.len, 16);
  for (int i = 0; i < g_nsym; i++) {
    SymEnt *e = &g_syms[i];
    if (!e->has_got) continue;
    uint64_t off = e->got_addr;            /* temporary byte offset */
    e->got_addr = g_got_addr + off;
    w64(g_got.p + off, e->addr);
  }
  /* BSS moved past the GOT: refresh every .bss section address. */
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      if (o->sec_kind[i] == 4) o->sec_addr[i] = g_bss_addr + (uint64_t)o->sec_off[i];
    }
  }
}

/* ------------------------------------------------------------------------- */
/* Relocations                                                                */
/* ------------------------------------------------------------------------- */
static int patch32(Buf *b, size_t off, uint32_t v) {
  if (off + 4 > b->len) return -1;
  w32(b->p + off, v);
  return 0;
}

static int apply_reloc(int kind, Obj *o, int target, uint64_t r_off, uint32_t type,
                       uint64_t S, int64_t A, const char *name) {
  Buf *b = kind == 1 ? &g_text : kind == 2 ? &g_ro : &g_data;
  size_t off = o->sec_off[target] + (size_t)r_off;
  uint64_t P = o->sec_addr[target] + r_off;
  uint64_t X = S + (uint64_t)A;
  switch (type) {
    case 0:
      return 0;
    case 257: { /* ABS64 */
      if (off + 8 > b->len) return -1;
      w64(b->p + off, X); return 0;
    }
    case 258: { /* ABS32 */
      if (off + 4 > b->len) return -1;
      w32(b->p + off, (uint32_t)X); return 0;
    }
    case 259: { /* ABS16 */
      if (off + 2 > b->len) return -1;
      w16(b->p + off, (uint16_t)X); return 0;
    }
    case 260: { /* PREL64 */
      if (off + 8 > b->len) return -1;
      w64(b->p + off, X - P); return 0;
    }
    case 261: { /* PREL32 */
      if (off + 4 > b->len) return -1;
      w32(b->p + off, (uint32_t)(X - P)); return 0;
    }
    case 262: { /* PREL16 */
      if (off + 2 > b->len) return -1;
      w16(b->p + off, (uint16_t)(X - P)); return 0;
    }
    case 263: case 264: case 265: case 266: case 267: case 268: case 269: { /* MOVW_UABS */
      if (off + 4 > b->len) return -1;
      int shift = (type <= 266) ? ((type <= 264) ? 0 : 16) : ((type <= 268) ? 32 : 48);
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((X >> shift) & 0xFFFF);
      insn = (insn & ~(0xFFFFu << 5)) | (imm << 5);
      w32(b->p + off, insn); return 0;
    }
    case 275: case 276: { /* ADR_PREL_PG_HI21 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      int64_t delta = (int64_t)((X & ~0xFFFULL) - (P & ~0xFFFULL));
      int64_t imm = delta >> 12;
      insn = (insn & ~((3u << 29) | (0x7FFFFu << 5)));
      insn |= (((uint32_t)imm & 3u) << 29);
      insn |= ((((uint32_t)imm >> 2) & 0x7FFFFu) << 5);
      w32(b->p + off, insn); return 0;
    }
    case 277: case 278: { /* ADD/LDST8 ABS_LO12_NC */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)(X & 0xFFF);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    case 284: { /* LDST16 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((X & 0xFFF) >> 1);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    case 285: { /* LDST32 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((X & 0xFFF) >> 2);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    case 286: { /* LDST64 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((X & 0xFFF) >> 3);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    case 299: { /* LDST128 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((X & 0xFFF) >> 4);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    case 282: case 283: { /* JUMP26 / CALL26 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      int64_t d = (int64_t)(X - P);
      int64_t imm = d >> 2;
      insn = (insn & 0xFC000000u) | ((uint32_t)imm & 0x03FFFFFFu);
      w32(b->p + off, insn); return 0;
    }
    case 280: { /* CONDBR19 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)(((int64_t)(X - P) >> 2) & 0x7FFFF);
      insn = (insn & ~(0x7FFFFu << 5)) | (imm << 5);
      w32(b->p + off, insn); return 0;
    }
    case 279: { /* TSTBR14 */
      if (off + 4 > b->len) return -1;
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)(((int64_t)(X - P) >> 2) & 0x3FFF);
      insn = (insn & ~(0x3FFFu << 5)) | (imm << 5);
      w32(b->p + off, insn); return 0;
    }
    case 311: { /* ADR_GOT_PAGE: page of the symbol's GOT entry */
      if (off + 4 > b->len) return -1;
      SymEnt *e = sym_find(name ? name : "");
      if (!e || !e->has_got) { WALL_LOGE("linker", "missing GOT entry for '%s'\n", name ? name : "?"); return -1; }
      uint64_t G = e->got_addr;
      uint32_t insn = r32(b->p + off);
      int64_t delta = (int64_t)((G & ~0xFFFULL) - (P & ~0xFFFULL));
      int64_t imm = delta >> 12;
      insn = (insn & ~((3u << 29) | (0x7FFFFu << 5)));
      insn |= (((uint32_t)imm & 3u) << 29);
      insn |= ((((uint32_t)imm >> 2) & 0x7FFFFu) << 5);
      w32(b->p + off, insn); return 0;
    }
    case 312: { /* LD64_GOT_LO12_NC: low 12 bits of the GOT entry */
      if (off + 4 > b->len) return -1;
      SymEnt *e = sym_find(name ? name : "");
      if (!e || !e->has_got) { WALL_LOGE("linker", "missing GOT entry for '%s'\n", name ? name : "?"); return -1; }
      uint32_t insn = r32(b->p + off);
      uint32_t imm = (uint32_t)((e->got_addr & 0xFFF) >> 3);
      insn = (insn & ~(0xFFFu << 10)) | (imm << 10);
      w32(b->p + off, insn); return 0;
    }
    default:
      WALL_LOGE("linker", "unsupported relocation type %u for '%s'\n", type, name ? name : "?");
      return -1;
  }
}

static int do_relocations(void) {
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      const unsigned char *sh = sh_ptr(o, i);
      if (r32(sh + 4) != SHT_RELA) continue;
      int target = (int)r32(sh + 44);
      if (target <= 0 || target >= o->shnum || o->sec_kind[target] == 0) continue;
      uint64_t roff = r64(sh + 24);
      uint64_t rsz = r64(sh + 32);
      if (roff + rsz > o->size) return -1;
      uint64_t n = rsz / 24;
      for (uint64_t j = 0; j < n; j++) {
        const unsigned char *e = o->buf + roff + j * 24;
        uint64_t r_off = r64(e);
        uint64_t r_info = r64(e + 8);
        int64_t add = (int64_t)r64(e + 16);
        uint32_t symidx = (uint32_t)(r_info >> 32);
        uint32_t type = (uint32_t)(r_info & 0xffffffffu);
        uint64_t S = 0;
        const char *name = "";
        int rc = sym_addr(o, symidx, &S, &name);
        if (rc != 0) {
          if (allow_undef_name(name)) {
            S = 0;
            if (g_warn_unresolved) lwarn("unresolved symbol '%s' in '%s' (explicitly allowed)", name, o->name);
          } else if (g_warn_unresolved) {
            S = 0;
            lwarn("unresolved symbol '%s' in '%s'", name, o->name);
          } else {
            lerror("unresolved symbol '%s' in '%s'", name, o->name);
            return -1;
          }
        }
        if (apply_reloc(o->sec_kind[target], o, target, r_off, type, S, add, name) != 0) {
          WALL_LOGE("linker", "relocation failed for '%s' in '%s'\n", name, o->name);
          return -1;
        }
      }
    }
  }
  return 0;
}

/* ------------------------------------------------------------------------- */
/* Output                                                                     */
/* ------------------------------------------------------------------------- */
static void write_sh(unsigned char *sh, uint32_t name, uint32_t type, uint64_t flags,
                     uint64_t addr, uint64_t off, uint64_t size, uint32_t link,
                     uint32_t info, uint64_t align, uint64_t ent) {
  w32(sh + 0, name);
  w32(sh + 4, type);
  w64(sh + 8, flags);
  w64(sh + 16, addr);
  w64(sh + 24, off);
  w64(sh + 32, size);
  w32(sh + 40, link);
  w32(sh + 44, info);
  w64(sh + 48, align);
  w64(sh + 56, ent);
}

/* ------------------------------------------------------------------------- */
/* SHA-1 for --build-id                                                       */
/* ------------------------------------------------------------------------- */
typedef struct { uint32_t h[5]; uint64_t bytes; unsigned char buf[64]; size_t n; } Sha1;
static uint32_t rol32(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }
static void sha1_block(Sha1 *s, const unsigned char *p) {
  uint32_t w[80];
  for (int i = 0; i < 16; i++)
    w[i] = ((uint32_t)p[i * 4] << 24) | ((uint32_t)p[i * 4 + 1] << 16) |
           ((uint32_t)p[i * 4 + 2] << 8) | (uint32_t)p[i * 4 + 3];
  for (int i = 16; i < 80; i++) w[i] = rol32(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
  uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
  for (int i = 0; i < 80; i++) {
    uint32_t f, k;
    if (i < 20) { f = (b & c) | ((~b) & d); k = 0x5A827999u; }
    else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1u; }
    else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
    else { f = b ^ c ^ d; k = 0xCA62C1D6u; }
    uint32_t t = rol32(a, 5) + f + e + k + w[i];
    e = d; d = c; c = rol32(b, 30); b = a; a = t;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}
static void sha1_init(Sha1 *s) {
  s->h[0] = 0x67452301u; s->h[1] = 0xEFCDAB89u; s->h[2] = 0x98BADCFEu;
  s->h[3] = 0x10325476u; s->h[4] = 0xC3D2E1F0u;
  s->bytes = 0; s->n = 0;
}
static void sha1_update(Sha1 *s, const unsigned char *p, size_t n) {
  s->bytes += n;
  while (n) {
    size_t k = 64 - s->n;
    if (k > n) k = n;
    memcpy(s->buf + s->n, p, k);
    s->n += k; p += k; n -= k;
    if (s->n == 64) { sha1_block(s, s->buf); s->n = 0; }
  }
}
static void sha1_final(Sha1 *s, unsigned char out[20]) {
  uint64_t bits = s->bytes * 8;
  unsigned char one = 0x80, zero = 0, len[8];
  sha1_update(s, &one, 1);
  while (s->n != 56) sha1_update(s, &zero, 1);
  for (int i = 0; i < 8; i++) len[7 - i] = (unsigned char)(bits >> (i * 8));
  sha1_update(s, len, 8);
  for (int i = 0; i < 5; i++) {
    out[i * 4] = (unsigned char)(s->h[i] >> 24);
    out[i * 4 + 1] = (unsigned char)(s->h[i] >> 16);
    out[i * 4 + 2] = (unsigned char)(s->h[i] >> 8);
    out[i * 4 + 3] = (unsigned char)(s->h[i]);
  }
}

/* Reserve the .note.gnu.build-id payload in .rodata before addresses are set. */
static uint64_t g_note_off;
static void prepare_build_id(void) {
  if (!g_build_id) return;
  b_align(&g_ro, 4);
  g_note_off = g_ro.len;
  unsigned char note[36];
  memset(note, 0, sizeof(note));
  w32(note + 0, 4);       /* namesz, including NUL */
  w32(note + 4, 20);      /* descsz: SHA-1 */
  w32(note + 8, 3);       /* NT_GNU_BUILD_ID */
  memcpy(note + 12, "GNU\0", 4);
  b_append(&g_ro, note, sizeof(note));
}

/* -r / --relocatable: merge objects into one ET_REL file, preserving the
 * symbol table and RELA records for a later final link. */
static int write_relocatable(const char *path, const char *comment) {
  Buf otext, oro, odata;
  memset(&otext, 0, sizeof(otext));
  memset(&oro, 0, sizeof(oro));
  memset(&odata, 0, sizeof(odata));
  uint64_t obss = 0;

  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      int kind = o->sec_kind[i];
      if (!kind) continue;
      const unsigned char *sh = sh_ptr(o, i);
      uint64_t off = r64(sh + 24), sz = r64(sh + 32), al = r64(sh + 48);
      if (al < 1) al = 1;
      if (kind == 4) {
        obss = align_up(obss, al);
        o->sec_off[i] = (size_t)obss;
        obss += sz;
      } else {
        Buf *b = kind == 1 ? &otext : kind == 2 ? &oro : &odata;
        b_align(b, (size_t)al);
        o->sec_off[i] = b->len;
        b_append(b, o->buf + off, (size_t)sz);
      }
    }
  }

  size_t *nsym = (size_t *)calloc((size_t)g_nobjs + 1, sizeof(size_t));
  uint32_t **maps = (uint32_t **)calloc((size_t)g_nobjs + 1, sizeof(uint32_t *));
  if (!nsym || !maps) return -1;
  for (int k = 0; k < g_nobjs; k++) nsym[k] = (size_t)(g_objs[k].symsz / 24);

  int total = 1;   /* symbol 0 is the null symbol */
  for (int pass = 0; pass < 2; pass++) {
    for (int k = 0; k < g_nobjs; k++) {
      Obj *o = &g_objs[k];
      if (pass == 0) maps[k] = (uint32_t *)calloc(nsym[k] ? nsym[k] : 1, sizeof(uint32_t));
      for (size_t j = 0; j < nsym[k]; j++) {
        const unsigned char *s = o->buf + o->symoff + j * 24;
        int bind = s[4] >> 4;
        int islocal = (bind == 0);
        if ((pass == 0) != islocal) continue;
        maps[k][j] = (uint32_t)total++;
      }
    }
  }
  int first_global = 1;
  {
    int t = 1;
    for (int k = 0; k < g_nobjs; k++) {
      Obj *o = &g_objs[k];
      for (size_t j = 0; j < nsym[k]; j++) {
        const unsigned char *s = o->buf + o->symoff + j * 24;
        if ((s[4] >> 4) == 0) t++;
      }
    }
    first_global = t;
  }

  Buf str; memset(&str, 0, sizeof(str));
  b_byte(&str, 0);
  unsigned char *symbuf = (unsigned char *)calloc((size_t)total, 24);
  if (!symbuf) return -1;
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (size_t j = 0; j < nsym[k]; j++) {
      uint32_t ni = maps[k][j];
      if (!ni) continue;
      const unsigned char *s = o->buf + o->symoff + j * 24;
      uint32_t nm = r32(s + 0);
      unsigned char info = s[4], other = s[5];
      uint16_t shndx = r16(s + 6);
      uint64_t val = r64(s + 8), sz = r64(s + 16);
      const char *name = (nm < o->strsz) ? (const char *)(o->buf + o->stroff + nm) : "";
      unsigned char *e = symbuf + (size_t)ni * 24;
      w32(e + 0, name[0] ? (uint32_t)b_stradd(&str, name) : 0);
      e[4] = info;
      e[5] = other;
      uint16_t osh = 0;
      uint64_t oval = val;
      if (shndx == SHN_UNDEF) osh = 0;
      else if (shndx == SHN_ABS) osh = SHN_ABS;
      else if (shndx == SHN_COMMON) osh = SHN_COMMON;
      else if (shndx < o->shnum && o->sec_kind[shndx]) {
        osh = (uint16_t)o->sec_kind[shndx];
        oval = (uint64_t)o->sec_off[shndx] + val;
      }
      w16(e + 6, osh);
      w64(e + 8, oval);
      w64(e + 16, sz);
    }
  }
  Buf symtab; memset(&symtab, 0, sizeof(symtab));
  b_append(&symtab, symbuf, (size_t)total * 24);
  free(symbuf);

  Buf rel[5];
  memset(rel, 0, sizeof(rel));
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      const unsigned char *sh = sh_ptr(o, i);
      if (r32(sh + 4) != SHT_RELA) continue;
      int target = (int)r32(sh + 44);
      if (target <= 0 || target >= o->shnum) continue;
      int kind = o->sec_kind[target];
      if (kind < 1 || kind > 3) continue;
      uint64_t roff = r64(sh + 24), rsz = r64(sh + 32);
      if (roff + rsz > o->size) continue;
      uint64_t n = rsz / 24;
      for (uint64_t j = 0; j < n; j++) {
        const unsigned char *e = o->buf + roff + j * 24;
        uint64_t r_off = r64(e);
        uint64_t r_info = r64(e + 8);
        int64_t add = (int64_t)r64(e + 16);
        uint32_t si = (uint32_t)(r_info >> 32);
        uint32_t ty = (uint32_t)(r_info & 0xffffffffu);
        uint32_t nsi = (si < nsym[k]) ? maps[k][si] : 0;
        unsigned char ne[24];
        w64(ne + 0, (uint64_t)o->sec_off[target] + r_off);
        w64(ne + 8, ((uint64_t)nsi << 32) | ty);
        w64(ne + 16, (uint64_t)add);
        b_append(&rel[kind], ne, 24);
      }
    }
  }

  Buf shstr; memset(&shstr, 0, sizeof(shstr));
  b_byte(&shstr, 0);
  int n_text = b_stradd(&shstr, ".text");
  int n_ro = b_stradd(&shstr, ".rodata");
  int n_data = b_stradd(&shstr, ".data");
  int n_bss = b_stradd(&shstr, ".bss");
  int n_rt = b_stradd(&shstr, ".rela.text");
  int n_rr = b_stradd(&shstr, ".rela.rodata");
  int n_rd = b_stradd(&shstr, ".rela.data");
  int n_sym = b_stradd(&shstr, ".symtab");
  int n_str = b_stradd(&shstr, ".strtab");
  int n_comment = b_stradd(&shstr, ".comment");
  int n_shstr = b_stradd(&shstr, ".shstrtab");

  int idx_text = 1, idx_ro = 2, idx_data = 3, idx_bss = 4;
  int next = 5;
  int idx_rt = 0, idx_rr = 0, idx_rd = 0;
  if (rel[1].len) idx_rt = next++;
  if (rel[2].len) idx_rr = next++;
  if (rel[3].len) idx_rd = next++;
  int idx_sym = next++;
  int idx_str = next++;
  int idx_comment = next++;
  int idx_shstr = next++;
  int shnum = next;

  uint64_t off = 64;
  uint64_t text_off = align_up(off, 16); off = text_off + otext.len;
  uint64_t ro_off = align_up(off, 16); off = ro_off + oro.len;
  uint64_t data_off = align_up(off, 16); off = data_off + odata.len;
  uint64_t bss_off = align_up(off, 16);
  uint64_t rt_off = align_up(off, 8); off = rt_off + rel[1].len;
  uint64_t rr_off = align_up(off, 8); off = rr_off + rel[2].len;
  uint64_t rd_off = align_up(off, 8); off = rd_off + rel[3].len;
  uint64_t sym_off = align_up(off, 8); off = sym_off + symtab.len;
  uint64_t str_off = align_up(off, 1); off = str_off + str.len;
  uint64_t comment_off = align_up(off, 1); off = comment_off + strlen(comment) + 1;
  uint64_t shstr_off = align_up(off, 1); off = shstr_off + shstr.len;
  uint64_t shoff = align_up(off, 8);
  uint64_t total_sz = shoff + (uint64_t)shnum * 64;

  unsigned char *img = (unsigned char *)calloc(1, (size_t)total_sz);
  if (!img) return -1;
  img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
  img[4] = 2; img[5] = 1; img[6] = 1;
  w16(img + 16, 1);          /* ET_REL */
  w16(img + 18, 183);        /* EM_AARCH64 */
  w32(img + 20, 1);
  w64(img + 40, shoff);
  w16(img + 52, 64);
  w16(img + 54, 0);
  w16(img + 56, 0);
  w16(img + 58, 64);
  w16(img + 60, (uint16_t)shnum);
  w16(img + 62, (uint16_t)idx_shstr);

  if (otext.len) memcpy(img + text_off, otext.p, otext.len);
  if (oro.len) memcpy(img + ro_off, oro.p, oro.len);
  if (odata.len) memcpy(img + data_off, odata.p, odata.len);
  if (rel[1].len) memcpy(img + rt_off, rel[1].p, rel[1].len);
  if (rel[2].len) memcpy(img + rr_off, rel[2].p, rel[2].len);
  if (rel[3].len) memcpy(img + rd_off, rel[3].p, rel[3].len);
  memcpy(img + sym_off, symtab.p, symtab.len);
  memcpy(img + str_off, str.p, str.len);
  memcpy(img + comment_off, comment, strlen(comment) + 1);
  memcpy(img + shstr_off, shstr.p, shstr.len);

  unsigned char *sh = img + shoff;
  memset(sh, 0, (size_t)shnum * 64);
  write_sh(sh + 64 * idx_text, (uint32_t)n_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
           0, text_off, otext.len, 0, 0, 16, 0);
  write_sh(sh + 64 * idx_ro, (uint32_t)n_ro, SHT_PROGBITS, SHF_ALLOC,
           0, ro_off, oro.len, 0, 0, 16, 0);
  write_sh(sh + 64 * idx_data, (uint32_t)n_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
           0, data_off, odata.len, 0, 0, 16, 0);
  write_sh(sh + 64 * idx_bss, (uint32_t)n_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
           0, bss_off, obss, 0, 0, 16, 0);
  if (idx_rt) write_sh(sh + 64 * idx_rt, (uint32_t)n_rt, SHT_RELA, 0,
                       0, rt_off, rel[1].len, (uint32_t)idx_sym, (uint32_t)idx_text, 8, 24);
  if (idx_rr) write_sh(sh + 64 * idx_rr, (uint32_t)n_rr, SHT_RELA, 0,
                       0, rr_off, rel[2].len, (uint32_t)idx_sym, (uint32_t)idx_ro, 8, 24);
  if (idx_rd) write_sh(sh + 64 * idx_rd, (uint32_t)n_rd, SHT_RELA, 0,
                       0, rd_off, rel[3].len, (uint32_t)idx_sym, (uint32_t)idx_data, 8, 24);
  write_sh(sh + 64 * idx_sym, (uint32_t)n_sym, SHT_SYMTAB, 0,
           0, sym_off, symtab.len, (uint32_t)idx_str, (uint32_t)first_global, 8, 24);
  write_sh(sh + 64 * idx_str, (uint32_t)n_str, SHT_STRTAB, 0,
           0, str_off, str.len, 0, 0, 1, 0);
  write_sh(sh + 64 * idx_comment, (uint32_t)n_comment, SHT_PROGBITS, 0,
           0, comment_off, strlen(comment) + 1, 0, 0, 1, 0);
  write_sh(sh + 64 * idx_shstr, (uint32_t)n_shstr, SHT_STRTAB, 0,
           0, shstr_off, shstr.len, 0, 0, 1, 0);

  FILE *f = fopen(path, "wb");
  if (!f) { free(img); return -1; }
  size_t wr = fwrite(img, 1, (size_t)total_sz, f);
  fclose(f);
  free(img);
  free(otext.p); free(oro.p); free(odata.p);
  free(rel[1].p); free(rel[2].p); free(rel[3].p);
  free(symtab.p); free(str.p); free(shstr.p);
  for (int k = 0; k < g_nobjs; k++) free(maps[k]);
  free(maps); free(nsym);
  return wr == (size_t)total_sz ? 0 : -1;
}

static int write_output(const char *path, const char *comment, uint64_t entry, int shared) {
  Buf shstr; memset(&shstr, 0, sizeof(shstr));
  b_byte(&shstr, 0);
  int n_text = b_stradd(&shstr, ".text");
  int n_ro = b_stradd(&shstr, ".rodata");
  int n_data = b_stradd(&shstr, ".data");
  int n_bss = b_stradd(&shstr, ".bss");
  int n_comment = b_stradd(&shstr, ".comment");
  int n_note = b_stradd(&shstr, ".note.gnu.build-id");
  int n_got = b_stradd(&shstr, ".got");
  int n_symtab = b_stradd(&shstr, ".symtab");
  int n_strtab = b_stradd(&shstr, ".strtab");
  int n_shstr = b_stradd(&shstr, ".shstrtab");

  /* Symbols are optional output: a small, runnable binary needs none. */
  int keep_syms = ((!g_strip || g_nretain > 0) && g_nsym > 0);
  Buf str; memset(&str, 0, sizeof(str));
  b_byte(&str, 0);
  Buf symtab; memset(&symtab, 0, sizeof(symtab));
  if (keep_syms) {
    static const unsigned char zero_sym[24] = {0};
    b_append(&symtab, zero_sym, sizeof(zero_sym));
    for (int i = 0; i < g_nsym; i++) {
      if (!retain_has(g_syms[i].name)) continue;
      int no = b_stradd(&str, g_syms[i].name);
      unsigned char e[24];
      memset(e, 0, sizeof(e));
      w32(e + 0, (uint32_t)no);
      e[4] = (unsigned char)((g_syms[i].bind << 4) | 0); /* STT_NOTYPE */
      w16(e + 6, SHN_ABS);
      w64(e + 8, g_syms[i].addr);
      w64(e + 16, 0);
      b_append(&symtab, e, 24);
    }
  }

  uint64_t base = shared ? 0 : 0x400000;
  uint64_t text_off = text_file_off();
  uint64_t ro_off = align_up(text_off + g_text.len, 16);
  int rw = (g_data.len || g_got.len || g_bss) ? 1 : 0;
  uint64_t data_off = rw ? align_up(ro_off + g_ro.len, 0x1000) : 0;
  uint64_t got_off = align_up(data_off + g_data.len, 8);
  uint64_t rw_file_end = got_off + g_got.len;
  uint64_t bss_off = align_up(rw_file_end, 16);
  uint64_t file_end = rw ? rw_file_end : (ro_off + g_ro.len);
  uint64_t comment_off = align_up(file_end, 8);
  size_t comment_size = strlen(comment) + 1;
  uint64_t symoff = align_up(comment_off + comment_size, 8);
  uint64_t stroff = symoff + (keep_syms ? symtab.len : 0);
  uint64_t shstroff = align_up(stroff + (keep_syms ? str.len : 0), 8);
  uint64_t shoff = align_up(shstroff + shstr.len, 8);

  int n_present = (g_text.len > 0) + (g_ro.len > 0) + (g_data.len > 0) +
                  (g_got.len > 0) + (g_bss > 0) + 1 /* .comment */ +
                  (g_build_id ? 1 : 0);
  int shstr_idx = 1 + n_present + (keep_syms ? 2 : 0);
  int shnum = shstr_idx + 1;
  int phnum = (rw ? 3 : 2) + (g_build_id ? 1 : 0);
  uint64_t total = shoff + (uint64_t)shnum * 64;
  unsigned char *img = (unsigned char *)calloc(1, (size_t)total);
  if (!img) { WALL_LOGE("linker", "out of memory\n"); return -1; }

  /* ELF header */
  img[0] = 0x7f; img[1] = 'E'; img[2] = 'L'; img[3] = 'F';
  img[4] = 2; img[5] = 1; img[6] = 1; img[7] = 0;
  w16(img + 16, shared ? 3 : 2); /* ET_DYN / ET_EXEC */
  w16(img + 18, 183);            /* EM_AARCH64 */
  w32(img + 20, 1);
  w64(img + 24, entry);
  w64(img + 32, 64);             /* phoff */
  w64(img + 40, shoff);
  w32(img + 48, 0);
  w16(img + 52, 64);
  w16(img + 54, 56);
  w16(img + 56, (uint16_t)phnum);
  w16(img + 58, 64);
  w16(img + 60, (uint16_t)shnum);
  w16(img + 62, (uint16_t)shstr_idx);

  /* Program headers: RX, optional RW, GNU_STACK. */
  unsigned char *ph = img + 64;
  memset(ph, 0, (size_t)phnum * 56);
  w32(ph + 0, 1); w32(ph + 4, 5); /* PT_LOAD R|X, starts at file offset 0 */
  w64(ph + 8, 0);
  w64(ph + 16, base);
  w64(ph + 24, base);
  w64(ph + 32, ro_off + g_ro.len);
  w64(ph + 40, ro_off + g_ro.len);
  w64(ph + 48, 0x1000);

  unsigned char *ph2 = ph + 56;
  if (rw) {
    w32(ph2 + 0, 1); w32(ph2 + 4, 6); /* PT_LOAD R|W */
    w64(ph2 + 8, data_off);
    w64(ph2 + 16, base + data_off);
    w64(ph2 + 24, base + data_off);
    w64(ph2 + 32, rw_file_end - data_off);
    w64(ph2 + 40, (rw_file_end - data_off) + g_bss);
    w64(ph2 + 48, 0x1000);
    ph2 += 56;
  }
  w32(ph2 + 0, 0x6474e551); w32(ph2 + 4, g_execstack ? 7u : 6u); /* PT_GNU_STACK */
  w64(ph2 + 48, 16);
  if (g_build_id) {
    unsigned char *pn = ph2 + 56;
    w32(pn + 0, PT_NOTE); w32(pn + 4, 4);   /* PT_NOTE, R */
    w64(pn + 8, ro_off + g_note_off);
    w64(pn + 16, base + ro_off + g_note_off);
    w64(pn + 24, base + ro_off + g_note_off);
    w64(pn + 32, 36);
    w64(pn + 40, 36);
    w64(pn + 48, 4);
  }

  if (g_text.len) memcpy(img + text_off, g_text.p, g_text.len);
  if (g_ro.len) memcpy(img + ro_off, g_ro.p, g_ro.len);
  if (g_data.len) memcpy(img + data_off, g_data.p, g_data.len);
  if (g_got.len) memcpy(img + got_off, g_got.p, g_got.len);
  memcpy(img + comment_off, comment, comment_size);
  if (keep_syms && symtab.len) memcpy(img + symoff, symtab.p, symtab.len);
  if (keep_syms && str.len) memcpy(img + stroff, str.p, str.len);
  memcpy(img + shstroff, shstr.p, shstr.len);

  /* Section headers, allocated only for sections that exist. */
  unsigned char *sh = img + shoff;
  memset(sh, 0, (size_t)shnum * 64);
  int si = 1;
  if (g_text.len) {
    write_sh(sh + 64 * si, (uint32_t)n_text, SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
             base + text_off, text_off, g_text.len, 0, 0, 16, 0);
    si++;
  }
  if (g_ro.len) {
    write_sh(sh + 64 * si, (uint32_t)n_ro, SHT_PROGBITS, SHF_ALLOC,
             base + ro_off, ro_off, g_ro.len, 0, 0, 16, 0);
    si++;
  }
  if (g_data.len) {
    write_sh(sh + 64 * si, (uint32_t)n_data, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
             base + data_off, data_off, g_data.len, 0, 0, 16, 0);
    si++;
  }
  if (g_got.len) {
    write_sh(sh + 64 * si, (uint32_t)n_got, SHT_PROGBITS, SHF_ALLOC | SHF_WRITE,
             base + got_off, got_off, g_got.len, 0, 0, 8, 0);
    si++;
  }
  if (g_bss) {
    write_sh(sh + 64 * si, (uint32_t)n_bss, SHT_NOBITS, SHF_ALLOC | SHF_WRITE,
             base + bss_off, bss_off, g_bss, 0, 0, 16, 0);
    si++;
  }
  write_sh(sh + 64 * si, (uint32_t)n_comment, SHT_PROGBITS, 0,
           0, comment_off, comment_size, 0, 0, 1, 0);
  si++;
  if (g_build_id) {
    write_sh(sh + 64 * si, (uint32_t)n_note, SHT_NOTE, SHF_ALLOC,
             base + ro_off + g_note_off, ro_off + g_note_off, 36, 0, 0, 4, 0);
    si++;
  }
  if (keep_syms) {
    write_sh(sh + 64 * si, (uint32_t)n_symtab, SHT_SYMTAB, 0,
             0, symoff, symtab.len, (uint32_t)(si + 1), 1, 8, 24);
    si++;
    write_sh(sh + 64 * si, (uint32_t)n_strtab, SHT_STRTAB, 0,
             0, stroff, str.len, 0, 0, 1, 0);
    si++;
  }
  int shstr_actual = si;
  write_sh(sh + 64 * si, (uint32_t)n_shstr, SHT_STRTAB, 0,
           0, shstroff, shstr.len, 0, 0, 1, 0);
  si++;
  w16(img + 60, (uint16_t)si);
  w16(img + 62, (uint16_t)shstr_actual);

  /* The build-id is SHA-1 over the finished image with the note descriptor
   * still zeroed.  It is patched in after the section headers are final. */
  if (g_build_id) {
    unsigned char digest[20];
    Sha1 sha;
    sha1_init(&sha);
    sha1_update(&sha, img, (size_t)total);
    sha1_final(&sha, digest);
    memcpy(img + ro_off + g_note_off + 16, digest, sizeof(digest));
  }

  FILE *f = fopen(path, "wb");
  if (!f) { free(img); WALL_LOGE("linker", "cannot write '%s'\n", path); return -1; }
  size_t wr = fwrite(img, 1, (size_t)total, f);
  fclose(f);
  free(img);
  free(shstr.p); free(str.p); free(symtab.p);
  if (wr != (size_t)total) { WALL_LOGE("linker", "short write to '%s'\n", path); return -1; }
  if (!shared) chmod(path, 0755);
  if ((g_map_path && g_map_path[0]) || g_map_stdout) {
    FILE *mf = g_map_stdout ? stdout : fopen(g_map_path, "w");
    if (!mf) { lwarn("cannot write map '%s'", g_map_path); }
    else {
      fprintf(mf, "wall linker map for %s\n", path);
      fprintf(mf, "text   0x%llx .. 0x%llx\n",
              (unsigned long long)g_text_addr,
              (unsigned long long)(g_text_addr + g_text.len));
      fprintf(mf, "rodata 0x%llx .. 0x%llx\n",
              (unsigned long long)g_ro_addr,
              (unsigned long long)(g_ro_addr + g_ro.len));
      fprintf(mf, "data   0x%llx .. 0x%llx\n",
              (unsigned long long)g_data_addr,
              (unsigned long long)(g_data_addr + g_data.len));
      fprintf(mf, "got    0x%llx .. 0x%llx\n",
              (unsigned long long)g_got_addr,
              (unsigned long long)(g_got_addr + g_got.len));
      fprintf(mf, "bss    0x%llx .. 0x%llx\n",
              (unsigned long long)g_bss_addr,
              (unsigned long long)(g_bss_addr + g_bss));
      fprintf(mf, "\nsymbols:\n");
      for (int i = 0; i < g_nsym; i++)
        fprintf(mf, "  0x%016llx  %s\n", (unsigned long long)g_syms[i].addr, g_syms[i].name);
      if (!g_map_stdout) fclose(mf);
    }
  }
  if (g_print_memory_usage) {
    fprintf(stdout, "Memory region         Used Size  Region Size  %%age Used\n");
    fprintf(stdout, "text                  %8zu B       n/a\n", g_text.len);
    fprintf(stdout, "rodata                %8zu B       n/a\n", g_ro.len);
    fprintf(stdout, "data                  %8zu B       n/a\n", g_data.len);
    fprintf(stdout, "bss                   %8llu B       n/a\n", (unsigned long long)g_bss);
    fprintf(stdout, "total                 %8llu B\n",
            (unsigned long long)(g_text.len + g_ro.len + g_data.len + g_got.len + g_bss));
  }
  return 0;
}
/* ------------------------------------------------------------------------- */
/* ELF relocation reference table (generated by scripts/gen_linker_tables.sh).  */
/* Used by the --list-relocs diagnostic; the linker itself implements the     */
/* AArch64 subset.                                                           */
/* ------------------------------------------------------------------------- */
typedef struct { const char *arch; unsigned value; const char *name; const char *doc; } WallRelocRef;
static const WallRelocRef g_reloc_ref[] = {
  {"aarch64", 0, "R_AARCH64_NONE", "NONE relocation (aarch64)."},
  {"aarch64", 0x101, "R_AARCH64_ABS64", "ABS64 relocation (aarch64)."},
  {"aarch64", 0x102, "R_AARCH64_ABS32", "ABS32 relocation (aarch64)."},
  {"aarch64", 0x103, "R_AARCH64_ABS16", "ABS16 relocation (aarch64)."},
  {"aarch64", 0x104, "R_AARCH64_PREL64", "PREL64 relocation (aarch64)."},
  {"aarch64", 0x105, "R_AARCH64_PREL32", "PREL32 relocation (aarch64)."},
  {"aarch64", 0x106, "R_AARCH64_PREL16", "PREL16 relocation (aarch64)."},
  {"aarch64", 0x107, "R_AARCH64_MOVW_UABS_G0", "MOVW_UABS_G0 relocation (aarch64)."},
  {"aarch64", 0x108, "R_AARCH64_MOVW_UABS_G0_NC", "MOVW_UABS_G0_NC relocation (aarch64)."},
  {"aarch64", 0x109, "R_AARCH64_MOVW_UABS_G1", "MOVW_UABS_G1 relocation (aarch64)."},
  {"aarch64", 0x10a, "R_AARCH64_MOVW_UABS_G1_NC", "MOVW_UABS_G1_NC relocation (aarch64)."},
  {"aarch64", 0x10b, "R_AARCH64_MOVW_UABS_G2", "MOVW_UABS_G2 relocation (aarch64)."},
  {"aarch64", 0x10c, "R_AARCH64_MOVW_UABS_G2_NC", "MOVW_UABS_G2_NC relocation (aarch64)."},
  {"aarch64", 0x10d, "R_AARCH64_MOVW_UABS_G3", "MOVW_UABS_G3 relocation (aarch64)."},
  {"aarch64", 0x10e, "R_AARCH64_MOVW_SABS_G0", "MOVW_SABS_G0 relocation (aarch64)."},
  {"aarch64", 0x10f, "R_AARCH64_MOVW_SABS_G1", "MOVW_SABS_G1 relocation (aarch64)."},
  {"aarch64", 0x110, "R_AARCH64_MOVW_SABS_G2", "MOVW_SABS_G2 relocation (aarch64)."},
  {"aarch64", 0x111, "R_AARCH64_LD_PREL_LO19", "LD_PREL_LO19 relocation (aarch64)."},
  {"aarch64", 0x112, "R_AARCH64_ADR_PREL_LO21", "ADR_PREL_LO21 relocation (aarch64)."},
  {"aarch64", 0x113, "R_AARCH64_ADR_PREL_PG_HI21", "ADR_PREL_PG_HI21 relocation (aarch64)."},
  {"aarch64", 0x114, "R_AARCH64_ADR_PREL_PG_HI21_NC", "ADR_PREL_PG_HI21_NC relocation (aarch64)."},
  {"aarch64", 0x115, "R_AARCH64_ADD_ABS_LO12_NC", "ADD_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x116, "R_AARCH64_LDST8_ABS_LO12_NC", "LDST8_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x117, "R_AARCH64_TSTBR14", "TSTBR14 relocation (aarch64)."},
  {"aarch64", 0x118, "R_AARCH64_CONDBR19", "CONDBR19 relocation (aarch64)."},
  {"aarch64", 0x11a, "R_AARCH64_JUMP26", "JUMP26 relocation (aarch64)."},
  {"aarch64", 0x11b, "R_AARCH64_CALL26", "CALL26 relocation (aarch64)."},
  {"aarch64", 0x11c, "R_AARCH64_LDST16_ABS_LO12_NC", "LDST16_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x11d, "R_AARCH64_LDST32_ABS_LO12_NC", "LDST32_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x11e, "R_AARCH64_LDST64_ABS_LO12_NC", "LDST64_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x11f, "R_AARCH64_MOVW_PREL_G0", "MOVW_PREL_G0 relocation (aarch64)."},
  {"aarch64", 0x120, "R_AARCH64_MOVW_PREL_G0_NC", "MOVW_PREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x121, "R_AARCH64_MOVW_PREL_G1", "MOVW_PREL_G1 relocation (aarch64)."},
  {"aarch64", 0x122, "R_AARCH64_MOVW_PREL_G1_NC", "MOVW_PREL_G1_NC relocation (aarch64)."},
  {"aarch64", 0x123, "R_AARCH64_MOVW_PREL_G2", "MOVW_PREL_G2 relocation (aarch64)."},
  {"aarch64", 0x124, "R_AARCH64_MOVW_PREL_G2_NC", "MOVW_PREL_G2_NC relocation (aarch64)."},
  {"aarch64", 0x125, "R_AARCH64_MOVW_PREL_G3", "MOVW_PREL_G3 relocation (aarch64)."},
  {"aarch64", 0x12b, "R_AARCH64_LDST128_ABS_LO12_NC", "LDST128_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x12c, "R_AARCH64_MOVW_GOTOFF_G0", "MOVW_GOTOFF_G0 relocation (aarch64)."},
  {"aarch64", 0x12d, "R_AARCH64_MOVW_GOTOFF_G0_NC", "MOVW_GOTOFF_G0_NC relocation (aarch64)."},
  {"aarch64", 0x12e, "R_AARCH64_MOVW_GOTOFF_G1", "MOVW_GOTOFF_G1 relocation (aarch64)."},
  {"aarch64", 0x12f, "R_AARCH64_MOVW_GOTOFF_G1_NC", "MOVW_GOTOFF_G1_NC relocation (aarch64)."},
  {"aarch64", 0x130, "R_AARCH64_MOVW_GOTOFF_G2", "MOVW_GOTOFF_G2 relocation (aarch64)."},
  {"aarch64", 0x131, "R_AARCH64_MOVW_GOTOFF_G2_NC", "MOVW_GOTOFF_G2_NC relocation (aarch64)."},
  {"aarch64", 0x132, "R_AARCH64_MOVW_GOTOFF_G3", "MOVW_GOTOFF_G3 relocation (aarch64)."},
  {"aarch64", 0x133, "R_AARCH64_GOTREL64", "GOTREL64 relocation (aarch64)."},
  {"aarch64", 0x134, "R_AARCH64_GOTREL32", "GOTREL32 relocation (aarch64)."},
  {"aarch64", 0x135, "R_AARCH64_GOT_LD_PREL19", "GOT_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x136, "R_AARCH64_LD64_GOTOFF_LO15", "LD64_GOTOFF_LO15 relocation (aarch64)."},
  {"aarch64", 0x137, "R_AARCH64_ADR_GOT_PAGE", "ADR_GOT_PAGE relocation (aarch64)."},
  {"aarch64", 0x138, "R_AARCH64_LD64_GOT_LO12_NC", "LD64_GOT_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x139, "R_AARCH64_LD64_GOTPAGE_LO15", "LD64_GOTPAGE_LO15 relocation (aarch64)."},
  {"aarch64", 0x13a, "R_AARCH64_PLT32", "PLT32 relocation (aarch64)."},
  {"aarch64", 0x13b, "R_AARCH64_GOTPCREL32", "GOTPCREL32 relocation (aarch64)."},
  {"aarch64", 0x200, "R_AARCH64_TLSGD_ADR_PREL21", "TLSGD_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x201, "R_AARCH64_TLSGD_ADR_PAGE21", "TLSGD_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x202, "R_AARCH64_TLSGD_ADD_LO12_NC", "TLSGD_ADD_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x203, "R_AARCH64_TLSGD_MOVW_G1", "TLSGD_MOVW_G1 relocation (aarch64)."},
  {"aarch64", 0x204, "R_AARCH64_TLSGD_MOVW_G0_NC", "TLSGD_MOVW_G0_NC relocation (aarch64)."},
  {"aarch64", 0x205, "R_AARCH64_TLSLD_ADR_PREL21", "TLSLD_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x206, "R_AARCH64_TLSLD_ADR_PAGE21", "TLSLD_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x207, "R_AARCH64_TLSLD_ADD_LO12_NC", "TLSLD_ADD_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x208, "R_AARCH64_TLSLD_MOVW_G1", "TLSLD_MOVW_G1 relocation (aarch64)."},
  {"aarch64", 0x209, "R_AARCH64_TLSLD_MOVW_G0_NC", "TLSLD_MOVW_G0_NC relocation (aarch64)."},
  {"aarch64", 0x20a, "R_AARCH64_TLSLD_LD_PREL19", "TLSLD_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x20b, "R_AARCH64_TLSLD_MOVW_DTPREL_G2", "TLSLD_MOVW_DTPREL_G2 relocation (aarch64)."},
  {"aarch64", 0x20c, "R_AARCH64_TLSLD_MOVW_DTPREL_G1", "TLSLD_MOVW_DTPREL_G1 relocation (aarch64)."},
  {"aarch64", 0x20d, "R_AARCH64_TLSLD_MOVW_DTPREL_G1_NC", "TLSLD_MOVW_DTPREL_G1_NC relocation (aarch64)."},
  {"aarch64", 0x20e, "R_AARCH64_TLSLD_MOVW_DTPREL_G0", "TLSLD_MOVW_DTPREL_G0 relocation (aarch64)."},
  {"aarch64", 0x20f, "R_AARCH64_TLSLD_MOVW_DTPREL_G0_NC", "TLSLD_MOVW_DTPREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x210, "R_AARCH64_TLSLD_ADD_DTPREL_HI12", "TLSLD_ADD_DTPREL_HI12 relocation (aarch64)."},
  {"aarch64", 0x211, "R_AARCH64_TLSLD_ADD_DTPREL_LO12", "TLSLD_ADD_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x212, "R_AARCH64_TLSLD_ADD_DTPREL_LO12_NC", "TLSLD_ADD_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x213, "R_AARCH64_TLSLD_LDST8_DTPREL_LO12", "TLSLD_LDST8_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x214, "R_AARCH64_TLSLD_LDST8_DTPREL_LO12_NC", "TLSLD_LDST8_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x215, "R_AARCH64_TLSLD_LDST16_DTPREL_LO12", "TLSLD_LDST16_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x216, "R_AARCH64_TLSLD_LDST16_DTPREL_LO12_NC", "TLSLD_LDST16_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x217, "R_AARCH64_TLSLD_LDST32_DTPREL_LO12", "TLSLD_LDST32_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x218, "R_AARCH64_TLSLD_LDST32_DTPREL_LO12_NC", "TLSLD_LDST32_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x219, "R_AARCH64_TLSLD_LDST64_DTPREL_LO12", "TLSLD_LDST64_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x21a, "R_AARCH64_TLSLD_LDST64_DTPREL_LO12_NC", "TLSLD_LDST64_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x21b, "R_AARCH64_TLSIE_MOVW_GOTTPREL_G1", "TLSIE_MOVW_GOTTPREL_G1 relocation (aarch64)."},
  {"aarch64", 0x21c, "R_AARCH64_TLSIE_MOVW_GOTTPREL_G0_NC", "TLSIE_MOVW_GOTTPREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x21d, "R_AARCH64_TLSIE_ADR_GOTTPREL_PAGE21", "TLSIE_ADR_GOTTPREL_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x21e, "R_AARCH64_TLSIE_LD64_GOTTPREL_LO12_NC", "TLSIE_LD64_GOTTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x21f, "R_AARCH64_TLSIE_LD_GOTTPREL_PREL19", "TLSIE_LD_GOTTPREL_PREL19 relocation (aarch64)."},
  {"aarch64", 0x220, "R_AARCH64_TLSLE_MOVW_TPREL_G2", "TLSLE_MOVW_TPREL_G2 relocation (aarch64)."},
  {"aarch64", 0x221, "R_AARCH64_TLSLE_MOVW_TPREL_G1", "TLSLE_MOVW_TPREL_G1 relocation (aarch64)."},
  {"aarch64", 0x222, "R_AARCH64_TLSLE_MOVW_TPREL_G1_NC", "TLSLE_MOVW_TPREL_G1_NC relocation (aarch64)."},
  {"aarch64", 0x223, "R_AARCH64_TLSLE_MOVW_TPREL_G0", "TLSLE_MOVW_TPREL_G0 relocation (aarch64)."},
  {"aarch64", 0x224, "R_AARCH64_TLSLE_MOVW_TPREL_G0_NC", "TLSLE_MOVW_TPREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x225, "R_AARCH64_TLSLE_ADD_TPREL_HI12", "TLSLE_ADD_TPREL_HI12 relocation (aarch64)."},
  {"aarch64", 0x226, "R_AARCH64_TLSLE_ADD_TPREL_LO12", "TLSLE_ADD_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x227, "R_AARCH64_TLSLE_ADD_TPREL_LO12_NC", "TLSLE_ADD_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x228, "R_AARCH64_TLSLE_LDST8_TPREL_LO12", "TLSLE_LDST8_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x229, "R_AARCH64_TLSLE_LDST8_TPREL_LO12_NC", "TLSLE_LDST8_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x22a, "R_AARCH64_TLSLE_LDST16_TPREL_LO12", "TLSLE_LDST16_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x22b, "R_AARCH64_TLSLE_LDST16_TPREL_LO12_NC", "TLSLE_LDST16_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x22c, "R_AARCH64_TLSLE_LDST32_TPREL_LO12", "TLSLE_LDST32_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x22d, "R_AARCH64_TLSLE_LDST32_TPREL_LO12_NC", "TLSLE_LDST32_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x22e, "R_AARCH64_TLSLE_LDST64_TPREL_LO12", "TLSLE_LDST64_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x22f, "R_AARCH64_TLSLE_LDST64_TPREL_LO12_NC", "TLSLE_LDST64_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x230, "R_AARCH64_TLSDESC_LD_PREL19", "TLSDESC_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x231, "R_AARCH64_TLSDESC_ADR_PREL21", "TLSDESC_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x232, "R_AARCH64_TLSDESC_ADR_PAGE21", "TLSDESC_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x233, "R_AARCH64_TLSDESC_LD64_LO12", "TLSDESC_LD64_LO12 relocation (aarch64)."},
  {"aarch64", 0x234, "R_AARCH64_TLSDESC_ADD_LO12", "TLSDESC_ADD_LO12 relocation (aarch64)."},
  {"aarch64", 0x235, "R_AARCH64_TLSDESC_OFF_G1", "TLSDESC_OFF_G1 relocation (aarch64)."},
  {"aarch64", 0x236, "R_AARCH64_TLSDESC_OFF_G0_NC", "TLSDESC_OFF_G0_NC relocation (aarch64)."},
  {"aarch64", 0x237, "R_AARCH64_TLSDESC_LDR", "TLSDESC_LDR relocation (aarch64)."},
  {"aarch64", 0x238, "R_AARCH64_TLSDESC_ADD", "TLSDESC_ADD relocation (aarch64)."},
  {"aarch64", 0x239, "R_AARCH64_TLSDESC_CALL", "TLSDESC_CALL relocation (aarch64)."},
  {"aarch64", 0x23a, "R_AARCH64_TLSLE_LDST128_TPREL_LO12", "TLSLE_LDST128_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x23b, "R_AARCH64_TLSLE_LDST128_TPREL_LO12_NC", "TLSLE_LDST128_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x23c, "R_AARCH64_TLSLD_LDST128_DTPREL_LO12", "TLSLD_LDST128_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x23d, "R_AARCH64_TLSLD_LDST128_DTPREL_LO12_NC", "TLSLD_LDST128_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x400, "R_AARCH64_COPY", "COPY relocation (aarch64)."},
  {"aarch64", 0x401, "R_AARCH64_GLOB_DAT", "GLOB_DAT relocation (aarch64)."},
  {"aarch64", 0x402, "R_AARCH64_JUMP_SLOT", "JUMP_SLOT relocation (aarch64)."},
  {"aarch64", 0x403, "R_AARCH64_RELATIVE", "RELATIVE relocation (aarch64)."},
  {"aarch64", 0x404, "R_AARCH64_TLS_DTPMOD64", "TLS_DTPMOD64 relocation (aarch64)."},
  {"aarch64", 0x405, "R_AARCH64_TLS_DTPREL64", "TLS_DTPREL64 relocation (aarch64)."},
  {"aarch64", 0x406, "R_AARCH64_TLS_TPREL64", "TLS_TPREL64 relocation (aarch64)."},
  {"aarch64", 0x407, "R_AARCH64_TLSDESC", "TLSDESC relocation (aarch64)."},
  {"aarch64", 0x408, "R_AARCH64_IRELATIVE", "IRELATIVE relocation (aarch64)."},
  {"aarch64", 0x244, "R_AARCH64_AUTH_ABS64", "AUTH_ABS64 relocation (aarch64)."},
  {"aarch64", 0x245, "R_AARCH64_AUTH_MOVW_GOTOFF_G0", "AUTH_MOVW_GOTOFF_G0 relocation (aarch64)."},
  {"aarch64", 0x246, "R_AARCH64_AUTH_MOVW_GOTOFF_G0_NC", "AUTH_MOVW_GOTOFF_G0_NC relocation (aarch64)."},
  {"aarch64", 0x247, "R_AARCH64_AUTH_MOVW_GOTOFF_G1", "AUTH_MOVW_GOTOFF_G1 relocation (aarch64)."},
  {"aarch64", 0x248, "R_AARCH64_AUTH_MOVW_GOTOFF_G1_NC", "AUTH_MOVW_GOTOFF_G1_NC relocation (aarch64)."},
  {"aarch64", 0x249, "R_AARCH64_AUTH_MOVW_GOTOFF_G2", "AUTH_MOVW_GOTOFF_G2 relocation (aarch64)."},
  {"aarch64", 0x24a, "R_AARCH64_AUTH_MOVW_GOTOFF_G2_NC", "AUTH_MOVW_GOTOFF_G2_NC relocation (aarch64)."},
  {"aarch64", 0x24b, "R_AARCH64_AUTH_MOVW_GOTOFF_G3", "AUTH_MOVW_GOTOFF_G3 relocation (aarch64)."},
  {"aarch64", 0x24c, "R_AARCH64_AUTH_GOT_LD_PREL19", "AUTH_GOT_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x24d, "R_AARCH64_AUTH_LD64_GOTOFF_LO15", "AUTH_LD64_GOTOFF_LO15 relocation (aarch64)."},
  {"aarch64", 0x24e, "R_AARCH64_AUTH_ADR_GOT_PAGE", "AUTH_ADR_GOT_PAGE relocation (aarch64)."},
  {"aarch64", 0x24f, "R_AARCH64_AUTH_LD64_GOT_LO12_NC", "AUTH_LD64_GOT_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x250, "R_AARCH64_AUTH_LD64_GOTPAGE_LO15", "AUTH_LD64_GOTPAGE_LO15 relocation (aarch64)."},
  {"aarch64", 0x251, "R_AARCH64_AUTH_GOT_ADD_LO12_NC", "AUTH_GOT_ADD_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x252, "R_AARCH64_AUTH_GOT_ADR_PREL_LO21", "AUTH_GOT_ADR_PREL_LO21 relocation (aarch64)."},
  {"aarch64", 0x253, "R_AARCH64_AUTH_TLSDESC_ADR_PAGE21", "AUTH_TLSDESC_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x254, "R_AARCH64_AUTH_TLSDESC_LD64_LO12", "AUTH_TLSDESC_LD64_LO12 relocation (aarch64)."},
  {"aarch64", 0x255, "R_AARCH64_AUTH_TLSDESC_ADD_LO12", "AUTH_TLSDESC_ADD_LO12 relocation (aarch64)."},
  {"aarch64", 0x411, "R_AARCH64_AUTH_RELATIVE", "AUTH_RELATIVE relocation (aarch64)."},
  {"aarch64", 0x412, "R_AARCH64_AUTH_GLOB_DAT", "AUTH_GLOB_DAT relocation (aarch64)."},
  {"aarch64", 0x413, "R_AARCH64_AUTH_TLSDESC", "AUTH_TLSDESC relocation (aarch64)."},
  {"aarch64", 0x414, "R_AARCH64_AUTH_IRELATIVE", "AUTH_IRELATIVE relocation (aarch64)."},
  {"aarch64", 0x001, "R_AARCH64_P32_ABS32", "P32_ABS32 relocation (aarch64)."},
  {"aarch64", 0x002, "R_AARCH64_P32_ABS16", "P32_ABS16 relocation (aarch64)."},
  {"aarch64", 0x003, "R_AARCH64_P32_PREL32", "P32_PREL32 relocation (aarch64)."},
  {"aarch64", 0x004, "R_AARCH64_P32_PREL16", "P32_PREL16 relocation (aarch64)."},
  {"aarch64", 0x005, "R_AARCH64_P32_MOVW_UABS_G0", "P32_MOVW_UABS_G0 relocation (aarch64)."},
  {"aarch64", 0x006, "R_AARCH64_P32_MOVW_UABS_G0_NC", "P32_MOVW_UABS_G0_NC relocation (aarch64)."},
  {"aarch64", 0x007, "R_AARCH64_P32_MOVW_UABS_G1", "P32_MOVW_UABS_G1 relocation (aarch64)."},
  {"aarch64", 0x008, "R_AARCH64_P32_MOVW_SABS_G0", "P32_MOVW_SABS_G0 relocation (aarch64)."},
  {"aarch64", 0x009, "R_AARCH64_P32_LD_PREL_LO19", "P32_LD_PREL_LO19 relocation (aarch64)."},
  {"aarch64", 0x00a, "R_AARCH64_P32_ADR_PREL_LO21", "P32_ADR_PREL_LO21 relocation (aarch64)."},
  {"aarch64", 0x00b, "R_AARCH64_P32_ADR_PREL_PG_HI21", "P32_ADR_PREL_PG_HI21 relocation (aarch64)."},
  {"aarch64", 0x00c, "R_AARCH64_P32_ADD_ABS_LO12_NC", "P32_ADD_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x00d, "R_AARCH64_P32_LDST8_ABS_LO12_NC", "P32_LDST8_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x00e, "R_AARCH64_P32_LDST16_ABS_LO12_NC", "P32_LDST16_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x00f, "R_AARCH64_P32_LDST32_ABS_LO12_NC", "P32_LDST32_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x010, "R_AARCH64_P32_LDST64_ABS_LO12_NC", "P32_LDST64_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x011, "R_AARCH64_P32_LDST128_ABS_LO12_NC", "P32_LDST128_ABS_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x012, "R_AARCH64_P32_TSTBR14", "P32_TSTBR14 relocation (aarch64)."},
  {"aarch64", 0x013, "R_AARCH64_P32_CONDBR19", "P32_CONDBR19 relocation (aarch64)."},
  {"aarch64", 0x014, "R_AARCH64_P32_JUMP26", "P32_JUMP26 relocation (aarch64)."},
  {"aarch64", 0x015, "R_AARCH64_P32_CALL26", "P32_CALL26 relocation (aarch64)."},
  {"aarch64", 0x016, "R_AARCH64_P32_MOVW_PREL_G0", "P32_MOVW_PREL_G0 relocation (aarch64)."},
  {"aarch64", 0x017, "R_AARCH64_P32_MOVW_PREL_G0_NC", "P32_MOVW_PREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x018, "R_AARCH64_P32_MOVW_PREL_G1", "P32_MOVW_PREL_G1 relocation (aarch64)."},
  {"aarch64", 0x019, "R_AARCH64_P32_GOT_LD_PREL19", "P32_GOT_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x01a, "R_AARCH64_P32_ADR_GOT_PAGE", "P32_ADR_GOT_PAGE relocation (aarch64)."},
  {"aarch64", 0x01b, "R_AARCH64_P32_LD32_GOT_LO12_NC", "P32_LD32_GOT_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x01c, "R_AARCH64_P32_LD32_GOTPAGE_LO14", "P32_LD32_GOTPAGE_LO14 relocation (aarch64)."},
  {"aarch64", 0x01d, "R_AARCH64_P32_PLT32", "P32_PLT32 relocation (aarch64)."},
  {"aarch64", 0x050, "R_AARCH64_P32_TLSGD_ADR_PREL21", "P32_TLSGD_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x051, "R_AARCH64_P32_TLSGD_ADR_PAGE21", "P32_TLSGD_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x052, "R_AARCH64_P32_TLSGD_ADD_LO12_NC", "P32_TLSGD_ADD_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x053, "R_AARCH64_P32_TLSLD_ADR_PREL21", "P32_TLSLD_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x054, "R_AARCH64_P32_TLSLD_ADR_PAGE21", "P32_TLSLD_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x055, "R_AARCH64_P32_TLSLD_ADD_LO12_NC", "P32_TLSLD_ADD_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x056, "R_AARCH64_P32_TLSLD_LD_PREL19", "P32_TLSLD_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x057, "R_AARCH64_P32_TLSLD_MOVW_DTPREL_G1", "P32_TLSLD_MOVW_DTPREL_G1 relocation (aarch64)."},
  {"aarch64", 0x058, "R_AARCH64_P32_TLSLD_MOVW_DTPREL_G0", "P32_TLSLD_MOVW_DTPREL_G0 relocation (aarch64)."},
  {"aarch64", 0x059, "R_AARCH64_P32_TLSLD_MOVW_DTPREL_G0_NC", "P32_TLSLD_MOVW_DTPREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x05a, "R_AARCH64_P32_TLSLD_ADD_DTPREL_HI12", "P32_TLSLD_ADD_DTPREL_HI12 relocation (aarch64)."},
  {"aarch64", 0x05b, "R_AARCH64_P32_TLSLD_ADD_DTPREL_LO12", "P32_TLSLD_ADD_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x05c, "R_AARCH64_P32_TLSLD_ADD_DTPREL_LO12_NC", "P32_TLSLD_ADD_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x05d, "R_AARCH64_P32_TLSLD_LDST8_DTPREL_LO12", "P32_TLSLD_LDST8_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x05e, "R_AARCH64_P32_TLSLD_LDST8_DTPREL_LO12_NC", "P32_TLSLD_LDST8_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x05f, "R_AARCH64_P32_TLSLD_LDST16_DTPREL_LO12", "P32_TLSLD_LDST16_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x060, "R_AARCH64_P32_TLSLD_LDST16_DTPREL_LO12_NC", "P32_TLSLD_LDST16_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x061, "R_AARCH64_P32_TLSLD_LDST32_DTPREL_LO12", "P32_TLSLD_LDST32_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x062, "R_AARCH64_P32_TLSLD_LDST32_DTPREL_LO12_NC", "P32_TLSLD_LDST32_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x063, "R_AARCH64_P32_TLSLD_LDST64_DTPREL_LO12", "P32_TLSLD_LDST64_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x064, "R_AARCH64_P32_TLSLD_LDST64_DTPREL_LO12_NC", "P32_TLSLD_LDST64_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x065, "R_AARCH64_P32_TLSLD_LDST128_DTPREL_LO12", "P32_TLSLD_LDST128_DTPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x066, "R_AARCH64_P32_TLSLD_LDST128_DTPREL_LO12_NC", "P32_TLSLD_LDST128_DTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x067, "R_AARCH64_P32_TLSIE_ADR_GOTTPREL_PAGE21", "P32_TLSIE_ADR_GOTTPREL_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x068, "R_AARCH64_P32_TLSIE_LD32_GOTTPREL_LO12_NC", "P32_TLSIE_LD32_GOTTPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x069, "R_AARCH64_P32_TLSIE_LD_GOTTPREL_PREL19", "P32_TLSIE_LD_GOTTPREL_PREL19 relocation (aarch64)."},
  {"aarch64", 0x06a, "R_AARCH64_P32_TLSLE_MOVW_TPREL_G1", "P32_TLSLE_MOVW_TPREL_G1 relocation (aarch64)."},
  {"aarch64", 0x06b, "R_AARCH64_P32_TLSLE_MOVW_TPREL_G0", "P32_TLSLE_MOVW_TPREL_G0 relocation (aarch64)."},
  {"aarch64", 0x06c, "R_AARCH64_P32_TLSLE_MOVW_TPREL_G0_NC", "P32_TLSLE_MOVW_TPREL_G0_NC relocation (aarch64)."},
  {"aarch64", 0x06d, "R_AARCH64_P32_TLSLE_ADD_TPREL_HI12", "P32_TLSLE_ADD_TPREL_HI12 relocation (aarch64)."},
  {"aarch64", 0x06e, "R_AARCH64_P32_TLSLE_ADD_TPREL_LO12", "P32_TLSLE_ADD_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x06f, "R_AARCH64_P32_TLSLE_ADD_TPREL_LO12_NC", "P32_TLSLE_ADD_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x070, "R_AARCH64_P32_TLSLE_LDST8_TPREL_LO12", "P32_TLSLE_LDST8_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x071, "R_AARCH64_P32_TLSLE_LDST8_TPREL_LO12_NC", "P32_TLSLE_LDST8_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x072, "R_AARCH64_P32_TLSLE_LDST16_TPREL_LO12", "P32_TLSLE_LDST16_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x073, "R_AARCH64_P32_TLSLE_LDST16_TPREL_LO12_NC", "P32_TLSLE_LDST16_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x074, "R_AARCH64_P32_TLSLE_LDST32_TPREL_LO12", "P32_TLSLE_LDST32_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x075, "R_AARCH64_P32_TLSLE_LDST32_TPREL_LO12_NC", "P32_TLSLE_LDST32_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x076, "R_AARCH64_P32_TLSLE_LDST64_TPREL_LO12", "P32_TLSLE_LDST64_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x077, "R_AARCH64_P32_TLSLE_LDST64_TPREL_LO12_NC", "P32_TLSLE_LDST64_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x078, "R_AARCH64_P32_TLSLE_LDST128_TPREL_LO12", "P32_TLSLE_LDST128_TPREL_LO12 relocation (aarch64)."},
  {"aarch64", 0x079, "R_AARCH64_P32_TLSLE_LDST128_TPREL_LO12_NC", "P32_TLSLE_LDST128_TPREL_LO12_NC relocation (aarch64)."},
  {"aarch64", 0x07a, "R_AARCH64_P32_TLSDESC_LD_PREL19", "P32_TLSDESC_LD_PREL19 relocation (aarch64)."},
  {"aarch64", 0x07b, "R_AARCH64_P32_TLSDESC_ADR_PREL21", "P32_TLSDESC_ADR_PREL21 relocation (aarch64)."},
  {"aarch64", 0x07c, "R_AARCH64_P32_TLSDESC_ADR_PAGE21", "P32_TLSDESC_ADR_PAGE21 relocation (aarch64)."},
  {"aarch64", 0x07d, "R_AARCH64_P32_TLSDESC_LD32_LO12", "P32_TLSDESC_LD32_LO12 relocation (aarch64)."},
  {"aarch64", 0x07e, "R_AARCH64_P32_TLSDESC_ADD_LO12", "P32_TLSDESC_ADD_LO12 relocation (aarch64)."},
  {"aarch64", 0x07f, "R_AARCH64_P32_TLSDESC_CALL", "P32_TLSDESC_CALL relocation (aarch64)."},
  {"aarch64", 0x0b4, "R_AARCH64_P32_COPY", "P32_COPY relocation (aarch64)."},
  {"aarch64", 0x0b5, "R_AARCH64_P32_GLOB_DAT", "P32_GLOB_DAT relocation (aarch64)."},
  {"aarch64", 0x0b6, "R_AARCH64_P32_JUMP_SLOT", "P32_JUMP_SLOT relocation (aarch64)."},
  {"aarch64", 0x0b7, "R_AARCH64_P32_RELATIVE", "P32_RELATIVE relocation (aarch64)."},
  {"aarch64", 0x0b8, "R_AARCH64_P32_TLS_DTPREL", "P32_TLS_DTPREL relocation (aarch64)."},
  {"aarch64", 0x0b9, "R_AARCH64_P32_TLS_DTPMOD", "P32_TLS_DTPMOD relocation (aarch64)."},
  {"aarch64", 0x0ba, "R_AARCH64_P32_TLS_TPREL", "P32_TLS_TPREL relocation (aarch64)."},
  {"aarch64", 0x0bb, "R_AARCH64_P32_TLSDESC", "P32_TLSDESC relocation (aarch64)."},
  {"aarch64", 0x0bc, "R_AARCH64_P32_IRELATIVE", "P32_IRELATIVE relocation (aarch64)."},
  {"arm", 0x00, "R_ARM_NONE", "NONE relocation (arm)."},
  {"arm", 0x01, "R_ARM_PC24", "PC24 relocation (arm)."},
  {"arm", 0x02, "R_ARM_ABS32", "ABS32 relocation (arm)."},
  {"arm", 0x03, "R_ARM_REL32", "REL32 relocation (arm)."},
  {"arm", 0x04, "R_ARM_LDR_PC_G0", "LDR_PC_G0 relocation (arm)."},
  {"arm", 0x05, "R_ARM_ABS16", "ABS16 relocation (arm)."},
  {"arm", 0x06, "R_ARM_ABS12", "ABS12 relocation (arm)."},
  {"arm", 0x07, "R_ARM_THM_ABS5", "THM_ABS5 relocation (arm)."},
  {"arm", 0x08, "R_ARM_ABS8", "ABS8 relocation (arm)."},
  {"arm", 0x09, "R_ARM_SBREL32", "SBREL32 relocation (arm)."},
  {"arm", 0x0a, "R_ARM_THM_CALL", "THM_CALL relocation (arm)."},
  {"arm", 0x0b, "R_ARM_THM_PC8", "THM_PC8 relocation (arm)."},
  {"arm", 0x0c, "R_ARM_BREL_ADJ", "BREL_ADJ relocation (arm)."},
  {"arm", 0x0d, "R_ARM_TLS_DESC", "TLS_DESC relocation (arm)."},
  {"arm", 0x0e, "R_ARM_THM_SWI8", "THM_SWI8 relocation (arm)."},
  {"arm", 0x0f, "R_ARM_XPC25", "XPC25 relocation (arm)."},
  {"arm", 0x10, "R_ARM_THM_XPC22", "THM_XPC22 relocation (arm)."},
  {"arm", 0x11, "R_ARM_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (arm)."},
  {"arm", 0x12, "R_ARM_TLS_DTPOFF32", "TLS_DTPOFF32 relocation (arm)."},
  {"arm", 0x13, "R_ARM_TLS_TPOFF32", "TLS_TPOFF32 relocation (arm)."},
  {"arm", 0x14, "R_ARM_COPY", "COPY relocation (arm)."},
  {"arm", 0x15, "R_ARM_GLOB_DAT", "GLOB_DAT relocation (arm)."},
  {"arm", 0x16, "R_ARM_JUMP_SLOT", "JUMP_SLOT relocation (arm)."},
  {"arm", 0x17, "R_ARM_RELATIVE", "RELATIVE relocation (arm)."},
  {"arm", 0x18, "R_ARM_GOTOFF32", "GOTOFF32 relocation (arm)."},
  {"arm", 0x19, "R_ARM_BASE_PREL", "BASE_PREL relocation (arm)."},
  {"arm", 0x1a, "R_ARM_GOT_BREL", "GOT_BREL relocation (arm)."},
  {"arm", 0x1b, "R_ARM_PLT32", "PLT32 relocation (arm)."},
  {"arm", 0x1c, "R_ARM_CALL", "CALL relocation (arm)."},
  {"arm", 0x1d, "R_ARM_JUMP24", "JUMP24 relocation (arm)."},
  {"arm", 0x1e, "R_ARM_THM_JUMP24", "THM_JUMP24 relocation (arm)."},
  {"arm", 0x1f, "R_ARM_BASE_ABS", "BASE_ABS relocation (arm)."},
  {"arm", 0x20, "R_ARM_ALU_PCREL_7_0", "ALU_PCREL_7_0 relocation (arm)."},
  {"arm", 0x21, "R_ARM_ALU_PCREL_15_8", "ALU_PCREL_15_8 relocation (arm)."},
  {"arm", 0x22, "R_ARM_ALU_PCREL_23_15", "ALU_PCREL_23_15 relocation (arm)."},
  {"arm", 0x23, "R_ARM_LDR_SBREL_11_0_NC", "LDR_SBREL_11_0_NC relocation (arm)."},
  {"arm", 0x24, "R_ARM_ALU_SBREL_19_12_NC", "ALU_SBREL_19_12_NC relocation (arm)."},
  {"arm", 0x25, "R_ARM_ALU_SBREL_27_20_CK", "ALU_SBREL_27_20_CK relocation (arm)."},
  {"arm", 0x26, "R_ARM_TARGET1", "TARGET1 relocation (arm)."},
  {"arm", 0x27, "R_ARM_SBREL31", "SBREL31 relocation (arm)."},
  {"arm", 0x28, "R_ARM_V4BX", "V4BX relocation (arm)."},
  {"arm", 0x29, "R_ARM_TARGET2", "TARGET2 relocation (arm)."},
  {"arm", 0x2a, "R_ARM_PREL31", "PREL31 relocation (arm)."},
  {"arm", 0x2b, "R_ARM_MOVW_ABS_NC", "MOVW_ABS_NC relocation (arm)."},
  {"arm", 0x2c, "R_ARM_MOVT_ABS", "MOVT_ABS relocation (arm)."},
  {"arm", 0x2d, "R_ARM_MOVW_PREL_NC", "MOVW_PREL_NC relocation (arm)."},
  {"arm", 0x2e, "R_ARM_MOVT_PREL", "MOVT_PREL relocation (arm)."},
  {"arm", 0x2f, "R_ARM_THM_MOVW_ABS_NC", "THM_MOVW_ABS_NC relocation (arm)."},
  {"arm", 0x30, "R_ARM_THM_MOVT_ABS", "THM_MOVT_ABS relocation (arm)."},
  {"arm", 0x31, "R_ARM_THM_MOVW_PREL_NC", "THM_MOVW_PREL_NC relocation (arm)."},
  {"arm", 0x32, "R_ARM_THM_MOVT_PREL", "THM_MOVT_PREL relocation (arm)."},
  {"arm", 0x33, "R_ARM_THM_JUMP19", "THM_JUMP19 relocation (arm)."},
  {"arm", 0x34, "R_ARM_THM_JUMP6", "THM_JUMP6 relocation (arm)."},
  {"arm", 0x35, "R_ARM_THM_ALU_PREL_11_0", "THM_ALU_PREL_11_0 relocation (arm)."},
  {"arm", 0x36, "R_ARM_THM_PC12", "THM_PC12 relocation (arm)."},
  {"arm", 0x37, "R_ARM_ABS32_NOI", "ABS32_NOI relocation (arm)."},
  {"arm", 0x38, "R_ARM_REL32_NOI", "REL32_NOI relocation (arm)."},
  {"arm", 0x39, "R_ARM_ALU_PC_G0_NC", "ALU_PC_G0_NC relocation (arm)."},
  {"arm", 0x3a, "R_ARM_ALU_PC_G0", "ALU_PC_G0 relocation (arm)."},
  {"arm", 0x3b, "R_ARM_ALU_PC_G1_NC", "ALU_PC_G1_NC relocation (arm)."},
  {"arm", 0x3c, "R_ARM_ALU_PC_G1", "ALU_PC_G1 relocation (arm)."},
  {"arm", 0x3d, "R_ARM_ALU_PC_G2", "ALU_PC_G2 relocation (arm)."},
  {"arm", 0x3e, "R_ARM_LDR_PC_G1", "LDR_PC_G1 relocation (arm)."},
  {"arm", 0x3f, "R_ARM_LDR_PC_G2", "LDR_PC_G2 relocation (arm)."},
  {"arm", 0x40, "R_ARM_LDRS_PC_G0", "LDRS_PC_G0 relocation (arm)."},
  {"arm", 0x41, "R_ARM_LDRS_PC_G1", "LDRS_PC_G1 relocation (arm)."},
  {"arm", 0x42, "R_ARM_LDRS_PC_G2", "LDRS_PC_G2 relocation (arm)."},
  {"arm", 0x43, "R_ARM_LDC_PC_G0", "LDC_PC_G0 relocation (arm)."},
  {"arm", 0x44, "R_ARM_LDC_PC_G1", "LDC_PC_G1 relocation (arm)."},
  {"arm", 0x45, "R_ARM_LDC_PC_G2", "LDC_PC_G2 relocation (arm)."},
  {"arm", 0x46, "R_ARM_ALU_SB_G0_NC", "ALU_SB_G0_NC relocation (arm)."},
  {"arm", 0x47, "R_ARM_ALU_SB_G0", "ALU_SB_G0 relocation (arm)."},
  {"arm", 0x48, "R_ARM_ALU_SB_G1_NC", "ALU_SB_G1_NC relocation (arm)."},
  {"arm", 0x49, "R_ARM_ALU_SB_G1", "ALU_SB_G1 relocation (arm)."},
  {"arm", 0x4a, "R_ARM_ALU_SB_G2", "ALU_SB_G2 relocation (arm)."},
  {"arm", 0x4b, "R_ARM_LDR_SB_G0", "LDR_SB_G0 relocation (arm)."},
  {"arm", 0x4c, "R_ARM_LDR_SB_G1", "LDR_SB_G1 relocation (arm)."},
  {"arm", 0x4d, "R_ARM_LDR_SB_G2", "LDR_SB_G2 relocation (arm)."},
  {"arm", 0x4e, "R_ARM_LDRS_SB_G0", "LDRS_SB_G0 relocation (arm)."},
  {"arm", 0x4f, "R_ARM_LDRS_SB_G1", "LDRS_SB_G1 relocation (arm)."},
  {"arm", 0x50, "R_ARM_LDRS_SB_G2", "LDRS_SB_G2 relocation (arm)."},
  {"arm", 0x51, "R_ARM_LDC_SB_G0", "LDC_SB_G0 relocation (arm)."},
  {"arm", 0x52, "R_ARM_LDC_SB_G1", "LDC_SB_G1 relocation (arm)."},
  {"arm", 0x53, "R_ARM_LDC_SB_G2", "LDC_SB_G2 relocation (arm)."},
  {"arm", 0x54, "R_ARM_MOVW_BREL_NC", "MOVW_BREL_NC relocation (arm)."},
  {"arm", 0x55, "R_ARM_MOVT_BREL", "MOVT_BREL relocation (arm)."},
  {"arm", 0x56, "R_ARM_MOVW_BREL", "MOVW_BREL relocation (arm)."},
  {"arm", 0x57, "R_ARM_THM_MOVW_BREL_NC", "THM_MOVW_BREL_NC relocation (arm)."},
  {"arm", 0x58, "R_ARM_THM_MOVT_BREL", "THM_MOVT_BREL relocation (arm)."},
  {"arm", 0x59, "R_ARM_THM_MOVW_BREL", "THM_MOVW_BREL relocation (arm)."},
  {"arm", 0x5a, "R_ARM_TLS_GOTDESC", "TLS_GOTDESC relocation (arm)."},
  {"arm", 0x5b, "R_ARM_TLS_CALL", "TLS_CALL relocation (arm)."},
  {"arm", 0x5c, "R_ARM_TLS_DESCSEQ", "TLS_DESCSEQ relocation (arm)."},
  {"arm", 0x5d, "R_ARM_THM_TLS_CALL", "THM_TLS_CALL relocation (arm)."},
  {"arm", 0x5e, "R_ARM_PLT32_ABS", "PLT32_ABS relocation (arm)."},
  {"arm", 0x5f, "R_ARM_GOT_ABS", "GOT_ABS relocation (arm)."},
  {"arm", 0x60, "R_ARM_GOT_PREL", "GOT_PREL relocation (arm)."},
  {"arm", 0x61, "R_ARM_GOT_BREL12", "GOT_BREL12 relocation (arm)."},
  {"arm", 0x62, "R_ARM_GOTOFF12", "GOTOFF12 relocation (arm)."},
  {"arm", 0x63, "R_ARM_GOTRELAX", "GOTRELAX relocation (arm)."},
  {"arm", 0x64, "R_ARM_GNU_VTENTRY", "GNU_VTENTRY relocation (arm)."},
  {"arm", 0x65, "R_ARM_GNU_VTINHERIT", "GNU_VTINHERIT relocation (arm)."},
  {"arm", 0x66, "R_ARM_THM_JUMP11", "THM_JUMP11 relocation (arm)."},
  {"arm", 0x67, "R_ARM_THM_JUMP8", "THM_JUMP8 relocation (arm)."},
  {"arm", 0x68, "R_ARM_TLS_GD32", "TLS_GD32 relocation (arm)."},
  {"arm", 0x69, "R_ARM_TLS_LDM32", "TLS_LDM32 relocation (arm)."},
  {"arm", 0x6a, "R_ARM_TLS_LDO32", "TLS_LDO32 relocation (arm)."},
  {"arm", 0x6b, "R_ARM_TLS_IE32", "TLS_IE32 relocation (arm)."},
  {"arm", 0x6c, "R_ARM_TLS_LE32", "TLS_LE32 relocation (arm)."},
  {"arm", 0x6d, "R_ARM_TLS_LDO12", "TLS_LDO12 relocation (arm)."},
  {"arm", 0x6e, "R_ARM_TLS_LE12", "TLS_LE12 relocation (arm)."},
  {"arm", 0x6f, "R_ARM_TLS_IE12GP", "TLS_IE12GP relocation (arm)."},
  {"arm", 0x70, "R_ARM_PRIVATE_0", "PRIVATE_0 relocation (arm)."},
  {"arm", 0x71, "R_ARM_PRIVATE_1", "PRIVATE_1 relocation (arm)."},
  {"arm", 0x72, "R_ARM_PRIVATE_2", "PRIVATE_2 relocation (arm)."},
  {"arm", 0x73, "R_ARM_PRIVATE_3", "PRIVATE_3 relocation (arm)."},
  {"arm", 0x74, "R_ARM_PRIVATE_4", "PRIVATE_4 relocation (arm)."},
  {"arm", 0x75, "R_ARM_PRIVATE_5", "PRIVATE_5 relocation (arm)."},
  {"arm", 0x76, "R_ARM_PRIVATE_6", "PRIVATE_6 relocation (arm)."},
  {"arm", 0x77, "R_ARM_PRIVATE_7", "PRIVATE_7 relocation (arm)."},
  {"arm", 0x78, "R_ARM_PRIVATE_8", "PRIVATE_8 relocation (arm)."},
  {"arm", 0x79, "R_ARM_PRIVATE_9", "PRIVATE_9 relocation (arm)."},
  {"arm", 0x7a, "R_ARM_PRIVATE_10", "PRIVATE_10 relocation (arm)."},
  {"arm", 0x7b, "R_ARM_PRIVATE_11", "PRIVATE_11 relocation (arm)."},
  {"arm", 0x7c, "R_ARM_PRIVATE_12", "PRIVATE_12 relocation (arm)."},
  {"arm", 0x7d, "R_ARM_PRIVATE_13", "PRIVATE_13 relocation (arm)."},
  {"arm", 0x7e, "R_ARM_PRIVATE_14", "PRIVATE_14 relocation (arm)."},
  {"arm", 0x7f, "R_ARM_PRIVATE_15", "PRIVATE_15 relocation (arm)."},
  {"arm", 0x80, "R_ARM_ME_TOO", "ME_TOO relocation (arm)."},
  {"arm", 0x81, "R_ARM_THM_TLS_DESCSEQ16", "THM_TLS_DESCSEQ16 relocation (arm)."},
  {"arm", 0x82, "R_ARM_THM_TLS_DESCSEQ32", "THM_TLS_DESCSEQ32 relocation (arm)."},
  {"arm", 0x84, "R_ARM_THM_ALU_ABS_G0_NC", "THM_ALU_ABS_G0_NC relocation (arm)."},
  {"arm", 0x85, "R_ARM_THM_ALU_ABS_G1_NC", "THM_ALU_ABS_G1_NC relocation (arm)."},
  {"arm", 0x86, "R_ARM_THM_ALU_ABS_G2_NC", "THM_ALU_ABS_G2_NC relocation (arm)."},
  {"arm", 0x87, "R_ARM_THM_ALU_ABS_G3", "THM_ALU_ABS_G3 relocation (arm)."},
  {"arm", 0x88, "R_ARM_THM_BF16", "THM_BF16 relocation (arm)."},
  {"arm", 0x89, "R_ARM_THM_BF12", "THM_BF12 relocation (arm)."},
  {"arm", 0x8a, "R_ARM_THM_BF18", "THM_BF18 relocation (arm)."},
  {"arm", 0xa0, "R_ARM_IRELATIVE", "IRELATIVE relocation (arm)."},
  {"arm", 0xa1, "R_ARM_GOTFUNCDESC", "GOTFUNCDESC relocation (arm)."},
  {"arm", 0xa2, "R_ARM_GOTOFFFUNCDESC", "GOTOFFFUNCDESC relocation (arm)."},
  {"arm", 0xa3, "R_ARM_FUNCDESC", "FUNCDESC relocation (arm)."},
  {"arm", 0xa4, "R_ARM_FUNCDESC_VALUE", "FUNCDESC_VALUE relocation (arm)."},
  {"arm", 0xa5, "R_ARM_TLS_GD32_FDPIC", "TLS_GD32_FDPIC relocation (arm)."},
  {"arm", 0xa6, "R_ARM_TLS_LDM32_FDPIC", "TLS_LDM32_FDPIC relocation (arm)."},
  {"arm", 0xa7, "R_ARM_TLS_IE32_FDPIC", "TLS_IE32_FDPIC relocation (arm)."},
  {"riscv", 0, "R_RISCV_NONE", "NONE relocation (riscv)."},
  {"riscv", 1, "R_RISCV_32", "32 relocation (riscv)."},
  {"riscv", 2, "R_RISCV_64", "64 relocation (riscv)."},
  {"riscv", 3, "R_RISCV_RELATIVE", "RELATIVE relocation (riscv)."},
  {"riscv", 4, "R_RISCV_COPY", "COPY relocation (riscv)."},
  {"riscv", 5, "R_RISCV_JUMP_SLOT", "JUMP_SLOT relocation (riscv)."},
  {"riscv", 6, "R_RISCV_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (riscv)."},
  {"riscv", 7, "R_RISCV_TLS_DTPMOD64", "TLS_DTPMOD64 relocation (riscv)."},
  {"riscv", 8, "R_RISCV_TLS_DTPREL32", "TLS_DTPREL32 relocation (riscv)."},
  {"riscv", 9, "R_RISCV_TLS_DTPREL64", "TLS_DTPREL64 relocation (riscv)."},
  {"riscv", 10, "R_RISCV_TLS_TPREL32", "TLS_TPREL32 relocation (riscv)."},
  {"riscv", 11, "R_RISCV_TLS_TPREL64", "TLS_TPREL64 relocation (riscv)."},
  {"riscv", 12, "R_RISCV_TLSDESC", "TLSDESC relocation (riscv)."},
  {"riscv", 16, "R_RISCV_BRANCH", "BRANCH relocation (riscv)."},
  {"riscv", 17, "R_RISCV_JAL", "JAL relocation (riscv)."},
  {"riscv", 18, "R_RISCV_CALL", "CALL relocation (riscv)."},
  {"riscv", 19, "R_RISCV_CALL_PLT", "CALL_PLT relocation (riscv)."},
  {"riscv", 20, "R_RISCV_GOT_HI20", "GOT_HI20 relocation (riscv)."},
  {"riscv", 21, "R_RISCV_TLS_GOT_HI20", "TLS_GOT_HI20 relocation (riscv)."},
  {"riscv", 22, "R_RISCV_TLS_GD_HI20", "TLS_GD_HI20 relocation (riscv)."},
  {"riscv", 23, "R_RISCV_PCREL_HI20", "PCREL_HI20 relocation (riscv)."},
  {"riscv", 24, "R_RISCV_PCREL_LO12_I", "PCREL_LO12_I relocation (riscv)."},
  {"riscv", 25, "R_RISCV_PCREL_LO12_S", "PCREL_LO12_S relocation (riscv)."},
  {"riscv", 26, "R_RISCV_HI20", "HI20 relocation (riscv)."},
  {"riscv", 27, "R_RISCV_LO12_I", "LO12_I relocation (riscv)."},
  {"riscv", 28, "R_RISCV_LO12_S", "LO12_S relocation (riscv)."},
  {"riscv", 29, "R_RISCV_TPREL_HI20", "TPREL_HI20 relocation (riscv)."},
  {"riscv", 30, "R_RISCV_TPREL_LO12_I", "TPREL_LO12_I relocation (riscv)."},
  {"riscv", 31, "R_RISCV_TPREL_LO12_S", "TPREL_LO12_S relocation (riscv)."},
  {"riscv", 32, "R_RISCV_TPREL_ADD", "TPREL_ADD relocation (riscv)."},
  {"riscv", 33, "R_RISCV_ADD8", "ADD8 relocation (riscv)."},
  {"riscv", 34, "R_RISCV_ADD16", "ADD16 relocation (riscv)."},
  {"riscv", 35, "R_RISCV_ADD32", "ADD32 relocation (riscv)."},
  {"riscv", 36, "R_RISCV_ADD64", "ADD64 relocation (riscv)."},
  {"riscv", 37, "R_RISCV_SUB8", "SUB8 relocation (riscv)."},
  {"riscv", 38, "R_RISCV_SUB16", "SUB16 relocation (riscv)."},
  {"riscv", 39, "R_RISCV_SUB32", "SUB32 relocation (riscv)."},
  {"riscv", 40, "R_RISCV_SUB64", "SUB64 relocation (riscv)."},
  {"riscv", 41, "R_RISCV_GOT32_PCREL", "GOT32_PCREL relocation (riscv)."},
  {"riscv", 43, "R_RISCV_ALIGN", "ALIGN relocation (riscv)."},
  {"riscv", 44, "R_RISCV_RVC_BRANCH", "RVC_BRANCH relocation (riscv)."},
  {"riscv", 45, "R_RISCV_RVC_JUMP", "RVC_JUMP relocation (riscv)."},
  {"riscv", 51, "R_RISCV_RELAX", "RELAX relocation (riscv)."},
  {"riscv", 52, "R_RISCV_SUB6", "SUB6 relocation (riscv)."},
  {"riscv", 53, "R_RISCV_SET6", "SET6 relocation (riscv)."},
  {"riscv", 54, "R_RISCV_SET8", "SET8 relocation (riscv)."},
  {"riscv", 55, "R_RISCV_SET16", "SET16 relocation (riscv)."},
  {"riscv", 56, "R_RISCV_SET32", "SET32 relocation (riscv)."},
  {"riscv", 57, "R_RISCV_32_PCREL", "32_PCREL relocation (riscv)."},
  {"riscv", 58, "R_RISCV_IRELATIVE", "IRELATIVE relocation (riscv)."},
  {"riscv", 59, "R_RISCV_PLT32", "PLT32 relocation (riscv)."},
  {"riscv", 60, "R_RISCV_SET_ULEB128", "SET_ULEB128 relocation (riscv)."},
  {"riscv", 61, "R_RISCV_SUB_ULEB128", "SUB_ULEB128 relocation (riscv)."},
  {"riscv", 62, "R_RISCV_TLSDESC_HI20", "TLSDESC_HI20 relocation (riscv)."},
  {"riscv", 63, "R_RISCV_TLSDESC_LOAD_LO12", "TLSDESC_LOAD_LO12 relocation (riscv)."},
  {"riscv", 64, "R_RISCV_TLSDESC_ADD_LO12", "TLSDESC_ADD_LO12 relocation (riscv)."},
  {"riscv", 65, "R_RISCV_TLSDESC_CALL", "TLSDESC_CALL relocation (riscv)."},
  {"riscv", 191, "R_RISCV_VENDOR", "VENDOR relocation (riscv)."},
  {"riscv", 192, "R_RISCV_CUSTOM192", "CUSTOM192 relocation (riscv)."},
  {"riscv", 193, "R_RISCV_CUSTOM193", "CUSTOM193 relocation (riscv)."},
  {"riscv", 194, "R_RISCV_CUSTOM194", "CUSTOM194 relocation (riscv)."},
  {"riscv", 195, "R_RISCV_CUSTOM195", "CUSTOM195 relocation (riscv)."},
  {"riscv", 196, "R_RISCV_CUSTOM196", "CUSTOM196 relocation (riscv)."},
  {"riscv", 197, "R_RISCV_CUSTOM197", "CUSTOM197 relocation (riscv)."},
  {"riscv", 198, "R_RISCV_CUSTOM198", "CUSTOM198 relocation (riscv)."},
  {"riscv", 199, "R_RISCV_CUSTOM199", "CUSTOM199 relocation (riscv)."},
  {"riscv", 200, "R_RISCV_CUSTOM200", "CUSTOM200 relocation (riscv)."},
  {"riscv", 201, "R_RISCV_CUSTOM201", "CUSTOM201 relocation (riscv)."},
  {"riscv", 202, "R_RISCV_CUSTOM202", "CUSTOM202 relocation (riscv)."},
  {"riscv", 203, "R_RISCV_CUSTOM203", "CUSTOM203 relocation (riscv)."},
  {"riscv", 204, "R_RISCV_CUSTOM204", "CUSTOM204 relocation (riscv)."},
  {"riscv", 205, "R_RISCV_CUSTOM205", "CUSTOM205 relocation (riscv)."},
  {"riscv", 206, "R_RISCV_CUSTOM206", "CUSTOM206 relocation (riscv)."},
  {"riscv", 207, "R_RISCV_CUSTOM207", "CUSTOM207 relocation (riscv)."},
  {"riscv", 208, "R_RISCV_CUSTOM208", "CUSTOM208 relocation (riscv)."},
  {"riscv", 209, "R_RISCV_CUSTOM209", "CUSTOM209 relocation (riscv)."},
  {"riscv", 210, "R_RISCV_CUSTOM210", "CUSTOM210 relocation (riscv)."},
  {"riscv", 211, "R_RISCV_CUSTOM211", "CUSTOM211 relocation (riscv)."},
  {"riscv", 212, "R_RISCV_CUSTOM212", "CUSTOM212 relocation (riscv)."},
  {"riscv", 213, "R_RISCV_CUSTOM213", "CUSTOM213 relocation (riscv)."},
  {"riscv", 214, "R_RISCV_CUSTOM214", "CUSTOM214 relocation (riscv)."},
  {"riscv", 215, "R_RISCV_CUSTOM215", "CUSTOM215 relocation (riscv)."},
  {"riscv", 216, "R_RISCV_CUSTOM216", "CUSTOM216 relocation (riscv)."},
  {"riscv", 217, "R_RISCV_CUSTOM217", "CUSTOM217 relocation (riscv)."},
  {"riscv", 218, "R_RISCV_CUSTOM218", "CUSTOM218 relocation (riscv)."},
  {"riscv", 219, "R_RISCV_CUSTOM219", "CUSTOM219 relocation (riscv)."},
  {"riscv", 220, "R_RISCV_CUSTOM220", "CUSTOM220 relocation (riscv)."},
  {"riscv", 221, "R_RISCV_CUSTOM221", "CUSTOM221 relocation (riscv)."},
  {"riscv", 222, "R_RISCV_CUSTOM222", "CUSTOM222 relocation (riscv)."},
  {"riscv", 223, "R_RISCV_CUSTOM223", "CUSTOM223 relocation (riscv)."},
  {"riscv", 224, "R_RISCV_CUSTOM224", "CUSTOM224 relocation (riscv)."},
  {"riscv", 225, "R_RISCV_CUSTOM225", "CUSTOM225 relocation (riscv)."},
  {"riscv", 226, "R_RISCV_CUSTOM226", "CUSTOM226 relocation (riscv)."},
  {"riscv", 227, "R_RISCV_CUSTOM227", "CUSTOM227 relocation (riscv)."},
  {"riscv", 228, "R_RISCV_CUSTOM228", "CUSTOM228 relocation (riscv)."},
  {"riscv", 229, "R_RISCV_CUSTOM229", "CUSTOM229 relocation (riscv)."},
  {"riscv", 230, "R_RISCV_CUSTOM230", "CUSTOM230 relocation (riscv)."},
  {"riscv", 231, "R_RISCV_CUSTOM231", "CUSTOM231 relocation (riscv)."},
  {"riscv", 232, "R_RISCV_CUSTOM232", "CUSTOM232 relocation (riscv)."},
  {"riscv", 233, "R_RISCV_CUSTOM233", "CUSTOM233 relocation (riscv)."},
  {"riscv", 234, "R_RISCV_CUSTOM234", "CUSTOM234 relocation (riscv)."},
  {"riscv", 235, "R_RISCV_CUSTOM235", "CUSTOM235 relocation (riscv)."},
  {"riscv", 236, "R_RISCV_CUSTOM236", "CUSTOM236 relocation (riscv)."},
  {"riscv", 237, "R_RISCV_CUSTOM237", "CUSTOM237 relocation (riscv)."},
  {"riscv", 238, "R_RISCV_CUSTOM238", "CUSTOM238 relocation (riscv)."},
  {"riscv", 239, "R_RISCV_CUSTOM239", "CUSTOM239 relocation (riscv)."},
  {"riscv", 240, "R_RISCV_CUSTOM240", "CUSTOM240 relocation (riscv)."},
  {"riscv", 241, "R_RISCV_CUSTOM241", "CUSTOM241 relocation (riscv)."},
  {"riscv", 242, "R_RISCV_CUSTOM242", "CUSTOM242 relocation (riscv)."},
  {"riscv", 243, "R_RISCV_CUSTOM243", "CUSTOM243 relocation (riscv)."},
  {"riscv", 244, "R_RISCV_CUSTOM244", "CUSTOM244 relocation (riscv)."},
  {"riscv", 245, "R_RISCV_CUSTOM245", "CUSTOM245 relocation (riscv)."},
  {"riscv", 246, "R_RISCV_CUSTOM246", "CUSTOM246 relocation (riscv)."},
  {"riscv", 247, "R_RISCV_CUSTOM247", "CUSTOM247 relocation (riscv)."},
  {"riscv", 248, "R_RISCV_CUSTOM248", "CUSTOM248 relocation (riscv)."},
  {"riscv", 249, "R_RISCV_CUSTOM249", "CUSTOM249 relocation (riscv)."},
  {"riscv", 250, "R_RISCV_CUSTOM250", "CUSTOM250 relocation (riscv)."},
  {"riscv", 251, "R_RISCV_CUSTOM251", "CUSTOM251 relocation (riscv)."},
  {"riscv", 252, "R_RISCV_CUSTOM252", "CUSTOM252 relocation (riscv)."},
  {"riscv", 253, "R_RISCV_CUSTOM253", "CUSTOM253 relocation (riscv)."},
  {"riscv", 254, "R_RISCV_CUSTOM254", "CUSTOM254 relocation (riscv)."},
  {"riscv", 255, "R_RISCV_CUSTOM255", "CUSTOM255 relocation (riscv)."},
  {"x86_64", 0, "R_X86_64_NONE", "64_NONE relocation (x86_64)."},
  {"x86_64", 1, "R_X86_64_64", "64_64 relocation (x86_64)."},
  {"x86_64", 2, "R_X86_64_PC32", "64_PC32 relocation (x86_64)."},
  {"x86_64", 3, "R_X86_64_GOT32", "64_GOT32 relocation (x86_64)."},
  {"x86_64", 4, "R_X86_64_PLT32", "64_PLT32 relocation (x86_64)."},
  {"x86_64", 5, "R_X86_64_COPY", "64_COPY relocation (x86_64)."},
  {"x86_64", 6, "R_X86_64_GLOB_DAT", "64_GLOB_DAT relocation (x86_64)."},
  {"x86_64", 7, "R_X86_64_JUMP_SLOT", "64_JUMP_SLOT relocation (x86_64)."},
  {"x86_64", 8, "R_X86_64_RELATIVE", "64_RELATIVE relocation (x86_64)."},
  {"x86_64", 9, "R_X86_64_GOTPCREL", "64_GOTPCREL relocation (x86_64)."},
  {"x86_64", 10, "R_X86_64_32", "64_32 relocation (x86_64)."},
  {"x86_64", 11, "R_X86_64_32S", "64_32S relocation (x86_64)."},
  {"x86_64", 12, "R_X86_64_16", "64_16 relocation (x86_64)."},
  {"x86_64", 13, "R_X86_64_PC16", "64_PC16 relocation (x86_64)."},
  {"x86_64", 14, "R_X86_64_8", "64_8 relocation (x86_64)."},
  {"x86_64", 15, "R_X86_64_PC8", "64_PC8 relocation (x86_64)."},
  {"x86_64", 16, "R_X86_64_DTPMOD64", "64_DTPMOD64 relocation (x86_64)."},
  {"x86_64", 17, "R_X86_64_DTPOFF64", "64_DTPOFF64 relocation (x86_64)."},
  {"x86_64", 18, "R_X86_64_TPOFF64", "64_TPOFF64 relocation (x86_64)."},
  {"x86_64", 19, "R_X86_64_TLSGD", "64_TLSGD relocation (x86_64)."},
  {"x86_64", 20, "R_X86_64_TLSLD", "64_TLSLD relocation (x86_64)."},
  {"x86_64", 21, "R_X86_64_DTPOFF32", "64_DTPOFF32 relocation (x86_64)."},
  {"x86_64", 22, "R_X86_64_GOTTPOFF", "64_GOTTPOFF relocation (x86_64)."},
  {"x86_64", 23, "R_X86_64_TPOFF32", "64_TPOFF32 relocation (x86_64)."},
  {"x86_64", 24, "R_X86_64_PC64", "64_PC64 relocation (x86_64)."},
  {"x86_64", 25, "R_X86_64_GOTOFF64", "64_GOTOFF64 relocation (x86_64)."},
  {"x86_64", 26, "R_X86_64_GOTPC32", "64_GOTPC32 relocation (x86_64)."},
  {"x86_64", 27, "R_X86_64_GOT64", "64_GOT64 relocation (x86_64)."},
  {"x86_64", 28, "R_X86_64_GOTPCREL64", "64_GOTPCREL64 relocation (x86_64)."},
  {"x86_64", 29, "R_X86_64_GOTPC64", "64_GOTPC64 relocation (x86_64)."},
  {"x86_64", 30, "R_X86_64_GOTPLT64", "64_GOTPLT64 relocation (x86_64)."},
  {"x86_64", 31, "R_X86_64_PLTOFF64", "64_PLTOFF64 relocation (x86_64)."},
  {"x86_64", 32, "R_X86_64_SIZE32", "64_SIZE32 relocation (x86_64)."},
  {"x86_64", 33, "R_X86_64_SIZE64", "64_SIZE64 relocation (x86_64)."},
  {"x86_64", 34, "R_X86_64_GOTPC32_TLSDESC", "64_GOTPC32_TLSDESC relocation (x86_64)."},
  {"x86_64", 35, "R_X86_64_TLSDESC_CALL", "64_TLSDESC_CALL relocation (x86_64)."},
  {"x86_64", 36, "R_X86_64_TLSDESC", "64_TLSDESC relocation (x86_64)."},
  {"x86_64", 37, "R_X86_64_IRELATIVE", "64_IRELATIVE relocation (x86_64)."},
  {"x86_64", 41, "R_X86_64_GOTPCRELX", "64_GOTPCRELX relocation (x86_64)."},
  {"x86_64", 42, "R_X86_64_REX_GOTPCRELX", "64_REX_GOTPCRELX relocation (x86_64)."},
  {"x86_64", 43, "R_X86_64_CODE_4_GOTPCRELX", "64_CODE_4_GOTPCRELX relocation (x86_64)."},
  {"x86_64", 44, "R_X86_64_CODE_4_GOTTPOFF", "64_CODE_4_GOTTPOFF relocation (x86_64)."},
  {"x86_64", 45, "R_X86_64_CODE_4_GOTPC32_TLSDESC", "64_CODE_4_GOTPC32_TLSDESC relocation (x86_64)."},
  {"x86_64", 50, "R_X86_64_CODE_6_GOTTPOFF", "64_CODE_6_GOTTPOFF relocation (x86_64)."},
  {"i386", 0, "R_386_NONE", "NONE relocation (i386)."},
  {"i386", 1, "R_386_32", "32 relocation (i386)."},
  {"i386", 2, "R_386_PC32", "PC32 relocation (i386)."},
  {"i386", 3, "R_386_GOT32", "GOT32 relocation (i386)."},
  {"i386", 4, "R_386_PLT32", "PLT32 relocation (i386)."},
  {"i386", 5, "R_386_COPY", "COPY relocation (i386)."},
  {"i386", 6, "R_386_GLOB_DAT", "GLOB_DAT relocation (i386)."},
  {"i386", 7, "R_386_JUMP_SLOT", "JUMP_SLOT relocation (i386)."},
  {"i386", 8, "R_386_RELATIVE", "RELATIVE relocation (i386)."},
  {"i386", 9, "R_386_GOTOFF", "GOTOFF relocation (i386)."},
  {"i386", 10, "R_386_GOTPC", "GOTPC relocation (i386)."},
  {"i386", 11, "R_386_32PLT", "32PLT relocation (i386)."},
  {"i386", 14, "R_386_TLS_TPOFF", "TLS_TPOFF relocation (i386)."},
  {"i386", 15, "R_386_TLS_IE", "TLS_IE relocation (i386)."},
  {"i386", 16, "R_386_TLS_GOTIE", "TLS_GOTIE relocation (i386)."},
  {"i386", 17, "R_386_TLS_LE", "TLS_LE relocation (i386)."},
  {"i386", 18, "R_386_TLS_GD", "TLS_GD relocation (i386)."},
  {"i386", 19, "R_386_TLS_LDM", "TLS_LDM relocation (i386)."},
  {"i386", 20, "R_386_16", "16 relocation (i386)."},
  {"i386", 21, "R_386_PC16", "PC16 relocation (i386)."},
  {"i386", 22, "R_386_8", "8 relocation (i386)."},
  {"i386", 23, "R_386_PC8", "PC8 relocation (i386)."},
  {"i386", 24, "R_386_TLS_GD_32", "TLS_GD_32 relocation (i386)."},
  {"i386", 25, "R_386_TLS_GD_PUSH", "TLS_GD_PUSH relocation (i386)."},
  {"i386", 26, "R_386_TLS_GD_CALL", "TLS_GD_CALL relocation (i386)."},
  {"i386", 27, "R_386_TLS_GD_POP", "TLS_GD_POP relocation (i386)."},
  {"i386", 28, "R_386_TLS_LDM_32", "TLS_LDM_32 relocation (i386)."},
  {"i386", 29, "R_386_TLS_LDM_PUSH", "TLS_LDM_PUSH relocation (i386)."},
  {"i386", 30, "R_386_TLS_LDM_CALL", "TLS_LDM_CALL relocation (i386)."},
  {"i386", 31, "R_386_TLS_LDM_POP", "TLS_LDM_POP relocation (i386)."},
  {"i386", 32, "R_386_TLS_LDO_32", "TLS_LDO_32 relocation (i386)."},
  {"i386", 33, "R_386_TLS_IE_32", "TLS_IE_32 relocation (i386)."},
  {"i386", 34, "R_386_TLS_LE_32", "TLS_LE_32 relocation (i386)."},
  {"i386", 35, "R_386_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (i386)."},
  {"i386", 36, "R_386_TLS_DTPOFF32", "TLS_DTPOFF32 relocation (i386)."},
  {"i386", 37, "R_386_TLS_TPOFF32", "TLS_TPOFF32 relocation (i386)."},
  {"i386", 39, "R_386_TLS_GOTDESC", "TLS_GOTDESC relocation (i386)."},
  {"i386", 40, "R_386_TLS_DESC_CALL", "TLS_DESC_CALL relocation (i386)."},
  {"i386", 41, "R_386_TLS_DESC", "TLS_DESC relocation (i386)."},
  {"i386", 42, "R_386_IRELATIVE", "IRELATIVE relocation (i386)."},
  {"i386", 43, "R_386_GOT32X", "GOT32X relocation (i386)."},
  {"powerpc", 0, "R_PPC_NONE", "NONE relocation (powerpc)."},
  {"powerpc", 1, "R_PPC_ADDR32", "ADDR32 relocation (powerpc)."},
  {"powerpc", 2, "R_PPC_ADDR24", "ADDR24 relocation (powerpc)."},
  {"powerpc", 3, "R_PPC_ADDR16", "ADDR16 relocation (powerpc)."},
  {"powerpc", 4, "R_PPC_ADDR16_LO", "ADDR16_LO relocation (powerpc)."},
  {"powerpc", 5, "R_PPC_ADDR16_HI", "ADDR16_HI relocation (powerpc)."},
  {"powerpc", 6, "R_PPC_ADDR16_HA", "ADDR16_HA relocation (powerpc)."},
  {"powerpc", 7, "R_PPC_ADDR14", "ADDR14 relocation (powerpc)."},
  {"powerpc", 8, "R_PPC_ADDR14_BRTAKEN", "ADDR14_BRTAKEN relocation (powerpc)."},
  {"powerpc", 9, "R_PPC_ADDR14_BRNTAKEN", "ADDR14_BRNTAKEN relocation (powerpc)."},
  {"powerpc", 10, "R_PPC_REL24", "REL24 relocation (powerpc)."},
  {"powerpc", 11, "R_PPC_REL14", "REL14 relocation (powerpc)."},
  {"powerpc", 12, "R_PPC_REL14_BRTAKEN", "REL14_BRTAKEN relocation (powerpc)."},
  {"powerpc", 13, "R_PPC_REL14_BRNTAKEN", "REL14_BRNTAKEN relocation (powerpc)."},
  {"powerpc", 14, "R_PPC_GOT16", "GOT16 relocation (powerpc)."},
  {"powerpc", 15, "R_PPC_GOT16_LO", "GOT16_LO relocation (powerpc)."},
  {"powerpc", 16, "R_PPC_GOT16_HI", "GOT16_HI relocation (powerpc)."},
  {"powerpc", 17, "R_PPC_GOT16_HA", "GOT16_HA relocation (powerpc)."},
  {"powerpc", 18, "R_PPC_PLTREL24", "PLTREL24 relocation (powerpc)."},
  {"powerpc", 19, "R_PPC_COPY", "COPY relocation (powerpc)."},
  {"powerpc", 20, "R_PPC_GLOB_DAT", "GLOB_DAT relocation (powerpc)."},
  {"powerpc", 21, "R_PPC_JMP_SLOT", "JMP_SLOT relocation (powerpc)."},
  {"powerpc", 22, "R_PPC_RELATIVE", "RELATIVE relocation (powerpc)."},
  {"powerpc", 23, "R_PPC_LOCAL24PC", "LOCAL24PC relocation (powerpc)."},
  {"powerpc", 24, "R_PPC_UADDR32", "UADDR32 relocation (powerpc)."},
  {"powerpc", 25, "R_PPC_UADDR16", "UADDR16 relocation (powerpc)."},
  {"powerpc", 26, "R_PPC_REL32", "REL32 relocation (powerpc)."},
  {"powerpc", 27, "R_PPC_PLT32", "PLT32 relocation (powerpc)."},
  {"powerpc", 28, "R_PPC_PLTREL32", "PLTREL32 relocation (powerpc)."},
  {"powerpc", 29, "R_PPC_PLT16_LO", "PLT16_LO relocation (powerpc)."},
  {"powerpc", 30, "R_PPC_PLT16_HI", "PLT16_HI relocation (powerpc)."},
  {"powerpc", 31, "R_PPC_PLT16_HA", "PLT16_HA relocation (powerpc)."},
  {"powerpc", 32, "R_PPC_SDAREL16", "SDAREL16 relocation (powerpc)."},
  {"powerpc", 33, "R_PPC_SECTOFF", "SECTOFF relocation (powerpc)."},
  {"powerpc", 34, "R_PPC_SECTOFF_LO", "SECTOFF_LO relocation (powerpc)."},
  {"powerpc", 35, "R_PPC_SECTOFF_HI", "SECTOFF_HI relocation (powerpc)."},
  {"powerpc", 36, "R_PPC_SECTOFF_HA", "SECTOFF_HA relocation (powerpc)."},
  {"powerpc", 37, "R_PPC_ADDR30", "ADDR30 relocation (powerpc)."},
  {"powerpc", 67, "R_PPC_TLS", "TLS relocation (powerpc)."},
  {"powerpc", 68, "R_PPC_DTPMOD32", "DTPMOD32 relocation (powerpc)."},
  {"powerpc", 69, "R_PPC_TPREL16", "TPREL16 relocation (powerpc)."},
  {"powerpc", 70, "R_PPC_TPREL16_LO", "TPREL16_LO relocation (powerpc)."},
  {"powerpc", 71, "R_PPC_TPREL16_HI", "TPREL16_HI relocation (powerpc)."},
  {"powerpc", 72, "R_PPC_TPREL16_HA", "TPREL16_HA relocation (powerpc)."},
  {"powerpc", 73, "R_PPC_TPREL32", "TPREL32 relocation (powerpc)."},
  {"powerpc", 74, "R_PPC_DTPREL16", "DTPREL16 relocation (powerpc)."},
  {"powerpc", 75, "R_PPC_DTPREL16_LO", "DTPREL16_LO relocation (powerpc)."},
  {"powerpc", 76, "R_PPC_DTPREL16_HI", "DTPREL16_HI relocation (powerpc)."},
  {"powerpc", 77, "R_PPC_DTPREL16_HA", "DTPREL16_HA relocation (powerpc)."},
  {"powerpc", 78, "R_PPC_DTPREL32", "DTPREL32 relocation (powerpc)."},
  {"powerpc", 79, "R_PPC_GOT_TLSGD16", "GOT_TLSGD16 relocation (powerpc)."},
  {"powerpc", 80, "R_PPC_GOT_TLSGD16_LO", "GOT_TLSGD16_LO relocation (powerpc)."},
  {"powerpc", 81, "R_PPC_GOT_TLSGD16_HI", "GOT_TLSGD16_HI relocation (powerpc)."},
  {"powerpc", 82, "R_PPC_GOT_TLSGD16_HA", "GOT_TLSGD16_HA relocation (powerpc)."},
  {"powerpc", 83, "R_PPC_GOT_TLSLD16", "GOT_TLSLD16 relocation (powerpc)."},
  {"powerpc", 84, "R_PPC_GOT_TLSLD16_LO", "GOT_TLSLD16_LO relocation (powerpc)."},
  {"powerpc", 85, "R_PPC_GOT_TLSLD16_HI", "GOT_TLSLD16_HI relocation (powerpc)."},
  {"powerpc", 86, "R_PPC_GOT_TLSLD16_HA", "GOT_TLSLD16_HA relocation (powerpc)."},
  {"powerpc", 87, "R_PPC_GOT_TPREL16", "GOT_TPREL16 relocation (powerpc)."},
  {"powerpc", 88, "R_PPC_GOT_TPREL16_LO", "GOT_TPREL16_LO relocation (powerpc)."},
  {"powerpc", 89, "R_PPC_GOT_TPREL16_HI", "GOT_TPREL16_HI relocation (powerpc)."},
  {"powerpc", 90, "R_PPC_GOT_TPREL16_HA", "GOT_TPREL16_HA relocation (powerpc)."},
  {"powerpc", 91, "R_PPC_GOT_DTPREL16", "GOT_DTPREL16 relocation (powerpc)."},
  {"powerpc", 92, "R_PPC_GOT_DTPREL16_LO", "GOT_DTPREL16_LO relocation (powerpc)."},
  {"powerpc", 93, "R_PPC_GOT_DTPREL16_HI", "GOT_DTPREL16_HI relocation (powerpc)."},
  {"powerpc", 94, "R_PPC_GOT_DTPREL16_HA", "GOT_DTPREL16_HA relocation (powerpc)."},
  {"powerpc", 95, "R_PPC_TLSGD", "TLSGD relocation (powerpc)."},
  {"powerpc", 96, "R_PPC_TLSLD", "TLSLD relocation (powerpc)."},
  {"powerpc", 248, "R_PPC_IRELATIVE", "IRELATIVE relocation (powerpc)."},
  {"powerpc", 249, "R_PPC_REL16", "REL16 relocation (powerpc)."},
  {"powerpc", 250, "R_PPC_REL16_LO", "REL16_LO relocation (powerpc)."},
  {"powerpc", 251, "R_PPC_REL16_HI", "REL16_HI relocation (powerpc)."},
  {"powerpc", 252, "R_PPC_REL16_HA", "REL16_HA relocation (powerpc)."},
  {"powerpc64", 0, "R_PPC64_NONE", "NONE relocation (powerpc64)."},
  {"powerpc64", 1, "R_PPC64_ADDR32", "ADDR32 relocation (powerpc64)."},
  {"powerpc64", 2, "R_PPC64_ADDR24", "ADDR24 relocation (powerpc64)."},
  {"powerpc64", 3, "R_PPC64_ADDR16", "ADDR16 relocation (powerpc64)."},
  {"powerpc64", 4, "R_PPC64_ADDR16_LO", "ADDR16_LO relocation (powerpc64)."},
  {"powerpc64", 5, "R_PPC64_ADDR16_HI", "ADDR16_HI relocation (powerpc64)."},
  {"powerpc64", 6, "R_PPC64_ADDR16_HA", "ADDR16_HA relocation (powerpc64)."},
  {"powerpc64", 7, "R_PPC64_ADDR14", "ADDR14 relocation (powerpc64)."},
  {"powerpc64", 8, "R_PPC64_ADDR14_BRTAKEN", "ADDR14_BRTAKEN relocation (powerpc64)."},
  {"powerpc64", 9, "R_PPC64_ADDR14_BRNTAKEN", "ADDR14_BRNTAKEN relocation (powerpc64)."},
  {"powerpc64", 10, "R_PPC64_REL24", "REL24 relocation (powerpc64)."},
  {"powerpc64", 11, "R_PPC64_REL14", "REL14 relocation (powerpc64)."},
  {"powerpc64", 12, "R_PPC64_REL14_BRTAKEN", "REL14_BRTAKEN relocation (powerpc64)."},
  {"powerpc64", 13, "R_PPC64_REL14_BRNTAKEN", "REL14_BRNTAKEN relocation (powerpc64)."},
  {"powerpc64", 14, "R_PPC64_GOT16", "GOT16 relocation (powerpc64)."},
  {"powerpc64", 15, "R_PPC64_GOT16_LO", "GOT16_LO relocation (powerpc64)."},
  {"powerpc64", 16, "R_PPC64_GOT16_HI", "GOT16_HI relocation (powerpc64)."},
  {"powerpc64", 17, "R_PPC64_GOT16_HA", "GOT16_HA relocation (powerpc64)."},
  {"powerpc64", 19, "R_PPC64_COPY", "COPY relocation (powerpc64)."},
  {"powerpc64", 20, "R_PPC64_GLOB_DAT", "GLOB_DAT relocation (powerpc64)."},
  {"powerpc64", 21, "R_PPC64_JMP_SLOT", "JMP_SLOT relocation (powerpc64)."},
  {"powerpc64", 22, "R_PPC64_RELATIVE", "RELATIVE relocation (powerpc64)."},
  {"powerpc64", 26, "R_PPC64_REL32", "REL32 relocation (powerpc64)."},
  {"powerpc64", 38, "R_PPC64_ADDR64", "ADDR64 relocation (powerpc64)."},
  {"powerpc64", 39, "R_PPC64_ADDR16_HIGHER", "ADDR16_HIGHER relocation (powerpc64)."},
  {"powerpc64", 40, "R_PPC64_ADDR16_HIGHERA", "ADDR16_HIGHERA relocation (powerpc64)."},
  {"powerpc64", 41, "R_PPC64_ADDR16_HIGHEST", "ADDR16_HIGHEST relocation (powerpc64)."},
  {"powerpc64", 42, "R_PPC64_ADDR16_HIGHESTA", "ADDR16_HIGHESTA relocation (powerpc64)."},
  {"powerpc64", 44, "R_PPC64_REL64", "REL64 relocation (powerpc64)."},
  {"powerpc64", 47, "R_PPC64_TOC16", "TOC16 relocation (powerpc64)."},
  {"powerpc64", 48, "R_PPC64_TOC16_LO", "TOC16_LO relocation (powerpc64)."},
  {"powerpc64", 49, "R_PPC64_TOC16_HI", "TOC16_HI relocation (powerpc64)."},
  {"powerpc64", 50, "R_PPC64_TOC16_HA", "TOC16_HA relocation (powerpc64)."},
  {"powerpc64", 51, "R_PPC64_TOC", "TOC relocation (powerpc64)."},
  {"powerpc64", 56, "R_PPC64_ADDR16_DS", "ADDR16_DS relocation (powerpc64)."},
  {"powerpc64", 57, "R_PPC64_ADDR16_LO_DS", "ADDR16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 58, "R_PPC64_GOT16_DS", "GOT16_DS relocation (powerpc64)."},
  {"powerpc64", 59, "R_PPC64_GOT16_LO_DS", "GOT16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 63, "R_PPC64_TOC16_DS", "TOC16_DS relocation (powerpc64)."},
  {"powerpc64", 64, "R_PPC64_TOC16_LO_DS", "TOC16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 67, "R_PPC64_TLS", "TLS relocation (powerpc64)."},
  {"powerpc64", 68, "R_PPC64_DTPMOD64", "DTPMOD64 relocation (powerpc64)."},
  {"powerpc64", 69, "R_PPC64_TPREL16", "TPREL16 relocation (powerpc64)."},
  {"powerpc64", 70, "R_PPC64_TPREL16_LO", "TPREL16_LO relocation (powerpc64)."},
  {"powerpc64", 71, "R_PPC64_TPREL16_HI", "TPREL16_HI relocation (powerpc64)."},
  {"powerpc64", 72, "R_PPC64_TPREL16_HA", "TPREL16_HA relocation (powerpc64)."},
  {"powerpc64", 73, "R_PPC64_TPREL64", "TPREL64 relocation (powerpc64)."},
  {"powerpc64", 74, "R_PPC64_DTPREL16", "DTPREL16 relocation (powerpc64)."},
  {"powerpc64", 75, "R_PPC64_DTPREL16_LO", "DTPREL16_LO relocation (powerpc64)."},
  {"powerpc64", 76, "R_PPC64_DTPREL16_HI", "DTPREL16_HI relocation (powerpc64)."},
  {"powerpc64", 77, "R_PPC64_DTPREL16_HA", "DTPREL16_HA relocation (powerpc64)."},
  {"powerpc64", 78, "R_PPC64_DTPREL64", "DTPREL64 relocation (powerpc64)."},
  {"powerpc64", 79, "R_PPC64_GOT_TLSGD16", "GOT_TLSGD16 relocation (powerpc64)."},
  {"powerpc64", 80, "R_PPC64_GOT_TLSGD16_LO", "GOT_TLSGD16_LO relocation (powerpc64)."},
  {"powerpc64", 81, "R_PPC64_GOT_TLSGD16_HI", "GOT_TLSGD16_HI relocation (powerpc64)."},
  {"powerpc64", 82, "R_PPC64_GOT_TLSGD16_HA", "GOT_TLSGD16_HA relocation (powerpc64)."},
  {"powerpc64", 83, "R_PPC64_GOT_TLSLD16", "GOT_TLSLD16 relocation (powerpc64)."},
  {"powerpc64", 84, "R_PPC64_GOT_TLSLD16_LO", "GOT_TLSLD16_LO relocation (powerpc64)."},
  {"powerpc64", 85, "R_PPC64_GOT_TLSLD16_HI", "GOT_TLSLD16_HI relocation (powerpc64)."},
  {"powerpc64", 86, "R_PPC64_GOT_TLSLD16_HA", "GOT_TLSLD16_HA relocation (powerpc64)."},
  {"powerpc64", 87, "R_PPC64_GOT_TPREL16_DS", "GOT_TPREL16_DS relocation (powerpc64)."},
  {"powerpc64", 88, "R_PPC64_GOT_TPREL16_LO_DS", "GOT_TPREL16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 89, "R_PPC64_GOT_TPREL16_HI", "GOT_TPREL16_HI relocation (powerpc64)."},
  {"powerpc64", 90, "R_PPC64_GOT_TPREL16_HA", "GOT_TPREL16_HA relocation (powerpc64)."},
  {"powerpc64", 91, "R_PPC64_GOT_DTPREL16_DS", "GOT_DTPREL16_DS relocation (powerpc64)."},
  {"powerpc64", 92, "R_PPC64_GOT_DTPREL16_LO_DS", "GOT_DTPREL16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 93, "R_PPC64_GOT_DTPREL16_HI", "GOT_DTPREL16_HI relocation (powerpc64)."},
  {"powerpc64", 94, "R_PPC64_GOT_DTPREL16_HA", "GOT_DTPREL16_HA relocation (powerpc64)."},
  {"powerpc64", 95, "R_PPC64_TPREL16_DS", "TPREL16_DS relocation (powerpc64)."},
  {"powerpc64", 96, "R_PPC64_TPREL16_LO_DS", "TPREL16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 97, "R_PPC64_TPREL16_HIGHER", "TPREL16_HIGHER relocation (powerpc64)."},
  {"powerpc64", 98, "R_PPC64_TPREL16_HIGHERA", "TPREL16_HIGHERA relocation (powerpc64)."},
  {"powerpc64", 99, "R_PPC64_TPREL16_HIGHEST", "TPREL16_HIGHEST relocation (powerpc64)."},
  {"powerpc64", 100, "R_PPC64_TPREL16_HIGHESTA", "TPREL16_HIGHESTA relocation (powerpc64)."},
  {"powerpc64", 101, "R_PPC64_DTPREL16_DS", "DTPREL16_DS relocation (powerpc64)."},
  {"powerpc64", 102, "R_PPC64_DTPREL16_LO_DS", "DTPREL16_LO_DS relocation (powerpc64)."},
  {"powerpc64", 103, "R_PPC64_DTPREL16_HIGHER", "DTPREL16_HIGHER relocation (powerpc64)."},
  {"powerpc64", 104, "R_PPC64_DTPREL16_HIGHERA", "DTPREL16_HIGHERA relocation (powerpc64)."},
  {"powerpc64", 105, "R_PPC64_DTPREL16_HIGHEST", "DTPREL16_HIGHEST relocation (powerpc64)."},
  {"powerpc64", 106, "R_PPC64_DTPREL16_HIGHESTA", "DTPREL16_HIGHESTA relocation (powerpc64)."},
  {"powerpc64", 107, "R_PPC64_TLSGD", "TLSGD relocation (powerpc64)."},
  {"powerpc64", 108, "R_PPC64_TLSLD", "TLSLD relocation (powerpc64)."},
  {"powerpc64", 110, "R_PPC64_ADDR16_HIGH", "ADDR16_HIGH relocation (powerpc64)."},
  {"powerpc64", 111, "R_PPC64_ADDR16_HIGHA", "ADDR16_HIGHA relocation (powerpc64)."},
  {"powerpc64", 112, "R_PPC64_TPREL16_HIGH", "TPREL16_HIGH relocation (powerpc64)."},
  {"powerpc64", 113, "R_PPC64_TPREL16_HIGHA", "TPREL16_HIGHA relocation (powerpc64)."},
  {"powerpc64", 114, "R_PPC64_DTPREL16_HIGH", "DTPREL16_HIGH relocation (powerpc64)."},
  {"powerpc64", 115, "R_PPC64_DTPREL16_HIGHA", "DTPREL16_HIGHA relocation (powerpc64)."},
  {"powerpc64", 116, "R_PPC64_REL24_NOTOC", "REL24_NOTOC relocation (powerpc64)."},
  {"powerpc64", 123, "R_PPC64_PCREL_OPT", "PCREL_OPT relocation (powerpc64)."},
  {"powerpc64", 132, "R_PPC64_PCREL34", "PCREL34 relocation (powerpc64)."},
  {"powerpc64", 133, "R_PPC64_GOT_PCREL34", "GOT_PCREL34 relocation (powerpc64)."},
  {"powerpc64", 146, "R_PPC64_TPREL34", "TPREL34 relocation (powerpc64)."},
  {"powerpc64", 147, "R_PPC64_DTPREL34", "DTPREL34 relocation (powerpc64)."},
  {"powerpc64", 148, "R_PPC64_GOT_TLSGD_PCREL34", "GOT_TLSGD_PCREL34 relocation (powerpc64)."},
  {"powerpc64", 149, "R_PPC64_GOT_TLSLD_PCREL34", "GOT_TLSLD_PCREL34 relocation (powerpc64)."},
  {"powerpc64", 150, "R_PPC64_GOT_TPREL_PCREL34", "GOT_TPREL_PCREL34 relocation (powerpc64)."},
  {"powerpc64", 248, "R_PPC64_IRELATIVE", "IRELATIVE relocation (powerpc64)."},
  {"powerpc64", 249, "R_PPC64_REL16", "REL16 relocation (powerpc64)."},
  {"powerpc64", 250, "R_PPC64_REL16_LO", "REL16_LO relocation (powerpc64)."},
  {"powerpc64", 251, "R_PPC64_REL16_HI", "REL16_HI relocation (powerpc64)."},
  {"powerpc64", 252, "R_PPC64_REL16_HA", "REL16_HA relocation (powerpc64)."},
  {"mips", 0, "R_MIPS_NONE", "NONE relocation (mips)."},
  {"mips", 1, "R_MIPS_16", "16 relocation (mips)."},
  {"mips", 2, "R_MIPS_32", "32 relocation (mips)."},
  {"mips", 3, "R_MIPS_REL32", "REL32 relocation (mips)."},
  {"mips", 4, "R_MIPS_26", "26 relocation (mips)."},
  {"mips", 5, "R_MIPS_HI16", "HI16 relocation (mips)."},
  {"mips", 6, "R_MIPS_LO16", "LO16 relocation (mips)."},
  {"mips", 7, "R_MIPS_GPREL16", "GPREL16 relocation (mips)."},
  {"mips", 8, "R_MIPS_LITERAL", "LITERAL relocation (mips)."},
  {"mips", 9, "R_MIPS_GOT16", "GOT16 relocation (mips)."},
  {"mips", 10, "R_MIPS_PC16", "PC16 relocation (mips)."},
  {"mips", 11, "R_MIPS_CALL16", "CALL16 relocation (mips)."},
  {"mips", 12, "R_MIPS_GPREL32", "GPREL32 relocation (mips)."},
  {"mips", 13, "R_MIPS_UNUSED1", "UNUSED1 relocation (mips)."},
  {"mips", 14, "R_MIPS_UNUSED2", "UNUSED2 relocation (mips)."},
  {"mips", 15, "R_MIPS_UNUSED3", "UNUSED3 relocation (mips)."},
  {"mips", 16, "R_MIPS_SHIFT5", "SHIFT5 relocation (mips)."},
  {"mips", 17, "R_MIPS_SHIFT6", "SHIFT6 relocation (mips)."},
  {"mips", 18, "R_MIPS_64", "64 relocation (mips)."},
  {"mips", 19, "R_MIPS_GOT_DISP", "GOT_DISP relocation (mips)."},
  {"mips", 20, "R_MIPS_GOT_PAGE", "GOT_PAGE relocation (mips)."},
  {"mips", 21, "R_MIPS_GOT_OFST", "GOT_OFST relocation (mips)."},
  {"mips", 22, "R_MIPS_GOT_HI16", "GOT_HI16 relocation (mips)."},
  {"mips", 23, "R_MIPS_GOT_LO16", "GOT_LO16 relocation (mips)."},
  {"mips", 24, "R_MIPS_SUB", "SUB relocation (mips)."},
  {"mips", 25, "R_MIPS_INSERT_A", "INSERT_A relocation (mips)."},
  {"mips", 26, "R_MIPS_INSERT_B", "INSERT_B relocation (mips)."},
  {"mips", 27, "R_MIPS_DELETE", "DELETE relocation (mips)."},
  {"mips", 28, "R_MIPS_HIGHER", "HIGHER relocation (mips)."},
  {"mips", 29, "R_MIPS_HIGHEST", "HIGHEST relocation (mips)."},
  {"mips", 30, "R_MIPS_CALL_HI16", "CALL_HI16 relocation (mips)."},
  {"mips", 31, "R_MIPS_CALL_LO16", "CALL_LO16 relocation (mips)."},
  {"mips", 32, "R_MIPS_SCN_DISP", "SCN_DISP relocation (mips)."},
  {"mips", 33, "R_MIPS_REL16", "REL16 relocation (mips)."},
  {"mips", 34, "R_MIPS_ADD_IMMEDIATE", "ADD_IMMEDIATE relocation (mips)."},
  {"mips", 35, "R_MIPS_PJUMP", "PJUMP relocation (mips)."},
  {"mips", 36, "R_MIPS_RELGOT", "RELGOT relocation (mips)."},
  {"mips", 37, "R_MIPS_JALR", "JALR relocation (mips)."},
  {"mips", 38, "R_MIPS_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (mips)."},
  {"mips", 39, "R_MIPS_TLS_DTPREL32", "TLS_DTPREL32 relocation (mips)."},
  {"mips", 40, "R_MIPS_TLS_DTPMOD64", "TLS_DTPMOD64 relocation (mips)."},
  {"mips", 41, "R_MIPS_TLS_DTPREL64", "TLS_DTPREL64 relocation (mips)."},
  {"mips", 42, "R_MIPS_TLS_GD", "TLS_GD relocation (mips)."},
  {"mips", 43, "R_MIPS_TLS_LDM", "TLS_LDM relocation (mips)."},
  {"mips", 44, "R_MIPS_TLS_DTPREL_HI16", "TLS_DTPREL_HI16 relocation (mips)."},
  {"mips", 45, "R_MIPS_TLS_DTPREL_LO16", "TLS_DTPREL_LO16 relocation (mips)."},
  {"mips", 46, "R_MIPS_TLS_GOTTPREL", "TLS_GOTTPREL relocation (mips)."},
  {"mips", 47, "R_MIPS_TLS_TPREL32", "TLS_TPREL32 relocation (mips)."},
  {"mips", 48, "R_MIPS_TLS_TPREL64", "TLS_TPREL64 relocation (mips)."},
  {"mips", 49, "R_MIPS_TLS_TPREL_HI16", "TLS_TPREL_HI16 relocation (mips)."},
  {"mips", 50, "R_MIPS_TLS_TPREL_LO16", "TLS_TPREL_LO16 relocation (mips)."},
  {"mips", 51, "R_MIPS_GLOB_DAT", "GLOB_DAT relocation (mips)."},
  {"mips", 60, "R_MIPS_PC21_S2", "PC21_S2 relocation (mips)."},
  {"mips", 61, "R_MIPS_PC26_S2", "PC26_S2 relocation (mips)."},
  {"mips", 62, "R_MIPS_PC18_S3", "PC18_S3 relocation (mips)."},
  {"mips", 63, "R_MIPS_PC19_S2", "PC19_S2 relocation (mips)."},
  {"mips", 64, "R_MIPS_PCHI16", "PCHI16 relocation (mips)."},
  {"mips", 65, "R_MIPS_PCLO16", "PCLO16 relocation (mips)."},
  {"mips", 100, "R_MIPS16_26", "26 relocation (mips)."},
  {"mips", 101, "R_MIPS16_GPREL", "GPREL relocation (mips)."},
  {"mips", 102, "R_MIPS16_GOT16", "GOT16 relocation (mips)."},
  {"mips", 103, "R_MIPS16_CALL16", "CALL16 relocation (mips)."},
  {"mips", 104, "R_MIPS16_HI16", "HI16 relocation (mips)."},
  {"mips", 105, "R_MIPS16_LO16", "LO16 relocation (mips)."},
  {"mips", 106, "R_MIPS16_TLS_GD", "TLS_GD relocation (mips)."},
  {"mips", 107, "R_MIPS16_TLS_LDM", "TLS_LDM relocation (mips)."},
  {"mips", 108, "R_MIPS16_TLS_DTPREL_HI16", "TLS_DTPREL_HI16 relocation (mips)."},
  {"mips", 109, "R_MIPS16_TLS_DTPREL_LO16", "TLS_DTPREL_LO16 relocation (mips)."},
  {"mips", 110, "R_MIPS16_TLS_GOTTPREL", "TLS_GOTTPREL relocation (mips)."},
  {"mips", 111, "R_MIPS16_TLS_TPREL_HI16", "TLS_TPREL_HI16 relocation (mips)."},
  {"mips", 112, "R_MIPS16_TLS_TPREL_LO16", "TLS_TPREL_LO16 relocation (mips)."},
  {"mips", 126, "R_MIPS_COPY", "COPY relocation (mips)."},
  {"mips", 127, "R_MIPS_JUMP_SLOT", "JUMP_SLOT relocation (mips)."},
  {"mips", 133, "R_MICROMIPS_26_S1", "26_S1 relocation (mips)."},
  {"mips", 134, "R_MICROMIPS_HI16", "HI16 relocation (mips)."},
  {"mips", 135, "R_MICROMIPS_LO16", "LO16 relocation (mips)."},
  {"mips", 136, "R_MICROMIPS_GPREL16", "GPREL16 relocation (mips)."},
  {"mips", 137, "R_MICROMIPS_LITERAL", "LITERAL relocation (mips)."},
  {"mips", 138, "R_MICROMIPS_GOT16", "GOT16 relocation (mips)."},
  {"mips", 139, "R_MICROMIPS_PC7_S1", "PC7_S1 relocation (mips)."},
  {"mips", 140, "R_MICROMIPS_PC10_S1", "PC10_S1 relocation (mips)."},
  {"mips", 141, "R_MICROMIPS_PC16_S1", "PC16_S1 relocation (mips)."},
  {"mips", 142, "R_MICROMIPS_CALL16", "CALL16 relocation (mips)."},
  {"mips", 145, "R_MICROMIPS_GOT_DISP", "GOT_DISP relocation (mips)."},
  {"mips", 146, "R_MICROMIPS_GOT_PAGE", "GOT_PAGE relocation (mips)."},
  {"mips", 147, "R_MICROMIPS_GOT_OFST", "GOT_OFST relocation (mips)."},
  {"mips", 148, "R_MICROMIPS_GOT_HI16", "GOT_HI16 relocation (mips)."},
  {"mips", 149, "R_MICROMIPS_GOT_LO16", "GOT_LO16 relocation (mips)."},
  {"mips", 150, "R_MICROMIPS_SUB", "SUB relocation (mips)."},
  {"mips", 151, "R_MICROMIPS_HIGHER", "HIGHER relocation (mips)."},
  {"mips", 152, "R_MICROMIPS_HIGHEST", "HIGHEST relocation (mips)."},
  {"mips", 153, "R_MICROMIPS_CALL_HI16", "CALL_HI16 relocation (mips)."},
  {"mips", 154, "R_MICROMIPS_CALL_LO16", "CALL_LO16 relocation (mips)."},
  {"mips", 155, "R_MICROMIPS_SCN_DISP", "SCN_DISP relocation (mips)."},
  {"mips", 156, "R_MICROMIPS_JALR", "JALR relocation (mips)."},
  {"mips", 157, "R_MICROMIPS_HI0_LO16", "HI0_LO16 relocation (mips)."},
  {"mips", 162, "R_MICROMIPS_TLS_GD", "TLS_GD relocation (mips)."},
  {"mips", 163, "R_MICROMIPS_TLS_LDM", "TLS_LDM relocation (mips)."},
  {"mips", 164, "R_MICROMIPS_TLS_DTPREL_HI16", "TLS_DTPREL_HI16 relocation (mips)."},
  {"mips", 165, "R_MICROMIPS_TLS_DTPREL_LO16", "TLS_DTPREL_LO16 relocation (mips)."},
  {"mips", 166, "R_MICROMIPS_TLS_GOTTPREL", "TLS_GOTTPREL relocation (mips)."},
  {"mips", 169, "R_MICROMIPS_TLS_TPREL_HI16", "TLS_TPREL_HI16 relocation (mips)."},
  {"mips", 170, "R_MICROMIPS_TLS_TPREL_LO16", "TLS_TPREL_LO16 relocation (mips)."},
  {"mips", 172, "R_MICROMIPS_GPREL7_S2", "GPREL7_S2 relocation (mips)."},
  {"mips", 173, "R_MICROMIPS_PC23_S2", "PC23_S2 relocation (mips)."},
  {"mips", 174, "R_MICROMIPS_PC21_S1", "PC21_S1 relocation (mips)."},
  {"mips", 175, "R_MICROMIPS_PC26_S1", "PC26_S1 relocation (mips)."},
  {"mips", 176, "R_MICROMIPS_PC18_S3", "PC18_S3 relocation (mips)."},
  {"mips", 177, "R_MICROMIPS_PC19_S2", "PC19_S2 relocation (mips)."},
  {"mips", 218, "R_MIPS_NUM", "NUM relocation (mips)."},
  {"mips", 248, "R_MIPS_PC32", "PC32 relocation (mips)."},
  {"mips", 249, "R_MIPS_EH", "EH relocation (mips)."},
  {"loongarch", 0, "R_LARCH_NONE", "NONE relocation (loongarch)."},
  {"loongarch", 1, "R_LARCH_32", "32 relocation (loongarch)."},
  {"loongarch", 2, "R_LARCH_64", "64 relocation (loongarch)."},
  {"loongarch", 3, "R_LARCH_RELATIVE", "RELATIVE relocation (loongarch)."},
  {"loongarch", 4, "R_LARCH_COPY", "COPY relocation (loongarch)."},
  {"loongarch", 5, "R_LARCH_JUMP_SLOT", "JUMP_SLOT relocation (loongarch)."},
  {"loongarch", 6, "R_LARCH_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (loongarch)."},
  {"loongarch", 7, "R_LARCH_TLS_DTPMOD64", "TLS_DTPMOD64 relocation (loongarch)."},
  {"loongarch", 8, "R_LARCH_TLS_DTPREL32", "TLS_DTPREL32 relocation (loongarch)."},
  {"loongarch", 9, "R_LARCH_TLS_DTPREL64", "TLS_DTPREL64 relocation (loongarch)."},
  {"loongarch", 10, "R_LARCH_TLS_TPREL32", "TLS_TPREL32 relocation (loongarch)."},
  {"loongarch", 11, "R_LARCH_TLS_TPREL64", "TLS_TPREL64 relocation (loongarch)."},
  {"loongarch", 12, "R_LARCH_IRELATIVE", "IRELATIVE relocation (loongarch)."},
  {"loongarch", 20, "R_LARCH_MARK_LA", "MARK_LA relocation (loongarch)."},
  {"loongarch", 21, "R_LARCH_MARK_PCREL", "MARK_PCREL relocation (loongarch)."},
  {"loongarch", 22, "R_LARCH_SOP_PUSH_PCREL", "SOP_PUSH_PCREL relocation (loongarch)."},
  {"loongarch", 23, "R_LARCH_SOP_PUSH_ABSOLUTE", "SOP_PUSH_ABSOLUTE relocation (loongarch)."},
  {"loongarch", 24, "R_LARCH_SOP_PUSH_DUP", "SOP_PUSH_DUP relocation (loongarch)."},
  {"loongarch", 25, "R_LARCH_SOP_PUSH_GPREL", "SOP_PUSH_GPREL relocation (loongarch)."},
  {"loongarch", 26, "R_LARCH_SOP_PUSH_TLS_TPREL", "SOP_PUSH_TLS_TPREL relocation (loongarch)."},
  {"loongarch", 27, "R_LARCH_SOP_PUSH_TLS_GOT", "SOP_PUSH_TLS_GOT relocation (loongarch)."},
  {"loongarch", 28, "R_LARCH_SOP_PUSH_TLS_GD", "SOP_PUSH_TLS_GD relocation (loongarch)."},
  {"loongarch", 29, "R_LARCH_SOP_PUSH_PLT_PCREL", "SOP_PUSH_PLT_PCREL relocation (loongarch)."},
  {"loongarch", 30, "R_LARCH_SOP_ASSERT", "SOP_ASSERT relocation (loongarch)."},
  {"loongarch", 31, "R_LARCH_SOP_NOT", "SOP_NOT relocation (loongarch)."},
  {"loongarch", 32, "R_LARCH_SOP_SUB", "SOP_SUB relocation (loongarch)."},
  {"loongarch", 33, "R_LARCH_SOP_SL", "SOP_SL relocation (loongarch)."},
  {"loongarch", 34, "R_LARCH_SOP_SR", "SOP_SR relocation (loongarch)."},
  {"loongarch", 35, "R_LARCH_SOP_ADD", "SOP_ADD relocation (loongarch)."},
  {"loongarch", 36, "R_LARCH_SOP_AND", "SOP_AND relocation (loongarch)."},
  {"loongarch", 37, "R_LARCH_SOP_IF_ELSE", "SOP_IF_ELSE relocation (loongarch)."},
  {"loongarch", 38, "R_LARCH_SOP_POP_32_S_10_5", "SOP_POP_32_S_10_5 relocation (loongarch)."},
  {"loongarch", 39, "R_LARCH_SOP_POP_32_U_10_12", "SOP_POP_32_U_10_12 relocation (loongarch)."},
  {"loongarch", 40, "R_LARCH_SOP_POP_32_S_10_12", "SOP_POP_32_S_10_12 relocation (loongarch)."},
  {"loongarch", 41, "R_LARCH_SOP_POP_32_S_10_16", "SOP_POP_32_S_10_16 relocation (loongarch)."},
  {"loongarch", 42, "R_LARCH_SOP_POP_32_S_10_16_S2", "SOP_POP_32_S_10_16_S2 relocation (loongarch)."},
  {"loongarch", 43, "R_LARCH_SOP_POP_32_S_5_20", "SOP_POP_32_S_5_20 relocation (loongarch)."},
  {"loongarch", 44, "R_LARCH_SOP_POP_32_S_0_5_10_16_S2", "SOP_POP_32_S_0_5_10_16_S2 relocation (loongarch)."},
  {"loongarch", 45, "R_LARCH_SOP_POP_32_S_0_10_10_16_S2", "SOP_POP_32_S_0_10_10_16_S2 relocation (loongarch)."},
  {"loongarch", 46, "R_LARCH_SOP_POP_32_U", "SOP_POP_32_U relocation (loongarch)."},
  {"loongarch", 47, "R_LARCH_ADD8", "ADD8 relocation (loongarch)."},
  {"loongarch", 48, "R_LARCH_ADD16", "ADD16 relocation (loongarch)."},
  {"loongarch", 49, "R_LARCH_ADD24", "ADD24 relocation (loongarch)."},
  {"loongarch", 50, "R_LARCH_ADD32", "ADD32 relocation (loongarch)."},
  {"loongarch", 51, "R_LARCH_ADD64", "ADD64 relocation (loongarch)."},
  {"loongarch", 52, "R_LARCH_SUB8", "SUB8 relocation (loongarch)."},
  {"loongarch", 53, "R_LARCH_SUB16", "SUB16 relocation (loongarch)."},
  {"loongarch", 54, "R_LARCH_SUB24", "SUB24 relocation (loongarch)."},
  {"loongarch", 55, "R_LARCH_SUB32", "SUB32 relocation (loongarch)."},
  {"loongarch", 56, "R_LARCH_SUB64", "SUB64 relocation (loongarch)."},
  {"loongarch", 57, "R_LARCH_GNU_VTINHERIT", "GNU_VTINHERIT relocation (loongarch)."},
  {"loongarch", 58, "R_LARCH_GNU_VTENTRY", "GNU_VTENTRY relocation (loongarch)."},
  {"loongarch", 64, "R_LARCH_B16", "B16 relocation (loongarch)."},
  {"loongarch", 65, "R_LARCH_B21", "B21 relocation (loongarch)."},
  {"loongarch", 66, "R_LARCH_B26", "B26 relocation (loongarch)."},
  {"loongarch", 67, "R_LARCH_ABS_HI20", "ABS_HI20 relocation (loongarch)."},
  {"loongarch", 68, "R_LARCH_ABS_LO12", "ABS_LO12 relocation (loongarch)."},
  {"loongarch", 69, "R_LARCH_ABS64_LO20", "ABS64_LO20 relocation (loongarch)."},
  {"loongarch", 70, "R_LARCH_ABS64_HI12", "ABS64_HI12 relocation (loongarch)."},
  {"loongarch", 71, "R_LARCH_PCALA_HI20", "PCALA_HI20 relocation (loongarch)."},
  {"loongarch", 72, "R_LARCH_PCALA_LO12", "PCALA_LO12 relocation (loongarch)."},
  {"loongarch", 73, "R_LARCH_PCALA64_LO20", "PCALA64_LO20 relocation (loongarch)."},
  {"loongarch", 74, "R_LARCH_PCALA64_HI12", "PCALA64_HI12 relocation (loongarch)."},
  {"loongarch", 75, "R_LARCH_GOT_PC_HI20", "GOT_PC_HI20 relocation (loongarch)."},
  {"loongarch", 76, "R_LARCH_GOT_PC_LO12", "GOT_PC_LO12 relocation (loongarch)."},
  {"loongarch", 77, "R_LARCH_GOT64_PC_LO20", "GOT64_PC_LO20 relocation (loongarch)."},
  {"loongarch", 78, "R_LARCH_GOT64_PC_HI12", "GOT64_PC_HI12 relocation (loongarch)."},
  {"loongarch", 79, "R_LARCH_GOT_HI20", "GOT_HI20 relocation (loongarch)."},
  {"loongarch", 80, "R_LARCH_GOT_LO12", "GOT_LO12 relocation (loongarch)."},
  {"loongarch", 81, "R_LARCH_GOT64_LO20", "GOT64_LO20 relocation (loongarch)."},
  {"loongarch", 82, "R_LARCH_GOT64_HI12", "GOT64_HI12 relocation (loongarch)."},
  {"loongarch", 83, "R_LARCH_TLS_LE_HI20", "TLS_LE_HI20 relocation (loongarch)."},
  {"loongarch", 84, "R_LARCH_TLS_LE_LO12", "TLS_LE_LO12 relocation (loongarch)."},
  {"loongarch", 85, "R_LARCH_TLS_LE64_LO20", "TLS_LE64_LO20 relocation (loongarch)."},
  {"loongarch", 86, "R_LARCH_TLS_LE64_HI12", "TLS_LE64_HI12 relocation (loongarch)."},
  {"loongarch", 87, "R_LARCH_TLS_IE_PC_HI20", "TLS_IE_PC_HI20 relocation (loongarch)."},
  {"loongarch", 88, "R_LARCH_TLS_IE_PC_LO12", "TLS_IE_PC_LO12 relocation (loongarch)."},
  {"loongarch", 89, "R_LARCH_TLS_IE64_PC_LO20", "TLS_IE64_PC_LO20 relocation (loongarch)."},
  {"loongarch", 90, "R_LARCH_TLS_IE64_PC_HI12", "TLS_IE64_PC_HI12 relocation (loongarch)."},
  {"loongarch", 91, "R_LARCH_TLS_IE_HI20", "TLS_IE_HI20 relocation (loongarch)."},
  {"loongarch", 92, "R_LARCH_TLS_IE_LO12", "TLS_IE_LO12 relocation (loongarch)."},
  {"loongarch", 93, "R_LARCH_TLS_IE64_LO20", "TLS_IE64_LO20 relocation (loongarch)."},
  {"loongarch", 94, "R_LARCH_TLS_IE64_HI12", "TLS_IE64_HI12 relocation (loongarch)."},
  {"loongarch", 95, "R_LARCH_TLS_LD_PC_HI20", "TLS_LD_PC_HI20 relocation (loongarch)."},
  {"loongarch", 96, "R_LARCH_TLS_LD_HI20", "TLS_LD_HI20 relocation (loongarch)."},
  {"loongarch", 97, "R_LARCH_TLS_GD_PC_HI20", "TLS_GD_PC_HI20 relocation (loongarch)."},
  {"loongarch", 98, "R_LARCH_TLS_GD_HI20", "TLS_GD_HI20 relocation (loongarch)."},
  {"loongarch", 99, "R_LARCH_32_PCREL", "32_PCREL relocation (loongarch)."},
  {"loongarch", 100, "R_LARCH_RELAX", "RELAX relocation (loongarch)."},
  {"loongarch", 102, "R_LARCH_ALIGN", "ALIGN relocation (loongarch)."},
  {"loongarch", 103, "R_LARCH_PCREL20_S2", "PCREL20_S2 relocation (loongarch)."},
  {"loongarch", 105, "R_LARCH_ADD6", "ADD6 relocation (loongarch)."},
  {"loongarch", 106, "R_LARCH_SUB6", "SUB6 relocation (loongarch)."},
  {"loongarch", 107, "R_LARCH_ADD_ULEB128", "ADD_ULEB128 relocation (loongarch)."},
  {"loongarch", 108, "R_LARCH_SUB_ULEB128", "SUB_ULEB128 relocation (loongarch)."},
  {"loongarch", 109, "R_LARCH_64_PCREL", "64_PCREL relocation (loongarch)."},
  {"loongarch", 110, "R_LARCH_CALL36", "CALL36 relocation (loongarch)."},
  {"loongarch", 13, "R_LARCH_TLS_DESC32", "TLS_DESC32 relocation (loongarch)."},
  {"loongarch", 14, "R_LARCH_TLS_DESC64", "TLS_DESC64 relocation (loongarch)."},
  {"loongarch", 111, "R_LARCH_TLS_DESC_PC_HI20", "TLS_DESC_PC_HI20 relocation (loongarch)."},
  {"loongarch", 112, "R_LARCH_TLS_DESC_PC_LO12", "TLS_DESC_PC_LO12 relocation (loongarch)."},
  {"loongarch", 113, "R_LARCH_TLS_DESC64_PC_LO20", "TLS_DESC64_PC_LO20 relocation (loongarch)."},
  {"loongarch", 114, "R_LARCH_TLS_DESC64_PC_HI12", "TLS_DESC64_PC_HI12 relocation (loongarch)."},
  {"loongarch", 115, "R_LARCH_TLS_DESC_HI20", "TLS_DESC_HI20 relocation (loongarch)."},
  {"loongarch", 116, "R_LARCH_TLS_DESC_LO12", "TLS_DESC_LO12 relocation (loongarch)."},
  {"loongarch", 117, "R_LARCH_TLS_DESC64_LO20", "TLS_DESC64_LO20 relocation (loongarch)."},
  {"loongarch", 118, "R_LARCH_TLS_DESC64_HI12", "TLS_DESC64_HI12 relocation (loongarch)."},
  {"loongarch", 119, "R_LARCH_TLS_DESC_LD", "TLS_DESC_LD relocation (loongarch)."},
  {"loongarch", 120, "R_LARCH_TLS_DESC_CALL", "TLS_DESC_CALL relocation (loongarch)."},
  {"loongarch", 121, "R_LARCH_TLS_LE_HI20_R", "TLS_LE_HI20_R relocation (loongarch)."},
  {"loongarch", 122, "R_LARCH_TLS_LE_ADD_R", "TLS_LE_ADD_R relocation (loongarch)."},
  {"loongarch", 123, "R_LARCH_TLS_LE_LO12_R", "TLS_LE_LO12_R relocation (loongarch)."},
  {"loongarch", 124, "R_LARCH_TLS_LD_PCREL20_S2", "TLS_LD_PCREL20_S2 relocation (loongarch)."},
  {"loongarch", 125, "R_LARCH_TLS_GD_PCREL20_S2", "TLS_GD_PCREL20_S2 relocation (loongarch)."},
  {"loongarch", 126, "R_LARCH_TLS_DESC_PCREL20_S2", "TLS_DESC_PCREL20_S2 relocation (loongarch)."},
  {"hexagon", 0, "R_HEX_NONE", "NONE relocation (hexagon)."},
  {"hexagon", 1, "R_HEX_B22_PCREL", "B22_PCREL relocation (hexagon)."},
  {"hexagon", 2, "R_HEX_B15_PCREL", "B15_PCREL relocation (hexagon)."},
  {"hexagon", 3, "R_HEX_B7_PCREL", "B7_PCREL relocation (hexagon)."},
  {"hexagon", 4, "R_HEX_LO16", "LO16 relocation (hexagon)."},
  {"hexagon", 5, "R_HEX_HI16", "HI16 relocation (hexagon)."},
  {"hexagon", 6, "R_HEX_32", "32 relocation (hexagon)."},
  {"hexagon", 7, "R_HEX_16", "16 relocation (hexagon)."},
  {"hexagon", 8, "R_HEX_8", "8 relocation (hexagon)."},
  {"hexagon", 9, "R_HEX_GPREL16_0", "GPREL16_0 relocation (hexagon)."},
  {"hexagon", 10, "R_HEX_GPREL16_1", "GPREL16_1 relocation (hexagon)."},
  {"hexagon", 11, "R_HEX_GPREL16_2", "GPREL16_2 relocation (hexagon)."},
  {"hexagon", 12, "R_HEX_GPREL16_3", "GPREL16_3 relocation (hexagon)."},
  {"hexagon", 13, "R_HEX_HL16", "HL16 relocation (hexagon)."},
  {"hexagon", 14, "R_HEX_B13_PCREL", "B13_PCREL relocation (hexagon)."},
  {"hexagon", 15, "R_HEX_B9_PCREL", "B9_PCREL relocation (hexagon)."},
  {"hexagon", 16, "R_HEX_B32_PCREL_X", "B32_PCREL_X relocation (hexagon)."},
  {"hexagon", 17, "R_HEX_32_6_X", "32_6_X relocation (hexagon)."},
  {"hexagon", 18, "R_HEX_B22_PCREL_X", "B22_PCREL_X relocation (hexagon)."},
  {"hexagon", 19, "R_HEX_B15_PCREL_X", "B15_PCREL_X relocation (hexagon)."},
  {"hexagon", 20, "R_HEX_B13_PCREL_X", "B13_PCREL_X relocation (hexagon)."},
  {"hexagon", 21, "R_HEX_B9_PCREL_X", "B9_PCREL_X relocation (hexagon)."},
  {"hexagon", 22, "R_HEX_B7_PCREL_X", "B7_PCREL_X relocation (hexagon)."},
  {"hexagon", 23, "R_HEX_16_X", "16_X relocation (hexagon)."},
  {"hexagon", 24, "R_HEX_12_X", "12_X relocation (hexagon)."},
  {"hexagon", 25, "R_HEX_11_X", "11_X relocation (hexagon)."},
  {"hexagon", 26, "R_HEX_10_X", "10_X relocation (hexagon)."},
  {"hexagon", 27, "R_HEX_9_X", "9_X relocation (hexagon)."},
  {"hexagon", 28, "R_HEX_8_X", "8_X relocation (hexagon)."},
  {"hexagon", 29, "R_HEX_7_X", "7_X relocation (hexagon)."},
  {"hexagon", 30, "R_HEX_6_X", "6_X relocation (hexagon)."},
  {"hexagon", 31, "R_HEX_32_PCREL", "32_PCREL relocation (hexagon)."},
  {"hexagon", 32, "R_HEX_COPY", "COPY relocation (hexagon)."},
  {"hexagon", 33, "R_HEX_GLOB_DAT", "GLOB_DAT relocation (hexagon)."},
  {"hexagon", 34, "R_HEX_JMP_SLOT", "JMP_SLOT relocation (hexagon)."},
  {"hexagon", 35, "R_HEX_RELATIVE", "RELATIVE relocation (hexagon)."},
  {"hexagon", 36, "R_HEX_PLT_B22_PCREL", "PLT_B22_PCREL relocation (hexagon)."},
  {"hexagon", 37, "R_HEX_GOTREL_LO16", "GOTREL_LO16 relocation (hexagon)."},
  {"hexagon", 38, "R_HEX_GOTREL_HI16", "GOTREL_HI16 relocation (hexagon)."},
  {"hexagon", 39, "R_HEX_GOTREL_32", "GOTREL_32 relocation (hexagon)."},
  {"hexagon", 40, "R_HEX_GOT_LO16", "GOT_LO16 relocation (hexagon)."},
  {"hexagon", 41, "R_HEX_GOT_HI16", "GOT_HI16 relocation (hexagon)."},
  {"hexagon", 42, "R_HEX_GOT_32", "GOT_32 relocation (hexagon)."},
  {"hexagon", 43, "R_HEX_GOT_16", "GOT_16 relocation (hexagon)."},
  {"hexagon", 44, "R_HEX_DTPMOD_32", "DTPMOD_32 relocation (hexagon)."},
  {"hexagon", 45, "R_HEX_DTPREL_LO16", "DTPREL_LO16 relocation (hexagon)."},
  {"hexagon", 46, "R_HEX_DTPREL_HI16", "DTPREL_HI16 relocation (hexagon)."},
  {"hexagon", 47, "R_HEX_DTPREL_32", "DTPREL_32 relocation (hexagon)."},
  {"hexagon", 48, "R_HEX_DTPREL_16", "DTPREL_16 relocation (hexagon)."},
  {"hexagon", 49, "R_HEX_GD_PLT_B22_PCREL", "GD_PLT_B22_PCREL relocation (hexagon)."},
  {"hexagon", 50, "R_HEX_GD_GOT_LO16", "GD_GOT_LO16 relocation (hexagon)."},
  {"hexagon", 51, "R_HEX_GD_GOT_HI16", "GD_GOT_HI16 relocation (hexagon)."},
  {"hexagon", 52, "R_HEX_GD_GOT_32", "GD_GOT_32 relocation (hexagon)."},
  {"hexagon", 53, "R_HEX_GD_GOT_16", "GD_GOT_16 relocation (hexagon)."},
  {"hexagon", 54, "R_HEX_IE_LO16", "IE_LO16 relocation (hexagon)."},
  {"hexagon", 55, "R_HEX_IE_HI16", "IE_HI16 relocation (hexagon)."},
  {"hexagon", 56, "R_HEX_IE_32", "IE_32 relocation (hexagon)."},
  {"hexagon", 57, "R_HEX_IE_GOT_LO16", "IE_GOT_LO16 relocation (hexagon)."},
  {"hexagon", 58, "R_HEX_IE_GOT_HI16", "IE_GOT_HI16 relocation (hexagon)."},
  {"hexagon", 59, "R_HEX_IE_GOT_32", "IE_GOT_32 relocation (hexagon)."},
  {"hexagon", 60, "R_HEX_IE_GOT_16", "IE_GOT_16 relocation (hexagon)."},
  {"hexagon", 61, "R_HEX_TPREL_LO16", "TPREL_LO16 relocation (hexagon)."},
  {"hexagon", 62, "R_HEX_TPREL_HI16", "TPREL_HI16 relocation (hexagon)."},
  {"hexagon", 63, "R_HEX_TPREL_32", "TPREL_32 relocation (hexagon)."},
  {"hexagon", 64, "R_HEX_TPREL_16", "TPREL_16 relocation (hexagon)."},
  {"hexagon", 65, "R_HEX_6_PCREL_X", "6_PCREL_X relocation (hexagon)."},
  {"hexagon", 66, "R_HEX_GOTREL_32_6_X", "GOTREL_32_6_X relocation (hexagon)."},
  {"hexagon", 67, "R_HEX_GOTREL_16_X", "GOTREL_16_X relocation (hexagon)."},
  {"hexagon", 68, "R_HEX_GOTREL_11_X", "GOTREL_11_X relocation (hexagon)."},
  {"hexagon", 69, "R_HEX_GOT_32_6_X", "GOT_32_6_X relocation (hexagon)."},
  {"hexagon", 70, "R_HEX_GOT_16_X", "GOT_16_X relocation (hexagon)."},
  {"hexagon", 71, "R_HEX_GOT_11_X", "GOT_11_X relocation (hexagon)."},
  {"hexagon", 72, "R_HEX_DTPREL_32_6_X", "DTPREL_32_6_X relocation (hexagon)."},
  {"hexagon", 73, "R_HEX_DTPREL_16_X", "DTPREL_16_X relocation (hexagon)."},
  {"hexagon", 74, "R_HEX_DTPREL_11_X", "DTPREL_11_X relocation (hexagon)."},
  {"hexagon", 75, "R_HEX_GD_GOT_32_6_X", "GD_GOT_32_6_X relocation (hexagon)."},
  {"hexagon", 76, "R_HEX_GD_GOT_16_X", "GD_GOT_16_X relocation (hexagon)."},
  {"hexagon", 77, "R_HEX_GD_GOT_11_X", "GD_GOT_11_X relocation (hexagon)."},
  {"hexagon", 78, "R_HEX_IE_32_6_X", "IE_32_6_X relocation (hexagon)."},
  {"hexagon", 79, "R_HEX_IE_16_X", "IE_16_X relocation (hexagon)."},
  {"hexagon", 80, "R_HEX_IE_GOT_32_6_X", "IE_GOT_32_6_X relocation (hexagon)."},
  {"hexagon", 81, "R_HEX_IE_GOT_16_X", "IE_GOT_16_X relocation (hexagon)."},
  {"hexagon", 82, "R_HEX_IE_GOT_11_X", "IE_GOT_11_X relocation (hexagon)."},
  {"hexagon", 83, "R_HEX_TPREL_32_6_X", "TPREL_32_6_X relocation (hexagon)."},
  {"hexagon", 84, "R_HEX_TPREL_16_X", "TPREL_16_X relocation (hexagon)."},
  {"hexagon", 85, "R_HEX_TPREL_11_X", "TPREL_11_X relocation (hexagon)."},
  {"hexagon", 86, "R_HEX_LD_PLT_B22_PCREL", "LD_PLT_B22_PCREL relocation (hexagon)."},
  {"hexagon", 87, "R_HEX_LD_GOT_LO16", "LD_GOT_LO16 relocation (hexagon)."},
  {"hexagon", 88, "R_HEX_LD_GOT_HI16", "LD_GOT_HI16 relocation (hexagon)."},
  {"hexagon", 89, "R_HEX_LD_GOT_32", "LD_GOT_32 relocation (hexagon)."},
  {"hexagon", 90, "R_HEX_LD_GOT_16", "LD_GOT_16 relocation (hexagon)."},
  {"hexagon", 91, "R_HEX_LD_GOT_32_6_X", "LD_GOT_32_6_X relocation (hexagon)."},
  {"hexagon", 92, "R_HEX_LD_GOT_16_X", "LD_GOT_16_X relocation (hexagon)."},
  {"hexagon", 93, "R_HEX_LD_GOT_11_X", "LD_GOT_11_X relocation (hexagon)."},
  {"hexagon", 94, "R_HEX_23_REG", "23_REG relocation (hexagon)."},
  {"hexagon", 95, "R_HEX_GD_PLT_B22_PCREL_X", "GD_PLT_B22_PCREL_X relocation (hexagon)."},
  {"hexagon", 96, "R_HEX_GD_PLT_B32_PCREL_X", "GD_PLT_B32_PCREL_X relocation (hexagon)."},
  {"hexagon", 97, "R_HEX_LD_PLT_B22_PCREL_X", "LD_PLT_B22_PCREL_X relocation (hexagon)."},
  {"hexagon", 98, "R_HEX_LD_PLT_B32_PCREL_X", "LD_PLT_B32_PCREL_X relocation (hexagon)."},
  {"hexagon", 99, "R_HEX_27_REG", "27_REG relocation (hexagon)."},
  {"sparc", 0, "R_SPARC_NONE", "NONE relocation (sparc)."},
  {"sparc", 1, "R_SPARC_8", "8 relocation (sparc)."},
  {"sparc", 2, "R_SPARC_16", "16 relocation (sparc)."},
  {"sparc", 3, "R_SPARC_32", "32 relocation (sparc)."},
  {"sparc", 4, "R_SPARC_DISP8", "DISP8 relocation (sparc)."},
  {"sparc", 5, "R_SPARC_DISP16", "DISP16 relocation (sparc)."},
  {"sparc", 6, "R_SPARC_DISP32", "DISP32 relocation (sparc)."},
  {"sparc", 7, "R_SPARC_WDISP30", "WDISP30 relocation (sparc)."},
  {"sparc", 8, "R_SPARC_WDISP22", "WDISP22 relocation (sparc)."},
  {"sparc", 9, "R_SPARC_HI22", "HI22 relocation (sparc)."},
  {"sparc", 10, "R_SPARC_22", "22 relocation (sparc)."},
  {"sparc", 11, "R_SPARC_13", "13 relocation (sparc)."},
  {"sparc", 12, "R_SPARC_LO10", "LO10 relocation (sparc)."},
  {"sparc", 13, "R_SPARC_GOT10", "GOT10 relocation (sparc)."},
  {"sparc", 14, "R_SPARC_GOT13", "GOT13 relocation (sparc)."},
  {"sparc", 15, "R_SPARC_GOT22", "GOT22 relocation (sparc)."},
  {"sparc", 16, "R_SPARC_PC10", "PC10 relocation (sparc)."},
  {"sparc", 17, "R_SPARC_PC22", "PC22 relocation (sparc)."},
  {"sparc", 18, "R_SPARC_WPLT30", "WPLT30 relocation (sparc)."},
  {"sparc", 19, "R_SPARC_COPY", "COPY relocation (sparc)."},
  {"sparc", 20, "R_SPARC_GLOB_DAT", "GLOB_DAT relocation (sparc)."},
  {"sparc", 21, "R_SPARC_JMP_SLOT", "JMP_SLOT relocation (sparc)."},
  {"sparc", 22, "R_SPARC_RELATIVE", "RELATIVE relocation (sparc)."},
  {"sparc", 23, "R_SPARC_UA32", "UA32 relocation (sparc)."},
  {"sparc", 24, "R_SPARC_PLT32", "PLT32 relocation (sparc)."},
  {"sparc", 25, "R_SPARC_HIPLT22", "HIPLT22 relocation (sparc)."},
  {"sparc", 26, "R_SPARC_LOPLT10", "LOPLT10 relocation (sparc)."},
  {"sparc", 27, "R_SPARC_PCPLT32", "PCPLT32 relocation (sparc)."},
  {"sparc", 28, "R_SPARC_PCPLT22", "PCPLT22 relocation (sparc)."},
  {"sparc", 29, "R_SPARC_PCPLT10", "PCPLT10 relocation (sparc)."},
  {"sparc", 30, "R_SPARC_10", "10 relocation (sparc)."},
  {"sparc", 31, "R_SPARC_11", "11 relocation (sparc)."},
  {"sparc", 32, "R_SPARC_64", "64 relocation (sparc)."},
  {"sparc", 33, "R_SPARC_OLO10", "OLO10 relocation (sparc)."},
  {"sparc", 34, "R_SPARC_HH22", "HH22 relocation (sparc)."},
  {"sparc", 35, "R_SPARC_HM10", "HM10 relocation (sparc)."},
  {"sparc", 36, "R_SPARC_LM22", "LM22 relocation (sparc)."},
  {"sparc", 37, "R_SPARC_PC_HH22", "PC_HH22 relocation (sparc)."},
  {"sparc", 38, "R_SPARC_PC_HM10", "PC_HM10 relocation (sparc)."},
  {"sparc", 39, "R_SPARC_PC_LM22", "PC_LM22 relocation (sparc)."},
  {"sparc", 40, "R_SPARC_WDISP16", "WDISP16 relocation (sparc)."},
  {"sparc", 41, "R_SPARC_WDISP19", "WDISP19 relocation (sparc)."},
  {"sparc", 43, "R_SPARC_7", "7 relocation (sparc)."},
  {"sparc", 44, "R_SPARC_5", "5 relocation (sparc)."},
  {"sparc", 45, "R_SPARC_6", "6 relocation (sparc)."},
  {"sparc", 46, "R_SPARC_DISP64", "DISP64 relocation (sparc)."},
  {"sparc", 47, "R_SPARC_PLT64", "PLT64 relocation (sparc)."},
  {"sparc", 48, "R_SPARC_HIX22", "HIX22 relocation (sparc)."},
  {"sparc", 49, "R_SPARC_LOX10", "LOX10 relocation (sparc)."},
  {"sparc", 50, "R_SPARC_H44", "H44 relocation (sparc)."},
  {"sparc", 51, "R_SPARC_M44", "M44 relocation (sparc)."},
  {"sparc", 52, "R_SPARC_L44", "L44 relocation (sparc)."},
  {"sparc", 53, "R_SPARC_REGISTER", "REGISTER relocation (sparc)."},
  {"sparc", 54, "R_SPARC_UA64", "UA64 relocation (sparc)."},
  {"sparc", 55, "R_SPARC_UA16", "UA16 relocation (sparc)."},
  {"sparc", 56, "R_SPARC_TLS_GD_HI22", "TLS_GD_HI22 relocation (sparc)."},
  {"sparc", 57, "R_SPARC_TLS_GD_LO10", "TLS_GD_LO10 relocation (sparc)."},
  {"sparc", 58, "R_SPARC_TLS_GD_ADD", "TLS_GD_ADD relocation (sparc)."},
  {"sparc", 59, "R_SPARC_TLS_GD_CALL", "TLS_GD_CALL relocation (sparc)."},
  {"sparc", 60, "R_SPARC_TLS_LDM_HI22", "TLS_LDM_HI22 relocation (sparc)."},
  {"sparc", 61, "R_SPARC_TLS_LDM_LO10", "TLS_LDM_LO10 relocation (sparc)."},
  {"sparc", 62, "R_SPARC_TLS_LDM_ADD", "TLS_LDM_ADD relocation (sparc)."},
  {"sparc", 63, "R_SPARC_TLS_LDM_CALL", "TLS_LDM_CALL relocation (sparc)."},
  {"sparc", 64, "R_SPARC_TLS_LDO_HIX22", "TLS_LDO_HIX22 relocation (sparc)."},
  {"sparc", 65, "R_SPARC_TLS_LDO_LOX10", "TLS_LDO_LOX10 relocation (sparc)."},
  {"sparc", 66, "R_SPARC_TLS_LDO_ADD", "TLS_LDO_ADD relocation (sparc)."},
  {"sparc", 67, "R_SPARC_TLS_IE_HI22", "TLS_IE_HI22 relocation (sparc)."},
  {"sparc", 68, "R_SPARC_TLS_IE_LO10", "TLS_IE_LO10 relocation (sparc)."},
  {"sparc", 69, "R_SPARC_TLS_IE_LD", "TLS_IE_LD relocation (sparc)."},
  {"sparc", 70, "R_SPARC_TLS_IE_LDX", "TLS_IE_LDX relocation (sparc)."},
  {"sparc", 71, "R_SPARC_TLS_IE_ADD", "TLS_IE_ADD relocation (sparc)."},
  {"sparc", 72, "R_SPARC_TLS_LE_HIX22", "TLS_LE_HIX22 relocation (sparc)."},
  {"sparc", 73, "R_SPARC_TLS_LE_LOX10", "TLS_LE_LOX10 relocation (sparc)."},
  {"sparc", 74, "R_SPARC_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (sparc)."},
  {"sparc", 75, "R_SPARC_TLS_DTPMOD64", "TLS_DTPMOD64 relocation (sparc)."},
  {"sparc", 76, "R_SPARC_TLS_DTPOFF32", "TLS_DTPOFF32 relocation (sparc)."},
  {"sparc", 77, "R_SPARC_TLS_DTPOFF64", "TLS_DTPOFF64 relocation (sparc)."},
  {"sparc", 78, "R_SPARC_TLS_TPOFF32", "TLS_TPOFF32 relocation (sparc)."},
  {"sparc", 79, "R_SPARC_TLS_TPOFF64", "TLS_TPOFF64 relocation (sparc)."},
  {"sparc", 80, "R_SPARC_GOTDATA_HIX22", "GOTDATA_HIX22 relocation (sparc)."},
  {"sparc", 81, "R_SPARC_GOTDATA_LOX10", "GOTDATA_LOX10 relocation (sparc)."},
  {"sparc", 82, "R_SPARC_GOTDATA_OP_HIX22", "GOTDATA_OP_HIX22 relocation (sparc)."},
  {"sparc", 83, "R_SPARC_GOTDATA_OP_LOX10", "GOTDATA_OP_LOX10 relocation (sparc)."},
  {"sparc", 84, "R_SPARC_GOTDATA_OP", "GOTDATA_OP relocation (sparc)."},
  {"sparc", 85, "R_SPARC_H34", "H34 relocation (sparc)."},
  {"sparc", 86, "R_SPARC_SIZE32", "SIZE32 relocation (sparc)."},
  {"sparc", 87, "R_SPARC_SIZE64", "SIZE64 relocation (sparc)."},
  {"sparc", 88, "R_SPARC_WDISP10", "WDISP10 relocation (sparc)."},
  {"sparc", 249, "R_SPARC_IRELATIVE", "IRELATIVE relocation (sparc)."},
  {"systemz", 0, "R_390_NONE", "NONE relocation (systemz)."},
  {"systemz", 1, "R_390_8", "8 relocation (systemz)."},
  {"systemz", 2, "R_390_12", "12 relocation (systemz)."},
  {"systemz", 3, "R_390_16", "16 relocation (systemz)."},
  {"systemz", 4, "R_390_32", "32 relocation (systemz)."},
  {"systemz", 5, "R_390_PC32", "PC32 relocation (systemz)."},
  {"systemz", 6, "R_390_GOT12", "GOT12 relocation (systemz)."},
  {"systemz", 7, "R_390_GOT32", "GOT32 relocation (systemz)."},
  {"systemz", 8, "R_390_PLT32", "PLT32 relocation (systemz)."},
  {"systemz", 9, "R_390_COPY", "COPY relocation (systemz)."},
  {"systemz", 10, "R_390_GLOB_DAT", "GLOB_DAT relocation (systemz)."},
  {"systemz", 11, "R_390_JMP_SLOT", "JMP_SLOT relocation (systemz)."},
  {"systemz", 12, "R_390_RELATIVE", "RELATIVE relocation (systemz)."},
  {"systemz", 13, "R_390_GOTOFF", "GOTOFF relocation (systemz)."},
  {"systemz", 14, "R_390_GOTPC", "GOTPC relocation (systemz)."},
  {"systemz", 15, "R_390_GOT16", "GOT16 relocation (systemz)."},
  {"systemz", 16, "R_390_PC16", "PC16 relocation (systemz)."},
  {"systemz", 17, "R_390_PC16DBL", "PC16DBL relocation (systemz)."},
  {"systemz", 18, "R_390_PLT16DBL", "PLT16DBL relocation (systemz)."},
  {"systemz", 19, "R_390_PC32DBL", "PC32DBL relocation (systemz)."},
  {"systemz", 20, "R_390_PLT32DBL", "PLT32DBL relocation (systemz)."},
  {"systemz", 21, "R_390_GOTPCDBL", "GOTPCDBL relocation (systemz)."},
  {"systemz", 22, "R_390_64", "64 relocation (systemz)."},
  {"systemz", 23, "R_390_PC64", "PC64 relocation (systemz)."},
  {"systemz", 24, "R_390_GOT64", "GOT64 relocation (systemz)."},
  {"systemz", 25, "R_390_PLT64", "PLT64 relocation (systemz)."},
  {"systemz", 26, "R_390_GOTENT", "GOTENT relocation (systemz)."},
  {"systemz", 27, "R_390_GOTOFF16", "GOTOFF16 relocation (systemz)."},
  {"systemz", 28, "R_390_GOTOFF64", "GOTOFF64 relocation (systemz)."},
  {"systemz", 29, "R_390_GOTPLT12", "GOTPLT12 relocation (systemz)."},
  {"systemz", 30, "R_390_GOTPLT16", "GOTPLT16 relocation (systemz)."},
  {"systemz", 31, "R_390_GOTPLT32", "GOTPLT32 relocation (systemz)."},
  {"systemz", 32, "R_390_GOTPLT64", "GOTPLT64 relocation (systemz)."},
  {"systemz", 33, "R_390_GOTPLTENT", "GOTPLTENT relocation (systemz)."},
  {"systemz", 34, "R_390_PLTOFF16", "PLTOFF16 relocation (systemz)."},
  {"systemz", 35, "R_390_PLTOFF32", "PLTOFF32 relocation (systemz)."},
  {"systemz", 36, "R_390_PLTOFF64", "PLTOFF64 relocation (systemz)."},
  {"systemz", 37, "R_390_TLS_LOAD", "TLS_LOAD relocation (systemz)."},
  {"systemz", 38, "R_390_TLS_GDCALL", "TLS_GDCALL relocation (systemz)."},
  {"systemz", 39, "R_390_TLS_LDCALL", "TLS_LDCALL relocation (systemz)."},
  {"systemz", 40, "R_390_TLS_GD32", "TLS_GD32 relocation (systemz)."},
  {"systemz", 41, "R_390_TLS_GD64", "TLS_GD64 relocation (systemz)."},
  {"systemz", 42, "R_390_TLS_GOTIE12", "TLS_GOTIE12 relocation (systemz)."},
  {"systemz", 43, "R_390_TLS_GOTIE32", "TLS_GOTIE32 relocation (systemz)."},
  {"systemz", 44, "R_390_TLS_GOTIE64", "TLS_GOTIE64 relocation (systemz)."},
  {"systemz", 45, "R_390_TLS_LDM32", "TLS_LDM32 relocation (systemz)."},
  {"systemz", 46, "R_390_TLS_LDM64", "TLS_LDM64 relocation (systemz)."},
  {"systemz", 47, "R_390_TLS_IE32", "TLS_IE32 relocation (systemz)."},
  {"systemz", 48, "R_390_TLS_IE64", "TLS_IE64 relocation (systemz)."},
  {"systemz", 49, "R_390_TLS_IEENT", "TLS_IEENT relocation (systemz)."},
  {"systemz", 50, "R_390_TLS_LE32", "TLS_LE32 relocation (systemz)."},
  {"systemz", 51, "R_390_TLS_LE64", "TLS_LE64 relocation (systemz)."},
  {"systemz", 52, "R_390_TLS_LDO32", "TLS_LDO32 relocation (systemz)."},
  {"systemz", 53, "R_390_TLS_LDO64", "TLS_LDO64 relocation (systemz)."},
  {"systemz", 54, "R_390_TLS_DTPMOD", "TLS_DTPMOD relocation (systemz)."},
  {"systemz", 55, "R_390_TLS_DTPOFF", "TLS_DTPOFF relocation (systemz)."},
  {"systemz", 56, "R_390_TLS_TPOFF", "TLS_TPOFF relocation (systemz)."},
  {"systemz", 57, "R_390_20", "20 relocation (systemz)."},
  {"systemz", 58, "R_390_GOT20", "GOT20 relocation (systemz)."},
  {"systemz", 59, "R_390_GOTPLT20", "GOTPLT20 relocation (systemz)."},
  {"systemz", 60, "R_390_TLS_GOTIE20", "TLS_GOTIE20 relocation (systemz)."},
  {"systemz", 61, "R_390_IRELATIVE", "IRELATIVE relocation (systemz)."},
  {"systemz", 62, "R_390_PC12DBL", "PC12DBL relocation (systemz)."},
  {"systemz", 63, "R_390_PLT12DBL", "PLT12DBL relocation (systemz)."},
  {"systemz", 64, "R_390_PC24DBL", "PC24DBL relocation (systemz)."},
  {"systemz", 65, "R_390_PLT24DBL", "PLT24DBL relocation (systemz)."},
  {"csky", 0, "R_CKCORE_NONE", "NONE relocation (csky)."},
  {"csky", 1, "R_CKCORE_ADDR32", "ADDR32 relocation (csky)."},
  {"csky", 2, "R_CKCORE_PCREL_IMM8_4", "PCREL_IMM8_4 relocation (csky)."},
  {"csky", 3, "R_CKCORE_PCREL_IMM11_2", "PCREL_IMM11_2 relocation (csky)."},
  {"csky", 4, "R_CKCORE_PCREL_IMM4_2", "PCREL_IMM4_2 relocation (csky)."},
  {"csky", 5, "R_CKCORE_PCREL32", "PCREL32 relocation (csky)."},
  {"csky", 6, "R_CKCORE_PCREL_JSR_IMM11_2", "PCREL_JSR_IMM11_2 relocation (csky)."},
  {"csky", 7, "R_CKCORE_GNU_VTINHERIT", "GNU_VTINHERIT relocation (csky)."},
  {"csky", 8, "R_CKCORE_GNU_VTENTRY", "GNU_VTENTRY relocation (csky)."},
  {"csky", 9, "R_CKCORE_RELATIVE", "RELATIVE relocation (csky)."},
  {"csky", 10, "R_CKCORE_COPY", "COPY relocation (csky)."},
  {"csky", 11, "R_CKCORE_GLOB_DAT", "GLOB_DAT relocation (csky)."},
  {"csky", 12, "R_CKCORE_JUMP_SLOT", "JUMP_SLOT relocation (csky)."},
  {"csky", 13, "R_CKCORE_GOTOFF", "GOTOFF relocation (csky)."},
  {"csky", 14, "R_CKCORE_GOTPC", "GOTPC relocation (csky)."},
  {"csky", 15, "R_CKCORE_GOT32", "GOT32 relocation (csky)."},
  {"csky", 16, "R_CKCORE_PLT32", "PLT32 relocation (csky)."},
  {"csky", 17, "R_CKCORE_ADDRGOT", "ADDRGOT relocation (csky)."},
  {"csky", 18, "R_CKCORE_ADDRPLT", "ADDRPLT relocation (csky)."},
  {"csky", 19, "R_CKCORE_PCREL_IMM26_2", "PCREL_IMM26_2 relocation (csky)."},
  {"csky", 20, "R_CKCORE_PCREL_IMM16_2", "PCREL_IMM16_2 relocation (csky)."},
  {"csky", 21, "R_CKCORE_PCREL_IMM16_4", "PCREL_IMM16_4 relocation (csky)."},
  {"csky", 22, "R_CKCORE_PCREL_IMM10_2", "PCREL_IMM10_2 relocation (csky)."},
  {"csky", 23, "R_CKCORE_PCREL_IMM10_4", "PCREL_IMM10_4 relocation (csky)."},
  {"csky", 24, "R_CKCORE_ADDR_HI16", "ADDR_HI16 relocation (csky)."},
  {"csky", 25, "R_CKCORE_ADDR_LO16", "ADDR_LO16 relocation (csky)."},
  {"csky", 26, "R_CKCORE_GOTPC_HI16", "GOTPC_HI16 relocation (csky)."},
  {"csky", 27, "R_CKCORE_GOTPC_LO16", "GOTPC_LO16 relocation (csky)."},
  {"csky", 28, "R_CKCORE_GOTOFF_HI16", "GOTOFF_HI16 relocation (csky)."},
  {"csky", 29, "R_CKCORE_GOTOFF_LO16", "GOTOFF_LO16 relocation (csky)."},
  {"csky", 30, "R_CKCORE_GOT12", "GOT12 relocation (csky)."},
  {"csky", 31, "R_CKCORE_GOT_HI16", "GOT_HI16 relocation (csky)."},
  {"csky", 32, "R_CKCORE_GOT_LO16", "GOT_LO16 relocation (csky)."},
  {"csky", 33, "R_CKCORE_PLT12", "PLT12 relocation (csky)."},
  {"csky", 34, "R_CKCORE_PLT_HI16", "PLT_HI16 relocation (csky)."},
  {"csky", 35, "R_CKCORE_PLT_LO16", "PLT_LO16 relocation (csky)."},
  {"csky", 36, "R_CKCORE_ADDRGOT_HI16", "ADDRGOT_HI16 relocation (csky)."},
  {"csky", 37, "R_CKCORE_ADDRGOT_LO16", "ADDRGOT_LO16 relocation (csky)."},
  {"csky", 38, "R_CKCORE_ADDRPLT_HI16", "ADDRPLT_HI16 relocation (csky)."},
  {"csky", 39, "R_CKCORE_ADDRPLT_LO16", "ADDRPLT_LO16 relocation (csky)."},
  {"csky", 40, "R_CKCORE_PCREL_JSR_IMM26_2", "PCREL_JSR_IMM26_2 relocation (csky)."},
  {"csky", 41, "R_CKCORE_TOFFSET_LO16", "TOFFSET_LO16 relocation (csky)."},
  {"csky", 42, "R_CKCORE_DOFFSET_LO16", "DOFFSET_LO16 relocation (csky)."},
  {"csky", 43, "R_CKCORE_PCREL_IMM18_2", "PCREL_IMM18_2 relocation (csky)."},
  {"csky", 44, "R_CKCORE_DOFFSET_IMM18", "DOFFSET_IMM18 relocation (csky)."},
  {"csky", 45, "R_CKCORE_DOFFSET_IMM18_2", "DOFFSET_IMM18_2 relocation (csky)."},
  {"csky", 46, "R_CKCORE_DOFFSET_IMM18_4", "DOFFSET_IMM18_4 relocation (csky)."},
  {"csky", 47, "R_CKCORE_GOTOFF_IMM18", "GOTOFF_IMM18 relocation (csky)."},
  {"csky", 48, "R_CKCORE_GOT_IMM18_4", "GOT_IMM18_4 relocation (csky)."},
  {"csky", 49, "R_CKCORE_PLT_IMM18_4", "PLT_IMM18_4 relocation (csky)."},
  {"csky", 50, "R_CKCORE_PCREL_IMM7_4", "PCREL_IMM7_4 relocation (csky)."},
  {"csky", 51, "R_CKCORE_TLS_LE32", "TLS_LE32 relocation (csky)."},
  {"csky", 52, "R_CKCORE_TLS_IE32", "TLS_IE32 relocation (csky)."},
  {"csky", 53, "R_CKCORE_TLS_GD32", "TLS_GD32 relocation (csky)."},
  {"csky", 54, "R_CKCORE_TLS_LDM32", "TLS_LDM32 relocation (csky)."},
  {"csky", 55, "R_CKCORE_TLS_LDO32", "TLS_LDO32 relocation (csky)."},
  {"csky", 56, "R_CKCORE_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (csky)."},
  {"csky", 57, "R_CKCORE_TLS_DTPOFF32", "TLS_DTPOFF32 relocation (csky)."},
  {"csky", 58, "R_CKCORE_TLS_TPOFF32", "TLS_TPOFF32 relocation (csky)."},
  {"csky", 59, "R_CKCORE_PCREL_FLRW_IMM8_4", "PCREL_FLRW_IMM8_4 relocation (csky)."},
  {"csky", 60, "R_CKCORE_NOJSRI", "NOJSRI relocation (csky)."},
  {"csky", 61, "R_CKCORE_CALLGRAPH", "CALLGRAPH relocation (csky)."},
  {"csky", 62, "R_CKCORE_IRELATIVE", "IRELATIVE relocation (csky)."},
  {"csky", 63, "R_CKCORE_PCREL_BLOOP_IMM4_4", "PCREL_BLOOP_IMM4_4 relocation (csky)."},
  {"csky", 64, "R_CKCORE_PCREL_BLOOP_IMM12_4", "PCREL_BLOOP_IMM12_4 relocation (csky)."},
  {"csky", 65, "R_CKCORE_PCREL_VLRW_IMM12_1", "PCREL_VLRW_IMM12_1 relocation (csky)."},
  {"csky", 66, "R_CKCORE_PCREL_VLRW_IMM12_2", "PCREL_VLRW_IMM12_2 relocation (csky)."},
  {"csky", 67, "R_CKCORE_PCREL_VLRW_IMM12_4", "PCREL_VLRW_IMM12_4 relocation (csky)."},
  {"csky", 68, "R_CKCORE_PCREL_VLRW_IMM12_8", "PCREL_VLRW_IMM12_8 relocation (csky)."},
  {"arc", 0, "R_ARC_NONE", "NONE relocation (arc)."},
  {"arc", 1, "R_ARC_8", "8 relocation (arc)."},
  {"arc", 2, "R_ARC_16", "16 relocation (arc)."},
  {"arc", 3, "R_ARC_24", "24 relocation (arc)."},
  {"arc", 4, "R_ARC_32", "32 relocation (arc)."},
  {"arc", 8, "R_ARC_N8", "N8 relocation (arc)."},
  {"arc", 9, "R_ARC_N16", "N16 relocation (arc)."},
  {"arc", 10, "R_ARC_N24", "N24 relocation (arc)."},
  {"arc", 11, "R_ARC_N32", "N32 relocation (arc)."},
  {"arc", 12, "R_ARC_SDA", "SDA relocation (arc)."},
  {"arc", 13, "R_ARC_SECTOFF", "SECTOFF relocation (arc)."},
  {"arc", 14, "R_ARC_S21H_PCREL", "S21H_PCREL relocation (arc)."},
  {"arc", 15, "R_ARC_S21W_PCREL", "S21W_PCREL relocation (arc)."},
  {"arc", 16, "R_ARC_S25H_PCREL", "S25H_PCREL relocation (arc)."},
  {"arc", 17, "R_ARC_S25W_PCREL", "S25W_PCREL relocation (arc)."},
  {"arc", 18, "R_ARC_SDA32", "SDA32 relocation (arc)."},
  {"arc", 19, "R_ARC_SDA_LDST", "SDA_LDST relocation (arc)."},
  {"arc", 20, "R_ARC_SDA_LDST1", "SDA_LDST1 relocation (arc)."},
  {"arc", 21, "R_ARC_SDA_LDST2", "SDA_LDST2 relocation (arc)."},
  {"arc", 22, "R_ARC_SDA16_LD", "SDA16_LD relocation (arc)."},
  {"arc", 23, "R_ARC_SDA16_LD1", "SDA16_LD1 relocation (arc)."},
  {"arc", 24, "R_ARC_SDA16_LD2", "SDA16_LD2 relocation (arc)."},
  {"arc", 25, "R_ARC_S13_PCREL", "S13_PCREL relocation (arc)."},
  {"arc", 26, "R_ARC_W", "W relocation (arc)."},
  {"arc", 27, "R_ARC_32_ME", "32_ME relocation (arc)."},
  {"arc", 105, "R_ARC_32_ME_S", "32_ME_S relocation (arc)."},
  {"arc", 28, "R_ARC_N32_ME", "N32_ME relocation (arc)."},
  {"arc", 29, "R_ARC_SECTOFF_ME", "SECTOFF_ME relocation (arc)."},
  {"arc", 30, "R_ARC_SDA32_ME", "SDA32_ME relocation (arc)."},
  {"arc", 31, "R_ARC_W_ME", "W_ME relocation (arc)."},
  {"arc", 35, "R_AC_SECTOFF_U8", "SECTOFF_U8 relocation (arc)."},
  {"arc", 36, "R_AC_SECTOFF_U8_1", "SECTOFF_U8_1 relocation (arc)."},
  {"arc", 37, "R_AC_SECTOFF_U8_2", "SECTOFF_U8_2 relocation (arc)."},
  {"arc", 38, "R_AC_SECTOFF_S9", "SECTOFF_S9 relocation (arc)."},
  {"arc", 39, "R_AC_SECTOFF_S9_1", "SECTOFF_S9_1 relocation (arc)."},
  {"arc", 40, "R_AC_SECTOFF_S9_2", "SECTOFF_S9_2 relocation (arc)."},
  {"arc", 41, "R_ARC_SECTOFF_ME_1", "SECTOFF_ME_1 relocation (arc)."},
  {"arc", 42, "R_ARC_SECTOFF_ME_2", "SECTOFF_ME_2 relocation (arc)."},
  {"arc", 43, "R_ARC_SECTOFF_1", "SECTOFF_1 relocation (arc)."},
  {"arc", 44, "R_ARC_SECTOFF_2", "SECTOFF_2 relocation (arc)."},
  {"arc", 45, "R_ARC_SDA_12", "SDA_12 relocation (arc)."},
  {"arc", 48, "R_ARC_SDA16_ST2", "SDA16_ST2 relocation (arc)."},
  {"arc", 49, "R_ARC_32_PCREL", "32_PCREL relocation (arc)."},
  {"arc", 50, "R_ARC_PC32", "PC32 relocation (arc)."},
  {"arc", 59, "R_ARC_GOT32", "GOT32 relocation (arc)."},
  {"arc", 51, "R_ARC_GOTPC32", "GOTPC32 relocation (arc)."},
  {"arc", 52, "R_ARC_PLT32", "PLT32 relocation (arc)."},
  {"arc", 53, "R_ARC_COPY", "COPY relocation (arc)."},
  {"arc", 54, "R_ARC_GLOB_DAT", "GLOB_DAT relocation (arc)."},
  {"arc", 55, "R_ARC_JMP_SLOT", "JMP_SLOT relocation (arc)."},
  {"arc", 56, "R_ARC_RELATIVE", "RELATIVE relocation (arc)."},
  {"arc", 57, "R_ARC_GOTOFF", "GOTOFF relocation (arc)."},
  {"arc", 58, "R_ARC_GOTPC", "GOTPC relocation (arc)."},
  {"arc", 60, "R_ARC_S21W_PCREL_PLT", "S21W_PCREL_PLT relocation (arc)."},
  {"arc", 61, "R_ARC_S25H_PCREL_PLT", "S25H_PCREL_PLT relocation (arc)."},
  {"arc", 63, "R_ARC_JLI_SECTOFF", "JLI_SECTOFF relocation (arc)."},
  {"arc", 66, "R_ARC_TLS_DTPMOD", "TLS_DTPMOD relocation (arc)."},
  {"arc", 68, "R_ARC_TLS_TPOFF", "TLS_TPOFF relocation (arc)."},
  {"arc", 69, "R_ARC_TLS_GD_GOT", "TLS_GD_GOT relocation (arc)."},
  {"arc", 70, "R_ARC_TLS_GD_LD", "TLS_GD_LD relocation (arc)."},
  {"arc", 71, "R_ARC_TLS_GD_CALL", "TLS_GD_CALL relocation (arc)."},
  {"arc", 72, "R_ARC_TLS_IE_GOT", "TLS_IE_GOT relocation (arc)."},
  {"arc", 67, "R_ARC_TLS_DTPOFF", "TLS_DTPOFF relocation (arc)."},
  {"arc", 73, "R_ARC_TLS_DTPOFF_S9", "TLS_DTPOFF_S9 relocation (arc)."},
  {"arc", 74, "R_ARC_TLS_LE_S9", "TLS_LE_S9 relocation (arc)."},
  {"arc", 75, "R_ARC_TLS_LE_32", "TLS_LE_32 relocation (arc)."},
  {"arc", 76, "R_ARC_S25W_PCREL_PLT", "S25W_PCREL_PLT relocation (arc)."},
  {"arc", 77, "R_ARC_S21H_PCREL_PLT", "S21H_PCREL_PLT relocation (arc)."},
  {"arc", 78, "R_ARC_NPS_CMEM16", "NPS_CMEM16 relocation (arc)."},
  {"xtensa", 0, "R_XTENSA_NONE", "NONE relocation (xtensa)."},
  {"xtensa", 1, "R_XTENSA_32", "32 relocation (xtensa)."},
  {"xtensa", 2, "R_XTENSA_RTLD", "RTLD relocation (xtensa)."},
  {"xtensa", 3, "R_XTENSA_GLOB_DAT", "GLOB_DAT relocation (xtensa)."},
  {"xtensa", 4, "R_XTENSA_JMP_SLOT", "JMP_SLOT relocation (xtensa)."},
  {"xtensa", 5, "R_XTENSA_RELATIVE", "RELATIVE relocation (xtensa)."},
  {"xtensa", 6, "R_XTENSA_PLT", "PLT relocation (xtensa)."},
  {"xtensa", 8, "R_XTENSA_OP0", "OP0 relocation (xtensa)."},
  {"xtensa", 9, "R_XTENSA_OP1", "OP1 relocation (xtensa)."},
  {"xtensa", 10, "R_XTENSA_OP2", "OP2 relocation (xtensa)."},
  {"xtensa", 11, "R_XTENSA_ASM_EXPAND", "ASM_EXPAND relocation (xtensa)."},
  {"xtensa", 12, "R_XTENSA_ASM_SIMPLIFY", "ASM_SIMPLIFY relocation (xtensa)."},
  {"xtensa", 14, "R_XTENSA_32_PCREL", "32_PCREL relocation (xtensa)."},
  {"xtensa", 15, "R_XTENSA_GNU_VTINHERIT", "GNU_VTINHERIT relocation (xtensa)."},
  {"xtensa", 16, "R_XTENSA_GNU_VTENTRY", "GNU_VTENTRY relocation (xtensa)."},
  {"xtensa", 17, "R_XTENSA_DIFF8", "DIFF8 relocation (xtensa)."},
  {"xtensa", 18, "R_XTENSA_DIFF16", "DIFF16 relocation (xtensa)."},
  {"xtensa", 19, "R_XTENSA_DIFF32", "DIFF32 relocation (xtensa)."},
  {"xtensa", 20, "R_XTENSA_SLOT0_OP", "SLOT0_OP relocation (xtensa)."},
  {"xtensa", 21, "R_XTENSA_SLOT1_OP", "SLOT1_OP relocation (xtensa)."},
  {"xtensa", 22, "R_XTENSA_SLOT2_OP", "SLOT2_OP relocation (xtensa)."},
  {"xtensa", 23, "R_XTENSA_SLOT3_OP", "SLOT3_OP relocation (xtensa)."},
  {"xtensa", 24, "R_XTENSA_SLOT4_OP", "SLOT4_OP relocation (xtensa)."},
  {"xtensa", 25, "R_XTENSA_SLOT5_OP", "SLOT5_OP relocation (xtensa)."},
  {"xtensa", 26, "R_XTENSA_SLOT6_OP", "SLOT6_OP relocation (xtensa)."},
  {"xtensa", 27, "R_XTENSA_SLOT7_OP", "SLOT7_OP relocation (xtensa)."},
  {"xtensa", 28, "R_XTENSA_SLOT8_OP", "SLOT8_OP relocation (xtensa)."},
  {"xtensa", 29, "R_XTENSA_SLOT9_OP", "SLOT9_OP relocation (xtensa)."},
  {"xtensa", 30, "R_XTENSA_SLOT10_OP", "SLOT10_OP relocation (xtensa)."},
  {"xtensa", 31, "R_XTENSA_SLOT11_OP", "SLOT11_OP relocation (xtensa)."},
  {"xtensa", 32, "R_XTENSA_SLOT12_OP", "SLOT12_OP relocation (xtensa)."},
  {"xtensa", 33, "R_XTENSA_SLOT13_OP", "SLOT13_OP relocation (xtensa)."},
  {"xtensa", 34, "R_XTENSA_SLOT14_OP", "SLOT14_OP relocation (xtensa)."},
  {"xtensa", 35, "R_XTENSA_SLOT0_ALT", "SLOT0_ALT relocation (xtensa)."},
  {"xtensa", 36, "R_XTENSA_SLOT1_ALT", "SLOT1_ALT relocation (xtensa)."},
  {"xtensa", 37, "R_XTENSA_SLOT2_ALT", "SLOT2_ALT relocation (xtensa)."},
  {"xtensa", 38, "R_XTENSA_SLOT3_ALT", "SLOT3_ALT relocation (xtensa)."},
  {"xtensa", 39, "R_XTENSA_SLOT4_ALT", "SLOT4_ALT relocation (xtensa)."},
  {"xtensa", 40, "R_XTENSA_SLOT5_ALT", "SLOT5_ALT relocation (xtensa)."},
  {"xtensa", 41, "R_XTENSA_SLOT6_ALT", "SLOT6_ALT relocation (xtensa)."},
  {"xtensa", 42, "R_XTENSA_SLOT7_ALT", "SLOT7_ALT relocation (xtensa)."},
  {"xtensa", 43, "R_XTENSA_SLOT8_ALT", "SLOT8_ALT relocation (xtensa)."},
  {"xtensa", 44, "R_XTENSA_SLOT9_ALT", "SLOT9_ALT relocation (xtensa)."},
  {"xtensa", 45, "R_XTENSA_SLOT10_ALT", "SLOT10_ALT relocation (xtensa)."},
  {"xtensa", 46, "R_XTENSA_SLOT11_ALT", "SLOT11_ALT relocation (xtensa)."},
  {"xtensa", 47, "R_XTENSA_SLOT12_ALT", "SLOT12_ALT relocation (xtensa)."},
  {"xtensa", 48, "R_XTENSA_SLOT13_ALT", "SLOT13_ALT relocation (xtensa)."},
  {"xtensa", 49, "R_XTENSA_SLOT14_ALT", "SLOT14_ALT relocation (xtensa)."},
  {"xtensa", 50, "R_XTENSA_TLSDESC_FN", "TLSDESC_FN relocation (xtensa)."},
  {"xtensa", 51, "R_XTENSA_TLSDESC_ARG", "TLSDESC_ARG relocation (xtensa)."},
  {"xtensa", 52, "R_XTENSA_TLS_DTPOFF", "TLS_DTPOFF relocation (xtensa)."},
  {"xtensa", 53, "R_XTENSA_TLS_TPOFF", "TLS_TPOFF relocation (xtensa)."},
  {"xtensa", 54, "R_XTENSA_TLS_FUNC", "TLS_FUNC relocation (xtensa)."},
  {"xtensa", 55, "R_XTENSA_TLS_ARG", "TLS_ARG relocation (xtensa)."},
  {"xtensa", 56, "R_XTENSA_TLS_CALL", "TLS_CALL relocation (xtensa)."},
  {"m68k", 0, "R_68K_NONE", "NONE relocation (m68k)."},
  {"m68k", 1, "R_68K_32", "32 relocation (m68k)."},
  {"m68k", 2, "R_68K_16", "16 relocation (m68k)."},
  {"m68k", 3, "R_68K_8", "8 relocation (m68k)."},
  {"m68k", 4, "R_68K_PC32", "PC32 relocation (m68k)."},
  {"m68k", 5, "R_68K_PC16", "PC16 relocation (m68k)."},
  {"m68k", 6, "R_68K_PC8", "PC8 relocation (m68k)."},
  {"m68k", 7, "R_68K_GOTPCREL32", "GOTPCREL32 relocation (m68k)."},
  {"m68k", 8, "R_68K_GOTPCREL16", "GOTPCREL16 relocation (m68k)."},
  {"m68k", 9, "R_68K_GOTPCREL8", "GOTPCREL8 relocation (m68k)."},
  {"m68k", 10, "R_68K_GOTOFF32", "GOTOFF32 relocation (m68k)."},
  {"m68k", 11, "R_68K_GOTOFF16", "GOTOFF16 relocation (m68k)."},
  {"m68k", 12, "R_68K_GOTOFF8", "GOTOFF8 relocation (m68k)."},
  {"m68k", 13, "R_68K_PLT32", "PLT32 relocation (m68k)."},
  {"m68k", 14, "R_68K_PLT16", "PLT16 relocation (m68k)."},
  {"m68k", 15, "R_68K_PLT8", "PLT8 relocation (m68k)."},
  {"m68k", 16, "R_68K_PLTOFF32", "PLTOFF32 relocation (m68k)."},
  {"m68k", 17, "R_68K_PLTOFF16", "PLTOFF16 relocation (m68k)."},
  {"m68k", 18, "R_68K_PLTOFF8", "PLTOFF8 relocation (m68k)."},
  {"m68k", 19, "R_68K_COPY", "COPY relocation (m68k)."},
  {"m68k", 20, "R_68K_GLOB_DAT", "GLOB_DAT relocation (m68k)."},
  {"m68k", 21, "R_68K_JMP_SLOT", "JMP_SLOT relocation (m68k)."},
  {"m68k", 22, "R_68K_RELATIVE", "RELATIVE relocation (m68k)."},
  {"m68k", 23, "R_68K_GNU_VTINHERIT", "GNU_VTINHERIT relocation (m68k)."},
  {"m68k", 24, "R_68K_GNU_VTENTRY", "GNU_VTENTRY relocation (m68k)."},
  {"m68k", 25, "R_68K_TLS_GD32", "TLS_GD32 relocation (m68k)."},
  {"m68k", 26, "R_68K_TLS_GD16", "TLS_GD16 relocation (m68k)."},
  {"m68k", 27, "R_68K_TLS_GD8", "TLS_GD8 relocation (m68k)."},
  {"m68k", 28, "R_68K_TLS_LDM32", "TLS_LDM32 relocation (m68k)."},
  {"m68k", 29, "R_68K_TLS_LDM16", "TLS_LDM16 relocation (m68k)."},
  {"m68k", 30, "R_68K_TLS_LDM8", "TLS_LDM8 relocation (m68k)."},
  {"m68k", 31, "R_68K_TLS_LDO32", "TLS_LDO32 relocation (m68k)."},
  {"m68k", 32, "R_68K_TLS_LDO16", "TLS_LDO16 relocation (m68k)."},
  {"m68k", 33, "R_68K_TLS_LDO8", "TLS_LDO8 relocation (m68k)."},
  {"m68k", 34, "R_68K_TLS_IE32", "TLS_IE32 relocation (m68k)."},
  {"m68k", 35, "R_68K_TLS_IE16", "TLS_IE16 relocation (m68k)."},
  {"m68k", 36, "R_68K_TLS_IE8", "TLS_IE8 relocation (m68k)."},
  {"m68k", 37, "R_68K_TLS_LE32", "TLS_LE32 relocation (m68k)."},
  {"m68k", 38, "R_68K_TLS_LE16", "TLS_LE16 relocation (m68k)."},
  {"m68k", 39, "R_68K_TLS_LE8", "TLS_LE8 relocation (m68k)."},
  {"m68k", 40, "R_68K_TLS_DTPMOD32", "TLS_DTPMOD32 relocation (m68k)."},
  {"m68k", 41, "R_68K_TLS_DTPREL32", "TLS_DTPREL32 relocation (m68k)."},
  {"m68k", 42, "R_68K_TLS_TPREL32", "TLS_TPREL32 relocation (m68k)."},
  {"avr", 0, "R_AVR_NONE", "NONE relocation (avr)."},
  {"avr", 1, "R_AVR_32", "32 relocation (avr)."},
  {"avr", 2, "R_AVR_7_PCREL", "7_PCREL relocation (avr)."},
  {"avr", 3, "R_AVR_13_PCREL", "13_PCREL relocation (avr)."},
  {"avr", 4, "R_AVR_16", "16 relocation (avr)."},
  {"avr", 5, "R_AVR_16_PM", "16_PM relocation (avr)."},
  {"avr", 6, "R_AVR_LO8_LDI", "LO8_LDI relocation (avr)."},
  {"avr", 7, "R_AVR_HI8_LDI", "HI8_LDI relocation (avr)."},
  {"avr", 8, "R_AVR_HH8_LDI", "HH8_LDI relocation (avr)."},
  {"avr", 9, "R_AVR_LO8_LDI_NEG", "LO8_LDI_NEG relocation (avr)."},
  {"avr", 10, "R_AVR_HI8_LDI_NEG", "HI8_LDI_NEG relocation (avr)."},
  {"avr", 11, "R_AVR_HH8_LDI_NEG", "HH8_LDI_NEG relocation (avr)."},
  {"avr", 12, "R_AVR_LO8_LDI_PM", "LO8_LDI_PM relocation (avr)."},
  {"avr", 13, "R_AVR_HI8_LDI_PM", "HI8_LDI_PM relocation (avr)."},
  {"avr", 14, "R_AVR_HH8_LDI_PM", "HH8_LDI_PM relocation (avr)."},
  {"avr", 15, "R_AVR_LO8_LDI_PM_NEG", "LO8_LDI_PM_NEG relocation (avr)."},
  {"avr", 16, "R_AVR_HI8_LDI_PM_NEG", "HI8_LDI_PM_NEG relocation (avr)."},
  {"avr", 17, "R_AVR_HH8_LDI_PM_NEG", "HH8_LDI_PM_NEG relocation (avr)."},
  {"avr", 18, "R_AVR_CALL", "CALL relocation (avr)."},
  {"avr", 19, "R_AVR_LDI", "LDI relocation (avr)."},
  {"avr", 20, "R_AVR_6", "6 relocation (avr)."},
  {"avr", 21, "R_AVR_6_ADIW", "6_ADIW relocation (avr)."},
  {"avr", 22, "R_AVR_MS8_LDI", "MS8_LDI relocation (avr)."},
  {"avr", 23, "R_AVR_MS8_LDI_NEG", "MS8_LDI_NEG relocation (avr)."},
  {"avr", 24, "R_AVR_LO8_LDI_GS", "LO8_LDI_GS relocation (avr)."},
  {"avr", 25, "R_AVR_HI8_LDI_GS", "HI8_LDI_GS relocation (avr)."},
  {"avr", 26, "R_AVR_8", "8 relocation (avr)."},
  {"avr", 27, "R_AVR_8_LO8", "8_LO8 relocation (avr)."},
  {"avr", 28, "R_AVR_8_HI8", "8_HI8 relocation (avr)."},
  {"avr", 29, "R_AVR_8_HLO8", "8_HLO8 relocation (avr)."},
  {"avr", 30, "R_AVR_DIFF8", "DIFF8 relocation (avr)."},
  {"avr", 31, "R_AVR_DIFF16", "DIFF16 relocation (avr)."},
  {"avr", 32, "R_AVR_DIFF32", "DIFF32 relocation (avr)."},
  {"avr", 33, "R_AVR_LDS_STS_16", "LDS_STS_16 relocation (avr)."},
  {"avr", 34, "R_AVR_PORT6", "PORT6 relocation (avr)."},
  {"avr", 35, "R_AVR_PORT5", "PORT5 relocation (avr)."},
  {"ve", 0, "R_VE_NONE", "NONE relocation (ve)."},
  {"ve", 1, "R_VE_REFLONG", "REFLONG relocation (ve)."},
  {"ve", 2, "R_VE_REFQUAD", "REFQUAD relocation (ve)."},
  {"ve", 3, "R_VE_SREL32", "SREL32 relocation (ve)."},
  {"ve", 4, "R_VE_HI32", "HI32 relocation (ve)."},
  {"ve", 5, "R_VE_LO32", "LO32 relocation (ve)."},
  {"ve", 6, "R_VE_PC_HI32", "PC_HI32 relocation (ve)."},
  {"ve", 7, "R_VE_PC_LO32", "PC_LO32 relocation (ve)."},
  {"ve", 8, "R_VE_GOT32", "GOT32 relocation (ve)."},
  {"ve", 9, "R_VE_GOT_HI32", "GOT_HI32 relocation (ve)."},
  {"ve", 10, "R_VE_GOT_LO32", "GOT_LO32 relocation (ve)."},
  {"ve", 11, "R_VE_GOTOFF32", "GOTOFF32 relocation (ve)."},
  {"ve", 12, "R_VE_GOTOFF_HI32", "GOTOFF_HI32 relocation (ve)."},
  {"ve", 13, "R_VE_GOTOFF_LO32", "GOTOFF_LO32 relocation (ve)."},
  {"ve", 14, "R_VE_PLT32", "PLT32 relocation (ve)."},
  {"ve", 15, "R_VE_PLT_HI32", "PLT_HI32 relocation (ve)."},
  {"ve", 16, "R_VE_PLT_LO32", "PLT_LO32 relocation (ve)."},
  {"ve", 17, "R_VE_RELATIVE", "RELATIVE relocation (ve)."},
  {"ve", 18, "R_VE_GLOB_DAT", "GLOB_DAT relocation (ve)."},
  {"ve", 19, "R_VE_JUMP_SLOT", "JUMP_SLOT relocation (ve)."},
  {"ve", 20, "R_VE_COPY", "COPY relocation (ve)."},
  {"ve", 22, "R_VE_DTPMOD64", "DTPMOD64 relocation (ve)."},
  {"ve", 23, "R_VE_DTPOFF64", "DTPOFF64 relocation (ve)."},
  {"ve", 25, "R_VE_TLS_GD_HI32", "TLS_GD_HI32 relocation (ve)."},
  {"ve", 26, "R_VE_TLS_GD_LO32", "TLS_GD_LO32 relocation (ve)."},
  {"ve", 32, "R_VE_TPOFF_HI32", "TPOFF_HI32 relocation (ve)."},
  {"ve", 33, "R_VE_TPOFF_LO32", "TPOFF_LO32 relocation (ve)."},
  {"ve", 35, "R_VE_CALL_HI32", "CALL_HI32 relocation (ve)."},
  {"ve", 36, "R_VE_CALL_LO32", "CALL_LO32 relocation (ve)."},
  {"amdgpu", 0, "R_AMDGPU_NONE", "NONE relocation (amdgpu)."},
  {"amdgpu", 1, "R_AMDGPU_ABS32_LO", "ABS32_LO relocation (amdgpu)."},
  {"amdgpu", 2, "R_AMDGPU_ABS32_HI", "ABS32_HI relocation (amdgpu)."},
  {"amdgpu", 3, "R_AMDGPU_ABS64", "ABS64 relocation (amdgpu)."},
  {"amdgpu", 4, "R_AMDGPU_REL32", "REL32 relocation (amdgpu)."},
  {"amdgpu", 5, "R_AMDGPU_REL64", "REL64 relocation (amdgpu)."},
  {"amdgpu", 6, "R_AMDGPU_ABS32", "ABS32 relocation (amdgpu)."},
  {"amdgpu", 7, "R_AMDGPU_GOTPCREL", "GOTPCREL relocation (amdgpu)."},
  {"amdgpu", 8, "R_AMDGPU_GOTPCREL32_LO", "GOTPCREL32_LO relocation (amdgpu)."},
  {"amdgpu", 9, "R_AMDGPU_GOTPCREL32_HI", "GOTPCREL32_HI relocation (amdgpu)."},
  {"amdgpu", 10, "R_AMDGPU_REL32_LO", "REL32_LO relocation (amdgpu)."},
  {"amdgpu", 11, "R_AMDGPU_REL32_HI", "REL32_HI relocation (amdgpu)."},
  {"amdgpu", 13, "R_AMDGPU_RELATIVE64", "RELATIVE64 relocation (amdgpu)."},
  {"amdgpu", 14, "R_AMDGPU_REL16", "REL16 relocation (amdgpu)."},
  {"msp430", 0, "R_MSP430_NONE", "NONE relocation (msp430)."},
  {"msp430", 1, "R_MSP430_32", "32 relocation (msp430)."},
  {"msp430", 2, "R_MSP430_10_PCREL", "10_PCREL relocation (msp430)."},
  {"msp430", 3, "R_MSP430_16", "16 relocation (msp430)."},
  {"msp430", 4, "R_MSP430_16_PCREL", "16_PCREL relocation (msp430)."},
  {"msp430", 5, "R_MSP430_16_BYTE", "16_BYTE relocation (msp430)."},
  {"msp430", 6, "R_MSP430_16_PCREL_BYTE", "16_PCREL_BYTE relocation (msp430)."},
  {"msp430", 7, "R_MSP430_2X_PCREL", "2X_PCREL relocation (msp430)."},
  {"msp430", 8, "R_MSP430_RL_PCREL", "RL_PCREL relocation (msp430)."},
  {"msp430", 9, "R_MSP430_8", "8 relocation (msp430)."},
  {"msp430", 10, "R_MSP430_SYM_DIFF", "SYM_DIFF relocation (msp430)."},
  {"lanai", 0, "R_LANAI_NONE", "NONE relocation (lanai)."},
  {"lanai", 1, "R_LANAI_21", "21 relocation (lanai)."},
  {"lanai", 2, "R_LANAI_21_F", "21_F relocation (lanai)."},
  {"lanai", 3, "R_LANAI_25", "25 relocation (lanai)."},
  {"lanai", 4, "R_LANAI_32", "32 relocation (lanai)."},
  {"lanai", 5, "R_LANAI_HI16", "HI16 relocation (lanai)."},
  {"lanai", 6, "R_LANAI_LO16", "LO16 relocation (lanai)."},
  {"bpf", 0, "R_BPF_NONE", "NONE relocation (bpf)."},
  {"bpf", 1, "R_BPF_64_64", "64_64 relocation (bpf)."},
  {"bpf", 2, "R_BPF_64_ABS64", "64_ABS64 relocation (bpf)."},
  {"bpf", 3, "R_BPF_64_ABS32", "64_ABS32 relocation (bpf)."},
  {"bpf", 4, "R_BPF_64_NODYLD32", "64_NODYLD32 relocation (bpf)."},
  {"bpf", 10, "R_BPF_64_32", "64_32 relocation (bpf)."},
};
static const int g_reloc_ref_count = (int)(sizeof(g_reloc_ref) / sizeof(g_reloc_ref[0]));

static void wall_list_relocs(int all) {
  int shown = 0;
  for (int i = 0; i < g_reloc_ref_count; i++) {
    if (!all && strcmp(g_reloc_ref[i].arch, "aarch64") != 0) continue;
    printf("%-10s 0x%04x  %-44s %s\n", g_reloc_ref[i].arch,
           g_reloc_ref[i].value, g_reloc_ref[i].name, g_reloc_ref[i].doc);
    shown++;
  }
  fprintf(stderr, "linker: %d relocation names listed\n", shown);
}
/* ------------------------------------------------------------------------- */
/* Driver                                                                     */
/* ------------------------------------------------------------------------- */
/* ------------------------------------------------------------------------- */
/* In-place ELF64 .comment editor (formerly src/common/elfcomment.h)               */
/* ------------------------------------------------------------------------- */
static int elf_comment_set(const char *path, const char *comment) {
  FILE *f = fopen(path, "rb+");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long fsz = ftell(f);
  size_t cur = (size_t)(fsz > 0 ? fsz : 0);
  if (cur < 64) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  unsigned char *buf = (unsigned char *)malloc(cur);
  if (!buf) { fclose(f); return -1; }
  if (fread(buf, 1, cur, f) != cur) { free(buf); fclose(f); return -1; }

  int rc = -1;
  do {
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') break;
    if (buf[4] != 2 || buf[5] != 1) break;   /* ELF64 little-endian only */
    uint64_t shoff = r64(buf + 40);
    uint16_t shentsize = r16(buf + 58);
    uint16_t shnum = r16(buf + 60);
    uint16_t shstrndx = r16(buf + 62);
    if (!shoff || shentsize < 64 || !shnum || shstrndx >= shnum) break;
    if (shoff + (uint64_t)shentsize * shnum > (uint64_t)cur) break;
    unsigned char *strsh = buf + shoff + (uint64_t)shentsize * shstrndx;
    uint64_t stroff = r64(strsh + 24), strsz = r64(strsh + 32);
    if (stroff + strsz > (uint64_t)cur) break;
    const char *strtab = (const char *)(buf + stroff);
    int found = -1;
    for (uint16_t i = 0; i < shnum; i++) {
      unsigned char *sh = buf + shoff + (uint64_t)shentsize * i;
      uint32_t nameoff = r32(sh);
      if (nameoff >= strsz) continue;
      if (!strcmp(strtab + nameoff, ".comment")) { found = (int)i; break; }
    }
    if (found < 0) { rc = -2; break; }
    unsigned char *sh = buf + shoff + (uint64_t)shentsize * (uint64_t)found;
    uint64_t off = r64(sh + 24), size = r64(sh + 32), flags = r64(sh + 8);
    size_t plen = strlen(comment) + 1;
    if (off + size > (uint64_t)cur) { rc = -2; break; }
    if (size >= plen) {
      memset(buf + off, 0, (size_t)size);
      memcpy(buf + off, comment, plen);
      w64(sh + 32, (uint64_t)plen);
    } else if (!(flags & SHF_ALLOC)) {
      /* .comment is not loaded: grow it by appending after the current file
       * end and repointing sh_offset.  Nothing in the program headers moves. */
      unsigned char *nb = (unsigned char *)realloc(buf, cur + plen);
      if (!nb) { rc = -1; break; }
      buf = nb;
      sh = buf + shoff + (uint64_t)shentsize * (uint64_t)found;
      memset(buf + cur, 0, plen);
      memcpy(buf + cur, comment, plen);
      w64(sh + 24, (uint64_t)cur);
      w64(sh + 32, (uint64_t)plen);
      cur += plen;
    } else {
      /* Allocated section: only grow if the following file hole is large
       * enough; never move it away from its virtual address. */
      uint64_t boundary = shoff;
      for (uint16_t i = 0; i < shnum; i++) {
        unsigned char *s2 = buf + shoff + (uint64_t)shentsize * i;
        if (r32(s2 + 4) == SHT_NOBITS) continue;
        uint64_t o2 = r64(s2 + 24);
        if (o2 > off && o2 < boundary) boundary = o2;
      }
      if (off + plen > boundary) { rc = -2; break; }
      memset(buf + off, 0, (size_t)(boundary - off));
      memcpy(buf + off, comment, plen);
      w64(sh + 32, (uint64_t)plen);
    }
    rc = 0;
  } while (0);

  if (rc == 0) {
    if (fseek(f, 0, SEEK_SET) != 0) rc = -1;
    else {
      size_t wrote = fwrite(buf, 1, cur, f);
      fflush(f);
      if (wrote != cur) rc = -1;
    }
  }
  free(buf);
  fclose(f);
  return rc;
}

static int elf_comment_get(const char *path, char *out, size_t outlen) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long fsz = ftell(f);
  if (fsz < 64) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  unsigned char *buf = (unsigned char *)malloc((size_t)fsz);
  if (!buf) { fclose(f); return -1; }
  if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { free(buf); fclose(f); return -1; }

  int rc = -1;
  do {
    if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') break;
    if (buf[4] != 2 || buf[5] != 1) break;
    uint64_t shoff = r64(buf + 40);
    uint16_t shentsize = r16(buf + 58);
    uint16_t shnum = r16(buf + 60);
    uint16_t shstrndx = r16(buf + 62);
    if (!shoff || shentsize < 64 || !shnum || shstrndx >= shnum) break;
    if (shoff + (uint64_t)shentsize * shnum > (uint64_t)fsz) break;
    unsigned char *strsh = buf + shoff + (uint64_t)shentsize * shstrndx;
    uint64_t stroff = r64(strsh + 24), strsz = r64(strsh + 32);
    if (stroff + strsz > (uint64_t)fsz) break;
    const char *strtab = (const char *)(buf + stroff);
    for (uint16_t i = 0; i < shnum; i++) {
      unsigned char *sh = buf + shoff + (uint64_t)shentsize * i;
      uint32_t nameoff = r32(sh);
      if (nameoff >= strsz) continue;
      if (!strcmp(strtab + nameoff, ".comment")) {
        uint64_t off = r64(sh + 24), size = r64(sh + 32);
        if (off + size <= (uint64_t)fsz) {
          size_t n = size < outlen - 1 ? (size_t)size : outlen - 1;
          memcpy(out, buf + off, n);
          out[n] = 0;
          rc = 0;
        }
        break;
      }
    }
  } while (0);

  free(buf);
  fclose(f);
  return rc;
}

static int read_file_mem(const char *path, unsigned char **out, size_t *outsz) {
  FILE *f = fopen(path, "rb");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long sz = ftell(f);
  if (sz < 0) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  unsigned char *b = (unsigned char *)malloc((size_t)sz);
  if (!b && sz > 0) { fclose(f); return -1; }
  if (sz > 0 && fread(b, 1, (size_t)sz, f) != (size_t)sz) { free(b); fclose(f); return -1; }
  fclose(f);
  *out = b;
  *outsz = (size_t)sz;
  return 0;
}

static void add_libdir(const char *dir) {
  char **n = (char **)realloc(g_libdirs, (size_t)(g_nlibdirs + 1) * sizeof(char *));
  if (!n) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  g_libdirs = n;
  g_libdirs[g_nlibdirs++] = strdup(dir);
}

static int load_archive_path(const char *path) {
  unsigned char *buf = NULL;
  size_t sz = 0;
  if (read_file_mem(path, &buf, &sz) != 0) return -1;
  if (!is_ar(buf, sz)) {
    free(buf);
    WALL_LOGE("linker", "'%s' is not a static archive (Wall does not link shared libraries)\n", path);
    return -1;
  }
  if (add_archive(path, buf, sz) != 0) { free(buf); return -1; }
  return 0;
}

static int load_library(const char *name) {
  char cand[4096];
  if (strchr(name, '/')) return load_archive_path(name);
  for (int pass = 0; pass < 2; pass++) {
    const char *suffix = pass == 0 ? ".a" : ".so";
    for (int i = 0; i < g_nlibdirs; i++) {
      if (g_sysroot && g_sysroot[0])
        snprintf(cand, sizeof(cand), "%s%s/lib%s%s", g_sysroot, g_libdirs[i], name, suffix);
      else
        snprintf(cand, sizeof(cand), "%s/lib%s%s", g_libdirs[i], name, suffix);
      if (access(cand, R_OK) == 0) {
        if (pass == 1) {
          WALL_LOGE("linker", "'%s' is a shared library; the Wall linker produces fully static binaries\n", cand);
          return -1;
        }
        return load_archive_path(cand);
      }
    }
  }
  WALL_LOGE("linker", "cannot find -l%s (searched %d librar%s directories)\n",
            name, g_nlibdirs, g_nlibdirs == 1 ? "y" : "ies");
  return -1;
}

static void add_required(const char *sym) {
  char **n = (char **)realloc(g_required, (size_t)(g_nrequired + 1) * sizeof(char *));
  if (!n) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  g_required = n;
  g_required[g_nrequired++] = strdup(sym);
}

static void add_allow_undef(const char *sym) {
  char **n = (char **)realloc(g_allow_undef, (size_t)(g_nallow_undef + 1) * sizeof(char *));
  if (!n) { WALL_LOGE("linker", "out of memory\n"); exit(1); }
  g_allow_undef = n;
  g_allow_undef[g_nallow_undef++] = strdup(sym);
}

static int read_retain_file(const char *path) {
  FILE *f = fopen(path, "r");
  if (!f) { WALL_LOGE("linker", "cannot open retain file '%s'\n", path); return -1; }
  char line[512];
  while (fgets(line, sizeof(line), f)) {
    char *p = line;
    while (*p == ' ' || *p == '\t') p++;
    char *e = p + strlen(p);
    while (e > p && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ' || e[-1] == '\t')) *--e = 0;
    if (!*p) continue;
    char **n = (char **)realloc(g_retain, (size_t)(g_nretain + 1) * sizeof(char *));
    if (!n) { fclose(f); WALL_LOGE("linker", "out of memory\n"); return -1; }
    g_retain = n;
    g_retain[g_nretain++] = strdup(p);
  }
  fclose(f);
  g_strip = 0;   /* retaining an explicit list implies a symbol table */
  return 0;
}

static int parse_z_keyword(const char *kw) {
  if (!strcmp(kw, "defs") || !strcmp(kw, "no-undefined")) { g_no_undefined = 1; return 0; }
  if (!strcmp(kw, "muldefs")) { g_allow_multiple_definition = 1; return 0; }
  if (!strcmp(kw, "noexecstack")) { g_execstack = 0; return 0; }
  if (!strcmp(kw, "execstack")) { g_execstack = 1; return 0; }
  if (!strcmp(kw, "now") || !strcmp(kw, "lazy") || !strcmp(kw, "relro") ||
      !strcmp(kw, "norelro") || !strcmp(kw, "combreloc") || !strcmp(kw, "nocombreloc") ||
      !strcmp(kw, "text") || !strcmp(kw, "notext") || !strcmp(kw, "nodefaultlib") ||
      !strcmp(kw, "nodelete") || !strcmp(kw, "nocopyreloc") ||
      !strcmp(kw, "separate-code") || !strcmp(kw, "noseparate-code") ||
      !strcmp(kw, "ibt") || !strcmp(kw, "shstk") || !strcmp(kw, "cet-report") ||
      !strcmp(kw, "pack-relative-relocs") || !strcmp(kw, "nopack-relative-relocs")) {
    WALL_LOGW("linker", "'-z %s' has no effect on a fully static Wall executable\n", kw);
    return 0;
  }
  WALL_LOGE("linker", "unsupported -z keyword '%s'\n", kw);
  return -1;
}

static void print_link_stats(void) {
  unsigned long long insns = 0, relocs = 0;
  size_t text = g_text.len, ro = g_ro.len, data = g_data.len, got = g_got.len;
  for (int k = 0; k < g_nobjs; k++) {
    Obj *o = &g_objs[k];
    for (int i = 1; i < o->shnum; i++) {
      const unsigned char *sh = sh_ptr(o, i);
      if (r32(sh + 4) == SHT_RELA) relocs += r64(sh + 32) / 24;
    }
  }
  insns = (unsigned long long)(text / 4);
  fprintf(stderr, "linker: stats: %d object(s), %d symbol(s), %llu relocation(s)\n",
          g_nobjs, g_nsym, relocs);
  fprintf(stderr, "linker: stats: text=%zu rodata=%zu data=%zu got=%zu bss=%llu (~%llu insns)\n",
          text, ro, data, got, (unsigned long long)g_bss, insns);
}

static void usage(FILE *f) {
  fprintf(f,
    "wall version 1.0.0 - linker\n"
    "Usage: linker <input.o...> [-o <output>] [options]\n"
    "\n"
    "Self-contained static linker for AArch64 ELF objects.  It uses no\n"
    "external tool and links the Wall freestanding runtime (no libc).\n"
    "\n"
    "Options:\n"
    "  -o <file>            Output file (default: a.out)\n"
    "  -e, --entry <sym>    Entry symbol (default: _start)\n"
    "  --shared             Produce a shared object (ET_DYN)\n"
    "  -r, --relocatable    Produce a relocatable object (ET_REL)\n"
    "  --symbols            Keep .symtab/.strtab (default: stripped)\n"
    "  --map=<file>         Write a layout and symbol map\n"
    "  -u <sym>             Force <sym> to be undefined (pulls archives)\n"
    "  --defsym=S=VAL       Define absolute symbol S with value VAL\n"
    "  --whole-archive      Load every member of following archives\n"
    "  --no-whole-archive   Restore lazy archive loading\n"
    "  --build-id[=style]   Add a .note.gnu.build-id section\n"
    "  --no-runtime         Do not link the built-in Wall runtime\n"
    "  --runtime=<file>     Add an extra runtime object\n"
    "  --comment=<text>     Set the .comment section text\n"
    "  --stamp-comment=<t>  Rewrite .comment in existing ELF files (no link)\n"
    "  --get-comment        Print the .comment of an existing ELF file\n"
    "  -v, --verbose        Print the layout\n"
    "  --list-relocs[=all]  List ELF relocation names (all architectures)\n"
    "  -h, --help           Print this help\n"
    "  -V, --version        Print version information\n");
}

int linker_main(int argc, char **argv) {
  const char *out = "a.out";
  const char *entry_name = "_start";
  const char *comment = NULL;
  const char *extra_rt = NULL;
  int no_runtime = 0;
  int verbose = 0;
  int relocatable = 0;
  const char *stamp_comment = NULL;
  int get_comment = 0;
  const char **inputs = (const char **)calloc((size_t)(argc > 0 ? argc : 1), sizeof(char *));
  int ninputs = 0;
  const char **forced = (const char **)calloc((size_t)(argc > 0 ? argc : 1) * 2, sizeof(char *));
  int nforced = 0;
  const char **defsyms = (const char **)calloc((size_t)(argc > 0 ? argc : 1) * 2, sizeof(char *));
  uint64_t *defvals = (uint64_t *)calloc((size_t)(argc > 0 ? argc : 1) * 2, sizeof(uint64_t));
  int ndefsyms = 0;

  for (int i = 1; i < argc; i++) {
    const char *a = argv[i];

    if (!strcmp(a, "-h") || !strcmp(a, "--help")) { usage(stdout); return 0; }
    if (!strcmp(a, "--help=all")) { wall_print_options(stdout, "ld"); return 0; }
    if (!strcmp(a, "-V") || !strcmp(a, "--version")) { printf("wall version 1.0.0\n"); return 0; }
    if (!strcmp(a, "--list-relocs")) { wall_list_relocs(0); return 0; }
    if (!strcmp(a, "--list-relocs=all")) { wall_list_relocs(1); return 0; }

    if (!strcmp(a, "-o") || !strcmp(a, "--output")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a file\n", a); return 1; }
      out = argv[++i]; continue;
    }
    if (!strncmp(a, "-o", 2) && a[2]) { out = a + 2; continue; }
    if (!strcmp(a, "-e") || !strcmp(a, "--entry")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a symbol\n", a); return 1; }
      entry_name = argv[++i]; continue;
    }
    if (!strncmp(a, "--entry=", 8)) { entry_name = a + 8; continue; }
    if (!strcmp(a, "--shared") || !strcmp(a, "-shared") || !strcmp(a, "-Bshareable")) { g_shared = 1; continue; }
    if (!strcmp(a, "-pie") || !strcmp(a, "--pie")) { g_shared = 1; continue; }
    if (!strcmp(a, "-r") || !strcmp(a, "--relocatable")) { relocatable = 1; continue; }
    if (!strcmp(a, "--no-runtime")) { no_runtime = 1; continue; }
    if (!strcmp(a, "--strip") || !strcmp(a, "-s") || !strcmp(a, "--strip-all")) { g_strip = 1; continue; }
    if (!strcmp(a, "--symbols") || !strcmp(a, "--debug")) { g_strip = 0; continue; }
    if (!strcmp(a, "--strip-debug") || !strcmp(a, "-S") || !strcmp(a, "--strip-discarded") ||
        !strcmp(a, "--no-strip-discarded")) { /* Wall emits no debug/discarded sections. */ continue; }
    if (!strcmp(a, "--whole-archive")) { g_whole_archive = 1; continue; }
    if (!strcmp(a, "--no-whole-archive")) { g_whole_archive = 0; continue; }
    if (!strcmp(a, "-u") || !strcmp(a, "--undefined")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a symbol\n", a); return 1; }
      forced[nforced++] = argv[++i]; continue;
    }
    if (!strncmp(a, "-u", 2) && a[2]) { forced[nforced++] = a + 2; continue; }
    if (!strncmp(a, "--undefined=", 12)) { forced[nforced++] = a + 12; continue; }
    if (!strncmp(a, "--defsym=", 9)) {
      const char *spec = a + 9;
      const char *eq = strchr(spec, '=');
      if (!eq || eq == spec) { WALL_LOGE("linker", "bad --defsym '%s'\n", spec); return 1; }
      char *nm = (char *)malloc((size_t)(eq - spec) + 1);
      memcpy(nm, spec, (size_t)(eq - spec));
      nm[eq - spec] = 0;
      defsyms[ndefsyms] = nm;
      defvals[ndefsyms] = strtoull(eq + 1, NULL, 0);
      ndefsyms++; continue;
    }
    if (!strcmp(a, "--build-id")) { g_build_id = 1; continue; }
    if (!strncmp(a, "--build-id=", 11)) {
      const char *s = a + 11;
      if (!*s || !strcmp(s, "sha1")) g_build_id = 1;
      else if (!strcmp(s, "none")) g_build_id = 0;
      else { WALL_LOGE("linker", "unsupported build-id style '%s' (only sha1)\n", s); return 1; }
      continue;
    }
    if (!strcmp(a, "--no-build-id")) { g_build_id = 0; continue; }
    if (!strcmp(a, "--map") || !strcmp(a, "-Map")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a file\n", a); return 1; }
      g_map_path = argv[++i]; continue;
    }
    if (!strncmp(a, "--map=", 6)) { g_map_path = a + 6; continue; }
    if (!strncmp(a, "-Map=", 5)) { g_map_path = a + 5; continue; }
    if (!strcmp(a, "-M") || !strcmp(a, "--print-map")) { g_map_stdout = 1; continue; }
    if (!strncmp(a, "--runtime=", 10)) { extra_rt = a + 10; continue; }
    if (!strncmp(a, "--comment=", 10)) { comment = a + 10; continue; }
    if (!strncmp(a, "--stamp-comment=", 16)) { stamp_comment = a + 16; continue; }
    if (!strcmp(a, "--stamp-comment")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "--stamp-comment needs text\n"); return 1; }
      stamp_comment = argv[++i]; continue;
    }
    if (!strcmp(a, "--get-comment")) { get_comment = 1; continue; }
    if (!strcmp(a, "-v") || !strcmp(a, "--verbose")) { verbose = 1; continue; }
    if (!strcmp(a, "--trace") || !strcmp(a, "-t")) { g_trace = 1; continue; }
    if (!strcmp(a, "--no-trace")) { g_trace = 0; continue; }
    if (!strcmp(a, "--stats")) { g_stats = 1; continue; }
    if (!strcmp(a, "--print-memory-usage")) { g_print_memory_usage = 1; continue; }
    if (!strcmp(a, "--print-output-format")) { g_print_output_format = 1; continue; }
    if (!strncmp(a, "-O", 2)) { continue; } /* accepted optimisation hint */

    if (!strcmp(a, "-Map") || !strncmp(a, "-Map=", 5)) { /* handled above */ }
    if (!strncmp(a, "-Map", 4) && a[4]) { g_map_path = a + 4; continue; }
    if (!strncmp(a, "-z", 2) && a[2] == 0) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "-z needs a keyword\n"); return 1; }
      if (parse_z_keyword(argv[++i]) != 0) return 1;
      continue;
    }
    if (!strncmp(a, "-z", 2) && a[2]) { if (parse_z_keyword(a + 2) != 0) return 1; continue; }

    if (!strcmp(a, "-static") || !strcmp(a, "-Bstatic") || !strcmp(a, "-nostdlib") ||
        !strcmp(a, "-nodefaultlibs") || !strcmp(a, "-nostartfiles")) continue;
    if (!strcmp(a, "-Bdynamic")) { WALL_LOGE("linker", "dynamic linking is not supported; Wall links static executables\n"); return 1; }

    if (!strcmp(a, "-L") || !strcmp(a, "--library-path")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a directory\n", a); return 1; }
      add_libdir(argv[++i]); continue;
    }
    if (!strncmp(a, "-L", 2) && a[2]) { add_libdir(a + 2); continue; }
    if (!strncmp(a, "--library-path=", 16)) { add_libdir(a + 16); continue; }
    if (!strcmp(a, "-Y")) { if (i + 1 >= argc) { WALL_LOGE("linker", "-Y needs a path\n"); return 1; } add_libdir(argv[++i]); continue; }
    if (!strncmp(a, "-Y", 2) && a[2]) { add_libdir(a + 2); continue; }
    if (!strcmp(a, "-l") || !strcmp(a, "--library")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a library name\n", a); return 1; }
      if (load_library(argv[++i]) != 0) return 1;
      continue;
    }
    if (!strncmp(a, "-l", 2) && a[2]) { if (load_library(a + 2) != 0) return 1; continue; }
    if (!strncmp(a, "--library=", 10)) { if (load_library(a + 10) != 0) return 1; continue; }
    if (!strcmp(a, "--sysroot")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "--sysroot needs a directory\n"); return 1; }
      g_sysroot = argv[++i]; continue;
    }
    if (!strncmp(a, "--sysroot=", 10)) { g_sysroot = a + 10; continue; }

    if (!strcmp(a, "--push-state")) {
      if (g_nstate_stack >= 32) { WALL_LOGE("linker", "option state stack overflow\n"); return 1; }
      g_state_stack[g_nstate_stack].whole = g_whole_archive;
      g_state_stack[g_nstate_stack].execstack = g_execstack;
      g_nstate_stack++; continue;
    }
    if (!strcmp(a, "--pop-state")) {
      if (g_nstate_stack <= 0) { WALL_LOGE("linker", "--pop-state without --push-state\n"); return 1; }
      g_nstate_stack--;
      g_whole_archive = g_state_stack[g_nstate_stack].whole;
      g_execstack = g_state_stack[g_nstate_stack].execstack;
      continue;
    }

    if (!strcmp(a, "--allow-multiple-definition") || !strcmp(a, "-z")) {
      if (!strcmp(a, "-z")) {
        if (i + 1 >= argc) { WALL_LOGE("linker", "-z needs a keyword\n"); return 1; }
        if (parse_z_keyword(argv[++i]) != 0) return 1;
      } else g_allow_multiple_definition = 1;
      continue;
    }
    if (!strcmp(a, "--no-undefined")) { g_no_undefined = 1; continue; }
    if (!strcmp(a, "--define-common") || !strcmp(a, "-d")) { g_define_common = 1; continue; }
    if (!strcmp(a, "--no-define-common")) { g_define_common = 0; continue; }
    if (!strcmp(a, "--fatal-warnings")) { g_fatal_warnings = 1; continue; }
    if (!strcmp(a, "--no-fatal-warnings")) { g_fatal_warnings = 0; continue; }
    if (!strcmp(a, "--no-warnings")) { g_no_warnings = 1; continue; }
    if (!strcmp(a, "--warn-unresolved-symbols")) { g_warn_unresolved = 1; continue; }
    if (!strcmp(a, "--no-warn-unresolved-symbols")) { g_warn_unresolved = 0; continue; }
    if (!strcmp(a, "--warn-common")) { g_warn_common = 1; continue; }
    if (!strcmp(a, "--no-warn-common")) { g_warn_common = 0; continue; }
    if (!strcmp(a, "--sort-common")) { g_sort_common = 1; continue; }
    if (!strcmp(a, "--no-sort-common")) { g_sort_common = 0; continue; }
    if (!strcmp(a, "--require-defined")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "--require-defined needs a symbol\n"); return 1; }
      add_required(argv[++i]); continue;
    }
    if (!strncmp(a, "--require-defined=", 18)) { add_required(a + 18); continue; }
    if (!strcmp(a, "--retain-symbols-file")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "--retain-symbols-file needs a file\n"); return 1; }
      g_retain_file = argv[++i];
      if (read_retain_file(g_retain_file) != 0) return 1;
      continue;
    }
    if (!strncmp(a, "--retain-symbols-file=", 22)) {
      g_retain_file = a + 22;
      if (read_retain_file(g_retain_file) != 0) return 1;
      continue;
    }
    if (!strcmp(a, "-U") || !strcmp(a, "--allow-undefined")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a symbol\n", a); return 1; }
      add_allow_undef(argv[++i]); continue;
    }
    if (!strncmp(a, "-U", 2) && a[2]) { add_allow_undef(a + 2); continue; }
    if (!strcmp(a, "--error-execstack")) { g_error_execstack = 1; continue; }
    if (!strcmp(a, "--no-error-execstack")) { g_error_execstack = 0; continue; }
    if (!strcmp(a, "--warn-execstack") || !strcmp(a, "--warn-execstack-objects")) { g_warn_execstack = 1; continue; }
    if (!strcmp(a, "--no-warn-execstack")) { g_warn_execstack = 0; continue; }
    if (!strcmp(a, "--execstack")) { g_execstack = 1; continue; }
    if (!strcmp(a, "--noexecstack")) { g_execstack = 0; continue; }
    if (!strcmp(a, "--check-sections") || !strcmp(a, "--no-check-sections") ||
        !strcmp(a, "--error-rwx-segments") || !strcmp(a, "--no-error-rwx-segments") ||
        !strcmp(a, "--warn-rwx-segments") || !strcmp(a, "--no-warn-rwx-segments") ||
        !strcmp(a, "--rosegment") || !strcmp(a, "--keep-memory") || !strcmp(a, "--no-keep-memory") ||
        !strcmp(a, "--reduce-memory-overheads") || !strcmp(a, "--threads") ||
        !strcmp(a, "--no-threads") || !strcmp(a, "--no-relax") || !strcmp(a, "--no-gc-sections") ||
        !strcmp(a, "--no-emit-relocs") || !strcmp(a, "--no-gnu-unique") ||
        !strcmp(a, "--no-omagic") || !strcmp(a, "--no-warn-shared-textrel") ||
        !strcmp(a, "--no-warn-constructors") || !strcmp(a, "--no-warn-multiple-gp")) continue;
    if (!strcmp(a, "--no-export-dynamic")) continue;

    if (!strcmp(a, "--oformat")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "--oformat needs a format\n"); return 1; }
      g_oformat = argv[++i];
      if (strcmp(g_oformat, "elf64-aarch64") && strcmp(g_oformat, "elf64-littleaarch64") && strcmp(g_oformat, "default"))
        { WALL_LOGE("linker", "unsupported output format '%s'\n", g_oformat); return 1; }
      continue;
    }
    if (!strncmp(a, "--oformat=", 10)) {
      g_oformat = a + 10;
      if (strcmp(g_oformat, "elf64-aarch64") && strcmp(g_oformat, "elf64-littleaarch64") && strcmp(g_oformat, "default"))
        { WALL_LOGE("linker", "unsupported output format '%s'\n", g_oformat); return 1; }
      continue;
    }
    if (!strcmp(a, "-b") || !strcmp(a, "--format")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs a format\n", a); return 1; }
      const char *f = argv[++i];
      if (strcmp(f, "elf64") && strcmp(f, "elf64-littleaarch64") && strcmp(f, "default"))
        { WALL_LOGE("linker", "unsupported input format '%s'\n", f); return 1; }
      continue;
    }
    if (!strcmp(a, "-A") || !strcmp(a, "--architecture")) {
      if (i + 1 >= argc) { WALL_LOGE("linker", "%s needs an architecture\n", a); return 1; }
      const char *arch = argv[++i];
      if (strcmp(arch, "aarch64") && strcmp(arch, "aarch64linux") && strcmp(arch, "arm64"))
        { WALL_LOGE("linker", "unsupported architecture '%s'\n", arch); return 1; }
      continue;
    }
    if (!strncmp(a, "--target=", 9)) {
      const char *t = a + 9;
      if (strncmp(t, "aarch64", 7) != 0 && strncmp(t, "arm64", 5) != 0)
        { WALL_LOGE("linker", "unsupported target '%s'\n", t); return 1; }
      continue;
    }

    if (!strcmp(a, "-T") || !strcmp(a, "--script") || !strcmp(a, "--mri-script") ||
        !strcmp(a, "--plugin") || !strcmp(a, "--plugin-opt") || !strcmp(a, "--just-symbols") ||
        !strcmp(a, "-R") || !strcmp(a, "--wrap") || !strcmp(a, "--exclude-libs") ||
        !strcmp(a, "-F") || !strcmp(a, "--filter") || !strcmp(a, "--no-filter") ||
        !strcmp(a, "-f") || !strcmp(a, "--auxiliary") || !strcmp(a, "--audit") ||
        !strcmp(a, "--dynamic-linker") || !strcmp(a, "--soname") || !strcmp(a, "--rpath") ||
        !strcmp(a, "--rpath-link") || !strcmp(a, "--enable-new-dtags") ||
        !strcmp(a, "--disable-new-dtags") || !strcmp(a, "-E") || !strcmp(a, "--export-dynamic") ||
        !strcmp(a, "--dynamic-list") || !strcmp(a, "--dynamic-list-cpp-new") ||
        !strcmp(a, "--dynamic-list-cpp-typeinfo") || !strcmp(a, "--version-script") ||
        !strcmp(a, "--allow-shlib-undefined") || !strcmp(a, "--no-allow-shlib-undefined") ||
        !strcmp(a, "--hash-style") || !strcmp(a, "--hash-size") || !strcmp(a, "--spare-dynamic-tags") ||
        !strcmp(a, "--unique") || !strcmp(a, "--gnu-unique") || !strcmp(a, "--default-imported-symver") ||
        !strcmp(a, "--no-default-imported-symver") || !strcmp(a, "--no-undefined-version") ||
        !strcmp(a, "--undefined-version") || !strcmp(a, "--orphan-handling") ||
        !strcmp(a, "--package-metadata") || !strcmp(a, "--compress-debug-sections") ||
        !strcmp(a, "--no-compress-debug-sections") || !strcmp(a, "--cref") || !strcmp(a, "--no-cref") ||
        !strcmp(a, "--demangle") || !strcmp(a, "--no-demangle") || !strcmp(a, "--gc-sections") ||
        !strcmp(a, "--gc-keep-exported") || !strcmp(a, "--emit-relocs") || !strcmp(a, "-q") ||
        !strcmp(a, "--sort-section") || !strcmp(a, "--section-start") || !strcmp(a, "-Ttext") ||
        !strcmp(a, "-Tdata") || !strcmp(a, "-Tbss") || !strcmp(a, "-Ttext-segment") ||
        !strcmp(a, "--section-alignment") || !strcmp(a, "--max-page-size") ||
        !strcmp(a, "--common-page-size") || !strcmp(a, "--noinhibit-exec") ||
        !strcmp(a, "--omagic") || !strcmp(a, "-N") || !strcmp(a, "--nmagic") || !strcmp(a, "-n") ||
        !strcmp(a, "-i") || !strcmp(a, "--relax") || !strcmp(a, "--no-rosegment") ||
        !strcmp(a, "-C") || !strcmp(a, "--no-warn-section-align") || !strcmp(a, "--warn-section-align") ||
        !strcmp(a, "--warn-constructors") || !strcmp(a, "--warn-multiple-gp") ||
        !strcmp(a, "--warn-shared-textrel")) {
      WALL_LOGE("linker", "option '%s' is not implemented by the Wall linker\n", a);
      return 1;
    }

    if (a[0] == '-' && a[1] && strcmp(a, "-")) {
      int sup = 0;
      if (wall_option_known("ld", a, &sup))
        WALL_LOGE("linker", "option '%s' is listed in the catalogue but is not implemented\n", a);
      else
        WALL_LOGE("linker", "unknown option '%s'\n", a);
      return 1;
    }
    inputs[ninputs++] = argv[i];
  }
  (void)g_no_undefined;
  (void)g_trace;
  (void)g_stats;
  (void)g_print_output_format;
  (void)g_require_defined;
  (void)g_retain_file;
  (void)g_maplines;
  (void)g_nmaplines;

  if (g_print_output_format) { printf("elf64-aarch64\n"); return 0; }

  if (get_comment) {
    if (ninputs != 1) { WALL_LOGE("linker", "--get-comment needs one file\n"); return 1; }
    char buf[1024];
    if (elf_comment_get(inputs[0], buf, sizeof(buf)) != 0) {
      WALL_LOGE("linker", "no .comment in '%s'\n", inputs[0]);
      return 1;
    }
    printf("%s\n", buf);
    return 0;
  }

  if (stamp_comment) {
    if (ninputs == 0) { WALL_LOGE("linker", "--stamp-comment needs at least one file\n"); return 1; }
    int rc = 0;
    for (int i = 0; i < ninputs; i++) {
      int r = elf_comment_set(inputs[i], stamp_comment);
      if (r == -2) { WALL_LOGE("linker", "'%s' has no usable .comment section\n", inputs[i]); rc = 1; }
      else if (r != 0) { WALL_LOGE("linker", "cannot edit '%s'\n", inputs[i]); rc = 1; }
    }
    return rc;
  }

  if (ninputs == 0) { WALL_LOGE("linker", "no input files\n"); return 1; }

  for (int i = 0; i < ninputs; i++) {
    if (g_trace) fprintf(stderr, "linker: input %s\n", inputs[i]);
    FILE *f = fopen(inputs[i], "rb");
    if (!f) { WALL_LOGE("linker", "cannot open '%s'\n", inputs[i]); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); WALL_LOGE("linker", "cannot read '%s'\n", inputs[i]); return 1; }
    fclose(f);
    if (is_ar(buf, (size_t)sz)) {
      if (add_archive(inputs[i], buf, (size_t)sz) != 0) { WALL_LOGE("linker", "invalid archive '%s'\n", inputs[i]); return 1; }
    } else if (add_object(inputs[i], buf, (size_t)sz, 1) != 0) {
      WALL_LOGE("linker", "invalid object '%s'\n", inputs[i]); return 1;
    }
  }

  if (extra_rt) {
    FILE *f = fopen(extra_rt, "rb");
    if (!f) { WALL_LOGE("linker", "cannot open runtime '%s'\n", extra_rt); return 1; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)sz);
    if (!buf || fread(buf, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(buf); WALL_LOGE("linker", "cannot read runtime '%s'\n", extra_rt); return 1; }
    fclose(f);
    if (add_object(extra_rt, buf, (size_t)sz, 1) != 0) { WALL_LOGE("linker", "invalid runtime '%s'\n", extra_rt); return 1; }
  }

  if (!no_runtime && !relocatable) {
    if (add_object("<wallrt>", wallrt_obj_bytes, wallrt_obj_len, 0) != 0) { WALL_LOGE("linker", "embedded runtime is corrupt\n"); return 1; }
    if (add_object("<start>", start_obj_bytes, start_obj_len, 0) != 0) { WALL_LOGE("linker", "embedded start object is corrupt\n"); return 1; }
  }

  for (int k = 0; k < g_nobjs; k++) {
    if (!g_objs[k].execstack) continue;
    if (g_error_execstack) {
      lerror("'%s' requires an executable stack", g_objs[k].name);
      return 1;
    }
    if (g_warn_execstack) lwarn("'%s' requires an executable stack", g_objs[k].name);
  }

  for (int i = 0; i < nforced; i++) undef_add(forced[i]);
  if (resolve_archives() != 0) { WALL_LOGE("linker", "archive resolution failed\n"); return 1; }

  if (relocatable) {
    const char *cmt = (comment && comment[0]) ? comment : WALL_DEFAULT_COMMENT;
    return write_relocatable(out, cmt);
  }

  if (layout_objects() != 0) return 1;
  prepare_build_id();
  if (assign_addresses() != 0) return 1;
  if (register_symbols() != 0) return 1;
  for (int i = 0; i < ndefsyms; i++) sym_add(defsyms[i], defvals[i], 1);
  if (got_scan() != 0) return 1;
  build_got();
  allocate_commons();
  for (int i = 0; i < g_nrequired; i++) {
    SymEnt *e = sym_find(g_required[i]);
    if (!e || !e->defined) lerror("required symbol '%s' is not defined", g_required[i]);
  }
  if (g_had_error) return 1;
  if (do_relocations() != 0) return 1;
  if (g_had_error) return 1;

  uint64_t entry = 0;
  SymEnt *es = sym_find(entry_name);
  if (es) entry = es->addr;
  else if (!g_shared) WALL_LOGW("linker", "entry symbol '%s' not found\n", entry_name);

  if (verbose) {
    fprintf(stderr, "linker: text=%zu rodata=%zu data=%zu bss=%llu\n",
            g_text.len, g_ro.len, g_data.len, (unsigned long long)g_bss);
    fprintf(stderr, "linker: text@0x%llx rodata@0x%llx data@0x%llx bss@0x%llx entry=0x%llx\n",
            (unsigned long long)g_text_addr, (unsigned long long)g_ro_addr,
            (unsigned long long)g_data_addr, (unsigned long long)g_bss_addr,
            (unsigned long long)entry);
  }

  const char *cmt = (comment && comment[0]) ? comment : WALL_DEFAULT_COMMENT;
  if (write_output(out, cmt, entry, g_shared) != 0) return 1;
  if (g_stats) print_link_stats();
  return 0;
}

/* Entry point used by the merged wall driver and by the linker symlink. */
int wall_link(int argc, char **argv) { return linker_main(argc, argv); }

#ifndef WALL_MULTICALL
int main(int argc, char **argv) { return linker_main(argc, argv); }
#endif
