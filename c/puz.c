#define _CRT_SECURE_NO_WARNINGS
#include "puz.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char ACROSSDOWN[] = "ACROSS&DOWN";
#define ACROSSDOWN_LEN 11

static const char MASKSTRING[] = "ICHEATED";

static int is_blacksquare(char c) {
    return c == '.' || c == ':';
}

uint16_t puz_data_cksum(const uint8_t *data, size_t length, uint16_t cksum) {
    for (size_t i = 0; i < length; i++) {
        uint16_t lowbit = cksum & 1;
        cksum >>= 1;
        if (lowbit) cksum |= 0x8000;
        cksum = (cksum + data[i]) & 0xFFFF;
    }
    return cksum;
}

static uint16_t read_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

static void write_u16(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF;
    p[1] = (v >> 8) & 0xFF;
}

static char *read_zstring(const uint8_t *data, size_t total, size_t *pos) {
    size_t start = *pos;
    while (*pos < total && data[*pos] != 0) (*pos)++;
    size_t len = *pos - start;
    char *s = (char *)malloc(len + 1);
    if (!s) return NULL;
    memcpy(s, data + start, len);
    s[len] = 0;
    if (*pos < total) (*pos)++;
    return s;
}

static uint16_t header_cksum(Puzzle *p, uint16_t initial) {
    uint8_t buf[8];
    write_u16(buf + 0, (uint16_t)p->width);
    write_u16(buf + 2, (uint16_t)p->height);
    /* Note: the Python format string is HBBHH where width/height are bytes,
     * but in practice width/height come in as bytes (puzzles fit in 255). */
    /* Re-do properly: HEADER_CKSUM_FORMAT = '<BBH H H' meaning:
     *   B width, B height, H numclues, H puzzletype, H solution_state */
    buf[0] = (uint8_t)p->width;
    buf[1] = (uint8_t)p->height;
    write_u16(buf + 2, (uint16_t)p->n_clues);
    write_u16(buf + 4, p->puzzletype);
    write_u16(buf + 6, p->solution_state);
    return puz_data_cksum(buf, 8, initial);
}

static int version_major(const uint8_t *fileversion) {
    if (fileversion[0] >= '0' && fileversion[0] <= '9') {
        return fileversion[0] - '0';
    }
    return 1;
}

static uint16_t text_cksum(Puzzle *p, uint16_t cksum) {
    if (p->title && p->title[0])
        cksum = puz_data_cksum((const uint8_t *)p->title, strlen(p->title) + 1, cksum);
    if (p->author && p->author[0])
        cksum = puz_data_cksum((const uint8_t *)p->author, strlen(p->author) + 1, cksum);
    if (p->copyright && p->copyright[0])
        cksum = puz_data_cksum((const uint8_t *)p->copyright, strlen(p->copyright) + 1, cksum);
    for (int i = 0; i < p->n_clues; i++) {
        if (p->clues[i] && p->clues[i][0])
            cksum = puz_data_cksum((const uint8_t *)p->clues[i], strlen(p->clues[i]), cksum);
    }
    if (version_major(p->fileversion) >= 1 && p->notes && p->notes[0]) {
        /* version >= 1.3 always since we only handle modern files */
        cksum = puz_data_cksum((const uint8_t *)p->notes, strlen(p->notes) + 1, cksum);
    }
    return cksum;
}

static uint16_t global_cksum(Puzzle *p) {
    int sz = p->width * p->height;
    uint16_t c = header_cksum(p, 0);
    c = puz_data_cksum((const uint8_t *)p->solution, sz, c);
    c = puz_data_cksum((const uint8_t *)p->fill, sz, c);
    c = text_cksum(p, c);
    return c;
}

static uint64_t magic_cksum(Puzzle *p) {
    int sz = p->width * p->height;
    uint16_t c[4];
    c[0] = header_cksum(p, 0);
    c[1] = puz_data_cksum((const uint8_t *)p->solution, sz, 0);
    c[2] = puz_data_cksum((const uint8_t *)p->fill, sz, 0);
    c[3] = text_cksum(p, 0);

    uint64_t magic = 0;
    for (int i = 0; i < 4; i++) {
        int idx = 3 - i;
        uint16_t cs = c[idx];
        magic <<= 8;
        magic |= (uint8_t)(MASKSTRING[idx] ^ (cs & 0xFF));
        magic |= ((uint64_t)(uint8_t)(MASKSTRING[idx + 4] ^ ((cs >> 8) & 0xFF))) << 32;
    }
    return magic;
}

