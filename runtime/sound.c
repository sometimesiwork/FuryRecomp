/* Audio output for the recompiled game.
 *
 * Th
    static float eqb_x1, eqb_x2, eqb_y1, eqb_y2;
    static float eq5_x1, eq5_x2, eq5_y1, eq5_y2;e game's Adlib (OPL2) register writes are fed to Nuked OPL3, which
 * renders the FM stream; the game's one-bit PC-speaker effects are decoded
 * and mixed on top.
 *
 * Register writes carry a timestamp: the chip advances one sample for
 * every rendered sample and each queued write is applied at the sample
 * it was made, so note onsets and arpeggios keep the timing of the
 * original card instead of being quantised to whole output buffers.
 */

#include "opl3.h"
#include "kmdplayer.h"

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define NSAMP 1024              /* sample pairs per buffer */
#define NBUF  4
#define QSIZE 32768             /* ring of timestamped register writes */

static opl3_chip g_opl;
static int g_opl_ok;
static int g_core;              /* FM core: 0 = DBOPL (DOSBox), 1 = Nuked OPL3 */

/* DBOPL glue (dbopl.cpp) */
extern void dbopl_init(int rate);
extern void dbopl_write(int reg, int val);
extern void dbopl_gen(short *out);

static void chip_write(int reg, int val)
{
    if (g_core)
        OPL3_WriteReg(&g_opl, reg, val);
    else
        dbopl_write(reg, val);
}
static HWAVEOUT g_hwo;
static WAVEHDR g_hdr[NBUF];
static short g_buf[NBUF][NSAMP * 2];
static volatile int g_run;

static volatile unsigned g_head, g_tail;
static volatile int g_spk;
static int g_spk_only;
static unsigned short g_qreg[QSIZE];
static unsigned char g_qval[QSIZE];
static volatile unsigned g_due[QSIZE];      /* due sample for each write */
static volatile unsigned g_sample_now;      /* native sample clock */
static FILE *g_wav;
static volatile int g_kmd_want;          /* 1 = menu, 2 = level */
static unsigned g_kmd_acc;
static int g_kmd_disabled;
static int g_opltest;
static long g_wav_bytes;
static int g_rate = 48000;

/* Write a small WAV header so the tap can be dropped straight into a
   player; the RIFF and data sizes are patched at close. */
static void wav_header(void)
{
    unsigned char h[44];
    unsigned long rate = (unsigned long)g_rate;
    memset(h, 0, sizeof h);
    h[0] = 'R'; h[1] = 'I'; h[2] = 'F'; h[3] = 'F';
    h[8] = 'W'; h[9] = 'A'; h[10] = 'V'; h[11] = 'E';
    h[12] = 'f'; h[13] = 'm'; h[14] = 't'; h[15] = ' ';
    h[16] = 16; /* fmt chunk size */
    h[20] = 1;  /* PCM */
    h[22] = 2;  /* channels */
    h[24] = (unsigned char)rate;
    h[25] = (unsigned char)(rate >> 8);
    h[26] = (unsigned char)(rate >> 16);
    h[27] = (unsigned char)(rate >> 24);
    rate *= 4;                          /* byte rate: 2 channels x 2 bytes */
    h[28] = (unsigned char)rate;
    h[29] = (unsigned char)(rate >> 8);
    h[30] = (unsigned char)(rate >> 16);
    h[31] = (unsigned char)(rate >> 24);
    h[32] = 4;  /* block align */
    h[34] = 16; /* bits per sample */
    h[36] = 'd'; h[37] = 'a'; h[38] = 't'; h[39] = 'a';
    fwrite(h, 1, sizeof h, g_wav);
}

static void wav_patch(void)
{
    unsigned long data = (unsigned long)g_wav_bytes;
    unsigned char b[4];
    if (!g_wav)
        return;
    b[0] = (unsigned char)data;
    b[1] = (unsigned char)(data >> 8);
    b[2] = (unsigned char)(data >> 16);
    b[3] = (unsigned char)(data >> 24);
    fseek(g_wav, 40, SEEK_SET);
    fwrite(b, 1, 4, g_wav);
    fseek(g_wav, 4, SEEK_SET);
    data += 36;                         /* RIFF size */
    b[0] = (unsigned char)data;
    b[1] = (unsigned char)(data >> 8);
    b[2] = (unsigned char)(data >> 16);
    b[3] = (unsigned char)(data >> 24);
    fwrite(b, 1, 4, g_wav);
}
static int g_wav_bufs;
static volatile unsigned g_dropped;
static volatile unsigned g_writes, g_applied;
static volatile unsigned g_bufs;

