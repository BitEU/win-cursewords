#define _CRT_SECURE_NO_WARNINGS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <wchar.h>
#include <ctype.h>
#include <time.h>

#include "puz.h"

#define VERSION "2.0"

/* ----- Debug logging -----
 * Writes timestamped lines to %APPDATA%\win-cursewords\debug.log when the
 * WIN-CURSEWORDS_DEBUG env var is set (any non-empty value). Created on first
 * call; flushed after each write so a crash doesn't truncate the tail. */
static FILE *g_dlog = NULL;
static int g_dlog_tried = 0;

static void dlog_init(void) {
    if (g_dlog_tried) return;
    g_dlog_tried = 1;
    const char *enabled = getenv("WIN-CURSEWORDS_DEBUG");
    if (!enabled || !*enabled) return;
    const char *appdata = getenv("APPDATA");
    if (!appdata || !*appdata) return;
    char dir[MAX_PATH];
    snprintf(dir, sizeof(dir), "%s\\win-cursewords", appdata);
    CreateDirectoryA(dir, NULL); /* ignore "already exists" */
    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s\\debug.log", dir);
    g_dlog = fopen(path, "a");
    if (g_dlog) {
        SYSTEMTIME st; GetLocalTime(&st);
        fprintf(g_dlog, "\n=== win-cursewords v%s started %04d-%02d-%02d %02d:%02d:%02d ===\n",
                VERSION, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        fflush(g_dlog);
    }
}

static void dlog(const char *fmt, ...) {
    if (!g_dlog_tried) dlog_init();
    if (!g_dlog) return;
    SYSTEMTIME st; GetLocalTime(&st);
    fprintf(g_dlog, "[%02d:%02d:%02d.%03d] ",
            st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_dlog, fmt, ap);
    va_end(ap);
    fputc('\n', g_dlog);
    fflush(g_dlog);
}

/* ----- Console handles and dimensions ----- */
static HANDLE g_hOut = INVALID_HANDLE_VALUE;
static HANDLE g_hIn = INVALID_HANDLE_VALUE;
static int g_term_w = 80, g_term_h = 25;
static WORD g_default_attr = 0x07;

/* Attribute bits */
#define ATTR_NORMAL  (g_default_attr)
#define ATTR_DIM     (FOREGROUND_INTENSITY ? (g_default_attr & ~FOREGROUND_INTENSITY) : g_default_attr)
/* dim approximation: just normal foreground without bright bit */
static WORD attr_normal(void) { return g_default_attr; }
static WORD attr_dim(void) { return (WORD)(g_default_attr & 0x0F & ~FOREGROUND_INTENSITY) | (WORD)(g_default_attr & 0xF0); }
static WORD attr_bold(void) { return (WORD)((g_default_attr & 0x0F) | FOREGROUND_INTENSITY) | (WORD)(g_default_attr & 0xF0); }
static WORD attr_red(void) {
    /* red foreground keeping bg from default */
    return (WORD)(FOREGROUND_RED | FOREGROUND_INTENSITY) | (WORD)(g_default_attr & 0xF0);
}
static WORD attr_reverse(WORD a) {
    WORD fg = a & 0x0F;
    WORD bg = (a & 0xF0) >> 4;
    return (WORD)(bg | (fg << 4));
}
static WORD attr_underline(WORD a) { return a | COMMON_LVB_UNDERSCORE; }

/* ----- Drawing primitives ----- */
static void set_cursor(int x, int y) {
    COORD c; c.X = (SHORT)x; c.Y = (SHORT)y;
    SetConsoleCursorPosition(g_hOut, c);
}

static void write_at_n(int x, int y, const wchar_t *s, int len, WORD attr) {
    if (len <= 0) return;
    COORD c; c.X = (SHORT)x; c.Y = (SHORT)y;
    DWORD wrote;
    WriteConsoleOutputCharacterW(g_hOut, s, len, c, &wrote);
    WORD *attrs = (WORD *)malloc(sizeof(WORD) * len);
    for (int i = 0; i < len; i++) attrs[i] = attr;
    WriteConsoleOutputAttribute(g_hOut, attrs, len, c, &wrote);
    free(attrs);
}

/* Convenience: requires s to be NUL-terminated. Use write_at_n for stack
 * buffers where you control the length explicitly. */
static void write_at(int x, int y, const wchar_t *s, WORD attr) {
    write_at_n(x, y, s, (int)wcslen(s), attr);
}

static void write_at_a(int x, int y, const wchar_t *s, int len, const WORD *attrs) {
    if (len <= 0) return;
    COORD c; c.X = (SHORT)x; c.Y = (SHORT)y;
    DWORD wrote;
    WriteConsoleOutputCharacterW(g_hOut, s, len, c, &wrote);
    WriteConsoleOutputAttribute(g_hOut, attrs, len, c, &wrote);
}

static void clear_eol(int x, int y) {
    int len = g_term_w - x;
    if (len <= 0) return;
    wchar_t *s = (wchar_t *)malloc(sizeof(wchar_t) * (len + 1));
    for (int i = 0; i < len; i++) s[i] = L' ';
    s[len] = 0;
    write_at(x, y, s, attr_normal());
    free(s);
}

static void clear_screen(void) {
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (!GetConsoleScreenBufferInfo(g_hOut, &csbi)) return;
    DWORD cells = csbi.dwSize.X * csbi.dwSize.Y;
    COORD origin = {0, 0};
    DWORD wrote;
    FillConsoleOutputCharacterW(g_hOut, L' ', cells, origin, &wrote);
    FillConsoleOutputAttribute(g_hOut, csbi.wAttributes, cells, origin, &wrote);
    set_cursor(0, 0);
}

/* ----- Box-drawing characters ----- */
#define WC_VLINE     L'│'
#define WC_HLINE     L'─'
#define WC_ULCORNER  L'┌'
#define WC_URCORNER  L'┐'
#define WC_LLCORNER  L'└'
#define WC_LRCORNER  L'┘'
#define WC_TTEE      L'┬'
#define WC_BTEE      L'┴'
#define WC_LTEE      L'├'
#define WC_RTEE      L'┤'
#define WC_BIGPLUS   L'┼'
#define WC_LHBLOCK   L'▌'
#define WC_RHBLOCK   L'▐'
#define WC_FULLBLOCK L'█'

/* Subscript digit for cell number */
static wchar_t small_num_digit(int d) {
    static const wchar_t map[10] = {
        L'₀', L'₁', L'₂', L'₃', L'₄',
        L'₅', L'₆', L'₇', L'₈', L'₉'
    };
    return map[d % 10];
}
/* Encircle uppercase letter or space */
static wchar_t encircle(wchar_t c) {
    if (c >= L'A' && c <= L'Z') return (wchar_t)(0x24B6 + (c - L'A'));
    if (c == L' ') return L'◯';
    return c;
}

/* ----- Cell / Grid model ----- */
typedef struct {
    char solution; /* '.', letter, or alnum */
    char entry;    /* '-', letter */
    int number;    /* 0 if none */
    int marked_wrong;
    int corrected;
    int revealed;
    int circled;
} Cell;

typedef struct {
    int x, y; /* column, row */
} Pos;

typedef struct {
    int cell_indices[64]; /* cell index in row-major */
    int len;
} Word;

typedef struct {
    int grid_x, grid_y; /* term offsets */
    int row_count, column_count;
    Cell *cells;
    char *title;
    char *author;

    Word *across_words;
    int n_across;
    Word *down_words;
    int n_down;

    /* spaces['across']/['down'] in row-major / col-major order */
    int *spaces_across; int n_spaces_across;
    int *spaces_down;   int n_spaces_down;

    /* clue text by index */
    char **clues_across; int n_clues_across;
    char **clues_down;   int n_clues_down;

    int notify_y, notify_x;

    Puzzle *puz;
} Grid;

static int cell_idx(Grid *g, int x, int y) { return y * g->column_count + x; }

static int is_block_char(char c) { return c == '.' || c == ':'; }
static int is_letter_char(char c) {
    return (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z');
}
static int is_blank_cell(Cell *c) { return c->entry == '-'; }
static int is_blankish_cell(Cell *c) { return is_blank_cell(c) || c->marked_wrong; }
static int is_correct_cell(Cell *c) { return c->entry == c->solution || is_block_char(c->solution); }
static int cell_is_block(Cell *c) { return is_block_char(c->solution); }
static int cell_is_letter(Cell *c) { return is_letter_char(c->solution); }

/* ----- Build words and numbering for the grid ----- */
static void grid_load(Grid *g, Puzzle *p, int gx, int gy) {
    g->grid_x = gx; g->grid_y = gy;
    g->row_count = p->height;
    g->column_count = p->width;
    g->title = p->title ? p->title : "";
    g->author = p->author ? p->author : "";
    g->puz = p;

    int sz = p->width * p->height;
    g->cells = (Cell *)calloc(sz, sizeof(Cell));
    for (int i = 0; i < sz; i++) {
        g->cells[i].solution = p->solution[i];
        g->cells[i].entry = p->fill[i] ? p->fill[i] : '-';
        if (g->cells[i].entry == 0) g->cells[i].entry = '-';
    }

    /* Apply markup */
    if (p->markup) {
        for (int i = 0; i < sz; i++) {
            uint8_t md = p->markup[i];
            if (md & PUZ_MARKUP_CIRCLED) g->cells[i].circled = 1;
            if (md & PUZ_MARKUP_REVEALED) g->cells[i].revealed = 1;
            if (md & PUZ_MARKUP_INCORRECT) g->cells[i].marked_wrong = 1;
            if (md & PUZ_MARKUP_PREV_INCORRECT) g->cells[i].corrected = 1;
        }
    }

    /* Build across words: scan rows */
    int max_words = sz;
    g->across_words = (Word *)calloc(max_words, sizeof(Word));
    g->down_words = (Word *)calloc(max_words, sizeof(Word));
    g->n_across = 0;
    g->n_down = 0;

    for (int y = 0; y < g->row_count; y++) {
        Word w = {{0}, 0};
        for (int x = 0; x < g->column_count; x++) {
            int idx = cell_idx(g, x, y);
            if (cell_is_letter(&g->cells[idx])) {
                if (w.len < 64) w.cell_indices[w.len++] = idx;
            } else {
                if (w.len > 1) g->across_words[g->n_across++] = w;
                w.len = 0;
            }
        }
        if (w.len > 1) g->across_words[g->n_across++] = w;
    }

    for (int x = 0; x < g->column_count; x++) {
        Word w = {{0}, 0};
        for (int y = 0; y < g->row_count; y++) {
            int idx = cell_idx(g, x, y);
            if (cell_is_letter(&g->cells[idx])) {
                if (w.len < 64) w.cell_indices[w.len++] = idx;
            } else {
                if (w.len > 1) g->down_words[g->n_down++] = w;
                w.len = 0;
            }
        }
        if (w.len > 1) g->down_words[g->n_down++] = w;
    }

    /* Sort down_words row-major (by first cell's row, then col) so the index
     * matches puz->down which is built in row-major order. */
    for (int i = 1; i < g->n_down; i++) {
        Word v = g->down_words[i];
        int j = i - 1;
        while (j >= 0 && g->down_words[j].cell_indices[0] > v.cell_indices[0]) {
            g->down_words[j + 1] = g->down_words[j];
            j--;
        }
        g->down_words[j + 1] = v;
    }

    /* Numbering: collect first cells of all words, sort row-major */
    int *firsts = (int *)malloc(sizeof(int) * (g->n_across + g->n_down));
    int n_firsts = 0;
    for (int i = 0; i < g->n_across; i++) {
        int c = g->across_words[i].cell_indices[0];
        int found = 0;
        for (int k = 0; k < n_firsts; k++) if (firsts[k] == c) { found = 1; break; }
        if (!found) firsts[n_firsts++] = c;
    }
    for (int i = 0; i < g->n_down; i++) {
        int c = g->down_words[i].cell_indices[0];
        int found = 0;
        for (int k = 0; k < n_firsts; k++) if (firsts[k] == c) { found = 1; break; }
        if (!found) firsts[n_firsts++] = c;
    }
    /* Sort row-major */
    for (int i = 1; i < n_firsts; i++) {
        int v = firsts[i], j = i - 1;
        while (j >= 0 && firsts[j] > v) { firsts[j + 1] = firsts[j]; j--; }
        firsts[j + 1] = v;
    }
    for (int i = 0; i < n_firsts; i++) {
        g->cells[firsts[i]].number = i + 1;
    }
    free(firsts);

    /* spaces */
    g->spaces_across = (int *)malloc(sizeof(int) * sz);
    g->n_spaces_across = 0;
    for (int y = 0; y < g->row_count; y++)
        for (int x = 0; x < g->column_count; x++) {
            int idx = cell_idx(g, x, y);
            if (cell_is_letter(&g->cells[idx])) g->spaces_across[g->n_spaces_across++] = idx;
        }
    g->spaces_down = (int *)malloc(sizeof(int) * sz);
    g->n_spaces_down = 0;
    for (int x = 0; x < g->column_count; x++)
        for (int y = 0; y < g->row_count; y++) {
            int idx = cell_idx(g, x, y);
            if (cell_is_letter(&g->cells[idx])) g->spaces_down[g->n_spaces_down++] = idx;
        }

    /* Clue text association: walk puz->across and puz->down which give clue_index */
    g->n_clues_across = p->n_across;
    g->n_clues_down = p->n_down;
    g->clues_across = (char **)calloc(g->n_clues_across > 0 ? g->n_clues_across : 1, sizeof(char *));
    g->clues_down = (char **)calloc(g->n_clues_down > 0 ? g->n_clues_down : 1, sizeof(char *));
    for (int i = 0; i < p->n_across; i++) {
        int ci = p->across[i].clue_index;
        g->clues_across[i] = (ci >= 0 && ci < p->n_clues) ? p->clues[ci] : "";
    }
    for (int i = 0; i < p->n_down; i++) {
        int ci = p->down[i].clue_index;
        g->clues_down[i] = (ci >= 0 && ci < p->n_clues) ? p->clues[ci] : "";
    }
}

static void grid_free(Grid *g) {
    free(g->cells);
    free(g->across_words);
    free(g->down_words);
    free(g->spaces_across);
    free(g->spaces_down);
    free(g->clues_across);
    free(g->clues_down);
}

/* Translate grid (col, row) to terminal (term_x, term_y) of the cell content. */
static void to_term(Grid *g, int gx, int gy, int *tx, int *ty) {
    *tx = g->grid_x + (4 * gx) + 2;
    *ty = g->grid_y + (2 * gy) + 1;
}

/* Compile cell display: returns characters to draw and an attribute hint.
 * out: 3 wchars representing "value markup_or_space_already" — caller renders.
 * The Python compile_cell returns (value_with_color, markup) and draws as
 * value+markup at (y, x), occupying 2 cells (wide value + space + markup char). */

/* Draw a single cell at its position with optional underline/reverse flags */
static void draw_cell_styled(Grid *g, int gx, int gy, int underline, int reverse) {
    int idx = cell_idx(g, gx, gy);
    Cell *c = &g->cells[idx];
    int tx, ty;
    to_term(g, gx, gy, &tx, &ty);

    wchar_t value;
    WORD value_attr = attr_bold();
    int use_red_lower = 0;

    if (cell_is_block(c)) {
        /* Block: three full-block chars. The original uses ▐█▌ but on conhost
         * the half-block chars are East-Asian-ambiguous-width and render
         * 2 cells wide in many fonts, smearing to the right. Plain █ is
         * single-cell-reliable. */
        wchar_t blk[3] = { WC_FULLBLOCK, WC_FULLBLOCK, WC_FULLBLOCK };
        WORD attrs[3] = { attr_dim(), attr_dim(), attr_dim() };
        write_at_a(tx - 1, ty, blk, 3, attrs);
        return;
    }

    char ch = is_blank_cell(c) ? ' ' : c->entry;
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
    value = (wchar_t)ch;

    if (c->circled) value = encircle(value);

    if (c->marked_wrong && !c->revealed) {
        if (value >= L'A' && value <= L'Z') value = (wchar_t)(value - L'A' + L'a');
        value_attr = attr_red();
        use_red_lower = 1;
    } else {
        value_attr = attr_bold();
    }

    wchar_t markup_char = L' ';
    WORD markup_attr = attr_normal();
    if (c->corrected) { markup_char = L'.'; markup_attr = attr_red(); }
    if (c->revealed)  { markup_char = L':'; markup_attr = attr_red(); }

    if (underline) value_attr = attr_underline(value_attr);
    if (reverse)   value_attr = attr_reverse(value_attr);

    /* Fill all 3 inner cells: leading space, value, markup. */
    wchar_t buf[3] = { L' ', value, markup_char };
    WORD attrs[3] = { attr_normal(), value_attr, markup_attr };
    write_at_a(tx - 1, ty, buf, 3, attrs);
    (void)use_red_lower;
}

static void draw_cell(Grid *g, int gx, int gy)              { draw_cell_styled(g, gx, gy, 0, 0); }
static void draw_highlighted_cell(Grid *g, int gx, int gy)  { draw_cell_styled(g, gx, gy, 1, 0); }
static void draw_cursor_cell(Grid *g, int gx, int gy)       { draw_cell_styled(g, gx, gy, 0, 1); }

/* Draw the entire grid frame and all cells */
static void grid_draw(Grid *g, int empty) {
    /* Blank the grid area first so no stale chars from the prior screen
     * (or from a wider previous puzzle state) bleed through. */
    int total_w = g->column_count * 4 + 1;
    int total_h = g->row_count * 2 + 1;
    wchar_t *blank = (wchar_t *)malloc(sizeof(wchar_t) * (total_w + 1));
    for (int i = 0; i < total_w; i++) blank[i] = L' ';
    blank[total_w] = 0;
    for (int r = 0; r < total_h; r++) {
        write_at(g->grid_x, g->grid_y + r, blank, attr_normal());
    }
    free(blank);

    /* Top/middle separators rows */
    for (int i = 0; i <= g->row_count; i++) {
        int ty = g->grid_y + 2 * i;
        int tx = g->grid_x;

        for (int j = 0; j <= g->column_count; j++) {
            wchar_t corner;
            if (i == 0 && j == 0) corner = WC_ULCORNER;
            else if (i == 0 && j == g->column_count) corner = WC_URCORNER;
            else if (i == g->row_count && j == 0) corner = WC_LLCORNER;
            else if (i == g->row_count && j == g->column_count) corner = WC_LRCORNER;
            else if (i == 0) corner = WC_TTEE;
            else if (i == g->row_count) corner = WC_BTEE;
            else if (j == 0) corner = WC_LTEE;
            else if (j == g->column_count) corner = WC_RTEE;
            else corner = WC_BIGPLUS;

            int cx = tx + j * 4;
            wchar_t cs[1] = { corner };
            write_at_n(cx, ty, cs, 1, attr_dim());

            if (j < g->column_count) {
                wchar_t hl[3];
                /* If this is a separator above a cell, place the cell number subscript over the dashes. */
                if (i > 0 && i <= g->row_count && j < g->column_count) {
                    int gy_above = i - 1; /* the cell whose bottom border this is — wait, no, this is top border of cell row=i */
                    (void)gy_above;
                }
                /* Render hlines first */
                hl[0] = WC_HLINE; hl[1] = WC_HLINE; hl[2] = WC_HLINE;

                /* For top edge of cell at row=i, col=j (cell row index = i, since top of row i is separator i): */
                if (!empty && i < g->row_count) {
                    int idx = cell_idx(g, j, i);
                    int num = g->cells[idx].number;
                    if (num > 0) {
                        char nbuf[8];
                        snprintf(nbuf, sizeof(nbuf), "%d", num);
                        int nlen = (int)strlen(nbuf);
                        if (nlen > 3) nlen = 3;
                        for (int k = 0; k < nlen; k++)
                            hl[k] = small_num_digit(nbuf[k] - '0');
                    }
                }
                WORD attrs[3] = { attr_dim(), attr_dim(), attr_dim() };
                /* small num is drawn in normal weight */
                if (!empty && i < g->row_count) {
                    int idx = cell_idx(g, j, i);
                    int num = g->cells[idx].number;
                    if (num > 0) {
                        char nbuf[8];
                        snprintf(nbuf, sizeof(nbuf), "%d", num);
                        int nlen = (int)strlen(nbuf);
                        if (nlen > 3) nlen = 3;
                        for (int k = 0; k < nlen; k++) attrs[k] = attr_normal();
                    }
                }
                write_at_a(cx + 1, ty, hl, 3, attrs);
            }
        }
    }

    /* Cell rows: vertical lines and contents */
    for (int i = 0; i < g->row_count; i++) {
        int ty = g->grid_y + 2 * i + 1;
        for (int j = 0; j <= g->column_count; j++) {
            int cx = g->grid_x + j * 4;
            wchar_t v[1] = { WC_VLINE };
            write_at_n(cx, ty, v, 1, attr_dim());
        }
        for (int j = 0; j < g->column_count; j++) {
            if (empty) {
                int cx = g->grid_x + j * 4 + 1;
                wchar_t sp[3] = { L' ', L' ', L' ' };
                write_at_n(cx, ty, sp, 3, attr_normal());
            } else {
                draw_cell(g, j, i);
            }
        }
    }
}

/* ----- Notification ----- */
static void clear_notification(Grid *g) {
    int len = g_term_w - g->notify_x;
    if (len <= 0) return;
    wchar_t *s = (wchar_t *)malloc(sizeof(wchar_t) * (len + 1));
    for (int i = 0; i < len; i++) s[i] = L' ';
    s[len] = 0;
    write_at(g->notify_x, g->notify_y, s, attr_normal());
    free(s);
}

static double g_notify_until = 0.0;
static double now_seconds(void) {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart / (double)f.QuadPart;
}

static void send_notification(Grid *g, const char *msg) {
    clear_notification(g);
    size_t len = strlen(msg);
    wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (len + 1));
    for (size_t i = 0; i < len; i++) w[i] = (wchar_t)(unsigned char)msg[i];
    w[len] = 0;
    write_at(g->notify_x, g->notify_y, w, attr_reverse(attr_normal()));
    free(w);
    g_notify_until = now_seconds() + 5.0;
}

static void maybe_clear_notification(Grid *g) {
    if (g_notify_until > 0.0 && now_seconds() >= g_notify_until) {
        clear_notification(g);
        g_notify_until = 0.0;
    }
}

/* Read a key; returns 0 on no event. Sets *vk to virtual key code, *ch to ascii char,
 * *shift / *ctrl flags. */
typedef struct {
    int valid;
    WORD vk;
    char ch;
    int ctrl, shift, alt;
} Key;

static Key read_key_blocking(int timeout_ms) {
    Key k = {0};
    double deadline = -1.0;
    if (timeout_ms >= 0) deadline = now_seconds() + (double)timeout_ms / 1000.0;
    for (;;) {
        DWORD wait_ms;
        if (timeout_ms < 0) {
            wait_ms = INFINITE;
        } else {
            double remaining = deadline - now_seconds();
            if (remaining <= 0.0) {
                dlog("read_key: deadline elapsed -> invalid");
                return k;
            }
            wait_ms = (DWORD)(remaining * 1000.0);
        }
        DWORD waited = WaitForSingleObject(g_hIn, wait_ms);
        if (waited != WAIT_OBJECT_0) {
            dlog("read_key: WaitForSingleObject returned %lu (timeout=%lu) -> invalid",
                 (unsigned long)waited, (unsigned long)wait_ms);
            return k;
        }
        INPUT_RECORD rec;
        DWORD nread;
        while (PeekConsoleInputW(g_hIn, &rec, 1, &nread) && nread) {
            ReadConsoleInputW(g_hIn, &rec, 1, &nread);
            if (rec.EventType == KEY_EVENT) {
                const KEY_EVENT_RECORD *ke = &rec.Event.KeyEvent;
                dlog("read_key: KEY_EVENT down=%d vk=0x%02X ch=0x%02X repeat=%u mods=0x%lX",
                     (int)ke->bKeyDown, (unsigned)ke->wVirtualKeyCode,
                     (unsigned)(unsigned char)ke->uChar.AsciiChar,
                     (unsigned)ke->wRepeatCount,
                     (unsigned long)ke->dwControlKeyState);
                if (ke->bKeyDown) {
                    k.valid = 1;
                    k.vk = ke->wVirtualKeyCode;
                    k.ch = (char)ke->uChar.AsciiChar;
                    DWORD mods = ke->dwControlKeyState;
                    k.ctrl = (mods & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
                    k.shift = (mods & SHIFT_PRESSED) != 0;
                    k.alt = (mods & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
                    return k;
                }
            } else {
                dlog("read_key: non-KEY event type=%u", (unsigned)rec.EventType);
            }
        }
        dlog("read_key: drained queue without keydown -> wait again");
    }
}

/* Get user input on the notification line. Stops on Enter/Esc or when char_limit reached.
 * input_filter: 0 = isalnum, 1 = isdigit. blocking forces wait.
 * Writes terminating null. Returns length. */
static int get_notification_input(Grid *g, const char *prompt, int char_limit,
                                   int filter_alnum_only, int blocking, char *out, size_t outsz) {
    g_notify_until = 0.0;
    clear_notification(g);
    size_t plen = strlen(prompt);
    /* render prompt + space in reverse */
    wchar_t *pw = (wchar_t *)malloc(sizeof(wchar_t) * (plen + 2));
    for (size_t i = 0; i < plen; i++) pw[i] = (wchar_t)(unsigned char)prompt[i];
    pw[plen] = L' ';
    pw[plen + 1] = 0;
    write_at(g->notify_x, g->notify_y, pw, attr_reverse(attr_normal()));
    free(pw);

    int input_x = g->notify_x + (int)plen + 1;
    int len = 0;
    out[0] = 0;

    dlog("prompt: \"%s\" char_limit=%d alnum=%d blocking=%d",
         prompt, char_limit, filter_alnum_only, blocking);

    while (len < char_limit) {
        Key k = read_key_blocking(blocking ? -1 : 5000);
        if (!k.valid) {
            dlog("prompt: invalid key -> break (len=%d)", len);
            break;
        }
        /* Ignore stray modifier-only key events and keys still held with Ctrl/Alt
         * (e.g. Ctrl released slowly after Ctrl+C). Otherwise the prompt would
         * cancel itself before the user can type a response. */
        if (k.vk == VK_SHIFT || k.vk == VK_CONTROL || k.vk == VK_MENU ||
            k.vk == VK_LSHIFT || k.vk == VK_RSHIFT ||
            k.vk == VK_LCONTROL || k.vk == VK_RCONTROL ||
            k.vk == VK_LMENU || k.vk == VK_RMENU) {
            dlog("prompt: skip modifier-only vk=0x%02X", (unsigned)k.vk);
            continue;
        }
        if (k.ctrl || k.alt) {
            dlog("prompt: skip ctrl/alt-held vk=0x%02X ch=0x%02X ctrl=%d alt=%d",
                 (unsigned)k.vk, (unsigned)(unsigned char)k.ch, k.ctrl, k.alt);
            continue;
        }
        if (k.vk == VK_RETURN || k.vk == VK_ESCAPE) {
            dlog("prompt: vk=%s -> break", k.vk == VK_RETURN ? "RETURN" : "ESCAPE");
            break;
        }
        if (k.vk == VK_BACK) {
            if (len > 0) {
                len--;
                out[len] = 0;
                wchar_t sp = L' ';
                write_at_n(input_x + len, g->notify_y, &sp, 1, attr_normal());
                /* clear rest */
                wchar_t blank = L' ';
                for (int i = len; i < char_limit; i++)
                    write_at_n(input_x + i, g->notify_y, &blank, 1, attr_normal());
            }
            continue;
        }
        char ch = k.ch;
        int ok;
        if (filter_alnum_only)
            ok = (ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9');
        else
            ok = ch >= '0' && ch <= '9';
        dlog("prompt: ch=0x%02X ('%c') ok=%d", (unsigned)(unsigned char)ch,
             (ch >= 32 && ch < 127) ? ch : '?', ok);
        if (ok && (size_t)len < outsz - 1) {
            out[len++] = ch;
            out[len] = 0;
            wchar_t wc = (wchar_t)(unsigned char)ch;
            write_at_n(input_x + len - 1, g->notify_y, &wc, 1, attr_normal());
        } else if (blocking) {
            dlog("prompt: invalid char (blocking, continue)");
            continue;
        } else {
            dlog("prompt: invalid char (non-blocking, break)");
            break;
        }
    }
    dlog("prompt: done len=%d out=\"%s\"", len, out);
    return len;
}

/* ----- Cursor and navigation ----- */
typedef struct {
    int pos_idx; /* row-major cell index */
    int direction; /* 0 = across, 1 = down */
    Grid *grid;
} Cursor;

static void cursor_pos_xy(Cursor *cu, int *x, int *y) {
    *x = cu->pos_idx % cu->grid->column_count;
    *y = cu->pos_idx / cu->grid->column_count;
}

static Word *find_current_word(Cursor *cu, int *idx_out) {
    Grid *g = cu->grid;
    Word *list = cu->direction == 0 ? g->across_words : g->down_words;
    int n = cu->direction == 0 ? g->n_across : g->n_down;
    for (int i = 0; i < n; i++) {
        for (int k = 0; k < list[i].len; k++) {
            if (list[i].cell_indices[k] == cu->pos_idx) {
                if (idx_out) *idx_out = i;
                return &list[i];
            }
        }
    }
    if (idx_out) *idx_out = -1;
    return NULL;
}

static int blank_cells_remaining(Grid *g) {
    int sz = g->row_count * g->column_count;
    for (int i = 0; i < sz; i++)
        if (cell_is_letter(&g->cells[i]) && is_blankish_cell(&g->cells[i])) return 1;
    return 0;
}

static int earliest_blank_in_word(Cursor *cu) {
    Word *w = find_current_word(cu, NULL);
    if (!w) return -1;
    for (int i = 0; i < w->len; i++) {
        if (is_blankish_cell(&cu->grid->cells[w->cell_indices[i]])) return w->cell_indices[i];
    }
    return -1;
}

static void cursor_switch(Cursor *cu) { cu->direction ^= 1; }

static int find_in_spaces(int *spaces, int n, int idx) {
    for (int i = 0; i < n; i++) if (spaces[i] == idx) return i;
    return -1;
}

static void cursor_move_right(Cursor *cu) {
    Grid *g = cu->grid;
    int i = find_in_spaces(g->spaces_across, g->n_spaces_across, cu->pos_idx);
    if (i < 0) return;
    int next = (i + 1) % g->n_spaces_across;
    cu->pos_idx = g->spaces_across[next];
}
static void cursor_move_left(Cursor *cu) {
    Grid *g = cu->grid;
    int i = find_in_spaces(g->spaces_across, g->n_spaces_across, cu->pos_idx);
    if (i < 0) return;
    int prev = (i - 1 + g->n_spaces_across) % g->n_spaces_across;
    cu->pos_idx = g->spaces_across[prev];
}
static void cursor_move_down(Cursor *cu) {
    Grid *g = cu->grid;
    int i = find_in_spaces(g->spaces_down, g->n_spaces_down, cu->pos_idx);
    if (i < 0) return;
    int next = (i + 1) % g->n_spaces_down;
    cu->pos_idx = g->spaces_down[next];
}
static void cursor_move_up(Cursor *cu) {
    Grid *g = cu->grid;
    int i = find_in_spaces(g->spaces_down, g->n_spaces_down, cu->pos_idx);
    if (i < 0) return;
    int prev = (i - 1 + g->n_spaces_down) % g->n_spaces_down;
    cu->pos_idx = g->spaces_down[prev];
}
static void cursor_advance(Cursor *cu) {
    if (cu->direction == 0) cursor_move_right(cu); else cursor_move_down(cu);
}
static void cursor_retreat(Cursor *cu) {
    if (cu->direction == 0) cursor_move_left(cu); else cursor_move_up(cu);
}

static int move_within_word(Cursor *cu, int overwrite, int wrap) {
    Word *w = find_current_word(cu, NULL);
    if (!w) return -1;
    int cur = -1;
    for (int i = 0; i < w->len; i++) if (w->cell_indices[i] == cu->pos_idx) { cur = i; break; }
    if (cur < 0) return -1;

    int total = wrap ? w->len : w->len - cur - 1;
    for (int s = 1; s <= total; s++) {
        int i = cur + s;
        if (wrap) i %= w->len;
        if (i == cur) break;
        if (i >= w->len) break;
        int idx = w->cell_indices[i];
        if (overwrite || is_blankish_cell(&cu->grid->cells[idx])) return idx;
    }
    return -1;
}

static void cursor_advance_to_next_word(Cursor *cu, int blank_placement);

static void cursor_advance_within_word(Cursor *cu, int overwrite, int wrap) {
    int next = move_within_word(cu, overwrite, wrap);
    if (next >= 0) cu->pos_idx = next;
    else cursor_advance_to_next_word(cu, 1);
}

static void cursor_advance_to_next_word(Cursor *cu, int blank_placement) {
    Grid *g = cu->grid;
    Word *list = cu->direction == 0 ? g->across_words : g->down_words;
    int n = cu->direction == 0 ? g->n_across : g->n_down;
    Word *other_list = cu->direction == 0 ? g->down_words : g->across_words;

    /* Find current word index — if cursor isn't on a word, retreat until it is. */
    int wi = -1;
    while (wi < 0) {
        Word *w = find_current_word(cu, &wi);
        if (!w) cursor_retreat(cu);
    }

    if (wi == n - 1) {
        cursor_switch(cu);
        cu->pos_idx = other_list[0].cell_indices[0];
    } else {
        cu->pos_idx = list[wi + 1].cell_indices[0];
    }

    if (blank_placement && !blank_cells_remaining(g)) blank_placement = 0;

    if (blank_placement) {
        int eb = earliest_blank_in_word(cu);
        if (eb >= 0) cu->pos_idx = eb;
        else cursor_advance_to_next_word(cu, 1);
    }
}

static void cursor_retreat_to_previous_word(Cursor *cu, int end_placement, int blank_placement) {
    Grid *g = cu->grid;
    Word *list = cu->direction == 0 ? g->across_words : g->down_words;
    int n = cu->direction == 0 ? g->n_across : g->n_down;
    Word *other_list = cu->direction == 0 ? g->down_words : g->across_words;
    int n_other = cu->direction == 0 ? g->n_down : g->n_across;

    int wi = -1;
    while (wi < 0) {
        Word *w = find_current_word(cu, &wi);
        if (!w) cursor_advance(cu);
    }

    int pos_in_word = end_placement ? -1 : 0;
    if (wi == 0) {
        cursor_switch(cu);
        Word *w = &other_list[n_other - 1];
        cu->pos_idx = pos_in_word < 0 ? w->cell_indices[w->len - 1] : w->cell_indices[0];
    } else {
        Word *w = &list[wi - 1];
        cu->pos_idx = pos_in_word < 0 ? w->cell_indices[w->len - 1] : w->cell_indices[0];
    }

    if (blank_placement && !blank_cells_remaining(g)) blank_placement = 0;

    if (blank_placement) {
        int eb = earliest_blank_in_word(cu);
        if (eb >= 0) cu->pos_idx = eb;
        else cursor_retreat_to_previous_word(cu, end_placement, 1);
    }
}

static void cursor_retreat_within_word(Cursor *cu, int end_placement, int blank_placement) {
    Word *w = find_current_word(cu, NULL);
    if (!w) return;
    int cur = -1;
    for (int i = 0; i < w->len; i++) if (w->cell_indices[i] == cu->pos_idx) { cur = i; break; }
    if (cur < 0) return;
    int eb = earliest_blank_in_word(cu);
    int eb_idx = -1;
    if (eb >= 0)
        for (int i = 0; i < w->len; i++) if (w->cell_indices[i] == eb) { eb_idx = i; break; }

    if (blank_placement && eb_idx >= 0 && cur > eb_idx) {
        cu->pos_idx = eb;
    } else if (!blank_placement && cur > 0) {
        cu->pos_idx = w->cell_indices[cur - 1];
    } else {
        cursor_retreat_to_previous_word(cu, end_placement, blank_placement);
    }
}

static void cursor_advance_perpendicular(Cursor *cu) {
    cursor_switch(cu);
    cursor_advance(cu);
    cursor_switch(cu);
}
static void cursor_retreat_perpendicular(Cursor *cu) {
    cursor_switch(cu);
    cursor_retreat(cu);
    cursor_switch(cu);
}

/* ----- Reveal/check ----- */
static void reveal_cell(Grid *g, int idx) {
    Cell *c = &g->cells[idx];
    if (is_blankish_cell(c) || !is_correct_cell(c)) {
        c->entry = c->solution;
        if (c->entry >= 'a' && c->entry <= 'z') c->entry = (char)(c->entry - 'a' + 'A');
        c->revealed = 1;
        c->marked_wrong = 0;
        int x = idx % g->column_count, y = idx / g->column_count;
        draw_cell(g, x, y);
    }
}
static void check_cell(Grid *g, int idx) {
    Cell *c = &g->cells[idx];
    if (!is_blank_cell(c) && !is_correct_cell(c)) {
        c->marked_wrong = 1;
        int x = idx % g->column_count, y = idx / g->column_count;
        draw_cell(g, x, y);
    }
}

/* ----- Save ----- */
static void grid_save(Grid *g, const char *filename) {
    int sz = g->row_count * g->column_count;
    char *fill = (char *)malloc(sz + 1);
    for (int i = 0; i < sz; i++) {
        Cell *c = &g->cells[i];
        if (cell_is_block(c)) fill[i] = '.';
        else if (is_blank_cell(c)) fill[i] = '-';
        else fill[i] = c->entry;
    }
    fill[sz] = 0;
    puz_set_fill(g->puz, fill);
    free(fill);

    /* Markup */
    int any = 0;
    for (int i = 0; i < sz; i++) {
        Cell *c = &g->cells[i];
        if (c->marked_wrong || c->corrected || c->revealed || c->circled) { any = 1; break; }
    }
    if (any || puz_has_markup(g->puz)) {
        uint8_t *md = (uint8_t *)calloc(sz, 1);
        for (int i = 0; i < sz; i++) {
            Cell *c = &g->cells[i];
            uint8_t v = 0;
            if (c->corrected) v |= PUZ_MARKUP_PREV_INCORRECT;
            if (c->marked_wrong) v |= PUZ_MARKUP_INCORRECT;
            if (c->revealed) v |= PUZ_MARKUP_REVEALED;
            if (c->circled) v |= PUZ_MARKUP_CIRCLED;
            md[i] = v;
        }
        puz_set_markup(g->puz, md, sz);
        free(md);
    }

    char err[256] = {0};
    if (puz_save_file(g->puz, filename, err, sizeof(err)) == 0) {
        send_notification(g, "Current puzzle state saved.");
    } else {
        send_notification(g, err);
    }
}

/* ----- Print mode ----- */
/* Render the grid to text lines (UTF-8) with no attributes. */
static int render_grid_text(Grid *g, int empty, int blank, int solution,
                             wchar_t ***lines_out) {
    int n_lines = g->row_count * 2 + 1;
    wchar_t **lines = (wchar_t **)calloc(n_lines, sizeof(wchar_t *));

    int line_chars = g->column_count * 4 + 1;
    for (int li = 0; li < n_lines; li++) {
        lines[li] = (wchar_t *)calloc(line_chars + 1, sizeof(wchar_t));
    }

    /* Top + cell-row separators */
    for (int i = 0; i < g->row_count; i++) {
        wchar_t *top = lines[i * 2];
        wchar_t *cell = lines[i * 2 + 1];
        for (int j = 0; j < g->column_count; j++) {
            int x = j * 4;
            wchar_t corner;
            if (i == 0 && j == 0) corner = WC_ULCORNER;
            else if (j == 0) corner = WC_LTEE;
            else if (i == 0) corner = WC_TTEE;
            else corner = WC_BIGPLUS;
            top[x] = corner;
            top[x + 1] = WC_HLINE;
            top[x + 2] = WC_HLINE;
            top[x + 3] = WC_HLINE;

            if (!empty) {
                int idx = cell_idx(g, j, i);
                int num = g->cells[idx].number;
                if (num > 0) {
                    char nb[8]; snprintf(nb, sizeof(nb), "%d", num);
                    int nl = (int)strlen(nb);
                    for (int k = 0; k < nl && k < 3; k++) top[x + 1 + k] = small_num_digit(nb[k] - '0');
                }
            }

            cell[x] = WC_VLINE;
            int idx = cell_idx(g, j, i);
            Cell *c = &g->cells[idx];
            if (empty) {
                cell[x + 1] = L' '; cell[x + 2] = L' '; cell[x + 3] = L' ';
            } else if (cell_is_block(c)) {
                cell[x + 1] = WC_FULLBLOCK; cell[x + 2] = WC_FULLBLOCK; cell[x + 3] = WC_FULLBLOCK;
            } else if (blank || solution) {
                wchar_t v = blank ? L' ' : (wchar_t)c->solution;
                if (v >= L'a' && v <= L'z') v = (wchar_t)(v - L'a' + L'A');
                if (c->circled) v = encircle(v);
                cell[x + 1] = L' ';
                cell[x + 2] = v;
                cell[x + 3] = L' ';
            } else {
                wchar_t v = is_blank_cell(c) ? L' ' : (wchar_t)c->entry;
                if (v >= L'a' && v <= L'z') v = (wchar_t)(v - L'a' + L'A');
                if (c->circled) v = encircle(v);
                cell[x + 1] = L' ';
                cell[x + 2] = v;
                cell[x + 3] = L' ';
            }
        }
        /* right edge */
        int rx = g->column_count * 4;
        top[rx] = (i == 0) ? WC_URCORNER : WC_RTEE;
        cell[rx] = WC_VLINE;
    }
    /* Bottom row */
    wchar_t *bot = lines[g->row_count * 2];
    for (int j = 0; j <= g->column_count; j++) {
        int x = j * 4;
        if (j == 0) bot[x] = WC_LLCORNER;
        else if (j == g->column_count) bot[x] = WC_LRCORNER;
        else bot[x] = WC_BTEE;
        if (j < g->column_count) {
            bot[x + 1] = WC_HLINE; bot[x + 2] = WC_HLINE; bot[x + 3] = WC_HLINE;
        }
    }

    *lines_out = lines;
    return n_lines;
}

static void print_wide_line(const wchar_t *s) {
    /* Convert and print via WriteConsoleW for stdout if it's a console; otherwise convert to UTF-8 and use printf. */
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode;
    if (GetConsoleMode(h, &mode)) {
        DWORD wrote;
        WriteConsoleW(h, s, (DWORD)wcslen(s), &wrote, NULL);
        WriteConsoleW(h, L"\r\n", 2, &wrote, NULL);
    } else {
        int n = WideCharToMultiByte(CP_UTF8, 0, s, -1, NULL, 0, NULL, NULL);
        char *buf = (char *)malloc(n + 2);
        WideCharToMultiByte(CP_UTF8, 0, s, -1, buf, n, NULL, NULL);
        printf("%s\n", buf);
        free(buf);
    }
}

static void wrap_text(const char *text, int width, int subseq_indent, char ***lines_out, int *n_lines) {
    int n = 0;
    char **lines = NULL;
    int len = (int)strlen(text);
    int pos = 0;
    int first = 1;
    while (pos < len) {
        int indent = first ? 0 : subseq_indent;
        int avail = width - indent;
        if (avail < 1) avail = 1;
        int take = len - pos;
        if (take > avail) {
            /* find last space within avail */
            take = avail;
            int sp = -1;
            for (int i = avail; i > 0; i--) {
                if (text[pos + i - 1] == ' ') { sp = i - 1; break; }
            }
            if (sp > 0) take = sp;
        }
        char *line = (char *)malloc(indent + take + 1);
        for (int i = 0; i < indent; i++) line[i] = ' ';
        memcpy(line + indent, text + pos, take);
        line[indent + take] = 0;
        lines = (char **)realloc(lines, sizeof(char *) * (n + 1));
        lines[n++] = line;
        pos += take;
        while (pos < len && text[pos] == ' ') pos++;
        first = 0;
    }
    if (n == 0) {
        lines = (char **)realloc(lines, sizeof(char *));
        lines[0] = strdup("");
        n = 1;
    }
    *lines_out = lines;
    *n_lines = n;
}

static void printer_output(Grid *g, const char *style, int width, int downs_only) {
    int print_width = width > 0 ? width : 92;

    int max_num_w = 1;
    if (g->n_clues_across > 0) {
        char b[8]; snprintf(b, sizeof(b), "%d", g->puz->across[g->n_clues_across - 1].num);
        if ((int)strlen(b) > max_num_w) max_num_w = (int)strlen(b);
    }
    if (g->n_clues_down > 0) {
        char b[8]; snprintf(b, sizeof(b), "%d", g->puz->down[g->n_clues_down - 1].num);
        if ((int)strlen(b) > max_num_w) max_num_w = (int)strlen(b);
    }

    /* Build clue_lines */
    char **clue_lines = NULL;
    int n_clue_lines = 0;
    #define ADDLINE(s) do { clue_lines = (char **)realloc(clue_lines, sizeof(char *) * (n_clue_lines + 1)); clue_lines[n_clue_lines++] = strdup(s); } while (0)

    if (!downs_only) {
        ADDLINE("ACROSS");
        ADDLINE("");
        for (int i = 0; i < g->n_clues_across; i++) {
            char buf[1024];
            snprintf(buf, sizeof(buf), "%*d. %s", max_num_w, g->puz->across[i].num, g->clues_across[i]);
            ADDLINE(buf);
        }
        ADDLINE("");
    }
    ADDLINE("DOWN");
    ADDLINE("");
    for (int i = 0; i < g->n_clues_down; i++) {
        char buf[1024];
        snprintf(buf, sizeof(buf), "%*d. %s", max_num_w, g->puz->down[i].num, g->clues_down[i]);
        ADDLINE(buf);
    }
    #undef ADDLINE

    int blank = strcmp(style ? style : "", "blank") == 0;
    int solution = strcmp(style ? style : "", "solution") == 0;

    wchar_t **grid_lines = NULL;
    int n_grid_lines = render_grid_text(g, 0, blank, solution, &grid_lines);
    int grid_line_w = (int)wcslen(grid_lines[0]);

    if (print_width < grid_line_w) {
        fprintf(stderr, "Puzzle is %d columns wide, cannot be printed at %d columns.\n",
                grid_line_w, print_width);
        exit(1);
    }
    if (print_width > 2 * grid_line_w) print_width = 2 * grid_line_w;

    /* Title line */
    {
        char buf[512];
        snprintf(buf, sizeof(buf), "%s - %s", g->title, g->author);
        int n = (int)strlen(buf);
        wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (n + 1));
        for (int i = 0; i < n; i++) w[i] = (wchar_t)(unsigned char)buf[i];
        w[n] = 0;
        print_wide_line(w);
        print_wide_line(L"");
        free(w);
    }

    int f_width = print_width - grid_line_w - 2;
    int gi = 0;
    char **current_clue = NULL;
    int n_current = 0, current_pos = 0;
    int ci = 0;

    if (f_width > 12) {
        while (gi < n_grid_lines) {
            if (current_pos >= n_current) {
                if (current_clue) {
                    for (int i = 0; i < n_current; i++) free(current_clue[i]);
                    free(current_clue);
                    current_clue = NULL;
                }
                if (ci < n_clue_lines) {
                    wrap_text(clue_lines[ci++], f_width, max_num_w + 2, &current_clue, &n_current);
                    current_pos = 0;
                } else {
                    current_clue = (char **)calloc(1, sizeof(char *));
                    current_clue[0] = strdup("");
                    n_current = 1;
                    current_pos = 0;
                }
            }
            const char *line = current_clue[current_pos++];
            int line_len = (int)strlen(line);
            if (line_len > f_width) line_len = f_width;
            int total_chars = f_width + 2 + grid_line_w;
            wchar_t *out = (wchar_t *)malloc(sizeof(wchar_t) * (total_chars + 1));
            for (int i = 0; i < f_width; i++) out[i] = i < line_len ? (wchar_t)(unsigned char)line[i] : L' ';
            out[f_width] = L' '; out[f_width + 1] = L' ';
            for (int i = 0; i < grid_line_w; i++) out[f_width + 2 + i] = grid_lines[gi][i];
            out[total_chars] = 0;
            print_wide_line(out);
            free(out);
            gi++;
        }
        if (current_clue) {
            /* leftover joined back */
            char *rem = (char *)calloc(2048, 1);
            int wrote = 0;
            for (int i = current_pos; i < n_current; i++) {
                int n = (int)strlen(current_clue[i]);
                if (wrote + n + 1 < 2047) {
                    if (wrote) rem[wrote++] = ' ';
                    memcpy(rem + wrote, current_clue[i], n);
                    wrote += n;
                }
            }
            if (wrote > 0) {
                clue_lines = (char **)realloc(clue_lines, sizeof(char *) * (n_clue_lines + 1));
                memmove(clue_lines + ci + 1, clue_lines + ci, sizeof(char *) * (n_clue_lines - ci));
                clue_lines[ci] = strdup(rem);
                n_clue_lines++;
            }
            free(rem);
            for (int i = 0; i < n_current; i++) free(current_clue[i]);
            free(current_clue);
            current_clue = NULL;
        }
    } else {
        for (int i = 0; i < n_grid_lines; i++) print_wide_line(grid_lines[i]);
    }

    /* Print remaining clues in columns */
    int num_cols = print_width > 64 ? 3 : 2;
    int column_w = print_width / num_cols - 2;

    char **wrapped = NULL;
    int n_wrapped = 0;
    for (int i = ci; i < n_clue_lines; i++) {
        if (clue_lines[i][0] == 0) {
            wrapped = (char **)realloc(wrapped, sizeof(char *) * (n_wrapped + 1));
            wrapped[n_wrapped++] = strdup("");
            continue;
        }
        int indent_w = max_num_w + 2;
        char **sub; int n_sub;
        wrap_text(clue_lines[i], column_w, indent_w, &sub, &n_sub);
        for (int k = 0; k < n_sub; k++) {
            wrapped = (char **)realloc(wrapped, sizeof(char *) * (n_wrapped + 1));
            wrapped[n_wrapped++] = sub[k];
        }
        free(sub);
    }

    int rows = (n_wrapped + num_cols - 1) / num_cols;
    for (int r = 0; r < rows; r++) {
        char *row = (char *)calloc(print_width + 16, 1);
        int rpos = 0;
        for (int col = 0; col < num_cols; col++) {
            int idx = r + col * rows;
            const char *part = idx < n_wrapped ? wrapped[idx] : "";
            int n = (int)strlen(part);
            for (int i = 0; i < column_w; i++) {
                row[rpos++] = i < n ? part[i] : ' ';
            }
            if (col < num_cols - 1) { row[rpos++] = ' '; row[rpos++] = ' '; }
        }
        row[rpos] = 0;
        /* Trim trailing spaces */
        while (rpos > 0 && row[rpos - 1] == ' ') row[--rpos] = 0;
        wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (rpos + 1));
        for (int i = 0; i < rpos; i++) w[i] = (wchar_t)(unsigned char)row[i];
        w[rpos] = 0;
        print_wide_line(w);
        free(w);
        free(row);
    }

    /* cleanup */
    for (int i = 0; i < n_grid_lines; i++) free(grid_lines[i]);
    free(grid_lines);
    for (int i = 0; i < n_clue_lines; i++) free(clue_lines[i]);
    free(clue_lines);
    for (int i = 0; i < n_wrapped; i++) free(wrapped[i]);
    free(wrapped);
}

/* ----- Main interactive loop ----- */

static void show_time(Grid *g, int seconds, int active) {
    int s = seconds % 60;
    int m = (seconds / 60) % 60;
    int h = seconds / 3600;
    char buf[32];
    if (h > 0) snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
    else snprintf(buf, sizeof(buf), "   %02d:%02d", m, s);
    int len = (int)strlen(buf);
    int x = g->grid_x + g->column_count * 4 - 7;
    int y = 2;
    wchar_t w[16]; for (int i = 0; i < len; i++) w[i] = (wchar_t)(unsigned char)buf[i]; w[len] = 0;
    write_at(x, y, w, attr_normal());
    (void)active;
}

static void draw_clue_for_word(Grid *g, Cursor *cu, int downs_only,
                                int info_x, int info_y, int clue_w) {
    int wi;
    Word *w = find_current_word(cu, &wi);
    const char *clue_text = "";
    int num = 0;
    if (w) {
        Word *list = cu->direction == 0 ? g->across_words : g->down_words;
        char **clue_arr = cu->direction == 0 ? g->clues_across : g->clues_down;
        PuzClueEntry *entries = cu->direction == 0 ? g->puz->across : g->puz->down;
        clue_text = clue_arr[wi];
        num = entries[wi].num;
        if (cu->direction == 0 && downs_only) clue_text = "—";
        (void)list;
    }
    char buf[2048];
    if (num > 0) snprintf(buf, sizeof(buf), "%d %s: %s", num, cu->direction == 0 ? "ACROSS" : "DOWN", clue_text);
    else buf[0] = 0;

    char **lines; int n_lines;
    wrap_text(buf, clue_w, 2, &lines, &n_lines);
    if (n_lines > 3) n_lines = 3;
    for (int i = 0; i < 3; i++) {
        const char *line = i < n_lines ? lines[i] : "";
        int len = (int)strlen(line);
        wchar_t *wl = (wchar_t *)malloc(sizeof(wchar_t) * (g_term_w + 1));
        int put = 0;
        for (int k = 0; k < len && put < g_term_w - info_x; k++) wl[put++] = (wchar_t)(unsigned char)line[k];
        for (; put < g_term_w - info_x; put++) wl[put] = L' ';
        wl[put] = 0;
        write_at(info_x, info_y + i, wl, attr_normal());
        free(wl);
    }
    for (int i = 0; i < n_lines; i++) free(lines[i]);
    if (n_lines > 0 || lines) free(lines);
}

static void word_eq_copy(int **dst, int *dst_len, Word *src) {
    int n = src ? src->len : 0;
    int *a = (int *)malloc(sizeof(int) * (n > 0 ? n : 1));
    if (src) for (int i = 0; i < n; i++) a[i] = src->cell_indices[i];
    *dst = a;
    *dst_len = n;
}

static int words_equal(int *a, int an, Word *b) {
    int bn = b ? b->len : 0;
    if (an != bn) return 0;
    if (!b) return 1;
    for (int i = 0; i < an; i++) if (a[i] != b->cell_indices[i]) return 0;
    return 1;
}

int run_interactive(Grid *g, int downs_only, const char *filename) {
    int gx = g->grid_x, gy = g->grid_y;
    int puzzle_w = g->column_count * 4;
    if (puzzle_w < 40) puzzle_w = 40;
    int min_w = puzzle_w + gx + 2;
    int min_h = 2 * g->row_count + gy + 2 + 3 + 2 + 2;

    if (g_term_w < min_w || g_term_h < min_h) {
        fprintf(stderr, "Terminal must be %d cols wide and %d rows tall (currently %dx%d).\n",
                min_w, min_h, g_term_w, g_term_h);
        return 1;
    }
    if (puz_has_rebus(g->puz)) {
        fprintf(stderr, "This puzzle contains rebus features not supported by win-cursewords.\n");
        return 1;
    }

    /* Hide cursor */
    CONSOLE_CURSOR_INFO ci;
    GetConsoleCursorInfo(g_hOut, &ci);
    CONSOLE_CURSOR_INFO ci_hidden = ci;
    ci_hidden.bVisible = FALSE;
    SetConsoleCursorInfo(g_hOut, &ci_hidden);

    clear_screen();

    /* Header */
    char headline[1024];
    int sw_width = (int)strlen("win-cursewords v" VERSION) + 5;
    int pz_width = g_term_w - sw_width - 2;
    char puzzle_info[512];
    snprintf(puzzle_info, sizeof(puzzle_info), "%s - %s", g->title, g->author);
    if ((int)strlen(puzzle_info) > pz_width && pz_width > 1) {
        puzzle_info[pz_width - 1] = 0;
    }
    snprintf(headline, sizeof(headline), " %-*s%*s ", pz_width, puzzle_info, sw_width, "win-cursewords v" VERSION);
    int hl_len = (int)strlen(headline);
    if (hl_len > g_term_w) hl_len = g_term_w;
    wchar_t *hw = (wchar_t *)malloc(sizeof(wchar_t) * (hl_len + 1));
    for (int i = 0; i < hl_len; i++) hw[i] = (wchar_t)(unsigned char)headline[i];
    hw[hl_len] = 0;
    write_at(0, 0, hw, attr_reverse(attr_normal()));
    free(hw);

    grid_draw(g, 0);

    /* Toolbar */
    const struct { const char *sc; const char *act; } cmds[] = {
        {"^Q", "quit"}, {"^S", "save"}, {"^P", "pause"}, {"^C", "check"},
        {"^R", "reveal"}, {"^G", "go to"}, {"^X", "clear"}, {"^Z", "reset"}
    };
    int ncmd = sizeof(cmds) / sizeof(cmds[0]);

    g->notify_x = gx;
    g->notify_y = g_term_h - 2;

    int tb_y;
    int single_line_tb = (g_term_w >= 15 * ncmd);
    if (single_line_tb) {
        tb_y = g_term_h - 1;
    } else {
        tb_y = g_term_h - 2;
        g->notify_y = g_term_h - 3;
    }
    /* draw toolbar */
    {
        int cx = gx, cy = tb_y;
        int half = ncmd / 2 - 1;
        for (int i = 0; i < ncmd; i++) {
            wchar_t sc[3] = { (wchar_t)cmds[i].sc[0], (wchar_t)cmds[i].sc[1], 0 };
            write_at(cx, cy, sc, attr_reverse(attr_normal()));
            wchar_t actbuf[64];
            int n = (int)mbstowcs(actbuf, cmds[i].act, 60);
            actbuf[n] = 0;
            wchar_t out[80];
            out[0] = L' ';
            wcscpy(out + 1, actbuf);
            int olen = (int)wcslen(out);
            for (int k = olen; k < 23; k++) out[k] = L' ';
            out[23] = 0;
            write_at(cx + 2, cy, out, attr_normal());
            cx += 25;
            if (!single_line_tb && i == half) { cx = gx; cy++; }
        }
    }

    int clue_w = (int)(1.3 * puzzle_w) - gx;
    if (clue_w > g_term_w - 2 - gx) clue_w = g_term_w - 2 - gx;
    int info_x = gx;
    int info_y = gy + 2 * g->row_count + 2;

    Cursor cur;
    cur.grid = g;
    cur.direction = 0;
    cur.pos_idx = g->across_words[0].cell_indices[0];

    int *old_word = NULL; int old_word_len = -1;
    int old_pos = cur.pos_idx;
    int puzzle_paused = 0;
    int puzzle_complete = 0;
    int modified_since_save = 0;
    int overwrite_mode = 0;
    int to_quit = 0;
    int timer_seconds = g->puz->timer_seconds;
    int timer_running = g->puz->timer_running ? g->puz->timer_running : 1;
    if (!g->puz->has_timer) timer_running = 1;
    double timer_anchor = now_seconds();
    int timer_anchor_secs = timer_seconds;

    show_time(g, timer_seconds, timer_running);

    while (!to_quit) {
        Word *cw = find_current_word(&cur, NULL);
        int cw_changed = !words_equal(old_word, old_word_len, cw);

        if (cw_changed) {
            overwrite_mode = 0;
            /* unhighlight old word */
            for (int i = 0; i < old_word_len; i++) {
                int x = old_word[i] % g->column_count, y = old_word[i] / g->column_count;
                draw_cell(g, x, y);
            }
            if (cw) {
                for (int i = 0; i < cw->len; i++) {
                    int x = cw->cell_indices[i] % g->column_count, y = cw->cell_indices[i] / g->column_count;
                    draw_highlighted_cell(g, x, y);
                }
            }
            draw_clue_for_word(g, &cur, downs_only, info_x, info_y, clue_w);
        } else {
            int x = old_pos % g->column_count, y = old_pos / g->column_count;
            draw_highlighted_cell(g, x, y);
        }

        int cx = cur.pos_idx % g->column_count, cy = cur.pos_idx / g->column_count;
        draw_cursor_cell(g, cx, cy);

        /* Completeness check */
        if (!puzzle_complete) {
            int sz = g->row_count * g->column_count;
            int complete = 1;
            for (int i = 0; i < sz; i++) {
                if (!is_correct_cell(&g->cells[i])) { complete = 0; break; }
            }
            if (complete) {
                puzzle_complete = 1;
                const char *msg = " You've completed the puzzle! ";
                int len = (int)strlen(msg);
                wchar_t *w = (wchar_t *)malloc(sizeof(wchar_t) * (len + 1));
                for (int i = 0; i < len; i++) w[i] = (wchar_t)(unsigned char)msg[i];
                w[len] = 0;
                write_at(gx, 2, w, attr_reverse(attr_normal()));
                free(w);
                timer_running = 0;
            }
        }

        /* Update timer if running */
        if (timer_running && !puzzle_paused) {
            int now_secs = timer_anchor_secs + (int)(now_seconds() - timer_anchor);
            if (now_secs != timer_seconds) {
                timer_seconds = now_secs;
                show_time(g, timer_seconds, timer_running);
            }
        }
        maybe_clear_notification(g);

        Key k = read_key_blocking(500);
        if (!k.valid) continue;

        old_pos = cur.pos_idx;
        free(old_word);
        word_eq_copy(&old_word, &old_word_len, cw);
        Cell *current_cell = &g->cells[cur.pos_idx];

        /* Decode special keys */
        int handled = 0;

        /* Ctrl combinations */
        if (k.ctrl && (k.vk == 'Q' || k.ch == 17)) {
            if (modified_since_save) {
                char buf[8];
                get_notification_input(g, "Quit without saving? (y/n)", 1, 1, 1, buf, sizeof(buf));
                if (buf[0] == 'y' || buf[0] == 'Y') to_quit = 1;
                else send_notification(g, "Quit command canceled.");
            } else to_quit = 1;
            handled = 1;
        }
        else if (k.ctrl && (k.vk == 'S' || k.ch == 19)) {
            puz_set_timer(g->puz, timer_seconds, timer_running);
            grid_save(g, filename);
            modified_since_save = 0;
            handled = 1;
        }
        else if (k.ctrl && (k.vk == 'P' || k.ch == 16) && !puzzle_complete) {
            if (timer_running) {
                timer_running = 0;
                grid_draw(g, 1);
                wchar_t pmsg[] = L"PUZZLE PAUSED";
                int plen = (int)wcslen(pmsg);
                wchar_t *blank = (wchar_t *)malloc(sizeof(wchar_t) * (g_term_w + 1));
                for (int i = 0; i < g_term_w - info_x; i++) blank[i] = L' ';
                blank[g_term_w - info_x] = 0;
                for (int i = 0; i < 3; i++) write_at(info_x, info_y + i, blank, attr_normal());
                free(blank);
                write_at(info_x, info_y, pmsg, attr_normal()); (void)plen;
                puzzle_paused = 1;
            } else {
                timer_running = 1;
                timer_anchor = now_seconds();
                timer_anchor_secs = timer_seconds;
                grid_draw(g, 0);
                free(old_word); old_word = NULL; old_word_len = -1;
                puzzle_paused = 0;
            }
            handled = 1;
        }
        else if (k.ctrl && (k.vk == 'Z' || k.ch == 26)) {
            char buf[8];
            get_notification_input(g, "Reset puzzle? (y/n)", 1, 1, 1, buf, sizeof(buf));
            if (buf[0] == 'y' || buf[0] == 'Y') {
                send_notification(g, "Puzzle reset.");
                int sz = g->row_count * g->column_count;
                for (int i = 0; i < sz; i++) {
                    Cell *c = &g->cells[i];
                    if (cell_is_letter(c)) {
                        c->entry = '-';
                        c->marked_wrong = 0;
                        c->corrected = 0;
                        c->revealed = 0;
                        int x = i % g->column_count, y = i / g->column_count;
                        draw_cell(g, x, y);
                    }
                }
                timer_seconds = 0;
                timer_anchor = now_seconds();
                timer_anchor_secs = 0;
                show_time(g, timer_seconds, timer_running);
                modified_since_save = 1;
                if (!puzzle_paused) { free(old_word); old_word = NULL; old_word_len = -1; }
            } else {
                send_notification(g, "Reset command canceled.");
            }
            handled = 1;
        }

        if (handled) continue;
        if (puzzle_paused) continue;

        if (k.ctrl && (k.vk == 'C' || k.ch == 3)) {
            char buf[8];
            get_notification_input(g, "Check (l)etter, (w)ord, or (p)uzzle?", 1, 1, 0, buf, sizeof(buf));
            char ch = buf[0] >= 'A' && buf[0] <= 'Z' ? (buf[0] + 32) : buf[0];
            if (ch == 'l') { check_cell(g, cur.pos_idx); send_notification(g, "Checked letter for errors."); }
            else if (ch == 'w' && cw) {
                for (int i = 0; i < cw->len; i++) check_cell(g, cw->cell_indices[i]);
                send_notification(g, "Checked word for errors.");
            } else if (ch == 'p') {
                int sz = g->row_count * g->column_count;
                for (int i = 0; i < sz; i++) check_cell(g, i);
                send_notification(g, "Checked puzzle for errors.");
            } else send_notification(g, "No valid input entered.");
            free(old_word); old_word = NULL; old_word_len = -1;
            continue;
        }
        if (k.ctrl && (k.vk == 'R' || k.ch == 18)) {
            char buf[8];
            get_notification_input(g, "Reveal (l)etter, (w)ord, or (p)uzzle?", 1, 1, 0, buf, sizeof(buf));
            char ch = buf[0] >= 'A' && buf[0] <= 'Z' ? (buf[0] + 32) : buf[0];
            if (ch == 'l') { reveal_cell(g, cur.pos_idx); send_notification(g, "Revealed answers for letter."); }
            else if (ch == 'w' && cw) {
                for (int i = 0; i < cw->len; i++) reveal_cell(g, cw->cell_indices[i]);
                send_notification(g, "Revealed answers for word.");
            } else if (ch == 'p') {
                int sz = g->row_count * g->column_count;
                for (int i = 0; i < sz; i++) reveal_cell(g, i);
                send_notification(g, "Revealed answers for puzzle.");
            } else send_notification(g, "No valid input entered.");
            modified_since_save = 1;
            free(old_word); old_word = NULL; old_word_len = -1;
            continue;
        }
        if (k.ctrl && (k.vk == 'G' || k.ch == 7)) {
            char buf[8];
            get_notification_input(g, "Enter square number:", 3, 0, 0, buf, sizeof(buf));
            if (buf[0]) {
                int n = atoi(buf);
                int found = -1;
                int sz = g->row_count * g->column_count;
                for (int i = 0; i < sz; i++) if (g->cells[i].number == n) { found = i; break; }
                if (found >= 0) {
                    cur.pos_idx = found;
                    char m[64]; snprintf(m, sizeof(m), "Moved cursor to square %d.", n);
                    send_notification(g, m);
                } else send_notification(g, "Not a valid number.");
            } else send_notification(g, "No valid number entered.");
            continue;
        }
        if (k.ctrl && (k.vk == 'X' || k.ch == 24)) {
            char buf[8];
            get_notification_input(g, "Clear puzzle? (y/n)", 1, 1, 1, buf, sizeof(buf));
            if (buf[0] == 'y' || buf[0] == 'Y') {
                send_notification(g, "Puzzle cleared.");
                int sz = g->row_count * g->column_count;
                for (int i = 0; i < sz; i++) {
                    Cell *c = &g->cells[i];
                    if (cell_is_letter(c)) {
                        c->entry = '-';
                        if (c->marked_wrong) { c->marked_wrong = 0; c->corrected = 1; }
                        int x = i % g->column_count, y = i / g->column_count;
                        draw_cell(g, x, y);
                    }
                }
                free(old_word); old_word = NULL; old_word_len = -1;
                modified_since_save = 1;
            } else {
                send_notification(g, "Clear command canceled.");
            }
            continue;
        }

        /* Letter entry */
        if (!puzzle_complete && !k.ctrl && k.ch && ((k.ch >= 'A' && k.ch <= 'Z') || (k.ch >= 'a' && k.ch <= 'z') || (k.ch >= '0' && k.ch <= '9'))) {
            if (!is_blankish_cell(current_cell)) overwrite_mode = 1;
            char ch = k.ch;
            if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');
            current_cell->entry = ch;
            if (current_cell->marked_wrong) { current_cell->marked_wrong = 0; current_cell->corrected = 1; }
            modified_since_save = 1;
            cursor_advance_within_word(&cur, overwrite_mode, 1);
            continue;
        }

        /* Backspace / Delete */
        if (!puzzle_complete && (k.vk == VK_BACK || k.vk == VK_DELETE)) {
            if (cell_is_letter(current_cell)) {
                current_cell->entry = '-';
                if (current_cell->marked_wrong) { current_cell->marked_wrong = 0; current_cell->corrected = 1; }
            }
            overwrite_mode = 1;
            modified_since_save = 1;
            if (k.vk == VK_BACK) cursor_retreat_within_word(&cur, 1, 0);
            else cursor_advance_within_word(&cur, 1, 0);
            continue;
        }

        /* Tab / shift-tab */
        if (k.vk == VK_TAB && !k.shift) {
            if (is_blankish_cell(current_cell)) cursor_advance_to_next_word(&cur, 1);
            else cursor_advance_within_word(&cur, 0, 0);
            continue;
        }
        if (k.vk == VK_TAB && k.shift) {
            cursor_retreat_within_word(&cur, 0, 1);
            continue;
        }

        /* Page up/down */
        if (k.vk == VK_NEXT) { cursor_advance_to_next_word(&cur, 0); continue; }
        if (k.vk == VK_PRIOR) { cursor_retreat_to_previous_word(&cur, 0, 0); continue; }

        /* Enter / Space → switch direction */
        if (k.vk == VK_RETURN || k.ch == ' ') {
            cursor_switch(&cur);
            if (!find_current_word(&cur, NULL)) cursor_switch(&cur);
            continue;
        }

        /* Arrow keys */
        if (cur.direction == 0) {
            if (k.vk == VK_RIGHT && !k.shift) { cursor_advance(&cur); continue; }
            if (k.vk == VK_LEFT && !k.shift) { cursor_retreat(&cur); continue; }
            if (k.vk == VK_DOWN && !k.shift) { cursor_switch(&cur); continue; }
            if (k.vk == VK_UP && !k.shift) { cursor_switch(&cur); continue; }
            if (k.vk == VK_RIGHT && k.shift) { cursor_advance_within_word(&cur, 0, 0); continue; }
            if (k.vk == VK_LEFT && k.shift) { cursor_retreat_within_word(&cur, 0, 1); continue; }
            if (k.vk == VK_DOWN && k.shift) { cursor_advance_perpendicular(&cur); continue; }
            if (k.vk == VK_UP && k.shift) { cursor_retreat_perpendicular(&cur); continue; }
        } else {
            if (k.vk == VK_DOWN && !k.shift) { cursor_advance(&cur); continue; }
            if (k.vk == VK_UP && !k.shift) { cursor_retreat(&cur); continue; }
            if (k.vk == VK_LEFT && !k.shift) { cursor_switch(&cur); continue; }
            if (k.vk == VK_RIGHT && !k.shift) { cursor_switch(&cur); continue; }
            if (k.vk == VK_DOWN && k.shift) { cursor_advance_within_word(&cur, 0, 0); continue; }
            if (k.vk == VK_UP && k.shift) { cursor_retreat_within_word(&cur, 0, 1); continue; }
            if (k.vk == VK_RIGHT && k.shift) { cursor_advance_perpendicular(&cur); continue; }
            if (k.vk == VK_LEFT && k.shift) { cursor_retreat_perpendicular(&cur); continue; }
        }

        /* Bracket keys for perpendicular jumps */
        if (k.ch == '}' || k.ch == ']') {
            cursor_advance_perpendicular(&cur);
            if (k.ch == '}' && blank_cells_remaining(g)) {
                while (!is_blankish_cell(&g->cells[cur.pos_idx])) cursor_advance_perpendicular(&cur);
            }
            continue;
        }
        if (k.ch == '{' || k.ch == '[') {
            cursor_retreat_perpendicular(&cur);
            if (k.ch == '{' && blank_cells_remaining(g)) {
                while (!is_blankish_cell(&g->cells[cur.pos_idx])) cursor_retreat_perpendicular(&cur);
            }
            continue;
        }
    }

    SetConsoleCursorInfo(g_hOut, &ci);
    clear_screen();
    free(old_word);
    return 0;
}

/* ----- Argument parsing & main ----- */

static void usage(void) {
    fprintf(stderr,
        "usage: win-cursewords [-h] [--downs-only] [--version] PUZfile\n"
        "       win-cursewords [--print] [--blank | --solution] [--width INT] PUZfile\n");
}

int main(int argc, char **argv) {
    const char *filename = NULL;
    int downs_only = 0;
    int print_mode = 0;
    int blank = 0, solution = 0;
    int print_width = 0;

    for (int i = 1; i < argc; i++) {
        const char *a = argv[i];
        if (strcmp(a, "-h") == 0 || strcmp(a, "--help") == 0) { usage(); return 0; }
        else if (strcmp(a, "--downs-only") == 0) downs_only = 1;
        else if (strcmp(a, "--print") == 0) print_mode = 1;
        else if (strcmp(a, "--blank") == 0) blank = 1;
        else if (strcmp(a, "--solution") == 0) solution = 1;
        else if (strcmp(a, "--width") == 0 && i + 1 < argc) print_width = atoi(argv[++i]);
        else if (strcmp(a, "--version") == 0) { printf("%s\n", VERSION); return 0; }
        else if (a[0] == '-') { usage(); return 2; }
        else filename = a;
    }
    if (!filename) { usage(); return 2; }
    if (blank && solution) { usage(); return 2; }

    g_hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    g_hIn = GetStdHandle(STD_INPUT_HANDLE);

    /* Set output codepage to UTF-8 for non-console pipes; conhost wide writes don't need it. */
    SetConsoleOutputCP(CP_UTF8);

    DWORD outmode_dummy;
    int is_console_out = GetConsoleMode(g_hOut, &outmode_dummy) ? 1 : 0;
    DWORD inmode_orig = 0;
    int is_console_in = GetConsoleMode(g_hIn, &inmode_orig) ? 1 : 0;

    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (is_console_out && GetConsoleScreenBufferInfo(g_hOut, &csbi)) {
        g_term_w = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        g_term_h = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        g_default_attr = csbi.wAttributes;
    }

    char err[256] = {0};
    Puzzle *puz = puz_load_file(filename, err, sizeof(err));
    if (!puz) {
        fprintf(stderr, "Unable to parse %s as a .puz file: %s\n", filename, err);
        return 1;
    }

    int gx = 2, gy = 4;
    Grid g = {0};
    grid_load(&g, puz, gx, gy);

    if (print_mode || !is_console_out) {
        const char *style = solution ? "solution" : (blank ? "blank" : NULL);
        printer_output(&g, style, print_width, downs_only);
        grid_free(&g);
        puz_free(puz);
        return 0;
    }

    /* Switch input to raw mode */
    DWORD inmode = inmode_orig;
    inmode &= ~(ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT | ENABLE_PROCESSED_INPUT
                | ENABLE_MOUSE_INPUT | ENABLE_QUICK_EDIT_MODE);
    inmode |= ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT;
    SetConsoleMode(g_hIn, inmode);

    /* Disable wrap-on-end-of-line so we don't scroll */
    DWORD outmode = 0;
    GetConsoleMode(g_hOut, &outmode);
    SetConsoleMode(g_hOut, outmode & ~ENABLE_WRAP_AT_EOL_OUTPUT);

    int rc = run_interactive(&g, downs_only, filename);

    SetConsoleMode(g_hIn, inmode_orig);
    SetConsoleMode(g_hOut, outmode);

    grid_free(&g);
    puz_free(puz);
    (void)is_console_in;
    return rc;
}