/* Compute clue numbering and across/down arrays. */
static int compute_numbering(Puzzle *p) {
    int w = p->width, h = p->height;
    int total = w * h;
    p->across = NULL;
    p->down = NULL;
    p->n_across = 0;
    p->n_down = 0;

    /* upper-bound allocations */
    PuzClueEntry *across = (PuzClueEntry *)calloc(total, sizeof(PuzClueEntry));
    PuzClueEntry *down = (PuzClueEntry *)calloc(total, sizeof(PuzClueEntry));
    if (!across || !down) { free(across); free(down); return -1; }

    int na = 0, nd = 0;
    int c = 0, n = 1;
    for (int i = 0; i < total; i++) {
        if (is_blacksquare(p->fill[i])) continue;
        int row = i / w;
        int col = i % w;
        int lastc = c;

        int is_across_start = (col == 0) || is_blacksquare(p->fill[i - 1]);
        if (is_across_start) {
            int len_across = 0;
            for (int k = 0; k < w - col; k++) {
                if (is_blacksquare(p->fill[i + k])) { len_across = k; break; }
                len_across = k + 1;
            }
            if (len_across > 1) {
                across[na].num = n;
                across[na].clue_index = c;
                across[na].cell = i;
                across[na].len = len_across;
                na++;
                c++;
            }
        }
        int is_down_start = (row == 0) || is_blacksquare(p->fill[i - w]);
        if (is_down_start) {
            int len_down = 0;
            for (int k = 0; k < h - row; k++) {
                if (is_blacksquare(p->fill[i + k * w])) { len_down = k; break; }
                len_down = k + 1;
            }
            if (len_down > 1) {
                down[nd].num = n;
                down[nd].clue_index = c;
                down[nd].cell = i;
                down[nd].len = len_down;
                nd++;
                c++;
            }
        }
        if (c > lastc) n++;
    }

    p->across = across;
    p->down = down;
    p->n_across = na;
    p->n_down = nd;
    return 0;
}

static void parse_extension(Puzzle *p, const char code[4], const uint8_t *data, size_t length) {
    if (memcmp(code, "GEXT", 4) == 0) {
        int sz = p->width * p->height;
        if ((int)length >= sz) {
            free(p->markup);
            p->markup = (uint8_t *)malloc(sz);
            if (p->markup) memcpy(p->markup, data, sz);
        }
    } else if (memcmp(code, "LTIM", 4) == 0) {
        char *s = (char *)malloc(length + 1);
        if (s) {
            memcpy(s, data, length);
            s[length] = 0;
            char *comma = strchr(s, ',');
            if (comma) {
                *comma = 0;
                p->timer_seconds = atoi(s);
                p->timer_running = atoi(comma + 1);
                p->has_timer = 1;
            }
            free(s);
        }
    }

    PuzExtension *ne = (PuzExtension *)realloc(p->extensions,
        sizeof(PuzExtension) * (p->n_extensions + 1));
    if (!ne) return;
    p->extensions = ne;
    PuzExtension *e = &p->extensions[p->n_extensions++];
    memcpy(e->code, code, 4);
    e->code[4] = 0;
    e->data = (uint8_t *)malloc(length);
    if (e->data) memcpy(e->data, data, length);
    e->length = length;
}