unsigned sound_writes(void) { return g_writes; }
unsigned sound_applied(void) { return g_applied; }

static void wav_tap(const short *buf)
{
    if (g_wav && g_wav_bytes < 40L * 49716 * 4) {
        fwrite(buf, 1, NSAMP * 2 * sizeof(short), g_wav);
        g_wav_bytes += NSAMP * 2 * (long)sizeof(short);
        if (++g_wav_bufs >= 32) {
            g_wav_bufs = 0;
            fflush(g_wav);
        }
    }
}

/* Apply the writes that are due at the current sample clock.  The guest
   writes them at the sample they belong to, so one sample of slack is
   allowed either way; the comparison is signed, since past-due and
   future-due differ by almost the address space in unsigned terms. */
static void apply_queue(void)
{
    unsigned now = g_sample_now;
    int budget = 8192;
    while (budget-- > 0 && g_tail != g_head) {
        unsigned t = g_tail;
        unsigned short reg = g_qreg[t];
        unsigned char val = g_qval[t];
        int diff = (int)(g_due[t] - now);
        if (diff > 1)
            break;                    /* scheduled in the future */
        g_tail = (t + 1) % QSIZE;
        g_applied++;
        if (reg == 0x105)
            val = 0;                  /* Fury is an OPL2 title */
        if (!kmd_player_active())
            chip_write(reg, val);
    }
}

static float g_vol = 0.85f;
static float g_width = 0.22f;      /* mid/side widening fraction */
static int   g_mono;               /* exact mono output on request */

