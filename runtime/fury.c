/*
 * Runtime for the statically recompiled FURY.EXE.
 *
 * Memory model
 *   guest linear address A  ->  host address GUEST_BASE + A
 *   the original executable image (data, tables, strings) is loaded at
 *   GUEST_BASE + 0x2070, so every absolute address the recompiled code uses keeps
 *   its original meaning, including code that the game reads or patches as
 *   data.
 *   the recompiled 32-bit code lives at 0x20000000 and addresses guest memory
 *   through the segment base variables.
 */

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>

#define GUEST_BASE   0x08000000u
#define ORG_PARA     (GUEST_BASE >> 4)
#define GUEST_SIZE   0x140000u
#define VAR_BASE     0x080F0000u
#define VAR_SIZE     0x2000u
#define STACK_HOST   0x03000000u
#define CODEBASE     0x20000000u
#define CODE_MAX     0x400000u

#define IMAGE_AT     0x2070u          /* guest address of image byte 0 */
#define PSP_AT       0x1F70u

#define VIDEO_AT     0xA0000u

/* fixed variables shared with the recompiled code */
#define V_DS_BASE    (*(volatile uint32_t *)(VAR_BASE + 0x000))
#define V_DS_SEL     (*(volatile uint16_t *)(VAR_BASE + 0x004))
#define V_ES_BASE    (*(volatile uint32_t *)(VAR_BASE + 0x008))
#define V_ES_SEL     (*(volatile uint16_t *)(VAR_BASE + 0x00C))
#define V_SS_SEL     (*(volatile uint16_t *)(VAR_BASE + 0x010))
#define V_TMP_A      (*(volatile uint32_t *)(VAR_BASE + 0x014))
#define V_TMP_V      (*(volatile uint32_t *)(VAR_BASE + 0x018))
#define V_TMP_T      (*(volatile uint32_t *)(VAR_BASE + 0x01C))
#define V_IF         (*(volatile uint32_t *)(VAR_BASE + 0x020))
#define V_SS_BASE    (*(volatile uint32_t *)(VAR_BASE + 0x024))
#define V_IN_HANDLER (*(volatile uint32_t *)(VAR_BASE + 0x028))
#define V_IN_TIMER   (*(volatile uint32_t *)(VAR_BASE + 0x168))
#define V_VID_BASE   (*(volatile uint32_t *)(VAR_BASE + 0x040))

#define PTR_X86_INT      (VAR_BASE + 0x100)
#define PTR_OUT_DX_AL    (VAR_BASE + 0x104)
#define PTR_OUT_DX_AX    (VAR_BASE + 0x108)
#define PTR_IN_AL_DX     (VAR_BASE + 0x10C)
#define PTR_OUT_IMM_AL   (VAR_BASE + 0x110)
#define PTR_IN_AL_IMM    (VAR_BASE + 0x114)
#define PTR_CHECK_TIMER  (VAR_BASE + 0x118)
#define PTR_CODE_PTR     (VAR_BASE + 0x11C)
#define PTR_TRACE        (VAR_BASE + 0x120)
#define PTR_VGA_STORE8   (VAR_BASE + 0x130)
#define PTR_VGA_STORE16  (VAR_BASE + 0x134)
#define PTR_VGA_STORE32  (VAR_BASE + 0x138)
#define PTR_VGA_LOAD8    (VAR_BASE + 0x13C)
#define PTR_VGA_LOAD16   (VAR_BASE + 0x140)
#define PTR_VGA_LOAD32   (VAR_BASE + 0x144)
#define PTR_VGA_STOS8    (VAR_BASE + 0x148)
#define PTR_VGA_STOS16   (VAR_BASE + 0x14C)
#define PTR_VGA_MOVSB    (VAR_BASE + 0x150)
#define PTR_VGA_MOVSW    (VAR_BASE + 0x154)
#define VGA_ARG0         (*(volatile uint32_t *)(VAR_BASE + 0x158))
#define VGA_ARG1         (*(volatile uint32_t *)(VAR_BASE + 0x15C))
#define VGA_ARG2         (*(volatile uint32_t *)(VAR_BASE + 0x160))
#define VGA_RES          (*(volatile uint32_t *)(VAR_BASE + 0x164))
#define PTR_FLAT_RET     (VAR_BASE + 0x16C)

extern void x86_int(void);
extern void x86_out_dx_al(void);
extern void x86_out_dx_ax(void);
extern void x86_in_al_dx(void);
extern void x86_out_imm_al(void);
extern void x86_in_al_imm(void);
extern void SR_CheckTimer(void);
extern void x86_code_ptr(void);
extern void x86_trace(void);
extern void run_guest(uint32_t entry);
extern void vga_store8(void);
extern void vga_store16(void);
extern void vga_store32(void);
extern void vga_load8(void);
extern void vga_load16(void);
extern void vga_load32(void);
extern void vga_stos8(void);
extern void vga_stos16(void);
extern void vga_movsb(void);
extern void vga_movsw(void);
extern void flat_retf(void);

/* sound.c */
int  sound_init(void);
void sound_write(unsigned reg, unsigned val);
void sound_shutdown(void);
unsigned sound_dropped(void);
unsigned sound_writes(void);
unsigned sound_applied(void);
unsigned sound_buffers(void);
unsigned sound_queued(void);
void sound_set_speaker(int level);
void sound_music_level(int level);
static volatile int sound_ok;

static uint8_t *M;                    /* guest memory at GUEST_BASE */

#define GB(addr)  (M[(uint32_t)(addr)])            /* host address */
#define GW(addr)  (*(uint16_t *)(M + (uint32_t)(addr)))
#define GD(addr)  (*(uint32_t *)(M + (uint32_t)(addr)))

/* ------------------------------------------------------------------ log -- */

static FILE *logf;
static long log_lines;
static int  log_limit = 300000;
static int  debug_log;              /* FURY_DEBUG: verbose port/video tracing */

static void lg(const char *fmt, ...)
{
    va_list ap;
    if (!logf || log_lines > log_limit)
        return;
    va_start(ap, fmt);
    vfprintf(logf, fmt, ap);
    va_end(ap);
    fputc('\n', logf);
    log_lines++;
    if (log_lines == log_limit)
        fprintf(logf, "...log truncated...\n");
}

/* --------------------------------------------------------------- symbols - */

static uint32_t *symtab;
static uint32_t  symcount;

/* The last branches the guest executed; the crash filter prints them. */
static uint32_t trace_ring[128];
static volatile LONG trace_pos;

static uint32_t sym_nearest(uint32_t flat)
{
    uint32_t i, best = 0, best_guest = 0;
    for (i = 0; i < symcount; i++) {
        uint32_t f = symtab[i * 2 + 1];
        if (f <= flat && f >= best) {
            best = f;
            best_guest = symtab[i * 2];
        }
    }
    return best_guest;
}

static uint32_t sym_lookup(uint32_t guest)
{
    uint32_t lo = 0, hi = symcount;
    while (lo < hi) {
        uint32_t mid = (lo + hi) / 2;
        if (symtab[mid * 2] == guest)
            return symtab[mid * 2 + 1];
        if (symtab[mid * 2] < guest)
            lo = mid + 1;
        else
            hi = mid;
    }
    return 0;
}

/* ---------------------------------------------------------------- ports -- */

static uint8_t  seq_idx, seq_reg[8];
static uint8_t  gc_idx, gc_reg[16];
static uint8_t  crtc_idx, crtc_reg[32];
static uint8_t  attr_idx, attr_reg[32], attr_flip;
static uint8_t  misc_out = 0x63;
static uint8_t  dac_idx, dac_state;
static unsigned dac_reads, dac_writes;
static uint8_t  dac_read_idx;
static uint8_t  adlib_idx;              /* Adlib register selected on port 0x388 */
static unsigned adlib_hist[256];        /* debug: written Adlib registers */
static unsigned adlib_kon, adlib_koff;  /* debug: 0xB0 key-on / key-off writes */
static uint8_t  palette[256 * 3];
static uint8_t  pixel_mask = 0xFF;
static int      video_mode = 3;         /* the mode the BIOS was asked for */
static volatile int level_started;      /* 1 = a 16-colour screen, 2 = level */
static volatile int data01_seen;        /* dat\\data01.bin was opened */
static volatile DWORD data01_tick;
static volatile int fast_time;          /* FURY_FAST_TIME: force the time-out */

static uint8_t  pic_imr = 0xFF, pic_irr, pic_isr;
static uint8_t  pit_mode[3];
static uint16_t pit_div[3];
static uint8_t  pit_latch[3];

static volatile int pit_period_ms = 11;     /* ~88 Hz */
static volatile int pit_period_us = 11000;  /* the PIT period the game programmed */
static int tick_override;                   /* FURY_TICK_MS: force the tick rate */

__declspec(dllimport) UINT __stdcall timeBeginPeriod(UINT);

static uint8_t  speaker;
static unsigned spk_hi, spk_lo;

static uint8_t  kbd_queue[256];
static volatile int kbd_head, kbd_tail;
/* Pending flags the generated code tests before calling the hook. */
#define V_TICK (*(volatile LONG *)(VAR_BASE + 0x170))
#define V_KBD  (*(volatile LONG *)(VAR_BASE + 0x174))

static uint32_t vretrace_hits;

static void kbd_push(uint8_t sc)
{
    int next = (kbd_tail + 1) & 255;
    if (next != kbd_head) {
        kbd_queue[kbd_tail] = sc;
        kbd_tail = next;
        V_KBD = 1;
    }
}

static int kbd_pop(void)
{
    int v;
    if (kbd_head == kbd_tail)
        return 0;               /* no key: the game sees a zero scancode */
    v = kbd_queue[kbd_head];
    kbd_head = (kbd_head + 1) & 255;
    return v;
}

static double now_ms(void)
{
    LARGE_INTEGER c;
    static LARGE_INTEGER freq;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)freq.QuadPart;
}

static int vga_retrace_phase(void)
{
    /* 16 ms frame, ~2 ms vertical retrace */
    double t = now_ms();
    double ph = t - (double)(long long)(t / 16.0) * 16.0;
    return ph < 2.0;
}

/* The VGA window is modelled at its real linear place, guest 0xA0000.  In
   planar modes the four hardware planes are the four 64 KiB blocks at
   0xA0000 + p*0x10000; in chain-4 (mode 13h) the window is linear.  The
   recompiled stores go through the helpers below, which apply the map mask,
   the write mode, the ALU and the bit mask the game programmed, exactly like
   the adapter would. */
static uint32_t vid_plane_off;
static unsigned vid_mask_hist[16];
static unsigned gc5_hist[8];            /* write-mode register values seen */
static uint32_t vid_cur_base = 0x080A0000u;

static void vid_set_plane(int plane)
{
    vid_plane_off = (uint32_t)(plane & 3) << 16;
    vid_cur_base = 0x080A0000u + vid_plane_off;
    /* ES writes are emulated by the VGA helpers; DS/SS video accesses (the
       few routines that use them) keep the plane in the segment base. */
    if (V_DS_SEL == 0xA000) V_DS_BASE = vid_cur_base;
    if (V_SS_SEL == 0xA000) V_SS_BASE = vid_cur_base;
    V_VID_BASE = vid_cur_base;
}

/* --- planar VGA write emulation ----------------------------------------- */

static uint8_t vga_latch[4];
static int vga_raw;                     /* FURY_VGA_RAW: ignore latches/ALU/mask */
static uint32_t vga_last_load;

static int vid_chain4(void)
{
    return (seq_reg[4] & 0x08) != 0;
}

static uint8_t vga_ror(uint8_t v, int n)
{
    n &= 7;
    if (!n) return v;
    return (uint8_t)((v >> n) | (v << (8 - n)));
}

static uint8_t vga_alu(int fn, uint8_t a, uint8_t b)
{
    switch (fn & 3) {
    case 1: return (uint8_t)(a & b);
    case 2: return (uint8_t)(a | b);
    case 3: return (uint8_t)(a ^ b);
    default: return a;
    }
}

