/* Project Oogie: DDS files for texture packs (pc_dds.c). The blocks are kept
 * as they are (BC1/BC2/BC3/BC7 or 32-bit), with every mip level, so the GPU
 * gets them without a decode. */
#ifndef PC_DDS_H
#define PC_DDS_H
#include <stdint.h>
#include <wchar.h>

#define PC_DDS_MAX_MIPS 16

typedef struct {
    uint32_t dxgi;              /* DXGI_FORMAT value */
    uint32_t w, h, mips;
    uint32_t block;             /* bytes per 4x4 block, 0 for 32-bit pixels */
    unsigned char *data;        /* the whole file; the levels point into it */
    const unsigned char *level[PC_DDS_MAX_MIPS];
    uint32_t pitch[PC_DDS_MAX_MIPS];
} PcDds;

/* Reads path; 0 when it is not a DDS this can hand to the GPU. */
int pc_dds_load(const char *path, PcDds *out);
/* A DDS stored size bytes at offset inside path (size 0: the whole file). */
int pc_dds_load_part(const char *path, uint32_t offset, uint32_t size, PcDds *out);
void pc_dds_free(PcDds *d);

/* path (UTF-8, or the ANSI code page when it is not valid UTF-8; relative
 * to the current folder or absolute) as an absolute "\\?\" path, which
 * Windows opens past the old 260-character limit: the PS2 packs' own folder
 * names take 130 characters, so a pack in mods\ was found but none of its
 * files could be opened. Returns 0 if it does not fit in n characters. */
#define PC_LONG_PATH 1024
int pc_path_wide(const char *path, wchar_t *out, int n);

#endif
