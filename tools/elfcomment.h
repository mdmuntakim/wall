/* elfcomment.h - Wall ELF .comment writer.
 *
 * A tiny, dependency-free ELF64 section editor used by the Wall toolchain.
 * It replaces the contents of the `.comment` section of an ELF file in place
 * with a caller supplied string, so that binaries produced by the Wall
 * toolchain carry the Wall compiler identity instead of a host compiler's.
 *
 * The routine works directly on the section header table:
 *
 *   1. validate the ELF magic and class,
 *   2. locate the section name string table (e_shstrndx),
 *   3. find the section whose name is ".comment",
 *   4. overwrite its bytes and update sh_size,
 *   5. write the file back.
 *
 * It never moves other sections, so program headers and loadable segments are
 * untouched and the resulting binary still runs.
 */
#ifndef WALL_ELFCOMMENT_H
#define WALL_ELFCOMMENT_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint16_t wall_rd16(const unsigned char *p) { uint16_t v; memcpy(&v, p, 2); return v; }
static uint32_t wall_rd32(const unsigned char *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static uint64_t wall_rd64(const unsigned char *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static void wall_wr32(unsigned char *p, uint32_t v) { memcpy(p, &v, 4); }
static void wall_wr64(unsigned char *p, uint64_t v) { memcpy(p, &v, 8); }

/* wall_elf_set_comment:
 *   path     - ELF file to edit (must be writable)
 *   comment  - NUL terminated comment string
 *   returns 0 on success, -1 on I/O or parse error, -2 when the file has no
 *   ".comment" section or the existing section is too small.
 */
static int wall_elf_set_comment(const char *path, const char *comment) {
  FILE *f = fopen(path, "rb+");
  if (!f) return -1;
  if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return -1; }
  long fsz = ftell(f);
  if (fsz < 64) { fclose(f); return -1; }
  if (fseek(f, 0, SEEK_SET) != 0) { fclose(f); return -1; }
  unsigned char *buf = (unsigned char *)malloc((size_t)fsz);
  if (!buf) { fclose(f); return -1; }
  if (fread(buf, 1, (size_t)fsz, f) != (size_t)fsz) { free(buf); fclose(f); return -1; }

  if (buf[0] != 0x7f || buf[1] != 'E' || buf[2] != 'L' || buf[3] != 'F') { free(buf); fclose(f); return -1; }
  if (buf[4] != 2 || buf[5] != 1) { /* only ELF64 little-endian */ free(buf); fclose(f); return -1; }

  uint64_t shoff = wall_rd64(buf + 40);
  uint16_t shentsize = wall_rd16(buf + 58);
  uint16_t shnum = wall_rd16(buf + 60);
  uint16_t shstrndx = wall_rd16(buf + 62);
  if (shoff == 0 || shentsize < 64 || shnum == 0 || shstrndx >= shnum) { free(buf); fclose(f); return -1; }
  if (shoff + (uint64_t)shentsize * shnum > (uint64_t)fsz) { free(buf); fclose(f); return -1; }

  unsigned char *strsh = buf + shoff + (uint64_t)shentsize * shstrndx;
  uint64_t stroff = wall_rd64(strsh + 24);
  uint64_t strsz = wall_rd64(strsh + 32);
  if (stroff + strsz > (uint64_t)fsz) { free(buf); fclose(f); return -1; }
  const char *strtab = (const char *)(buf + stroff);

  int found = -1;
  for (uint16_t i = 0; i < shnum; i++) {
    unsigned char *sh = buf + shoff + (uint64_t)shentsize * i;
    uint32_t nameoff = wall_rd32(sh + 0);
    if (nameoff >= strsz) continue;
    const char *nm = strtab + nameoff;
    if (strcmp(nm, ".comment") == 0) { found = (int)i; break; }
  }
  if (found < 0) { free(buf); fclose(f); return -2; }

  unsigned char *sh = buf + shoff + (uint64_t)shentsize * found;
  uint64_t off = wall_rd64(sh + 24);
  uint64_t size = wall_rd64(sh + 32);
  size_t plen = strlen(comment) + 1;
  if (off + size > (uint64_t)fsz || size < plen) { free(buf); fclose(f); return -2; }

  memset(buf + off, 0, (size_t)size);
  memcpy(buf + off, comment, plen);
  wall_wr64(sh + 32, (uint64_t)plen);

  if (fseek(f, 0, SEEK_SET) != 0) { free(buf); fclose(f); return -1; }
  size_t wrote = fwrite(buf, 1, (size_t)fsz, f);
  fflush(f);
  free(buf);
  fclose(f);
  return wrote == (size_t)fsz ? 0 : -1;
}

/* wall_elf_get_comment:
 *   Copies the .comment payload into out (NUL terminated).  Returns 0 on
 *   success, -1 on error.  Useful for self-checks in the toolchain.
 */
static int wall_elf_get_comment(const char *path, char *out, size_t outlen) {
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
  if (buf[0] == 0x7f && buf[1] == 'E' && buf[2] == 'L' && buf[3] == 'F' && buf[4] == 2) {
    uint64_t shoff = wall_rd64(buf + 40);
    uint16_t shentsize = wall_rd16(buf + 58);
    uint16_t shnum = wall_rd16(buf + 60);
    uint16_t shstrndx = wall_rd16(buf + 62);
    if (shoff && shentsize >= 64 && shnum && shstrndx < shnum && shoff + (uint64_t)shentsize * shnum <= (uint64_t)fsz) {
      unsigned char *strsh = buf + shoff + (uint64_t)shentsize * shstrndx;
      uint64_t stroff = wall_rd64(strsh + 24);
      uint64_t strsz = wall_rd64(strsh + 32);
      if (stroff + strsz <= (uint64_t)fsz) {
        const char *strtab = (const char *)(buf + stroff);
        for (uint16_t i = 0; i < shnum; i++) {
          unsigned char *sh = buf + shoff + (uint64_t)shentsize * i;
          uint32_t nameoff = wall_rd32(sh + 0);
          if (nameoff >= strsz) continue;
          if (strcmp(strtab + nameoff, ".comment") == 0) {
            uint64_t off = wall_rd64(sh + 24);
            uint64_t size = wall_rd64(sh + 32);
            if (off + size <= (uint64_t)fsz) {
              size_t n = size < outlen - 1 ? (size_t)size : outlen - 1;
              memcpy(out, buf + off, n);
              out[n] = 0;
              rc = 0;
            }
            break;
          }
        }
      }
    }
  }
  free(buf);
  fclose(f);
  return rc;
}

#endif /* WALL_ELFCOMMENT_H */