static void render_source(short *o)
{
    g_sample_now++;
    apply_queue();
    {
    static float lp[4], dc;
    static float fm_hp;          /* sub-sonic DC blocker */
    static float lp500;          /* 500 Hz one-pole for the tilt shelf */
    static float eq8_x1, eq8_x2, eq8_y1, eq8_y2;   /* 2.5 kHz peak */
    float s, spk;
    int mus = 0;
if (!g_spk_only && g_opl_ok) {
        short tmp[2] = {0, 0};
        float x, y;
        if (g_core)
            OPL3_Generate(&g_opl, tmp);
        else
            dbopl_gen(tmp);
        /* authentic OPL2 mix: flat chip output, no EQ cuts.  The reference
           capture carries a strong low body (kick at 35 Hz) and a melody
           body ~8 dB hotter above 500 Hz; a first-order tilt shelf matches
           that without touching the notes. */
        /* DBOPL is a mono OPL2 core feeding the same value into both

           half-sum lanes; Nuked renders a panned stereo pair.  The 1.35

           factor puts the DBOPL average level at the same place as the

           Nuked reference so downstram dynamics do not re-calibrate. */
        x = (float)(((int)tmp[0] + (int)tmp[1]) * 0.75f) * (g_core ? 1.0f : 1.35f);
        lp500 += (x - lp500) * 0.0506f;          /* 400 Hz one-pole */
        y = x + 2.5f * (x - lp500);              /* +11 dB above 400 Hz */
        /* 2.5 kHz presence: the reference has +6 dB more body at 1200-5000 */
        {
            float px = y;
            float py = 1.13504f * px - 1.64310f * eq8_x1 + 0.59365f * eq8_x2
                + 1.64310f * eq8_y1 - 0.72848f * eq8_y2;
            eq8_x2 = eq8_x1; eq8_x1 = px;
            eq8_y2 = eq8_y1; eq8_y1 = py;
            y = py;
        }
        fm_hp += (y - fm_hp) * 0.0025f;          /* sub-sonic DC only */
        y -= fm_hp;
        mus = (int)y;
    }
    /* The effects arrive as a fast one-bit PC-speaker stream.  Decode it
       bipolar, then apply a four-pole 4 kHz low-pass (the physical speaker
       response) and remove the DC offset. */
    s = g_spk ? 1.0f : -1.0f;
    lp[0] += (s - lp[0]) * 0.40f;
    lp[1] += (lp[0] - lp[1]) * 0.40f;
    lp[2] += (lp[1] - lp[2]) * 0.40f;
    lp[3] += (lp[2] - lp[3]) * 0.40f;
    dc += (lp[3] - dc) * 0.0005f;
    spk = (lp[3] - dc) * 0.45f * 32767.0f;
    {   /* -- output stage: level rider, widening, limiter, dither -- */
        static float env;             /* ~100 ms level envelope */
        static float ride = 1.0f;     /* slow level rider gain */
        static float lim = 1.0f;      /* linked stereo peak limiter */
        static float dly[35];         /* 0.7 ms widening delay */
        static int   di;
        static unsigned dseed = 0x12345678u;
        float mono = (mus + spk) * g_vol;
        float a = mono < 0 ? -mono : mono;
        float v, l, r, tgt;

        /* A slow level rider evens out the average level between the
           menu, the intro and the level music: ballistics of about two
           seconds, a range of only -2 / +2 dB, so it never pumps but
           keeps quiet passages from disappearing.  The fast limiter on
           top then only shapes the few samples that would still cross
           the ceiling, instead of working all the time. */
        env += (a - env) * 0.0002f;
        {
            float need = env > 300.0f ? 21000.0f / (env * ride) : 1.0f;
            if (need < 0.8f) need = 0.8f;
            if (need > 1.25f) need = 1.25f;
            ride += (need - ride) * 0.00001f;
        }
        v = mono * ride;

        /* Frequency-dependent widening, band-split Haas: only the signal
           above ~300 Hz is delayed and fed to the side channel, the bass
           stays dead centre (a Haas shift on the bass blurs the kick and
           turns the low end phasey).  The very high frequencies are
           dialled back out of the side so the shimmer stays centred too.
           The mono mixdown (L+R = 2*M) and FURY_MONO=1 behave as before. */
        if (!g_mono && g_width > 0.0f) {
            static float wlp1, wlp2, whp;   /* ~300 Hz split, ~9 kHz tame */
            float low, high, side;
            wlp1 += (v - wlp1) * 0.0373f;   /* 300 Hz one-pole at 49.7 kHz */
            wlp2 += (wlp1 - wlp2) * 0.0373f;
            low = wlp2;
            high = v - low;
            dly[di] = high;
            di = di + 1 == 35 ? 0 : di + 1;
            whp += (dly[di] - whp) * 0.68f; /* keep >9 kHz out of the side */
            side = whp * g_width;
            l = v + side;
            r = v - side;
        } else {
            l = r = v;
        }

        /* Linked stereo peak limiter: instant attack, ~24 ms release,
           both channels share one gain so the mono mixdown keeps its
           exact 2*M balance. */
        {
            float al = l < 0 ? -l : l;
            float ar = r < 0 ? -r : r;
            float am = al > ar ? al : ar;
            tgt = am > 30600.0f ? 30600.0f / am : 1.0f;
        }
        if (tgt < lim) lim = tgt;
        else lim += (tgt - lim) * 0.0008f;
        l *= lim;
        r *= lim;

        /* TPDF dither, about one LSB: quiet passages become a
           continuum instead of quantisation steps. */
        dseed ^= dseed << 13; dseed ^= dseed >> 17; dseed ^= dseed << 5;
        {
            float d1 = (float)(int)dseed * (1.0f / 2147483648.0f);
            dseed ^= dseed << 13; dseed ^= dseed >> 17; dseed ^= dseed << 5;
            {
                float d2 = (float)(int)dseed * (1.0f / 2147483648.0f);
                float dth = (d1 + d2) * 0.25f;
                l += dth;
                r += dth;
            }
        }
        if (l > 32767.0f) l = 32767.0f;
        if (l < -32768.0f) l = -32768.0f;
        if (r > 32767.0f) r = 32767.0f;
        if (r < -32768.0f) r = -32768.0f;
        o[0] = (short)l;
        o[1] = (short)r;
    }
    }
}

