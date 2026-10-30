/* KMD player for Fury of the Furries.
 *
 * The original game feeds an OPL2 with six melodic channels plus the five
 * rhythm voices.  This is a C port of the player logic used by FURY.EXE:
 * 64-row patterns, 30 bytes per row (6 * note/instrument/effect/param and
 * six percussion bytes), 28-byte FM instruments and the driver's note and
 * volume tables.
 *
 * The music tick rate of the original sound ISR is 50 Hz; the song speed is
 * the number of ticks per row.
 */

#include "kmdplayer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FILE 65536
#define ROWS     64
#define ROW_SIZE 30

static opl3_chip *g_chip;
static unsigned char g_file[MAX_FILE];
static int g_file_size;
static int g_active;

static unsigned char *g_orders;
static unsigned char *g_inst;
static unsigned char *g_pat;
static int g_inst_count;
static int g_order_count;

static int g_pattern;
static int g_row;
static int g_tick;
static int g_speed;

struct kmd_channel {
    int cur_inst;
    int note;
    int period;              /* block<<10 | fnum */
    int keyon;
    int vol;
    unsigned char *idata;    /* 12 instrument bytes */
};

static struct kmd_channel g_ch[6];

/* Note periods (note-12), taken from the original driver's table. */
static const unsigned short period_tab[64] = {
    3424, 3232, 3048, 2880, 2702, 2560, 2416, 2280,
    2152, 2032, 1920, 1812, 1712, 1616, 1524, 1440,
    1356, 1280, 1208, 1140, 1076, 1016,  960,  906,
     856,  808,  762,  720,  678,  640,  604,  570,
     538,  508,  480,  453,  428,  404,  381,  360,
     339,  320,  302,  285,  269,  254,  240,  226,
     214,  202,  190,  180,  170,  160,  151,  143,
     135,  127,  120,  113,  107,  101,   95,   90
};

/* Attenuation for a 0..63 volume value, from the original driver. */
static const unsigned char vol_tab[64] = {
    63, 45, 39, 34, 31, 29, 27, 25, 24, 22, 21, 20, 19, 18, 17, 17,
    16, 15, 15, 14, 13, 13, 12, 12, 11, 11, 10, 10,  9,  9,  9,  8,
     8,  8,  7,  7,  7,  6,  6,  6,  5,  5,  5,  5,  4,  4,  4,  4,
     3,  3,  3,  3,  2,  2,  2,  2,  1,  1,  1,  1,  1,  1,  0,  0
};

static const unsigned char op1_off[6] = { 0x00, 0x01, 0x02, 0x08, 0x09, 0x0A };
static const unsigned char op2_off[6] = { 0x03, 0x04, 0x05, 0x0B, 0x0C, 0x0D };

static int fnum_for_period(unsigned short period)
{
    unsigned short f, q;
    unsigned long v;
    int block = 0;

    if (!period)
        return 0;
    f = (unsigned short)(0xF727u / period);
    q = (unsigned short)(0x12u / period);
    v = ((unsigned long)q << 16) | f;
    while (q != 0 || (v & 0xFFFFu) >= 0x400u) {
        v >>= 1;
        q = (unsigned short)(v >> 16);
        if (++block >= 7)
            break;
    }
    return ((block & 7) << 10) | (int)(v & 0x3FFu);
}

static unsigned char *instrument(int idx)
{
    if (idx < 0 || idx >= g_inst_count)
        return NULL;
    return g_inst + idx * 28 + 16;
}

static void set_volume(int ch, int vol)
{
    struct kmd_channel *c = &g_ch[ch];
    unsigned char tl;
    if (!c->idata)
        return;
    if (vol < 0) vol = 0;
    if (vol > 63) vol = 63;
    c->vol = vol;
    tl = vol_tab[vol & 63];
    OPL3_WriteReg(g_chip, 0x40 + op2_off[ch],
                  (unsigned char)((c->idata[3] & 0xC0) | tl));
}