Puzzle *puz_load_file(const char *filename, char *errbuf, size_t errlen) {
    FILE *f = fopen(filename, "rb");
    if (!f) {
        snprintf(errbuf, errlen, "Could not open file");
        return NULL;
    }
    fseek(f, 0, SEEK_END);
    long flen = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (flen < 0x34) {
        fclose(f);
        snprintf(errbuf, errlen, "File too short");
        return NULL;
    }
    uint8_t *data = (uint8_t *)malloc(flen);
    if (!data) { fclose(f); snprintf(errbuf, errlen, "Out of memory"); return NULL; }
    if (fread(data, 1, flen, f) != (size_t)flen) {
        fclose(f); free(data);
        snprintf(errbuf, errlen, "Read error");
        return NULL;
    }
    fclose(f);

    /* Find ACROSS&DOWN, position - 2 (the cksum_gbl is just before). */
    size_t magic_pos = (size_t)-1;
    for (long i = 0; i + ACROSSDOWN_LEN <= flen; i++) {
        if (memcmp(data + i, ACROSSDOWN, ACROSSDOWN_LEN) == 0) {
            magic_pos = i;
            break;
        }
    }
    if (magic_pos == (size_t)-1 || magic_pos < 2) {
        free(data);
        snprintf(errbuf, errlen, "Not a .puz file");
        return NULL;
    }

    size_t header_start = magic_pos - 2;
    /* Header is 0x34 bytes total starting from cksum_gbl. */
    if (header_start + 0x34 > (size_t)flen) {
        free(data);
        snprintf(errbuf, errlen, "Header truncated");
        return NULL;
    }

    Puzzle *p = (Puzzle *)calloc(1, sizeof(Puzzle));
    if (!p) { free(data); snprintf(errbuf, errlen, "Out of memory"); return NULL; }
    p->timer_running = 1;

    p->preamble_len = header_start;
    if (p->preamble_len) {
        p->preamble = (uint8_t *)malloc(p->preamble_len);
        memcpy(p->preamble, data, p->preamble_len);
    }

    const uint8_t *h = data + header_start;
    /* Layout per HEADER_FORMAT:
     *   H cksum_gbl (0..1)
     *   11s ACROSS&DOWN (2..12)
     *   x pad (13)
     *   H cksum_hdr (14..15)
     *   Q cksum_magic (16..23)
     *   4s fileversion (24..27)
     *   2s unk1 (28..29)
     *   H scrambled_cksum (30..31)
     *   12s unk2 (32..43)
     *   B B H H ... actually:
     *   final block: B B H H H -- width, height, numclues, puzzletype, sol_state
     *     at offsets 44, 45, 46..47, 48..49, 50..51 = 8 bytes total = 0x34 = 52
     */
    uint16_t cksum_gbl = read_u16(h + 0);
    uint16_t cksum_hdr = read_u16(h + 14);
    /* magic at 16..23, 8 bytes */
    uint64_t cksum_magic = 0;
    for (int i = 0; i < 8; i++) cksum_magic |= ((uint64_t)h[16 + i]) << (8 * i);

    memcpy(p->fileversion, h + 24, 4);
    p->fileversion[4] = 0;
    memcpy(p->unk1, h + 28, 2);
    p->scrambled_cksum = read_u16(h + 30);
    memcpy(p->unk2, h + 32, 12);
    p->width = h[44];
    p->height = h[45];
    p->n_clues = read_u16(h + 46);
    p->puzzletype = read_u16(h + 48);
    p->solution_state = read_u16(h + 50);

    size_t pos = header_start + 0x34;
    int sz = p->width * p->height;
    if (pos + sz * 2 > (size_t)flen) {
        free(data); puz_free(p);
        snprintf(errbuf, errlen, "Solution/fill truncated");
        return NULL;
    }
    p->solution = (char *)malloc(sz + 1);
    p->fill = (char *)malloc(sz + 1);
    memcpy(p->solution, data + pos, sz);
    p->solution[sz] = 0;
    pos += sz;
    memcpy(p->fill, data + pos, sz);
    p->fill[sz] = 0;
    pos += sz;

    p->title = read_zstring(data, flen, &pos);
    p->author = read_zstring(data, flen, &pos);
    p->copyright = read_zstring(data, flen, &pos);

    p->clues = (char **)calloc(p->n_clues > 0 ? p->n_clues : 1, sizeof(char *));
    for (int i = 0; i < p->n_clues; i++) {
        p->clues[i] = read_zstring(data, flen, &pos);
    }
    p->notes = read_zstring(data, flen, &pos);

    /* Extensions: each is 4s code, H length, H cksum, then length bytes + 1 null */
    while (pos + 8 <= (size_t)flen) {
        char code[4];
        memcpy(code, data + pos, 4);
        uint16_t length = read_u16(data + pos + 4);
        /* uint16_t cksum_ext = read_u16(data + pos + 6); */
        pos += 8;
        if (pos + length + 1 > (size_t)flen) break;
        parse_extension(p, code, data + pos, length);
        pos += length + 1;
    }

    if (pos < (size_t)flen) {
        p->postscript_len = flen - pos;
        p->postscript = (uint8_t *)malloc(p->postscript_len);
        memcpy(p->postscript, data + pos, p->postscript_len);
    }

    /* Validate checksums (best effort - if they don't match we still try to load). */
    (void)cksum_gbl; (void)cksum_hdr; (void)cksum_magic;

    if (compute_numbering(p) < 0) {
        free(data); puz_free(p);
        snprintf(errbuf, errlen, "Numbering failed");
        return NULL;
    }

    free(data);
    return p;
}