static void vga_load_latches(uint32_t off)
{
    int p;
    for (p = 0; p < 4; p++)
        vga_latch[p] = M[VIDEO_AT + p * 0x10000 + (off & 0xFFFF)];
}

static uint8_t vga_read_byte(uint32_t off)
{
    uint8_t res;
    int p, b;

    off &= 0xFFFF;
    vga_last_load = off;
    if (vid_chain4())
        return M[VIDEO_AT + (off & 0x3FFFF)];
    vga_load_latches(off);
    if (gc_reg[5] & 0x08) {
        /* read mode 1: colour compare */
        res = 0;
        for (b = 0; b < 8; b++) {
            int match = 1;
            for (p = 0; p < 4; p++) {
                if ((gc_reg[7] & (1u << p)) &&
                    (((vga_latch[p] >> b) & 1) != ((gc_reg[2] >> p) & 1)))
                    match = 0;
            }
            if (match)
                res |= (uint8_t)(1u << b);
        }
        return res;
    }
    return vga_latch[gc_reg[4] & 3];
}

static void vga_write_byte(uint32_t off, uint8_t value)
{
    int mode, rot, fn, p;
    uint8_t data, bm;

    off &= 0xFFFF;
    if (vid_chain4()) {
        M[VIDEO_AT + (off & 0x3FFFF)] = value;
        return;
    }
    /* The latches are loaded by CPU *reads*; a write combines its data with
       the latched bytes (write modes 0/1 and the bit mask).  Do not reload
       them from the destination here, or the read-modify-write blits the
       game does would write the destination back unchanged. */
    mode = gc_reg[5] & 3;
    if (debug_log && off == vga_last_load &&
        off >= 0x6100 && off < 0x6300) {
        static int nr;
        if (nr < 200) {
            lg("RMW off=%04X val=%02X mm=%02X gc4=%02X gc8=%02X", off, value,
               seq_reg[2] & 0x0F, gc_reg[4] & 3, gc_reg[8]);
            nr++;
        }
    }
    rot = gc_reg[3] & 7;
    fn = (gc_reg[3] >> 3) & 3;
    data = vga_ror(value, rot);
    bm = gc_reg[8];
    if (vga_raw && (mode == 0 || mode == 1))
        bm = 0xFF;
    for (p = 0; p < 4; p++) {
        uint8_t d, res;
        if (!(seq_reg[2] & (1u << p)))
            continue;
        if (mode == 2)
            d = (value & (1u << p)) ? 0xFF : 0x00;
        else if (mode == 3)
            d = (gc_reg[0] & (1u << p)) ? 0xFF : 0x00;
        else if (mode == 1)
            d = vga_raw ? value : vga_latch[p];
        else if (!vga_raw && (gc_reg[1] & (1u << p)))
            d = (gc_reg[0] & (1u << p)) ? 0xFF : 0x00;
        else
            d = data;
        res = vga_raw && mode == 0 ? data : vga_alu(fn, d, vga_latch[p]);
        M[VIDEO_AT + p * 0x10000 + off] = (uint8_t)((res & bm) |
                                                    (vga_latch[p] & ~bm));
    }
}

static int vid_is_video(uint32_t addr)
{
    /* Only the 0xA000 selector is the VGA window.  The four plane blocks are
       laid out at 0xA0000 + p*0x10000, which overlaps the host addresses of
       the C000 ROM segment; the selector check keeps ROM/BIOS scans out. */
    return V_ES_SEL == 0xA000 &&
           addr >= GUEST_BASE + VIDEO_AT &&
           addr < GUEST_BASE + VIDEO_AT + 0x40000;
}

static void vid_direct_store(uint32_t addr, uint32_t value, int size)
{
    if (size == 1) *(uint8_t *)(uintptr_t)addr = (uint8_t)value;
    else if (size == 2) *(uint16_t *)(uintptr_t)addr = (uint16_t)value;
    else *(uint32_t *)(uintptr_t)addr = value;
}

static uint32_t vid_direct_load(uint32_t addr, int size)
{
    if (size == 1) return *(uint8_t *)(uintptr_t)addr;
    if (size == 2) return *(uint16_t *)(uintptr_t)addr;
    return *(uint32_t *)(uintptr_t)addr;
}

static void vga_store(uint32_t addr, uint32_t value, int size)
{
    int i;
    if (!vid_is_video(addr)) {
        vid_direct_store(addr, value, size);
        return;
    }
    for (i = 0; i < size; i++)
        vga_write_byte(addr - (GUEST_BASE + VIDEO_AT) + i,
                       (uint8_t)(value >> (8 * i)));
}

static uint32_t vga_load(uint32_t addr, int size)
{
    uint32_t v = 0;
    int i;
    if (!vid_is_video(addr))
        return vid_direct_load(addr, size);
    for (i = 0; i < size; i++)
        v |= (uint32_t)vga_read_byte(addr - (GUEST_BASE + VIDEO_AT) + i) << (8 * i);
    return v;
}

void vga_store8_impl(uint32_t addr, uint32_t value)
{
    vga_store(addr, value, 1);
}

void vga_store16_impl(uint32_t addr, uint32_t value)
{
    vga_store(addr, value, 2);
}

void vga_store32_impl(uint32_t addr, uint32_t value)
{
    vga_store(addr, value, 4);
}

uint32_t vga_load8_impl(uint32_t addr)
{
    return vga_load(addr, 1);
}

uint32_t vga_load16_impl(uint32_t addr)
{
    return vga_load(addr, 2);
}

uint32_t vga_load32_impl(uint32_t addr)
{
    return vga_load(addr, 4);
}

void vga_stos8_impl(uint32_t addr, uint32_t value, uint32_t count)
{
    uint32_t i;
    int video = vid_is_video(addr);
    for (i = 0; i < count; i++) {
        uint32_t a;
        if (video)
            a = (GUEST_BASE + VIDEO_AT) +
                ((addr - (GUEST_BASE + VIDEO_AT) + i) & 0xFFFF);
        else
            a = addr + i;
        if (debug_log && video) {
            uint32_t off = (a - (GUEST_BASE + VIDEO_AT)) & 0xFFFF;
            static int ns;
            if (ns < 1) {
                lg("STOS first off=%04X val=%02X mm=%02X", off, value,
                   seq_reg[2] & 0x0F);
                ns++;
            }
        }
        vga_store(a, value, 1);
    }
}

void vga_stos16_impl(uint32_t addr, uint32_t value, uint32_t count)
{
    uint32_t i;
    int video = vid_is_video(addr);
    for (i = 0; i < count; i++) {
        uint32_t a = video
            ? (GUEST_BASE + VIDEO_AT) +
              ((addr - (GUEST_BASE + VIDEO_AT) + i * 2) & 0xFFFF)
            : addr + i * 2;
        vga_store(a, value, 2);
    }
}

void vga_movsb_impl(uint32_t src, uint32_t dst, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        vga_store(dst + i, *(uint8_t *)(uintptr_t)(src + i), 1);
}

void vga_movsw_impl(uint32_t src, uint32_t dst, uint32_t count)
{
    uint32_t i;
    for (i = 0; i < count; i++)
        vga_store(dst + i * 2, *(uint16_t *)(uintptr_t)(src + i * 2), 2);
}

static void vid_update(void)
{
    uint32_t mask = seq_reg[2] & 0x0F;
    int plane = 0;
    if (!(seq_reg[4] & 0x08)) {
        while (plane < 4 && !(mask & (1u << plane)))
            plane++;
        if (plane > 3)
            plane = 0;
    }
    vid_mask_hist[mask & 0x0F]++;
    vid_set_plane(plane);
}

static void out_port(uint32_t port, uint32_t value)
{
    port &= 0xFFFF;
    value &= 0xFF;

    /* The 16-colour palette refresher of the timer driver must not touch the
       DAC while a 256-colour screen is displayed. */
    if (V_IN_TIMER && video_mode == 0x13 &&
        (port == 0x3C8 || port == 0x3C9))
        return;

    if (port == 0x3C4) { seq_idx = value; return; }
    if (port == 0x3C5) {
        seq_reg[seq_idx & 7] = value;
        if ((seq_idx & 7) == 2 || (seq_idx & 7) == 4)
            vid_update();
        return;
    }
    if (port == 0x3CE) { gc_idx = value; return; }
    if (port == 0x3CF) {
        static int ngc;
        gc_reg[gc_idx & 15] = value;
        if ((gc_idx & 15) == 5)
            gc5_hist[value & 7]++;
        if ((gc_idx & 15) == 4)
            vid_set_plane(value & 3);
        if (debug_log && ngc < 300 && ((gc_idx & 15) == 5 || (gc_idx & 15) == 8 ||
                          (gc_idx & 15) == 0 || (gc_idx & 15) == 1 ||
                          (gc_idx & 15) == 2 || (gc_idx & 15) == 7 ||
                          (gc_idx & 15) == 3)) {
            uint32_t fr = *(volatile uint32_t *)0x080F0034u;
            uint32_t ret = (fr > 0x080F0000u && fr < 0x10000000u)
                               ? *(uint32_t *)(uintptr_t)(fr + 36) : 0;
            lg("GC idx=%d val=%02X from %08lX", gc_idx & 15, (unsigned)value, (unsigned long)ret);
            ngc++;
        }
        return;
    }
    if (port == 0x3D4) { crtc_idx = value; return; }
    if (port == 0x3D5) { crtc_reg[crtc_idx & 31] = value; return; }
    if (port == 0x3C0) {
        if (attr_flip) {
            attr_reg[attr_idx & 31] = value;
            attr_idx++;
        } else {
            attr_idx = value & 31;
        }
        attr_flip = !attr_flip;
        return;
    }
    if (port == 0x3C1) { return; }
    if (port == 0x3C2) { misc_out = value; return; }
    if (port == 0x3C6) { pixel_mask = value; return; }
    if (port == 0x3C7) { dac_read_idx = value; return; }
    if (port == 0x3C8) {
        static int n8;
        dac_idx = value; dac_state = 0;
        if (debug_log && n8 < 30) { lg("DAC set index %u", (unsigned)value); n8++; }
        return;
    }
    if (port == 0x3C9) {
        static int n9;
        static uint8_t acc[768];
        static int pos;
        if (debug_log && n9 < 48) { lg("DAC data %u", (unsigned)value); n9++; }
        if (pos < 768) acc[pos++] = (uint8_t)value;
        if (pos == 768) {
            int d = 0, k;
            for (k = 1; k < 768; k++) if (acc[k] != acc[0]) d = 1;
            if (debug_log) lg("DAC block done: %s %u %u %u ...", d ? "varying" : "UNIFORM",
               acc[0], acc[1], acc[2]);
            pos = 0;
        }
    }
    if (port == 0x3C9) {
        palette[(dac_idx * 3 + dac_state) % 768] = (uint8_t)(value * 255 / 63);
        dac_writes++;
        dac_state++;
        if (dac_state == 3) { dac_state = 0; dac_idx++; }
        return;
    }
    if (port == 0x3DA) return;
    if (port == 0x40 || port == 0x41 || port == 0x42) {
        int ch = port - 0x40;
        if (pit_latch[ch] == 0) { pit_div[ch] = value; pit_latch[ch] = 1; }
        else {
            pit_div[ch] = (uint16_t)((pit_div[ch] & 0x00FF) | (value << 8));
            pit_latch[ch] = 0;
            if (ch == 0 && pit_div[0]) {
                int ms = (int)(1193182.0 / pit_div[0] / 1000.0 + 0.5);
                if (ms < 1) ms = 1;
                if (tick_override) ms = tick_override;
                pit_period_ms = ms;
                pit_period_us = (int)(1000000.0 * pit_div[0] / 1193182.0);
                if (pit_period_us < 50) pit_period_us = 50;
                if (tick_override) pit_period_us = tick_override * 1000;
                lg("PIT ch0 divisor %u -> %d ms (%d us)", pit_div[0], ms, pit_period_us);
            }
            if (ch == 2 && pit_div[2]) {
                static int n2;
                if (debug_log && n2 < 30) { lg("PIT ch2 divisor %u", pit_div[2]); n2++; }
            }
        }
        return;
    }
    if (port == 0x43) {
        int ch = value >> 6;
        pit_mode[ch & 3] = (value >> 1) & 7;
        pit_latch[ch & 3] = 0;
        return;
    }
    if (port == 0x20) { pic_isr = 0; return; }
    if (port == 0x21) { pic_imr = value; return; }
    if (port == 0x61) {
        static int n; if (debug_log && value && n < 40) { lg("SPK <- %02X", value); n++; }
        if (value & 2) spk_hi++; else spk_lo++;
        sound_set_speaker((value & 2) ? 1 : 0);
        speaker = value; return;
    }
    if (port == 0x64) return;
    if (port == 0x201) return;
    if (port == 0x388 || port == 0x38A) {
        adlib_idx = (uint8_t)value;
        { static int n; if (debug_log && n < 20) { lg("OUT adlib %04X <- %02X", port, value); n++; } }
        return;
    }
    if (port == 0x389 || port == 0x38B) {
        sound_write(adlib_idx, (unsigned)value);
        adlib_hist[adlib_idx]++;
        if (adlib_idx >= 0xB0 && adlib_idx <= 0xB8) {
            if (value & 0x20) adlib_kon++;
            else adlib_koff++;
        }
        { static int n; if (debug_log && n < 2000) { lg("OUT adlib %02X <- %02X", adlib_idx, value); n++; } }
        return;
    }
    if (port >= 0x388 && port <= 0x38B) return;
    if (port >= 0x220 && port <= 0x22F) {
        { static int n; if (debug_log && n < 120) { lg("OUT sb %04X <- %02X", port, value); n++; } }
        return;
    }
    if (port == 0x3F8 || port == 0x3F9 || port == 0x3FA || port == 0x3FB ||
        port == 0x3FC || port == 0x3FD || port == 0x3FE || port == 0x3FF)
        return;
    if (port == 0x83 || port == 0x0A || port == 0x0B || port == 0x0C ||
        port == 0x02 || port == 0x03) {
        static int n; if (debug_log && n < 40) { lg("DMA %02X <- %02X", port, value); n++; }
        return;
    }
    lg("out %04X, %02X (unhandled)", port, value);
}