static void render_opl(short *out, int n)
{
    static short s0, s1, s2, s3;      /* left history */
    static short u0, u1, u2, u3;      /* right history */
    static double acc;
    static int have;
    const double step = 49716.0 / (double)g_rate;
    int i;
    if (!have) {
        short t[2];
        render_source(t); s0 = t[0]; u0 = t[1];
        render_source(t); s1 = t[0]; u1 = t[1];
        render_source(t); s2 = t[0]; u2 = t[1];
        render_source(t); s3 = t[0]; u3 = t[1];
        acc = 0.0;
        have = 1;
    }
    for (i = 0; i < n; i++) {
        double t = acc;
        double v;
        {
            double p0 = s0, p1 = s1, p2 = s2, p3 = s3;
            double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
            double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            double c = -0.5 * p0 + 0.5 * p2;
            v = ((a * t + b) * t + c) * t + p1;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            out[i * 2] = (short)v;
        }
        {
            double p0 = u0, p1 = u1, p2 = u2, p3 = u3;
            double a = -0.5 * p0 + 1.5 * p1 - 1.5 * p2 + 0.5 * p3;
            double b = p0 - 2.5 * p1 + 2.0 * p2 - 0.5 * p3;
            double c = -0.5 * p0 + 0.5 * p2;
            v = ((a * t + b) * t + c) * t + p1;
            if (v > 32767.0) v = 32767.0;
            if (v < -32768.0) v = -32768.0;
            out[i * 2 + 1] = (short)v;
        }
        acc += step;
        while (acc >= 1.0) {
            short t2[2];
            s0 = s1;
            s1 = s2;
            s2 = s3;
            u0 = u1;
            u1 = u2;
            u2 = u3;
            render_source(t2);
            s3 = t2[0];
            u3 = t2[1];
            acc -= 1.0;
        }
    }
}

void sound_set_speaker(int level)
{
    g_spk = level ? 1 : 0;
}

unsigned sound_buffers(void) { return g_bufs; }
unsigned sound_queued(void) { return (g_head - g_tail) % QSIZE; }

unsigned sound_queued(void);

/* Every ~5 s the audio thread reports its health to sound.log when
   FURY_SOUNDLOG is set: only an aid for debugging the timing, not used
   in normal play. */
static void audio_heartbeat(void)
{
    static unsigned bufs, prev_w;
    static volatile int enabled = -1;
    if (enabled == -1)
        enabled = getenv("FURY_SOUNDLOG") != NULL;
    if (!enabled || ++bufs % 250)          /* about every 5 seconds */
        return;
    {
        FILE *f = fopen("sound.log", "a");
        if (f) {
            unsigned w = sound_writes();
            fprintf(f, "t=%us bufs=%u writes=%u (+%u) queued=%u applied=%u "
                       "now=%u lag=%d\n",
                    bufs * NSAMP / g_rate, bufs, w, w - prev_w,
                    sound_queued(), g_applied, g_sample_now,
                    (int)(g_due[g_tail] - g_sample_now));
            fclose(f);
            prev_w = w;
        }
    }
}

static DWORD WINAPI audio_thread(LPVOID arg)
{
    int i;
    (void)arg;
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    for (i = 0; i < NBUF; i++) {
        memset(&g_hdr[i], 0, sizeof g_hdr[i]);
        g_hdr[i].lpData = (LPSTR)g_buf[i];
        g_hdr[i].dwBufferLength = NSAMP * 2 * sizeof(short);
        waveOutPrepareHeader(g_hwo, &g_hdr[i], sizeof g_hdr[i]);
        render_opl(g_buf[i], NSAMP);
        wav_tap(g_buf[i]);
        waveOutWrite(g_hwo, &g_hdr[i], sizeof g_hdr[i]);
        g_bufs++;
    }
    {
        DWORD next_ms = GetTickCount();
        while (g_run) {
        int any = 0;
        DWORD now = GetTickCount();
        if ((long)(now - next_ms) < 0) {
            Sleep(1);
            continue;
        }
        next_ms += (DWORD)((NSAMP * 1000) / g_rate);
        for (i = 0; i < NBUF; i++) {
            if (g_hdr[i].dwFlags & WHDR_DONE) {
                render_opl(g_buf[i], NSAMP);
                audio_heartbeat();
                wav_tap(g_buf[i]);
                waveOutWrite(g_hwo, &g_hdr[i], sizeof g_hdr[i]);
                g_bufs++;
                any = 1;
            }
        }
        if (!any)
            Sleep(1);
        }
    }
    return 0;
}