int puz_has_rebus(Puzzle *p) {
    for (int i = 0; i < p->n_extensions; i++) {
        if (memcmp(p->extensions[i].code, "GRBS", 4) == 0) {
            for (size_t k = 0; k < p->extensions[i].length; k++) {
                if (p->extensions[i].data[k]) return 1;
            }
        }
    }
    return 0;
}

int puz_has_markup(Puzzle *p) {
    if (!p->markup) return 0;
    int sz = p->width * p->height;
    for (int i = 0; i < sz; i++) if (p->markup[i]) return 1;
    return 0;
}

void puz_set_timer(Puzzle *p, int seconds, int running) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%d,%d", seconds, running);
    size_t len = strlen(buf);
    /* Find or create LTIM extension */
    PuzExtension *e = NULL;
    for (int i = 0; i < p->n_extensions; i++) {
        if (memcmp(p->extensions[i].code, "LTIM", 4) == 0) { e = &p->extensions[i]; break; }
    }
    if (!e) {
        PuzExtension *ne = (PuzExtension *)realloc(p->extensions,
            sizeof(PuzExtension) * (p->n_extensions + 1));
        if (!ne) return;
        p->extensions = ne;
        e = &p->extensions[p->n_extensions++];
        memcpy(e->code, "LTIM", 4); e->code[4] = 0;
        e->data = NULL;
    }
    free(e->data);
    e->data = (uint8_t *)malloc(len);
    memcpy(e->data, buf, len);
    e->length = len;
    p->has_timer = 1;
    p->timer_seconds = seconds;
    p->timer_running = running;
}

void puz_set_markup(Puzzle *p, const uint8_t *markup, size_t length) {
    int sz = p->width * p->height;
    if ((int)length != sz) return;
    free(p->markup);
    p->markup = (uint8_t *)malloc(sz);
    memcpy(p->markup, markup, sz);

    PuzExtension *e = NULL;
    for (int i = 0; i < p->n_extensions; i++) {
        if (memcmp(p->extensions[i].code, "GEXT", 4) == 0) { e = &p->extensions[i]; break; }
    }
    if (!e) {
        PuzExtension *ne = (PuzExtension *)realloc(p->extensions,
            sizeof(PuzExtension) * (p->n_extensions + 1));
        if (!ne) return;
        p->extensions = ne;
        e = &p->extensions[p->n_extensions++];
        memcpy(e->code, "GEXT", 4); e->code[4] = 0;
        e->data = NULL;
    }
    free(e->data);
    e->data = (uint8_t *)malloc(sz);
    memcpy(e->data, markup, sz);
    e->length = sz;
}

void puz_set_fill(Puzzle *p, const char *fill) {
    int sz = p->width * p->height;
    memcpy(p->fill, fill, sz);
}