static uint8_t in_port(uint32_t port)
{
    port &= 0xFFFF;

    if (port == 0x3C4) return seq_idx;
    if (port == 0x3C5) return seq_reg[seq_idx & 7];
    if (port == 0x3CE) return gc_idx;
    if (port == 0x3CF) return gc_reg[gc_idx & 15];
    if (port == 0x3D4) return crtc_idx;
    if (port == 0x3D5) return crtc_reg[crtc_idx & 31];
    if (port == 0x3C0) return attr_idx | (attr_flip ? 0x80 : 0);
    if (port == 0x3C1) return attr_reg[attr_idx & 31];
    if (port == 0x3C2) return misc_out;
    if (port == 0x3C6) return pixel_mask;
    if (port == 0x3C7) return dac_read_idx;
    if (port == 0x3C8) return dac_idx;
    if (port == 0x3C9) {
        /* the DAC is six bits wide in both directions */
        uint8_t v = (uint8_t)(palette[(dac_read_idx * 3) % 768] * 63 / 255);
        dac_reads++;
        if (++dac_state == 3) { dac_state = 0; dac_read_idx++; }
        else (void)v;
        return v;
    }
    if (port == 0x3DA) {
        int ret = vga_retrace_phase();
        vretrace_hits++;
        return (uint8_t)((ret ? 0x08 : 0x00) | (ret ? 0x00 : 0x01) |
                         (attr_flip ? 0x00 : 0x00));
    }
    if (port == 0x60) {
        int sc = kbd_pop();
        { static int n; if (debug_log && n < 20) { lg("KBD read 60 -> %02X", sc); n++; } }
        return (uint8_t)sc;
    }
    if (port == 0x61) return speaker | 0x10;
    if (port == 0x64) return 0x14;      /* output buffer empty, self test ok */
    if (port == 0x201) return 0xFF;     /* no joystick: one-shots never decay */
    if (port == 0x20) return pic_irr;
    if (port == 0x21) return pic_imr;
    if (port == 0x40 || port == 0x41 || port == 0x42) return 0;
    if (port >= 0x388 && port <= 0x38B) {
        { static int n; if (debug_log && n < 60) { lg("IN adlib %04X", port); n++; } }
        return 0;
    }
    if (port >= 0x220 && port <= 0x22F) {
        { static int n; if (debug_log && n < 120) { lg("IN sb %04X", port); n++; } }
        return 0xFF;
    }
    if (port == 0x83 || port == 0x0A || port == 0x0B || port == 0x0C) return 0xAA;
    lg("in %04X (unhandled)", port);
    return 0xFF;
}

/* ------------------------------------------------------- helper entries -- */

void out_dx_al_impl(uint32_t port, uint32_t value)
{
    out_port(port, value);
}

void out_dx_ax_impl(uint32_t port, uint32_t value)
{
    out_port(port, value & 0xFF);
    out_port(port + 1, (value >> 8) & 0xFF);
}

uint32_t in_al_dx_impl(uint32_t port)
{
    return in_port(port);
}

void out_imm_al_impl(uint32_t port, uint32_t value)
{
    out_port(port, value);
}

uint32_t in_al_imm_impl(uint32_t port)
{
    return in_port(port);
}

uint32_t x86_code_ptr_impl(uint32_t guest)
{
    uint32_t flat = sym_lookup(guest);
    if (!flat && guest >= 0xFC00 && guest < 0x10000)
        flat = sym_lookup(guest + 0x0207);
    if (guest >= 0x83800 && guest <= 0x83A00 && debug_log)
        lg("code ptr %08X -> %08X", guest, flat);
    if (!flat)
        lg("code ptr: no symbol for guest %08X", guest);
    return flat;
}

/* ------------------------------------------------------------------ dos -- */

static HANDLE files[64];
static int    file_used[64];

static int path_variants(const char *in, char *out, size_t n, int i)
{
    size_t k;
    if (i == 0) { snprintf(out, n, "%s", in); return 0; }
    if (i == 1) {
        for (k = 0; in[k] && k + 1 < n; k++)
            out[k] = (char)((in[k] >= 'A' && in[k] <= 'Z') ? in[k] + 32 : in[k]);
        out[k] = 0;
        return 0;
    }
    if (i == 2) {
        for (k = 0; in[k] && k + 1 < n; k++)
            out[k] = (char)((in[k] >= 'a' && in[k] <= 'z') ? in[k] - 32 : in[k]);
        out[k] = 0;
        return 0;
    }
    return -1;
}

static HANDLE open_any(const char *path, DWORD access, DWORD create)
{
    char buf[512];
    HANDLE h;
    int i;
    for (i = 0; i < 3; i++) {
        if (path_variants(path, buf, sizeof buf, i) < 0)
            break;
        h = CreateFileA(buf, access, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        NULL, create, FILE_ATTRIBUTE_NORMAL, NULL);
        if (h != INVALID_HANDLE_VALUE)
            return h;
    }
    return INVALID_HANDLE_VALUE;
}

static int alloc_handle(HANDLE h)
{
    int i;
    for (i = 0; i < 64; i++)
        if (!file_used[i]) {
            file_used[i] = 1;
            files[i] = h;
            return i;
        }
    CloseHandle(h);
    return -1;
}

static void set_cf(uint32_t *f, int on)
{
    if (on) f[8] |= 1;
    else    f[8] &= ~1u;
}

static void dos_int(uint32_t *f)
{
    uint32_t eax = f[7], ebx = f[4], ecx = f[6], edx = f[5];
    uint8_t ah = (uint8_t)(eax >> 8), al = (uint8_t)eax;
    char name[512];
    uint8_t *ds = (uint8_t *)(uintptr_t)V_DS_BASE;

    switch (ah) {
    case 0x09: {
        const char *p = (const char *)(ds + (uint16_t)edx);
        lg("DOS: print \"%.*s\"", 200, p);
        break;
    }
    case 0x25: {                       /* set interrupt vector */
        uint16_t off = (uint16_t)edx, seg = (uint16_t)V_DS_SEL;
        uint32_t guest;
        if (seg == 0x0207 && (off == 0xFCF6 || off == 0xFBE1)) {
            guest = (off == 0xFCF6) ? 0x00011D66u : 0x00011C51u;
            seg = (uint16_t)(guest >> 4);
            off = (uint16_t)(guest & 0xF);
        }
        guest = ((uint32_t)seg << 4) + off;
        if (guest == 0x11C51 || guest == 0x11D66) {
            uint32_t flat = sym_lookup(guest);
            lg("DOS: handler vector %02X -> guest %08X flat %08X", al, guest, flat);
            if (!flat) {
                lg("DOS: handler target missing from static symbol table");
                set_cf(f, 1);
                f[7] = (f[7] & 0xFFFF00FFu) | 0x00000100u;
                return;
            }
        }
        GW((uint32_t)al * 4) = off;
        GW((uint32_t)al * 4 + 2) = seg;
        lg("DOS: set vector %02X = %04X:%04X (flat %08X)", al, seg, off,
           sym_lookup(guest));
        break;
    }
    case 0x35: {                       /* get interrupt vector */
        uint16_t off = GW((uint32_t)al * 4);
        uint16_t seg = GW((uint32_t)al * 4 + 2);
        f[4] = off;
        if (seg == 0x11D6 && off == 0x0006) {
            seg = 0x0207; off = 0xFCF6;
        } else if (seg == 0x11C5 && off == 0x0001) {
            seg = 0x0207; off = 0xFBE1;
        }
        V_ES_SEL = seg;
        V_ES_BASE = ((uint32_t)seg + ORG_PARA) << 4;
        lg("DOS: get vector %02X -> %04X:%04X", al, seg, off);
        break;
    }
    case 0x3C: {                       /* create */
        HANDLE h;
        snprintf(name, sizeof name, "%s", (const char *)(ds + (uint16_t)edx));
        h = open_any(name, GENERIC_READ | GENERIC_WRITE, CREATE_ALWAYS);
        if (h == INVALID_HANDLE_VALUE) { f[7] = 5; set_cf(f, 1); lg("DOS: create %s failed", name); break; }
        f[7] = (uint32_t)alloc_handle(h);
        set_cf(f, 0);
        lg("DOS: create %s -> %u", name, f[7]);
        break;
    }
    case 0x3D: {                       /* open */
        HANDLE h;
        DWORD acc = (al & 3) == 0 ? GENERIC_READ :
                    (al & 3) == 1 ? GENERIC_WRITE : (GENERIC_READ | GENERIC_WRITE);
        snprintf(name, sizeof name, "%s", (const char *)(ds + (uint16_t)edx));
        h = open_any(name, acc, OPEN_EXISTING);
        if (h == INVALID_HANDLE_VALUE) {
            f[7] = 2; set_cf(f, 1);
            lg("DOS: open %s failed", name);
            break;
        }
        f[7] = (uint32_t)alloc_handle(h);
        set_cf(f, 0);
        if (strstr(name, "data01") || strstr(name, "DATA01")) {
            data01_seen = 1;
            data01_tick = GetTickCount();
        }
        lg("DOS: open %s -> %u", name, f[7]);
        break;
    }
    case 0x3E: {                       /* close */
        int h = (int)(ebx & 0x3F);
        if (file_used[h]) { CloseHandle(files[h]); file_used[h] = 0; }
        f[7] = 0;
        set_cf(f, 0);
        break;
    }
    case 0x3F: {                       /* read */
        int h = (int)(ebx & 0x3F);
        DWORD got = 0;
        if (!file_used[h]) { set_cf(f, 1); f[7] = 6; break; }
        if (!ReadFile(files[h], ds + (uint16_t)edx, ecx, &got, NULL)) {
            set_cf(f, 1); f[7] = 5; break;
        }
        f[7] = got;
        set_cf(f, 0);
        break;
    }
    case 0x40: {                       /* write */
        int h = (int)(ebx & 0x3F);
        DWORD put = 0;
        if (h == 1 || h == 2) {
            f[7] = ecx;
            set_cf(f, 0);
            break;
        }
        if (!file_used[h]) { set_cf(f, 1); f[7] = 6; break; }
        if (!WriteFile(files[h], ds + (uint16_t)edx, ecx, &put, NULL)) {
            set_cf(f, 1); f[7] = 5; break;
        }
        f[7] = put;
        set_cf(f, 0);
        break;
    }
    case 0x41: {                       /* delete */
        snprintf(name, sizeof name, "%s", (const char *)(ds + (uint16_t)edx));
        if (!DeleteFileA(name)) { set_cf(f, 1); f[7] = 2; }
        else { set_cf(f, 0); f[7] = 0; }
        break;
    }
    case 0x42: {                       /* seek */
        int h = (int)(ebx & 0x3F);
        LONG hi = (LONG)ecx, lo = (LONG)(int32_t)edx;
        LARGE_INTEGER off, res;
        if (!file_used[h]) { set_cf(f, 1); f[7] = 6; break; }
        off.HighPart = hi;
        off.LowPart = (DWORD)lo;
        if (!SetFilePointerEx(files[h], off, &res,
                              al == 0 ? FILE_BEGIN : al == 1 ? FILE_CURRENT : FILE_END)) {
            set_cf(f, 1); f[7] = 5; break;
        }
        f[7] = res.LowPart;
        f[5] = (uint32_t)res.HighPart;
        set_cf(f, 0);
        break;
    }
    case 0x4C: {
        lg("DOS: exit %u", (unsigned)(al & 0xFF));
        fflush(logf);
        ExitProcess(al);
        break;
    }
    case 0x30:                          /* version */
        f[7] = (f[7] & 0xFF00) | 5;
        f[4] = 0x001F;                  /* 5.0 */
        set_cf(f, 0);
        break;
    case 0x44:                          /* ioctl */
        f[5] = 0;
        f[7] = f[7] & 0xFF00;
        set_cf(f, 0);
        break;
    case 0x48:                          /* allocate: pretend success */
        f[7] = 0;
        f[4] = 0x9000;
        set_cf(f, 0);
        break;
    case 0x4A:                          /* resize: pretend success */
        set_cf(f, 0);
        break;
    case 0x2C: {                        /* time */
        SYSTEMTIME st;
        GetLocalTime(&st);
        f[6] = ((uint32_t)st.wHour << 8) | st.wMinute;
        f[5] = ((uint32_t)st.wSecond << 8) | (st.wMilliseconds / 10);
        break;
    }
    case 0x2A: {                        /* date */
        SYSTEMTIME st;
        GetLocalTime(&st);
        f[6] = (uint32_t)st.wYear;
        f[5] = ((uint32_t)st.wMonth << 8) | st.wDay;
        f[7] = ((uint32_t)(st.wDayOfWeek) << 8) | (f[7] & 0xFF);
        break;
    }
    default:
        lg("DOS: int21 AX=%04X BX=%04X CX=%04X DX=%04X (unhandled)",
           eax & 0xFFFF, ebx & 0xFFFF, ecx & 0xFFFF, edx & 0xFFFF);
        break;
    }
}