static void load_instrument(int ch, int idx)
{
    unsigned char *d = instrument(idx);
    int o1 = op1_off[ch], o2 = op2_off[ch];

    if (!d)
        return;
    g_ch[ch].idata = d;
    g_ch[ch].cur_inst = idx;
    OPL3_WriteReg(g_chip, 0x20 + o1, d[0]);
    OPL3_WriteReg(g_chip, 0x20 + o2, d[1]);
    OPL3_WriteReg(g_chip, 0x40 + o1, d[2]);
    OPL3_WriteReg(g_chip, 0x40 + o2, d[3]);
    OPL3_WriteReg(g_chip, 0x60 + o1, d[4]);
    OPL3_WriteReg(g_chip, 0x60 + o2, d[5]);
    OPL3_WriteReg(g_chip, 0x80 + o1, d[6]);
    OPL3_WriteReg(g_chip, 0x80 + o2, d[7]);
    OPL3_WriteReg(g_chip, 0xE0 + o1, d[8]);
    OPL3_WriteReg(g_chip, 0xE0 + o2, d[9]);
    OPL3_WriteReg(g_chip, 0xC0 + ch, d[10]);
    {
        int v = 0x3F ^ ((d[3] & 0x3F) * 2);
        set_volume(ch, v & 0x3F);
    }
}

static void note_on(int ch, int note)
{
    struct kmd_channel *c = &g_ch[ch];
    int idx = note - 12;
    int p, f;

    if (idx < 0) idx = 0;
    if (idx >= 64) idx = 63;
    p = fnum_for_period(period_tab[idx]);
    c->note = note;
    c->period = p;
    c->keyon = 1;
    f = p;
    OPL3_WriteReg(g_chip, 0xA0 + ch, (unsigned char)(f & 0xFF));
    OPL3_WriteReg(g_chip, 0xB0 + ch,
                  (unsigned char)(0x20 | (((f >> 10) & 7) << 2) |
                                  ((f >> 8) & 3)));
}

static void note_off(int ch)
{
    struct kmd_channel *c = &g_ch[ch];
    int f = c->period;
    c->keyon = 0;
    OPL3_WriteReg(g_chip, 0xB0 + ch,
                  (unsigned char)((((f >> 10) & 7) << 2) | ((f >> 8) & 3)));
}

static void load_drum(int type)
{
    int idx;
    for (idx = 0; idx < g_inst_count; idx++) {
        unsigned char *d = instrument(idx);
        if (!d || d[11] != type)
            continue;
        if (type == 1) {            /* bass drum: channel 6 */
            OPL3_WriteReg(g_chip, 0x30, d[0]);
            OPL3_WriteReg(g_chip, 0x33, d[1]);
            OPL3_WriteReg(g_chip, 0x50, d[2]);
            OPL3_WriteReg(g_chip, 0x53, d[3]);
            OPL3_WriteReg(g_chip, 0x70, d[4]);
            OPL3_WriteReg(g_chip, 0x73, d[5]);
            OPL3_WriteReg(g_chip, 0x90, d[6]);
            OPL3_WriteReg(g_chip, 0x93, d[7]);
            OPL3_WriteReg(g_chip, 0xF0, d[8]);
            OPL3_WriteReg(g_chip, 0xF3, d[9]);
            OPL3_WriteReg(g_chip, 0xC6, d[10]);
        } else if (type == 2) {     /* snare: channel 7 op2 */
            OPL3_WriteReg(g_chip, 0x34, d[0]);
            OPL3_WriteReg(g_chip, 0x54, d[2]);
            OPL3_WriteReg(g_chip, 0x74, d[4]);
            OPL3_WriteReg(g_chip, 0x94, d[6]);
            OPL3_WriteReg(g_chip, 0xF4, d[8]);
        } else if (type == 5) {     /* hi-hat: channel 7 op1 */
            OPL3_WriteReg(g_chip, 0x31, d[0]);
            OPL3_WriteReg(g_chip, 0x51, d[2]);
            OPL3_WriteReg(g_chip, 0x71, d[4]);
            OPL3_WriteReg(g_chip, 0x91, d[6]);
            OPL3_WriteReg(g_chip, 0xF1, d[8]);
        } else if (type == 3) {     /* tom: channel 8 op2 */
            OPL3_WriteReg(g_chip, 0x35, d[0]);
            OPL3_WriteReg(g_chip, 0x55, d[2]);
            OPL3_WriteReg(g_chip, 0x75, d[4]);
            OPL3_WriteReg(g_chip, 0x95, d[6]);
            OPL3_WriteReg(g_chip, 0xF5, d[8]);
        } else if (type == 4) {     /* cymbal: channel 8 op1 */
            OPL3_WriteReg(g_chip, 0x32, d[0]);
            OPL3_WriteReg(g_chip, 0x52, d[2]);
            OPL3_WriteReg(g_chip, 0x72, d[4]);
            OPL3_WriteReg(g_chip, 0x92, d[6]);
            OPL3_WriteReg(g_chip, 0xF2, d[8]);
        }
    }
}

