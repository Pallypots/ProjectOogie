/* Project Oogie: DDS files for texture packs (see pc_dds.h).
 * The PCSX2 packs for this game (docs/HD_ASSET_ENHANCEMENTS.md) are BC7
 * with a DX10 header and full mip chains; older packs use DXT1/3/5 FourCCs.
 * Layout: "DDS ", a 124-byte header (height at +12, width at +16, mip count
 * at +28, pixel format at +76: flags +80, FourCC +84, bit count +88), then
 * a 20-byte DX10 header when the FourCC is "DX10" (DXGI format first). */
#include "pc_dds.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint32_t rd32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

void pc_dds_free(PcDds *d)
{
    if (d) { free(d->data); memset(d, 0, sizeof *d); }
}

/* b is malloc'd and n bytes long; it belongs to out afterwards (freed on failure). */
static int dds_parse(unsigned char *b, long n, PcDds *out)
{
    uint32_t off = 128, fl, cc, i, w, h, mips;
    if (n < 128 || memcmp(b, "DDS ", 4) || rd32(b + 4) != 124) { free(b); return 0; }
    h = rd32(b + 12); w = rd32(b + 16); mips = rd32(b + 28);
    fl = rd32(b + 80); cc = rd32(b + 84);
    if (!w || !h || w > 16384 || h > 16384) { free(b); return 0; }
    if (!mips) mips = 1;
    if (mips > PC_DDS_MAX_MIPS) mips = PC_DDS_MAX_MIPS;
    if (fl & 4) {                                   /* DDPF_FOURCC */
        if (cc == 0x31545844u) { out->dxgi = 71; out->block = 8; }        /* DXT1 -> BC1_UNORM */
        else if (cc == 0x33545844u) { out->dxgi = 74; out->block = 16; }  /* DXT3 -> BC2_UNORM */
        else if (cc == 0x35545844u) { out->dxgi = 77; out->block = 16; }  /* DXT5 -> BC3_UNORM */
        else if (cc == 0x30315844u) {                                     /* DX10 */
            uint32_t g;
            if (n < 148) { free(b); return 0; }
            g = rd32(b + 128);
            off = 148;
            switch (g) {
            case 70: case 71: case 72: out->dxgi = 71; out->block = 8; break;     /* BC1 */
            case 73: case 74: case 75: out->dxgi = 74; out->block = 16; break;    /* BC2 */
            case 76: case 77: case 78: out->dxgi = 77; out->block = 16; break;    /* BC3 */
            case 97: case 98: case 99: out->dxgi = 98; out->block = 16; break;    /* BC7 (sRGB read as UNORM, as PCSX2) */
            case 27: case 28: case 29: out->dxgi = 28; out->block = 0; break;     /* R8G8B8A8 */
            case 87: case 90: case 91: out->dxgi = 87; out->block = 0; break;     /* B8G8R8A8 */
            default: free(b); return 0;
            }
        } else { free(b); return 0; }
    } else if ((fl & 0x40) && rd32(b + 88) == 32) {   /* DDPF_RGB, 32 bits */
        out->dxgi = rd32(b + 92) == 0x00FF0000u ? 87 : 28;   /* red mask: B8G8R8A8 or R8G8B8A8 */
        out->block = 0;
    } else { free(b); return 0; }
    if (out->block && ((w & 3) || (h & 3))) { free(b); return 0; }   /* D3D11: level 0 in whole blocks */
    out->w = w; out->h = h; out->data = b;
    for (i = 0; i < mips; i++) {
        uint32_t lw = w >> i ? w >> i : 1, lh = h >> i ? h >> i : 1, pitch, rows;
        if (out->block) { pitch = ((lw + 3) / 4) * out->block; rows = (lh + 3) / 4; }
        else { pitch = lw * 4; rows = lh; }
        if ((uint64_t)off + (uint64_t)pitch * rows > (uint64_t)n) break;
        out->level[i] = b + off; out->pitch[i] = pitch;
        off += pitch * rows;
    }
    out->mips = i;
    if (!out->mips) { pc_dds_free(out); return 0; }
    return 1;
}

/* off/size: a DDS stored inside a bigger file (the game's .stm archives);
 * size 0 reads the whole file. */
int pc_dds_load_part(const char *path, uint32_t offset, uint32_t size, PcDds *out)
{
    FILE *f;
    long n;
    unsigned char *b;
    memset(out, 0, sizeof *out);
    f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END); n = ftell(f);
    if (size) {
        if ((long)offset + (long)size > n) { fclose(f); return 0; }
        n = (long)size;
    } else
        offset = 0;
    if (n < 128 || n > 512L * 1024 * 1024 || fseek(f, (long)offset, SEEK_SET)) { fclose(f); return 0; }
    b = (unsigned char *)malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return 0; }
    fclose(f);
    return dds_parse(b, n, out);
}

int pc_dds_load(const char *path, PcDds *out) { return pc_dds_load_part(path, 0, 0, out); }