/* ----------------------------------------------------------------- bios -- */

static void bios_int(uint32_t *f)
{
    uint32_t eax = f[7], ebx = f[4], ecx = f[6], edx = f[5];
    uint8_t ah = (uint8_t)(eax >> 8), al = (uint8_t)eax;

    switch (ah) {
    case 0x00:
        video_mode = al;
        if (al == 0x0D) {
            level_started = data01_seen ? 2 : 1;
            if (level_started == 2)
                sound_music_level(1);
        }
        /* The BIOS clears the graphics memory when a mode is set.  Without
           this, the 256-colour title screen remains in the first plane and
           shows through the 16-colour level as large white glyphs. */
        if (al == 0x0D || al == 0x13 || al == 0x12)
            memset(M + VIDEO_AT, 0, 0x40000);
        seq_reg[2] = 0x0F;
        gc_reg[0] = gc_reg[1] = 0;
        gc_reg[3] = 0;
        gc_reg[4] = 0;
        gc_reg[5] = 0;
        gc_reg[8] = 0xFF;
        memset(vga_latch, 0, sizeof vga_latch);
        lg("BIOS: set video mode %02X", al);
        break;
    case 0x0F:
        f[7] = (f[7] & 0xFF00) | (uint32_t)video_mode;
        f[4] = (f[4] & 0xFF00) | 40;
        break;
    case 0x10: {
        int sub = al;
        if (sub == 0x00) {
            int reg = ebx & 0xFF, col = (ebx >> 8) & 0xFF;
            /* EGA/VGA attribute palette register: the pixel value selects
               one of these 6-bit DAC indices. */
            if (reg < 16) attr_reg[reg] = (uint8_t)(col & 0x3F);
            lg("BIOS: palette reg %d = %d", reg, col);
        } else if (sub == 0x02) {
            int k;
            for (k = 0; k < 16; k++) attr_reg[k] = (uint8_t)k;
            lg("BIOS: set all palette registers = %d", ebx & 0xFF);
        } else if (sub == 0x03) {
            lg("BIOS: toggle blink/intensity %d", ebx & 1);
        } else {
            lg("BIOS: int10 AH=10 AL=%02X BX=%04X (unhandled)", sub, ebx & 0xFFFF);
        }
        break;
    }
    case 0x01:
    case 0x02:
    case 0x03:
    case 0x05:
    case 0x06:
    case 0x07:
    case 0x08:
    case 0x09:
    case 0x0A:
    case 0x0B:
    case 0x0C:
    case 0x0D:
    case 0x0E:
    case 0x11:
    case 0x12:
    case 0x13:
    case 0x1A:
    case 0x1B:
        break;
    default:
        lg("BIOS: int10 AX=%04X BX=%04X CX=%04X DX=%04X (unhandled)",
           eax & 0xFFFF, ebx & 0xFFFF, ecx & 0xFFFF, edx & 0xFFFF);
        break;
    }
}

static void ems_int(uint32_t *f)
{
    uint32_t eax = f[7];
    uint8_t ah = (uint8_t)(eax >> 8);
    lg("EMS: int67 AH=%02X", ah);
    f[7] = ((uint32_t)0x84 << 8) | (eax & 0xFF);   /* not installed */
}

static uint16_t dsrw(uint32_t off)
{
    uint16_t v;
    memcpy(&v, (const void *)(uintptr_t)(V_DS_BASE + off), 2);
    return v;
}

void x86_trace_impl(uint32_t id, uint32_t *f)
{
    static unsigned long hits;
    hits++;
    if (hits > 400000)
        return;
    if (hits <= 12 || (hits % 2000) == 0)
        lg("TRACE %05lX #%lu ax=%04lX bx=%04lX si=%04lX di=%04lX | "
           "471=%04X 473=%04X 475=%04X 477=%04X 479=%04X 47D=%04X 481=%04X",
           (unsigned long)id, hits,
           (unsigned long)(f[7] & 0xFFFF), (unsigned long)(f[4] & 0xFFFF),
           (unsigned long)(f[1] & 0xFFFF), (unsigned long)(f[0] & 0xFFFF),
           (unsigned)dsrw(0x471), (unsigned)dsrw(0x473), (unsigned)dsrw(0x475),
           (unsigned)dsrw(0x477), (unsigned)dsrw(0x479), (unsigned)dsrw(0x47D),
           (unsigned)dsrw(0x481));
    {
        int k;
        char b[400];
        int n = 0;
        for (k = 36; k < 76; k += 4)
            n += snprintf(b + n, sizeof b - n, " %08lX",
                          (unsigned long)*(uint32_t *)(uintptr_t)((char *)f + k));
        lg("    stk:%s", b);
    }
    if (hits <= 200)
        lg("    es=%04X:%04lX word=%04X esbase=%08lX",
           (unsigned)V_ES_SEL, (unsigned long)(f[0] & 0xFFFF),
           (unsigned)*(uint16_t *)(uintptr_t)(V_ES_BASE + (f[0] & 0xFFFF)),
           (unsigned long)V_ES_BASE);
}

void x86_int_impl(uint32_t vec, uint32_t *f)
{
    if (debug_log)
        lg("INT %02X AX=%04X BX=%04X CX=%04X DX=%04X DS=%04X from %08lX", vec & 0xFF,
           f[7] & 0xFFFF, f[4] & 0xFFFF, f[6] & 0xFFFF, f[5] & 0xFFFF,
           V_DS_SEL, (unsigned long)*(volatile uint32_t *)0x080F0030);
    switch (vec & 0xFF) {
    case 0x10: bios_int(f); break;
    case 0x21: dos_int(f); break;
    case 0x67: ems_int(f); break;
    case 0x20:
        lg("INT 20: terminate");
        fflush(logf);
        ExitProcess(0);
        break;
    default:
        lg("INT %02X AX=%04X (unhandled)", vec & 0xFF, f[7] & 0xFFFF);
        break;
    }
}

/* ---------------------------------------------------------------- timer -- */

static volatile unsigned timer_dispatched, timer_calls;
static volatile int  timer_stop;
static DWORD WINAPI timer_thread(LPVOID arg)
{
    double last = now_ms();
    double bios = 0;
    (void)arg;
    while (!timer_stop) {
        double t;
        int us, n, k;
        Sleep(1);
        t = now_ms();
        us = (int)((t - last) * 1000.0);
        last = t;
        if (us < 0)
            us = 0;
        /* The game programs the PIT to its sample rate (divisor 149 is
           8 kHz).  One host tick per millisecond stands in for all the PIT
           interrupts of that millisecond. */
        n = us / pit_period_us;
        if (n > 64)
            n = 64;
        for (k = 0; k < n; k++)
            InterlockedIncrement(&V_TICK);
        /* BIOS tick count at 0040:006C: 18.2 per second */
        bios += us;
        while (bios >= 54945.0) {
            bios -= 54945.0;
            GW(0x46C)++;
        }
    }
    return 0;
}

uint32_t timer_check_impl(void)
{
    timer_calls++;
    if ((timer_calls & 0xFF) == 0) {
        trace_ring[trace_pos & 127] = *(volatile uint32_t *)0x080F0030;
        trace_pos++;
    }
    uint16_t off, seg;
    /* cached: this hook runs at every backward branch of the guest */
    static int notimer = -1;
    if (notimer < 0)
        notimer = getenv("FURY_NOTIMER") != NULL;
    if (notimer)
        return 0;
    uint32_t guest, flat;

    if (V_TICK <= 0 && V_KBD == 0)
        return 0;
    if (!V_IF || V_IN_HANDLER)
        return 0;

    /* IRQ1: the game installed its own int 9 handler, which reads the
       scancode from port 0x60.  Dispatch it the same way as the timer. */
    if (V_KBD) {
        V_KBD = 0;
        off = GW(9 * 4);
        seg = GW(9 * 4 + 2);
        guest = ((uint32_t)seg << 4) + off;
        flat = sym_lookup(guest);
        if (flat) {
            static int kn;
            if (debug_log && kn < 20) { lg("KBD irq -> %08X vec %04X:%04X", flat, seg, off); kn++; }
            return flat;
        }
        {
            static int warned;
            if (!warned) {
                lg("kbd: vector 9 = %04X:%04X has no flat address", seg, off);
                warned = 1;
            }
        }
    }
    if (V_TICK <= 0)
        return 0;
    if (InterlockedCompareExchange(&V_TICK, 0, 0) <= 0)
        return 0;
    InterlockedDecrement(&V_TICK);

    timer_dispatched++;
    off = GW(8 * 4);
    seg = GW(8 * 4 + 2);
    guest = ((uint32_t)seg << 4) + off;
    flat = sym_lookup(guest);
    if (!flat) {
        static int warned;
        if (!warned) {
            lg("timer: vector 8 = %04X:%04X has no flat address", seg, off);
            warned = 1;
        }
        return 0;
    }
    return flat;
}