static void process_row(void)
{
    unsigned char *row = g_pat + g_pattern * (ROWS * ROW_SIZE) + g_row * ROW_SIZE;
    int ch;
    int i;

    for (ch = 0; ch < 6; ch++) {
        int note = row[ch * 4 + 0];
        int ins  = row[ch * 4 + 1];
        int eff  = row[ch * 4 + 2];
        int par  = row[ch * 4 + 3];

        if (ins && ins != g_ch[ch].cur_inst)
            load_instrument(ch, ins - 1);
        if (note) {
            if (note == 0xF0)
                note_off(ch);
            else
                note_on(ch, note);
        }
        if (eff == 0x0C)
            set_volume(ch, par);
        else if (eff == 0x0F) {
            if (par)
                g_speed = par;
        } else if (eff == 0x0A) {
            if (par & 0xF0)
                set_volume(ch, g_ch[ch].vol - (par >> 4));
            else
                set_volume(ch, g_ch[ch].vol + (par & 0x0F));
        }
    }

    /* percussion */
    {
        int mask = row[24] & 0x1F;
        if (mask) {
            OPL3_WriteReg(g_chip, 0xBD, (unsigned char)(0xE0 | mask));
            OPL3_WriteReg(g_chip, 0x51, vol_tab[row[25] & 63]);
            OPL3_WriteReg(g_chip, 0x52, vol_tab[row[26] & 63]);
            OPL3_WriteReg(g_chip, 0x55, vol_tab[row[27] & 63]);
            OPL3_WriteReg(g_chip, 0x54, vol_tab[row[28] & 63]);
            OPL3_WriteReg(g_chip, 0x53, vol_tab[row[29] & 63]);
        } else {
            OPL3_WriteReg(g_chip, 0xBD, 0xE0);
        }
    }
}

void kmd_player_init(opl3_chip *chip)
{
    g_chip = chip;
    g_active = 0;
}

static void kmd_log_row(void)
{
    FILE *f = fopen("kmdplayer.log", "a");
    int ch;
    if (!f)
        return;
    fprintf(f, "load pat=%d row=%d speed=%d notes:", g_pattern, g_row, g_speed);
    for (ch = 0; ch < 6; ch++)
        fprintf(f, " %d/%d", g_ch[ch].note, g_ch[ch].cur_inst);
    fprintf(f, "\n");
    fclose(f);
}

int kmd_player_load(const char *path)
{
    FILE *f;
    int n;
    int i;

    if (!g_chip)
        return 0;
    f = fopen(path, "rb");
    if (!f)
        return 0;
    n = (int)fread(g_file, 1, sizeof g_file, f);
    fclose(f);
    if (n < 0x8A)
        return 0;
    g_file_size = n;
    g_inst_count = g_file[0];
    g_order_count = g_file[1];
    g_orders = g_file + 3;
    g_inst = g_file + 0x8A;
    g_pat = g_inst + g_inst_count * 28;
    if (g_pat + ROWS * ROW_SIZE > g_file + n)
        return 0;

    OPL3_Reset(g_chip, 49716);
    OPL3_WriteReg(g_chip, 0xBD, 0xE0);
    memset(g_ch, 0, sizeof g_ch);
    g_pattern = g_orders[0];
    g_row = 0;
    g_tick = 0;
    g_speed = 6;
    for (i = 1; i <= 5; i++)
        load_drum(i);
    /* The first row sets the real speed; play it immediately. */
    process_row();
    g_tick = 1;
    g_active = 1;
    {
        FILE *f = fopen("kmdplayer.log", "a");
        if (f) {
            fprintf(f, "LOAD %s size=%d inst=%d orders=%d first=%d\n",
                    path, n, g_inst_count, g_order_count, g_pattern);
            fclose(f);
        }
    }
    kmd_log_row();
    return 1;
}

void kmd_player_tick(void)
{
    if (!g_active)
        return;
    if (g_tick >= g_speed) {
        g_tick = 0;
        g_row++;
        if (g_row >= ROWS)
            g_row = 0;
        process_row();
    }
    g_tick++;
}

int kmd_player_active(void)
{
    return g_active;
}
