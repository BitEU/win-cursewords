#ifndef PUZ_H
#define PUZ_H

#include <stddef.h>
#include <stdint.h>

#define PUZ_MARKUP_PREV_INCORRECT 0x10
#define PUZ_MARKUP_INCORRECT      0x20
#define PUZ_MARKUP_REVEALED       0x40
#define PUZ_MARKUP_CIRCLED        0x80

typedef struct {
    char code[5];
    uint8_t *data;
    size_t length;
} PuzExtension;

typedef struct {
    int num;
    int clue_index;
    int cell;
    int len;
} PuzClueEntry;

typedef struct {
    char *title;
    char *author;
    char *copyright;
    char *notes;

    int width;
    int height;

    char *solution;
    char *fill;

    int n_clues;
    char **clues;

    uint8_t fileversion[5];
    uint8_t unk1[2];
    uint16_t scrambled_cksum;
    uint8_t unk2[12];
    uint16_t puzzletype;
    uint16_t solution_state;

    uint8_t *preamble;
    size_t preamble_len;
    uint8_t *postscript;
    size_t postscript_len;

    PuzExtension *extensions;
    int n_extensions;

    PuzClueEntry *across;
    int n_across;
    PuzClueEntry *down;
    int n_down;

    uint8_t *markup;

    int has_timer;
    int timer_seconds;
    int timer_running;
} Puzzle;

Puzzle *puz_load_file(const char *filename, char *errbuf, size_t errlen);
int puz_save_file(Puzzle *p, const char *filename, char *errbuf, size_t errlen);
void puz_free(Puzzle *p);

int puz_has_rebus(Puzzle *p);
int puz_has_markup(Puzzle *p);

void puz_set_timer(Puzzle *p, int seconds, int running);
void puz_set_markup(Puzzle *p, const uint8_t *markup, size_t length);
void puz_set_fill(Puzzle *p, const char *fill);

uint16_t puz_data_cksum(const uint8_t *data, size_t length, uint16_t cksum);

#endif