/* -------------------------------------------------------------- graphics - */

/* ------------------------------------------------------------- display -- */

/* Compose the visible 320x200 frame out of the emulated VGA state.  The game
   reprograms mode 13h into "mode X": 96 dwords per scan line, four pixels per
   dword, one plane per pixel column, page start from the CRTC. */
static void render_frame(uint32_t *dst)
{
    int x, y;
    uint8_t idx[320];
    int planar = !(seq_reg[4] & 0x08);
    int stride = 96;
    int start = ((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]);
    int pan = (attr_reg[0x13] >> 1) & 3;
    const uint8_t *src = M + VIDEO_AT;

    if (video_mode == 0x0D) {
        /* Mode 0Dh: 16 colours, four bit planes.  The game uses a 1248-pixel
           wide playfield (stride = CRTC offset * 2 = 156 bytes, 8 pixels per
           byte) and scrolls by moving the display start address; the two
           buffers sit 0x8000 bytes apart. */
        int mstride = crtc_reg[0x13] ? crtc_reg[0x13] * 2 : 156;
        int mstart = start;
        int mpan = attr_reg[0x13] & 7;
        const uint8_t *p16 = M + 0x1571C;     /* 155A:017C, the 16-colour palette */
        int have16 = 0, k;
        for (k = 0; k < 48; k++)
            if (p16[k]) { have16 = 1; break; }
        for (y = 0; y < 200; y++) {
            int row = mstart + y * mstride;
            for (x = 0; x < 320; x++) {
                int xx = x + mpan;
                int off = (row + (xx >> 3)) & 0xFFFF;
                unsigned v = 0;
                int p;
                for (p = 0; p < 4; p++)
                    v |= ((src[p * 0x10000 + off] >> (7 - (xx & 7))) & 1) << p;
                idx[x] = (uint8_t)v;
            }
            for (x = 0; x < 320; x++) {
                int ci = idx[x];
                int r, g, b;
                if (have16) {
                    /* The sound/timer handler copies this buffer to the DAC
                       every tick; reading it directly keeps the picture
                       independent of how the write lands. */
                    r = p16[ci * 3 + 0] * 255 / 63;
                    g = p16[ci * 3 + 1] * 255 / 63;
                    b = p16[ci * 3 + 2] * 255 / 63;
                } else {
                    int a = attr_reg[ci] & 0x3F;
                    r = palette[a * 3 + 0];
                    g = palette[a * 3 + 1];
                    b = palette[a * 3 + 2];
                }
                dst[y * 320 + x] = (uint32_t)b | ((uint32_t)g << 8)
                                 | ((uint32_t)r << 16);
            }
        }
        return;
    }

    for (y = 0; y < 200; y++) {
        int base = start + y * stride;
        if (planar) {
            for (x = 0; x < 320; x++) {
                int xx = x + pan;
                if (xx >= stride * 4)
                    xx -= stride * 4;
                idx[x] = src[(xx & 3) * 0x10000 + ((base + (xx >> 2)) & 0xFFFF)];
            }
        } else {
            int a = start * 4 + y * stride * 4;
            for (x = 0; x < 320; x++)
                idx[x] = src[(a + x) & 0xFFFF];
        }
        for (x = 0; x < 320; x++) {
            uint8_t c = idx[x];
            dst[y * 320 + x] = (uint32_t)palette[c * 3 + 2]
                             | ((uint32_t)palette[c * 3 + 1] << 8)
                             | ((uint32_t)palette[c * 3] << 16);
        }
    }
}

static HWND g_hwnd;
static volatile int g_win_quit;

/* GDI is resolved lazily: importing gdi32 statically makes the loader place
   the DLL before the TLS callback reserves the guest address space, and when
   it lands on 0x02000000 the guest mapping fails. */
typedef int (WINAPI *pfn_stretchdibits)(HDC, int, int, int, int, int, int, int, int,
                                        const void *, const BITMAPINFO *, UINT, DWORD);
typedef int (WINAPI *pfn_setstretchmode)(HDC, int);
static pfn_stretchdibits g_stretchdibits;
static pfn_setstretchmode g_setstretchmode;

static void present_frame(void)
{
    static uint32_t frame[320 * 200];
    BITMAPINFO bi;
    HDC dc;
    RECT rc;

    if (!g_hwnd)
        return;
    render_frame(frame);
    dc = GetDC(g_hwnd);
    if (!dc)
        return;
    GetClientRect(g_hwnd, &rc);
    memset(&bi, 0, sizeof bi);
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 320;
    bi.bmiHeader.biHeight = -200;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    if (!g_stretchdibits) {
        ReleaseDC(g_hwnd, dc);
        return;
    }
    if (g_setstretchmode)
        g_setstretchmode(dc, COLORONCOLOR);
    g_stretchdibits(dc, 0, 0, rc.right, rc.bottom, 0, 0, 320, 200,
                    frame, &bi, DIB_RGB_COLORS, SRCCOPY);
    ReleaseDC(g_hwnd, dc);
}

static LRESULT CALLBACK wnd_proc(HWND h, UINT m, WPARAM wp, LPARAM lp)
{
    switch (m) {
    case WM_CLOSE:
        g_win_quit = 1;
        DestroyWindow(h);
        return 0;
    case WM_DESTROY:
        g_hwnd = NULL;
        PostQuitMessage(0);
        return 0;
    case WM_KEYDOWN:
    case WM_SYSKEYDOWN: {
        int sc = (int)((lp >> 16) & 0xFF);
        if (sc)
            kbd_push((uint8_t)sc);
        return 0;
    }
    case WM_KEYUP:
    case WM_SYSKEYUP: {
        int sc = (int)((lp >> 16) & 0xFF);
        if (sc)
            kbd_push((uint8_t)(sc | 0x80));
        return 0;
    }
    }
    return DefWindowProc(h, m, wp, lp);
}

static DWORD WINAPI present_thread(LPVOID arg)
{
    (void)arg;
    while (!g_win_quit) {
        present_frame();
        Sleep(16);
    }
    return 0;
}

static DWORD WINAPI win_thread(LPVOID arg)
{
    WNDCLASSA wc;
    MSG msg;
    RECT rc;
    (void)arg;
    if (getenv("FURY_NOGUI"))
        return 0;
    {
        HMODULE gd = LoadLibraryA("gdi32.dll");
        if (gd) {
            g_stretchdibits = (pfn_stretchdibits)(void *)GetProcAddress(gd, "StretchDIBits");
            g_setstretchmode = (pfn_setstretchmode)(void *)GetProcAddress(gd, "SetStretchBltMode");
        }
        if (!g_stretchdibits)
            return 0;
    }
    memset(&wc, 0, sizeof wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = "FuryScreen";
    if (!RegisterClassA(&wc))
        return 0;
    rc.left = 0; rc.top = 0;
    rc.right = 320 * 3; rc.bottom = 200 * 3;
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    g_hwnd = CreateWindowA("FuryScreen", "Fury of the Furries (static recompilation)",
                           WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
                           rc.right - rc.left, rc.bottom - rc.top,
                           NULL, NULL, GetModuleHandle(NULL), NULL);
    if (!g_hwnd)
        return 0;
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return 0;
}

static int dump_bmp(const char *name)
{
    const int W = 320, H = 200;
    int row = (W * 3 + 3) & ~3;
    int size = 54 + row * H;
    uint8_t hdr[54];
    FILE *f = fopen(name, "wb");
    const uint8_t *src = M + VIDEO_AT;
    int y, x;

    if (!f)
        return -1;
    if (debug_log) {
        unsigned s = 0;
        int k;
        for (k = 0; k < 768; k++) s = s * 31 + palette[k];
        lg("SHOT %s pal=%08X dacw=%u idx=%u start=%04X seq4=%02X gc3=%02X gc5=%02X gc8=%02X gc0=%02X gc1=%02X gc4=%02X "
           "stride=%u x=%u y=%u base=%04X scr=%04X",
           name, s, dac_writes, dac_idx,
           (unsigned)((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]),
           seq_reg[4], gc_reg[3], gc_reg[5], gc_reg[8], gc_reg[0], gc_reg[1], gc_reg[4],
           (unsigned)dsrw(0x2196), (unsigned)dsrw(0x21A4), (unsigned)dsrw(0x21A6),
           (unsigned)dsrw(0x21AC), (unsigned)dsrw(0xB941));
        {
            const uint8_t *pp = M + 0x1571C;      /* 155A:017C, 16 colours */
            char b[160];
            int n = 0, k;
            for (k = 0; k < 48 && n < (int)sizeof b - 4; k++)
                n += snprintf(b + n, sizeof b - n, "%02X", pp[k]);
            lg("PALBUF %s", b);
        }
        lg("TIMERS a=%u b=%u c=%u ds=%04X dsval=%u dsvalB=%u o=%u,%u,%u,%u,%u,%u,%u,%u",
           (unsigned)GW(0x2066F), (unsigned)GW(0x57F1F),
           (unsigned)GW(0x269CB), V_DS_SEL,
           (unsigned)*(uint16_t *)(uintptr_t)(V_DS_BASE + 0x558F),
           (unsigned)*(uint16_t *)(uintptr_t)(V_DS_BASE + 0xB8EB),
           (unsigned)GW(0x1D12F), (unsigned)GW(0x1D131),
           (unsigned)GW(0x1D133), (unsigned)GW(0x1D135),
           (unsigned)GW(0x1D137), (unsigned)GW(0x1D139),
           (unsigned)GW(0x1D13B), (unsigned)GW(0x1D13D));
        lg("STATE %u active=%u pos=%u,%u snd=%u sf=%u mp=%u drv=%u,%u,%u,%u song=%04X,%04X ctr=%04X",
           (unsigned)GW(0x1D099),
           (unsigned)*(uint8_t *)(uintptr_t)(M + 0x2066C),
           (unsigned)GW(0x20671), (unsigned)GW(0x20673),
           sound_dropped(), (unsigned)GW(0x15711), (unsigned)GW(0x1C4DC),
           (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2A),   /* driver 0x8A */
           (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2B),   /* driver 0x8B */
           (unsigned)GW(0x7FE38),                            /* driver 0x98 */
           (unsigned)GW(0x7FE3A),                            /* driver 0x9A */
           (unsigned)GW(0x83D60), (unsigned)GW(0x83D62),
           (unsigned)GW(0x1570F));
        lg("DRV8C %u %u %u", (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2C),
           (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2D),
           (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2E));
        {
            unsigned tot = 0, keys = 0, i;
            for (i = 0; i < 256; i++) {
                tot += adlib_hist[i];
                if (i >= 0xB0 && i <= 0xB8)
                    keys += adlib_hist[i];
            }
            lg("ADLIB total=%u keyon=%u bd=%u a0=%u c0=%u w=%u a=%u q=%u buf=%u kon=%u koff=%u c=%u r=%u",
               tot, keys, adlib_hist[0xBD], adlib_hist[0xA0],
               adlib_hist[0xC0], sound_writes(), sound_applied(), sound_queued(),
               spk_hi, spk_lo, sound_buffers(), adlib_kon, adlib_koff,
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2C),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE33));
        }
        lg("LIST %04X %04X %04X %04X %04X %04X %04X %04X",
           (unsigned)GW(0x1D0B7), (unsigned)GW(0x1D0B9),
           (unsigned)GW(0x1D0BB), (unsigned)GW(0x1D0BD),
           (unsigned)GW(0x1D0BF), (unsigned)GW(0x1D0C1),
           (unsigned)GW(0x1D0C3), (unsigned)GW(0x1D0C5));
        lg("MASKS %u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u "
           "WM %u/%u/%u/%u", vid_mask_hist[0], vid_mask_hist[1],
           vid_mask_hist[2], vid_mask_hist[3], vid_mask_hist[4],
           vid_mask_hist[5], vid_mask_hist[6], vid_mask_hist[7],
           vid_mask_hist[8], vid_mask_hist[9], vid_mask_hist[10],
           vid_mask_hist[11], vid_mask_hist[12], vid_mask_hist[13],
           vid_mask_hist[14], vid_mask_hist[15],
           gc5_hist[0], gc5_hist[1], gc5_hist[2], gc5_hist[3]);
    }
    memset(hdr, 0, sizeof hdr);
    hdr[0] = 'B'; hdr[1] = 'M';
    *(uint32_t *)(hdr + 2) = size;
    *(uint32_t *)(hdr + 10) = 54;
    *(uint32_t *)(hdr + 14) = 40;
    *(int32_t *)(hdr + 18) = W;
    *(int32_t *)(hdr + 22) = H;
    *(uint16_t *)(hdr + 26) = 1;
    *(uint16_t *)(hdr + 28) = 24;
    fwrite(hdr, 1, 54, f);
    {
        uint8_t *line = malloc(row);
        /* mode 13h: the CRTC start register counts dwords, so the byte
           offset is four times the programmed value.  The horizontal pan
           register shifts each scan line by (value >> 1) pixels. */
        static uint32_t frame[320 * 200];
        render_frame(frame);
        (void)src;
        for (y = H - 1; y >= 0; y--) {
            for (x = 0; x < W; x++) {
                uint32_t c = frame[y * 320 + x];
                line[x * 3 + 0] = (uint8_t)(c & 0xFF);
                line[x * 3 + 1] = (uint8_t)((c >> 8) & 0xFF);
                line[x * 3 + 2] = (uint8_t)((c >> 16) & 0xFF);
            }
            fwrite(line, 1, row, f);
        }
        free(line);
    }
    fclose(f);
    return 0;
}