int sound_init(void)
{
    WAVEFORMATEX wf;
    DWORD tid;
    const char *wp;
    if (g_opl_ok)
        return 1;
    g_head = g_tail = 0;
    g_dropped = 0;
    g_spk_only = getenv("FURY_SPK_ONLY") != NULL;
    {
        const char *w = getenv("FURY_WIDTH");
        if (w) {
            int p = atoi(w);
            if (p < 0) p = 0;
            if (p > 100) p = 100;
            g_width = p / 100.0f;
        }
        g_mono = getenv("FURY_MONO") != NULL;
    }
    {
        const char *vol = getenv("FURY_VOLUME");
        if (vol) {
            int p = atoi(vol);
            if (p < 0) p = 0;
            if (p > 200) p = 200;
            g_vol = p / 100.0f;
        }
    }
    wp = getenv("FURY_SOUND_WAV");

    {
        const char *co = getenv("FURY_OPL");
        g_core = (co && strcmp(co, "nuked") == 0);
    }
    if (g_core) {
        OPL3_Reset(&g_opl, 49716);
        g_opl_ok = 1;
    } else {
        dbopl_init(49716);
        g_opl_ok = 1;
    }
    {
        const char *t = getenv("FURY_OPLTEST");
        if (t) {
            int block = atoi(t);
            g_opltest = 1;
            chip_write(0x20, 0x01);
            chip_write(0x23, 0x01);
            chip_write(0x40, 0x00);
            chip_write(0x43, 0x00);
            chip_write(0x60, 0xF0);
            chip_write(0x63, 0xF0);
            chip_write(0x80, 0x00);
            chip_write(0x83, 0x00);
            chip_write(0xC0, 0x00);
            chip_write(0xA0, 139);
            chip_write(0xB0,
                          0x20 | ((block & 7) << 2) | ((139 >> 8) & 3));
        }
    }
    g_kmd_disabled = 1;              /* the recompiled KMD player is primary */
    {
        const char *k = getenv("FURY_KMD");
        if (k && atoi(k) != 0)
            g_kmd_disabled = 0;      /* optional C-player fallback */
    }
    if (!g_kmd_disabled) {
        kmd_player_init(&g_opl);
        kmd_player_load("kmd/intro2.kmd");
    }
    memset(&wf, 0, sizeof wf);
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = 2;
    wf.wBitsPerSample = 16;
    wf.nBlockAlign = 4;
    {
        static const int rates[2] = {49716, 48000};
        MMRESULT r = MMSYSERR_NOTSUPPORTED;
        int ri;
        for (ri = 0; ri < 2; ri++) {
            wf.nSamplesPerSec = rates[ri];
            wf.nAvgBytesPerSec = rates[ri] * 4;
            r = waveOutOpen(&g_hwo, WAVE_MAPPER, &wf, 0, 0, CALLBACK_NULL);
            if (r == MMSYSERR_NOERROR) {
                g_rate = rates[ri];
                break;
            }
        }
        if (r != MMSYSERR_NOERROR) {
            g_opl_ok = 0;
            return 0;
        }
        {
            FILE *rlog = fopen("audiolog.txt", "w");
            if (rlog) {
                fprintf(rlog, "audio rate %d\n", g_rate);
                fclose(rlog);
            }
        }
    }
    if (wp) {
        g_wav = fopen(wp, "w+b");   /* sought back to patch at close */
        if (g_wav)
            wav_header();
    }
    g_run = 1;
    if (!CreateThread(NULL, 0, audio_thread, NULL, 0, &tid)) {
        g_run = 0;
        waveOutClose(g_hwo);
        g_opl_ok = 0;
        return 0;
    }
    return 1;
}

void sound_music_level(int level)
{
    if (level && !g_kmd_disabled)
        g_kmd_want = 2;
}

void sound_write(unsigned reg, unsigned val)
{
    unsigned h;
    if (!g_opl_ok)
        return;
    if (g_opltest || kmd_player_active()) {  /* test tone / KMD owns the OPL */
        g_writes++;
        return;
    }
    reg &= 0xFF;
    val &= 0xFF;
    h = g_head;
    if ((h + 1) % QSIZE == g_tail) {
        /* Queue full: release the oldest entry, it is the one the audio
           thread has had the most time for and it keeps the FIFO in
           order.  (This path is unreachable unless the audio thread is
           stalled for a very long time.) */
        g_tail = (g_tail + 1) % QSIZE;
        g_dropped++;
    }
    g_qreg[h] = (unsigned short)reg;
    g_qval[h] = (unsigned char)val;
    g_due[h] = g_sample_now;
    g_head = (h + 1) % QSIZE;
    g_writes++;
}

unsigned sound_dropped(void)
{
    return g_dropped;
}

void sound_shutdown(void)
{
    int i;
    if (!g_opl_ok)
        return;
    g_run = 0;
    Sleep(150);
    for (i = 0; i < NBUF; i++) {
        waveOutUnprepareHeader(g_hwo, &g_hdr[i], sizeof g_hdr[i]);
        g_hdr[i].lpData = NULL;
    }
    waveOutClose(g_hwo);
    if (g_wav) {
        wav_patch();
        fclose(g_wav);
        g_wav = NULL;
    }
    g_opl_ok = 0;
}