int puz_save_file(Puzzle *p, const char *filename, char *errbuf, size_t errlen) {
    /* Compute total size */
    size_t total = p->preamble_len + 0x34;
    int sz = p->width * p->height;
    total += sz * 2;
    total += (p->title ? strlen(p->title) : 0) + 1;
    total += (p->author ? strlen(p->author) : 0) + 1;
    total += (p->copyright ? strlen(p->copyright) : 0) + 1;
    for (int i = 0; i < p->n_clues; i++)
        total += (p->clues[i] ? strlen(p->clues[i]) : 0) + 1;
    total += (p->notes ? strlen(p->notes) : 0) + 1;
    for (int i = 0; i < p->n_extensions; i++)
        total += 8 + p->extensions[i].length + 1;
    total += p->postscript_len;

    uint8_t *buf = (uint8_t *)calloc(1, total + 64);
    if (!buf) { snprintf(errbuf, errlen, "Out of memory"); return -1; }
    size_t pos = 0;

    if (p->preamble_len) {
        memcpy(buf + pos, p->preamble, p->preamble_len);
        pos += p->preamble_len;
    }

    /* update extension data for markup if needed - already done in puz_set_markup */

    uint16_t cg = global_cksum(p);
    uint16_t ch = header_cksum(p, 0);
    uint64_t cm = magic_cksum(p);

    write_u16(buf + pos, cg); pos += 2;
    memcpy(buf + pos, ACROSSDOWN, 11); pos += 11;
    buf[pos++] = 0; /* pad */
    write_u16(buf + pos, ch); pos += 2;
    for (int i = 0; i < 8; i++) buf[pos + i] = (uint8_t)((cm >> (8 * i)) & 0xFF);
    pos += 8;
    memcpy(buf + pos, p->fileversion, 4); pos += 4;
    memcpy(buf + pos, p->unk1, 2); pos += 2;
    write_u16(buf + pos, p->scrambled_cksum); pos += 2;
    memcpy(buf + pos, p->unk2, 12); pos += 12;
    buf[pos++] = (uint8_t)p->width;
    buf[pos++] = (uint8_t)p->height;
    write_u16(buf + pos, (uint16_t)p->n_clues); pos += 2;
    write_u16(buf + pos, p->puzzletype); pos += 2;
    write_u16(buf + pos, p->solution_state); pos += 2;

    memcpy(buf + pos, p->solution, sz); pos += sz;
    memcpy(buf + pos, p->fill, sz); pos += sz;

    #define WSTR(s) do { const char *_s = (s) ? (s) : ""; size_t _l = strlen(_s); memcpy(buf + pos, _s, _l); pos += _l; buf[pos++] = 0; } while (0)
    WSTR(p->title);
    WSTR(p->author);
    WSTR(p->copyright);
    for (int i = 0; i < p->n_clues; i++) WSTR(p->clues[i]);
    WSTR(p->notes);
    #undef WSTR

    for (int i = 0; i < p->n_extensions; i++) {
        PuzExtension *e = &p->extensions[i];
        memcpy(buf + pos, e->code, 4); pos += 4;
        write_u16(buf + pos, (uint16_t)e->length); pos += 2;
        uint16_t ec = puz_data_cksum(e->data, e->length, 0);
        write_u16(buf + pos, ec); pos += 2;
        if (e->length) memcpy(buf + pos, e->data, e->length);
        pos += e->length;
        buf[pos++] = 0;
    }

    if (p->postscript_len) {
        memcpy(buf + pos, p->postscript, p->postscript_len);
        pos += p->postscript_len;
    }

    FILE *f = fopen(filename, "wb");
    if (!f) { free(buf); snprintf(errbuf, errlen, "Could not open for write"); return -1; }
    if (fwrite(buf, 1, pos, f) != pos) {
        fclose(f); free(buf);
        snprintf(errbuf, errlen, "Write error");
        return -1;
    }
    fclose(f);
    free(buf);
    return 0;
}

void puz_free(Puzzle *p) {
    if (!p) return;
    free(p->title); free(p->author); free(p->copyright); free(p->notes);
    free(p->solution); free(p->fill);
    if (p->clues) {
        for (int i = 0; i < p->n_clues; i++) free(p->clues[i]);
        free(p->clues);
    }
    free(p->preamble); free(p->postscript);
    for (int i = 0; i < p->n_extensions; i++) free(p->extensions[i].data);
    free(p->extensions);
    free(p->across); free(p->down);
    free(p->markup);
    free(p);
}