/* -------------------------------------------------------------- keyboard - */

static const uint8_t scancode_letters[26] = {
    0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26,
    0x32, 0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D,
    0x15, 0x2C
};

/* Scripted key injection for headless testing:
   FURY_KEYS="2000:1c,5000:39" (milliseconds:scancode, comma separated).
   An optional third field holds the key for that many milliseconds before
   the break code is sent ("4000:39:400"); ":h" keeps it held forever. */
static void key_put(unsigned sc)
{
    kbd_push((uint8_t)sc);
    {
        int head = GW(0x41A) & 0x1F;
        int tail = GW(0x41C) & 0x1F;
        int next = (head + 1) & 0x1F;
        if (next != tail) {
            GW(0x41E + head * 2) = (uint16_t)(((sc & 0xFF) << 8) | 0x20);
            GW(0x41A) = (uint16_t)(0x1E + next * 2);
        }
    }
}

static void key_break(unsigned sc)
{
    kbd_push((uint8_t)(sc | 0x80));
    {
        int head = GW(0x41A) & 0x1F;
        int tail = GW(0x41C) & 0x1F;
        int next = (head + 1) & 0x1F;
        if (next != tail) {
            GW(0x41E + head * 2) = (uint16_t)((((sc | 0x80) & 0xFF) << 8) | 0x20);
            GW(0x41A) = (uint16_t)(0x1E + next * 2);
        }
    }
}

/* Press and release after 60 ms. */
static void key_inject(unsigned sc)
{
    key_put(sc);
    /* the game's int 9 handler keeps a key "down" until the break code
       arrives, so every injected press has to be released too */
    Sleep(60);
    key_break(sc);
}

/* Press without the break code: the game keeps the key held. */
static void key_hold(unsigned sc)
{
    key_put(sc);
}

/* Press, keep the key down for `dur` ms, then release it. */
static void key_press(unsigned sc, unsigned dur)
{
    key_put(sc);
    Sleep(dur);
    key_break(sc);
}

static DWORD WINAPI key_thread(LPVOID arg)
{
    const char *spec = getenv("FURY_KEYS");
    DWORD start = GetTickCount();
    (void)arg;

    if (getenv("FURY_AUTOSTART")) {
        /* Walk through the presentation into the first level without a
           player: Escape leaves the intro, then a Space every 1.5 s confirms
           the menu entries and the level introduction pages. */
        DWORD next = 4000;
        while ((DWORD)(GetTickCount() - start) < 2500)
            Sleep(10);
        key_inject(0x01);
        for (;;) {
            DWORD t = (DWORD)(GetTickCount() - start);
            if (t >= 90000 || level_started == 2 ||
                (data01_seen && (DWORD)(GetTickCount() - data01_tick) > 6000))
                break;
            if (t >= next) {
                key_inject(0x39);
                lg("AUTOSTART space at %lu ms", (unsigned long)t);
                next += 1500;
            }
            Sleep(10);
        }
        if (!spec)
            return 0;
        /* FURY_KEYS timestamps are relative to the moment the autostart
           finished (i.e. to reaching the level), which makes scripted play
           tests independent of the intro timing. */
        start = GetTickCount();
    }

    if (!spec)
        return 0;
    while (*spec) {
        unsigned ms;
        unsigned sc;
        unsigned dur = 0;
        int used = 0;
        int hold = 0;
        if (sscanf(spec, "%u:%x%n", &ms, &sc, &used) != 2)
            break;
        spec += used;
        if (spec[0] == ':') {
            if (spec[1] == 'h' || spec[1] == 'H') {
                hold = 1;
                spec += 2;
            } else if (spec[1] >= '0' && spec[1] <= '9') {
                int n = 0;
                if (sscanf(spec + 1, "%u%n", &dur, &n) == 1)
                    spec += 1 + n;
            }
        }
        while (*spec == ',' || *spec == ' ')
            spec++;
        while ((DWORD)(GetTickCount() - start) < ms)
            Sleep(10);
        if (hold)
            key_hold(sc);
        else if (dur)
            key_press(sc, dur);
        else
            key_inject(sc);
        lg("KEYS inject %02X%s at %u ms%s%u", sc, hold ? " (hold)" : "", ms,
           dur ? " dur=" : "", dur);
    }
    return 0;
}

static DWORD WINAPI kbd_thread(LPVOID arg)
{
    (void)arg;
    for (;;) {
        int c = _getch();
        int sc = -1;
        if (c == 0 || c == 0xE0) {
            int c2 = _getch();
            switch (c2) {
            case 72: sc = 0x48; break;      /* up */
            case 80: sc = 0x50; break;      /* down */
            case 75: sc = 0x4B; break;      /* left */
            case 77: sc = 0x4D; break;      /* right */
            case 71: sc = 0x47; break;      /* home */
            case 79: sc = 0x4F; break;      /* end */
            case 73: sc = 0x49; break;      /* pgup */
            case 81: sc = 0x51; break;      /* pgdn */
            case 83: sc = 0x53; break;      /* del */
            case 59: sc = 0x3B; break;
            default: break;
            }
        } else if (c == 27) sc = 0x01;
        else if (c == 13) sc = 0x1C;
        else if (c == 32) sc = 0x39;
        else if (c == 9)  sc = 0x0F;
        else if (c == 8)  sc = 0x0E;
        else if (c >= 'a' && c <= 'z') sc = scancode_letters[c - 'a'];
        else if (c >= 'A' && c <= 'Z') sc = scancode_letters[c - 'A'];
        else if (c >= '0' && c <= '9') sc = (uint8_t)(0x02 + (c == '0' ? 10 : c - '1'));
        else if (c == '-' || c == '_') sc = 0x0C;
        else if (c == '=' || c == '+') sc = 0x0D;
        else if (c == ',' || c == '<') sc = 0x33;
        else if (c == '.' || c == '>') sc = 0x34;
        else if (c == '/' || c == '?') sc = 0x35;
        if (sc >= 0) {
            kbd_push((uint8_t)sc);
            Sleep(60);
            kbd_push((uint8_t)(sc | 0x80));
        }
    }
    return 0;
}

static volatile DWORD g_guest_tid;

static void vga_map(const char *tag)
{
    const uint8_t *v = M + VIDEO_AT;
    int blk;
    char line[512];
    int pos = 0;
    int hist[16];
    memset(hist, 0, sizeof hist);
    for (blk = 0; blk < 0x40000; blk += 4096) {
        int i, nz = 0;
        for (i = 0; i < 4096; i++)
            if (v[blk + i]) nz++;
        pos += snprintf(line + pos, sizeof line - pos, " %03X:%d", blk >> 10, nz);
        if (pos > 400) { lg("VMAP %s%s", tag, line); pos = 0; line[0] = 0; }
    }
    if (pos) lg("VMAP %s%s", tag, line);
    for (blk = 0; blk < 64000; blk++)
        hist[v[blk] >> 4]++;
    lg("VHIST %s %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d", tag,
       hist[0], hist[1], hist[2], hist[3], hist[4], hist[5], hist[6], hist[7],
       hist[8], hist[9], hist[10], hist[11], hist[12], hist[13], hist[14], hist[15]);
}

/* quick sampling of where the guest spends time.
   NOTE: never call lg() while the guest thread is suspended - the guest may
   already hold the log lock.  Snapshot, resume, then log. */
static DWORD WINAPI sample_thread(LPVOID arg)
{
    int i;
    (void)arg;
    Sleep(40000);
    for (i = 0; i < 120; i++) {
        HANDLE h;
        CONTEXT ctx;
        uint32_t eip = 0;
        Sleep(80);
        h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_guest_tid);
        if (!h)
            continue;
        SuspendThread(h);
        memset(&ctx, 0, sizeof ctx);
        ctx.ContextFlags = CONTEXT_CONTROL;
        GetThreadContext(h, &ctx);
        eip = (uint32_t)ctx.Eip;
        ResumeThread(h);
        CloseHandle(h);
        if (eip >= 0x20000000u && eip < 0x20200000u)
            lg("SMPL flat %08lX", (unsigned long)eip);
        else
            lg("SMPL host %08lX", (unsigned long)eip);
    }
    return 0;
}

static DWORD WINAPI watch_thread(LPVOID arg)
{
    int i;
    (void)arg;
    for (i = 0; i < 8; i++) {
        HANDLE h;
        char nm[32];
        FILE *f;
        Sleep(900);
        h = OpenThread(THREAD_GET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, g_guest_tid);
        if (!h)
            continue;
        SuspendThread(h);
        snprintf(nm, sizeof nm, "dump%d.bin", i);
        f = fopen(nm, "wb");
        if (f) {
            fwrite(M + VIDEO_AT, 1, 0x40000, f);
            fwrite(M + 0x32990, 1, 0x1C000, f);
            fwrite(palette, 1, 768, f);
            fwrite(crtc_reg, 1, 32, f);
            fwrite(seq_reg, 1, 8, f);
            fwrite(attr_reg, 1, 32, f);
            fclose(f);
        }
        ResumeThread(h);
        CloseHandle(h);
        lg("DUMP %d dac_w=%u start=%04X masks %u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u/%u",
           i, dac_writes,
           (unsigned)((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]),
           vid_mask_hist[0], vid_mask_hist[1], vid_mask_hist[2], vid_mask_hist[3],
           vid_mask_hist[4], vid_mask_hist[5], vid_mask_hist[6], vid_mask_hist[7],
           vid_mask_hist[8], vid_mask_hist[9], vid_mask_hist[10], vid_mask_hist[11],
           vid_mask_hist[12], vid_mask_hist[13], vid_mask_hist[14], vid_mask_hist[15]);
    }
    return 0;
}

static volatile int shots_wanted = 1;

/* Test hook: jump the level clock to its last second so the out-of-time
   cut-scene can be exercised without waiting five minutes. */
/* The sound-enable flag lives in the timer handler's code segment
   (155A:0171).  The shipped image starts with it cleared and normally the
   sound setup menu sets it; FURY_SOUND_FORCE keeps it set for testing. */
/* The timer handler's sound block is gated by 155A:0171.  Keep it set so the
   game's KMD player and PC-speaker effects run; the OPL writes go through
   sound.c into Nuked OPL3. */
static DWORD WINAPI music_thread(LPVOID arg)
{
    int n = 0;
    int dumped = 0;
    DWORD row_tick = GetTickCount();
    unsigned max98 = 0, last98 = 0, changes = 0;
    (void)arg;
    for (;;) {
        unsigned v98 = (unsigned)GW(0x7FE38);
        if (v98 != last98) {
            last98 = v98;
            changes++;
            if (v98 > max98)
                max98 = v98;
        }
        if (debug_log && (DWORD)(GetTickCount() - row_tick) >= 1000) {
            lg("ROWSTAT changes=%u max=%04X cur=%04X 8C=%u 9C=%u 8D=%u 8F=%u "
               "a=%04X b=%04X c=%04X d=%04X t0=%04X t1=%04X",
               changes, max98, v98,
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2C),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3C),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2D),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2F),
               (unsigned)GW(0x7FE38), (unsigned)GW(0x81E38),
               (unsigned)GW(0x80098), (unsigned)GW(0x15698),
               (unsigned)GW(0x7FED9), (unsigned)GW(0x81ED9));
            lg("CH %02X/%02X/%02X %02X/%02X/%02X %02X/%02X/%02X "
               "%02X/%02X/%02X %02X/%02X/%02X %02X/%02X/%02X",
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 0 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 0 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 0 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 1 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 1 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 1 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 2 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 2 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 2 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 3 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 3 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 3 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 4 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 4 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 4 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3D + 5 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3E + 5 * 0x18),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3F + 5 * 0x18));
            {
                unsigned p0 = GW(0x7FE47), p1 = GW(0x7FE5F);
                unsigned f0 = (p0 < 0xD60) ? GW(0x7FED9 + p0 * 2) : 0;
                unsigned f1 = (p1 < 0xD60) ? GW(0x7FED9 + p1 * 2) : 0;
                lg("CHV n0=%02X p0=%04X f0=%04X v0=%02X i0=%02X "
                   "n1=%02X p1=%04X f1=%04X v1=%02X i1=%02X",
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE42),
                   p0, f0,
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE49),
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE46),
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE5A),
                   p1, f1,
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE61),
                   (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE5E));
            }
            changes = max98 = 0;
            row_tick = GetTickCount();
        }
        *(uint16_t *)(uintptr_t)(M + 0x15711) = 1;   /* 155A:0171 sound on */
        if (debug_log && !dumped && GW(0x7FE3A) != 0) {
            unsigned seg = GW(0x7FE3A), off = GW(0x7FE38);
            uint8_t *pd = (uint8_t *)(uintptr_t)(M + ((uint32_t)seg << 4));
            lg("PAT %04X:%04X row0=%02X %02X %02X %02X %02X %02X %02X %02X "
               "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X %02X "
               "%02X %02X %02X %02X %02X %02X %02X %02X %02X %02X (off=%04X)",
               seg, off, pd[0], pd[1], pd[2], pd[3], pd[4], pd[5], pd[6], pd[7],
               pd[8], pd[9], pd[10], pd[11], pd[12], pd[13], pd[14], pd[15],
               pd[16], pd[17], pd[18], pd[19], pd[20], pd[21], pd[22], pd[23],
               pd[24], pd[25], pd[26], pd[27], pd[28], pd[29], off);
            dumped = 1;
        }
        if (debug_log && (++n % 200) == 0) {
            lg("MUS 98=%04X 9A=%04X 8B=%u 8C=%u 92=%u 93=%u 8A=%u 90=%u 8E=%u 8D=%u 8F=%u 9C=%u",
               (unsigned)GW(0x7FE38), (unsigned)GW(0x7FE3A),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2B),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2C),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE32),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE33),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2A),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE30),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2E),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2D),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE2F),
               (unsigned)*(uint8_t *)(uintptr_t)(M + 0x7FE3C));
        }
        Sleep(1);
    }
}

static DWORD WINAPI teleport_thread(LPVOID arg)
{
    const char *spec = getenv("FURY_TP");
    unsigned x, y;
    (void)arg;
    if (!spec || sscanf(spec, "%u,%u", &x, &y) != 2)
        return 0;
    while (!data01_seen)
        Sleep(100);
    while (level_started != 2)
        Sleep(100);
    /* The mode is switched while the level is still fading in; wait until the
       intro pages are over (the autostart waits six seconds as well). */
    Sleep(6500);
    {
        int i;
        uint16_t cx = (uint16_t)x, cy = (uint16_t)y;
        for (i = 0; i < 20; i++) {
            *(uint16_t *)(uintptr_t)(M + 0x1D150) = (uint16_t)(cx << 5); /* 1B0E:2070 */
            *(uint16_t *)(uintptr_t)(M + 0x1D154) = (uint16_t)(cx << 5); /* 1B0E:2074 */
            *(uint16_t *)(uintptr_t)(M + 0x1D152) = (uint16_t)(cy << 5); /* 1B0E:2072 */
            *(uint16_t *)(uintptr_t)(M + 0x1D156) = (uint16_t)(cy << 5); /* 1B0E:2076 */
            *(uint16_t *)(uintptr_t)(M + 0x1D15C) = cx;                 /* 1B0E:207C */
            *(uint16_t *)(uintptr_t)(M + 0x1D15E) = cy;                 /* 1B0E:207E */
            *(uint16_t *)(uintptr_t)(M + 0x20671) = (uint16_t)(cx - 8); /* 1B0E:5591 */
            *(uint16_t *)(uintptr_t)(M + 0x20673) = (uint16_t)(cy - 8); /* 1B0E:5593 */
            *(uint16_t *)(uintptr_t)(M + 0x22F8F) = (uint16_t)(cx - 16);/* 1B0E:74AF */
            *(uint16_t *)(uintptr_t)(M + 0x22F91) = (uint16_t)(cy - 16);/* 1B0E:74B1 */
            *(uint16_t *)(uintptr_t)(M + 0x1D12F) = (uint16_t)(cx - 8); /* 1B0E:204F */
            *(uint16_t *)(uintptr_t)(M + 0x1D131) = (uint16_t)(cy - 8); /* 1B0E:2051 */
            Sleep(100);
        }
    }
    lg("TELEPORT %u,%u", x, y);
    return 0;
}

static DWORD WINAPI fast_time_thread(LPVOID arg)
{
    const char *spec = getenv("FURY_FAST_TIME");
    uint16_t *p;
    (void)arg;
    if (!spec)
        return 0;
    Sleep((DWORD)atoi(spec));
    while (!data01_seen)
        Sleep(100);
    while (level_started != 2)
        Sleep(100);
    Sleep(1000);
    p = (uint16_t *)(M + 0x269CB);     /* 1B0E:B8EB, the level clock (1/30 s) */
    lg("FAST_TIME value=%u ds=%04X dsval=%u", (unsigned)*p, V_DS_SEL,
       (unsigned)*(uint16_t *)(uintptr_t)(V_DS_BASE + 0xB8EB));
    {
        int i;
        for (i = 0; i < 30; i++) {
            *p = 1;
            Sleep(200);
        }
    }
    return 0;
}

/* A single snapshot of the whole VGA window and its registers, after a delay
   given in milliseconds by FURY_DUMP_AT.  Used to study the level screens
   offline, one layout at a time. */
static DWORD WINAPI raw_dump_thread(LPVOID arg)
{
    const char *spec = getenv("FURY_DUMP_AT");
    FILE *f;
    (void)arg;
    if (!spec)
        return 0;
    Sleep((DWORD)atoi(spec));
    f = fopen("vga.bin", "wb");
    if (!f)
        return 0;
    fwrite(M + VIDEO_AT, 1, 0x40000, f);
    fwrite(palette, 1, 768, f);
    fwrite(seq_reg, 1, 8, f);
    fwrite(crtc_reg, 1, 32, f);
    fwrite(attr_reg, 1, 32, f);
    fwrite(gc_reg, 1, 16, f);
    fwrite(M + 0x1D0B0, 1, 0x100, f);   /* 1B0E:1FD0 object list + objects */
    fclose(f);
    lg("RAWDUMP vga.bin at %s ms (start=%04X seq=%02X/%02X)", spec,
       (unsigned)((crtc_reg[0x0C] << 8) | crtc_reg[0x0D]),
       seq_reg[2], seq_reg[4]);
    return 0;
}
static DWORD WINAPI shot_thread(LPVOID arg)
{
    int n = 0;
    int ms = 700;
    (void)arg;
    if (!getenv("FURY_SHOTS"))
        return 0;
    if (getenv("FURY_SHOT_MS"))
        ms = atoi(getenv("FURY_SHOT_MS"));
    if (ms < 50)
        ms = 50;
    while (shots_wanted && n < 1000) {
        Sleep(ms);
        if (!shots_wanted)
            break;
        {
            char nm[64];
            snprintf(nm, sizeof nm, "shot_%03d.bmp", n++);
            dump_bmp(nm);
        }
    }
    return 0;
}

/* ----------------------------------------------------------------- main -- */

static uint8_t *read_file(const char *path, DWORD *len)
{
    FILE *f = fopen(path, "rb");
    uint8_t *buf;
    long n;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    n = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((size_t)n + 16);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(buf);
        return NULL;
    }
    fclose(f);
    *len = (DWORD)n;
    return buf;
}

static volatile int g_running = 1;

/*
 * Reserve the guest memory before the C runtime creates its heap.  The heap is
 * often placed at 0x10000, which is exactly where the emulated real-mode
 * machine has to live, so the allocation has to happen during TLS callback
 * processing (before CRT initialisation).
 */
static void NTAPI early_map(PVOID module, DWORD reason, PVOID reserved)
{
    (void)module; (void)reserved;
    if (reason == DLL_PROCESS_ATTACH) {
        void *p = VirtualAlloc((void *)GUEST_BASE, GUEST_SIZE,
                               MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE);
        if (p)
            g_running = 2;              /* guest memory already mapped */
    }
}

#pragma section(".CRT$XLB", long, read)
__declspec(allocate(".CRT$XLB"))
static void (NTAPI *const tls_callback)(PVOID, DWORD, PVOID) = early_map;

static LONG WINAPI crash_filter(EXCEPTION_POINTERS *ep)
{
    CONTEXT *c = ep->ContextRecord;
    int i;
    lg("*** exception %08lX at %p (accessed %p)",
       (unsigned long)ep->ExceptionRecord->ExceptionCode,
       ep->ExceptionRecord->ExceptionAddress,
       (void *)ep->ExceptionRecord->ExceptionInformation[1]);
    lg("    DS=%04X@%08lX ES=%04X@%08lX SS=%04X@%08lX",
       V_DS_SEL, (unsigned long)V_DS_BASE, V_ES_SEL, (unsigned long)V_ES_BASE,
       V_SS_SEL, (unsigned long)V_SS_BASE);
    {
        const uint16_t *w = (const uint16_t *)(uintptr_t)V_DS_BASE;
        lg("    ds:0460=%04X 0470=%04X 0471=%04X 0473=%04X 0475=%04X "
           "0477=%04X 0479=%04X 047D=%04X 0481=%04X 0483=%04X",
           (unsigned)w[0x230], (unsigned)w[0x238],
           (unsigned)w[0x238], (unsigned)w[0x239],
           (unsigned)w[0x23A], (unsigned)w[0x23B],
           (unsigned)w[0x23C], (unsigned)w[0x23E],
           (unsigned)w[0x240], (unsigned)w[0x241]);
    }
    if (c) {
        lg("    eip=%08lX esp=%08lX ebp=%08lX", (unsigned long)c->Eip,
           (unsigned long)c->Esp, (unsigned long)c->Ebp);
        lg("    eax=%08lX ebx=%08lX ecx=%08lX edx=%08lX",
           (unsigned long)c->Eax, (unsigned long)c->Ebx,
           (unsigned long)c->Ecx, (unsigned long)c->Edx);
        lg("    esi=%08lX edi=%08lX", (unsigned long)c->Esi,
           (unsigned long)c->Edi);
        for (i = 0; i < 12; i++) {
            uint32_t *sp = (uint32_t *)(uintptr_t)(c->Esp + i * 4);
            lg("    [esp+%02X] = %08lX", i * 4, (unsigned long)*sp);
        }
        {
            LONG pos = trace_pos;
            int k;
            lg("    last branches (flat -> guest):");
            for (k = 31; k >= 0; k--) {
                uint32_t e = trace_ring[(pos - k) & 127];
                if (e)
                    lg("      %08lX -> %08lX", (unsigned long)e,
                       (unsigned long)sym_nearest(e));
            }
        }
        /* sound mixer state at DS:0x470 (debug aid for div faults) */
        if (M) {
            uint32_t dsv = V_DS_BASE;
            lg("    ds=%04X mixer 471=%04X 473=%04X 475=%04X 477=%04X 479=%04X 47d=%04X 481=%04X",
               V_DS_SEL,
               *(uint16_t *)(uintptr_t)(dsv + 0x471),
               *(uint16_t *)(uintptr_t)(dsv + 0x473),
               *(uint16_t *)(uintptr_t)(dsv + 0x475),
               *(uint16_t *)(uintptr_t)(dsv + 0x477),
               *(uint16_t *)(uintptr_t)(dsv + 0x479),
               *(uint16_t *)(uintptr_t)(dsv + 0x47d),
               *(uint16_t *)(uintptr_t)(dsv + 0x481));
            {
                static const char *hexd = "0123456789ABCDEF";
                char line[80];
                uint8_t *es = (uint8_t *)(uintptr_t)V_ES_BASE;
                int j, k;
                lg("    es=%04X base=%08lX", V_ES_SEL,
                   (unsigned long)V_ES_BASE);
                for (j = 0; j < 0x80; j += 16) {
                    char *p = line;
                    for (k = 0; k < 16; k++) {
                        *p++ = hexd[es[j + k] >> 4];
                        *p++ = hexd[es[j + k] & 15];
                        *p++ = ' ';
                    }
                    *p = 0;
                    lg("    es:%04X  %s", j, line);
                }
            }
        }
    }
    fflush(logf);
    return EXCEPTION_EXECUTE_HANDLER;
}

static LONG WINAPI ue_filter(EXCEPTION_POINTERS *ep)
{
    if (logf) {
        lg("UNHANDLED %08lX at %08lX",
           (unsigned long)ep->ExceptionRecord->ExceptionCode,
           (unsigned long)(uintptr_t)ep->ExceptionRecord->ExceptionAddress);
        fflush(logf);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

int main(int argc, char **argv)
{
    uint8_t *image, *code;
    DWORD image_len, code_len;
    uint32_t entry, symaddr;
    HANDLE section;
    void *lo, *hi;
    int i;
    DWORD t0;

    (void)argc; (void)argv;

    logf = fopen("fury.log", "w");
    debug_log = getenv("FURY_DEBUG") != NULL;
    SetUnhandledExceptionFilter(ue_filter);
    if (!getenv("FURY_CONSOLE") && !getenv("FURY_NOGUI"))
        ShowWindow(GetConsoleWindow(), SW_HIDE);
    if (!logf) logf = stderr;
    setvbuf(logf, NULL, _IONBF, 0);     /* never lose the tail of the log */
    if (getenv("FURY_TICK_MS"))
        tick_override = atoi(getenv("FURY_TICK_MS"));
    vga_raw = getenv("FURY_VGA_RAW") != NULL;
    if (tick_override)
        timeBeginPeriod(1);
    if (getenv("FURY_DEBUG"))
        fprintf(stderr, "tls pre-mapped guest memory: %d\n", g_running == 2);
    {
        unsigned long probe[12] = { 0x00200000, 0x00300000, 0x003E0000, 0x003F0000,
                                    0x00400000, 0x00500000, 0x01000000, 0x02000000,
                                    0x03000000, 0x04000000, 0x20000000, 0x30000000 };
        int k;
        (void)k; (void)probe;
    }

    M = (uint8_t *)GUEST_BASE;

    /* guest memory (normally mapped by the TLS callback) */
    if (g_running != 2 &&
        !VirtualAlloc((void *)GUEST_BASE, GUEST_SIZE, MEM_RESERVE | MEM_COMMIT,
                      PAGE_EXECUTE_READWRITE)) {
        MEMORY_BASIC_INFORMATION mbi;
        DWORD e = GetLastError();
        if (VirtualQuery((void *)GUEST_BASE, &mbi, sizeof mbi))
            fprintf(stderr, "guest map failed (%lu): base=%p size=%lx state=%lx\n",
                    e, mbi.BaseAddress, (unsigned long)mbi.RegionSize,
                    (unsigned long)mbi.State);
        else
            fprintf(stderr, "cannot map guest memory (error %lu)\n", e);
        return 1;
    }
    ZeroMemory((void *)GUEST_BASE, GUEST_SIZE);

    /* variables live inside the guest mapping, above the 640 KiB the game uses */
    ZeroMemory((void *)VAR_BASE, VAR_SIZE);

    /* 64 KiB stack mapped twice so 16-bit SP wraps */
    section = CreateFileMappingA(INVALID_HANDLE_VALUE, NULL, PAGE_READWRITE,
                                 0, 0x10000, NULL);
    if (!section) { fprintf(stderr, "stack section failed\n"); return 1; }
    lo = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, 0x10000, (void *)0x02FF0000);
    hi = MapViewOfFileEx(section, FILE_MAP_ALL_ACCESS, 0, 0, 0x10000, (void *)STACK_HOST);
    if (!lo || !hi) {
        fprintf(stderr, "stack map failed (error %lu)\n", GetLastError());
        return 1;
    }

    /* recompiled code */
    code = read_file("fury-flat.bin", &code_len);
    if (!code) {
        fprintf(stderr, "cannot read fury-flat.bin\n");
        return 1;
    }
    if (!VirtualAlloc((void *)CODEBASE, (code_len + 0xFFFF) & ~0xFFFFu,
                      MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE)) {
        fprintf(stderr, "cannot map code (error %lu)\n", GetLastError());
        return 1;
    }
    memcpy((void *)CODEBASE, code, code_len);
    entry = *(uint32_t *)(code + 0);
    symaddr = *(uint32_t *)(code + 4);
    symcount = *(uint32_t *)(code + 8);
    symtab = (uint32_t *)symaddr;
    lg("code %lu bytes at %08X, entry %08X, %u symbols", code_len, CODEBASE,
       entry, symcount);

    /* original program image (the memory dump of the loaded executable) */
    image = read_file("fury.img", &image_len);
    if (!image)
        image = read_file("dump\\fury.bin", &image_len);
    if (!image) {
        fprintf(stderr, "cannot read fury.img / dump\\fury.bin\n");
        return 1;
    }
    if (image_len > 0x8EFF3 - IMAGE_AT + 0x1000) {
        fprintf(stderr, "image too large (%lu bytes)\n", image_len);
        return 1;
    }
    lg("image %lu bytes, load at %08X", image_len, GUEST_BASE + IMAGE_AT);
    memcpy(M + IMAGE_AT, image, image_len);
    free(image);

    /* PSP */
    GB(PSP_AT + 0) = 0xCD;
    GB(PSP_AT + 1) = 0x20;
    GW(PSP_AT + 2) = 0xA000;            /* top of memory */
    GB(PSP_AT + 0x50) = 0xCD;           /* int 21 / retf */
    GB(PSP_AT + 0x51) = 0x21;
    GB(PSP_AT + 0x52) = 0xCB;
    GW(PSP_AT + 0x2C) = 0;              /* no environment */

    /* BIOS data area */
    GW(0x410) = 0x0021;       /* equipment: colour 80x25, 1 disk */
    GW(0x413) = 640;          /* KB */
    GW(0x41A) = 0x001E;       /* keyboard head */
    GW(0x41C) = 0x001E;       /* keyboard tail */
    GW(0x41E) = 0x001E;
    GW(0x480) = 0x001E;
    GW(0x482) = 0x003E;
    GB(0x449) = 0x03;
    GW(0x44A) = 40;
    GW(0x44C) = 0x1000;
    GW(0x463) = 0x03D4;
    GB(0x465) = 0x29;
    GW(0x472) = 0x1234;       /* reset word */

    /* helper pointer table */
    *(void **)PTR_X86_INT = (void *)x86_int;
    *(void **)PTR_OUT_DX_AL = (void *)x86_out_dx_al;
    *(void **)PTR_OUT_DX_AX = (void *)x86_out_dx_ax;
    *(void **)PTR_IN_AL_DX = (void *)x86_in_al_dx;
    *(void **)PTR_OUT_IMM_AL = (void *)x86_out_imm_al;
    *(void **)PTR_IN_AL_IMM = (void *)x86_in_al_imm;
    *(void **)PTR_CHECK_TIMER = (void *)SR_CheckTimer;
    *(void **)PTR_CODE_PTR = (void *)x86_code_ptr;
    *(void **)PTR_TRACE = (void *)x86_trace;
    *(void **)PTR_VGA_STORE8 = (void *)vga_store8;
    *(void **)PTR_VGA_STORE16 = (void *)vga_store16;
    *(void **)PTR_VGA_STORE32 = (void *)vga_store32;
    *(void **)PTR_VGA_LOAD8 = (void *)vga_load8;
    *(void **)PTR_VGA_LOAD16 = (void *)vga_load16;
    *(void **)PTR_VGA_LOAD32 = (void *)vga_load32;
    *(void **)PTR_VGA_STOS8 = (void *)vga_stos8;
    *(void **)PTR_VGA_STOS16 = (void *)vga_stos16;
    *(void **)PTR_VGA_MOVSB = (void *)vga_movsb;
    *(void **)PTR_VGA_MOVSW = (void *)vga_movsw;
    *(void **)PTR_FLAT_RET = (void *)flat_retf;

    if (!getenv("FURY_NOSOUND")) {
        if (sound_init()) {
            sound_ok = 1;
            lg("sound: OPL2 output enabled");
        } else {
            lg("sound: OPL2/waveOut init failed");
        }
    }

    V_IF = 1;
    V_SS_BASE = STACK_HOST;
    V_DS_BASE = GUEST_BASE;
    V_DS_SEL = 0;
    V_VID_BASE = 0x080A0000u;
    for (i = 0; i < 16; i++)
        attr_reg[i] = (uint8_t)i;       /* identity palette until the game says otherwise */
    gc_reg[8] = 0xFF;                   /* bit mask: no bits masked */
    seq_reg[2] = 0x0F;                  /* map mask: all planes writable */

    SetUnhandledExceptionFilter(crash_filter);

    g_guest_tid = GetCurrentThreadId();
    CreateThread(NULL, 0, timer_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, win_thread, NULL, 0, NULL);
    Sleep(150);
    CreateThread(NULL, 0, present_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, key_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, shot_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, raw_dump_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, fast_time_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, teleport_thread, NULL, 0, NULL);
    if (sound_ok)
        CreateThread(NULL, 0, music_thread, NULL, 0, NULL);
    if (getenv("FURY_DEBUG")) {
        CreateThread(NULL, 0, watch_thread, NULL, 0, NULL);
        CreateThread(NULL, 0, sample_thread, NULL, 0, NULL);
    }

    g_guest_tid = GetCurrentThreadId();
    if (getenv("FURY_DEBUG"))
        lg("FN in_port=%p out_port=%p timer=%p timer_check=%p render=%p dump=%p",
           (void *)in_port, (void *)out_port, (void *)timer_thread,
           (void *)timer_check_impl, (void *)render_frame, (void *)dump_bmp);
    lg("starting guest at %08X", entry);
    fflush(logf);

    __try {
        run_guest(entry);
    } __except (crash_filter(GetExceptionInformation())) {
        /* fall through */
    }

    lg("guest returned");
    fflush(logf);

    /* dump a few screenshots for inspection */
    for (i = 0; i < 1; i++) {
        char nm[64];
        snprintf(nm, sizeof nm, "shot_%03d.bmp", i);
        dump_bmp(nm);
    }
    fclose(logf);
    (void)t0; (void)g_running;
    return 0;
}
