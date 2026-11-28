/* dh_game.c -- DOWNHILL BIKERS' table for the System 23 host (../s23/include/s23_game.h): MAME's downhillu set (US, DH3 Ver. A -- the one MAME
 * runs; its parent downhill is World DH2 Ver. A) on Namco System 23 (the board Time Crisis II runs on), with the ASCA-3 I/O board (the same
 * H8/3334 program as Angler King's FCA-1, under its own "ASCA-3" name). Values from MAME's namcos23.cpp (namcos23_state::s23 / downhill,
 * ROM_START(downhillu), INPUT_PORTS_START(downhill)) and namcoio.cpp (namco_asca_3_device). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "s23_game.h"

static const eng_rom_t chips[] = {
    { "dh3vera.ic2", 0x200000, NULL, NULL, 0x5D9952E9 }, { "dh3vera.ic1", 0x200000, NULL, NULL, 0x64A236F3 },
    { "dh3vera.ic3", 0x80000, NULL, NULL, 0x98F9FC8B },  { "asc3_io-c.ic14", 0x20000, NULL, NULL, 0x2F272A7B },
    { "dh1mtah.2j", 0x800000, NULL, NULL, 0x3B56FAA7 }, { "dh1mtal.2h", 0x800000, NULL, NULL, 0x9FA07BFE },
    { "dh1cgll.4m", 0x800000, NULL, "dh1cgll.5m", 0xC0D5AD87 }, { "dh1cglm.4k", 0x800000, NULL, "dh1cglm.5k", 0x5D9A5E35 },
    { "dh1cgum.4j", 0x800000, NULL, "dh1cgum.5j", 0x1044D0A0 }, { "dh1cguu.5f", 0x800000, NULL, "dh1cguu.4f", 0x66CB0DD7 },
    { "dh1ccrl.7f", 0x400000, NULL, "dh1ccrl.7m", 0x65C857DF }, { "dh1ccrh.7e", 0x200000, NULL, "dh1ccrh.7k", 0xF21C482D },
    { "dh1pt0h.7a", 0x400000, NULL, NULL, 0x0E84A5D8 }, { "dh1pt0l.7c", 0x400000, NULL, NULL, 0xD120EEE5 },
    { "dh1pt1h.5a", 0x400000, NULL, NULL, 0x88CD4C90 }, { "dh1pt1l.5c", 0x400000, NULL, NULL, 0xDEE2F2BF },
    { "dh1pt2h.4a", 0x400000, NULL, NULL, 0x7E167C65 }, { "dh1pt2l.4c", 0x400000, NULL, NULL, 0x714E3090 },
    { "dh1wavel.2c", 0x800000, NULL, NULL, 0x10954726 }, { "dh1waveh.2a", 0x800000, NULL, NULL, 0x2ADFA312 },
};
/* downhillu.zip (a clone: a split set keeps the shared chips in downhill.zip), the parent, and the I/O board's device zip */
static const char *const zips[] = { "downhillu.zip", "downhill.zip", "namco_asca3.zip" };

int s23_rom_read(const char *dir, const char *name, uint8_t *dst, size_t n);   /* ../s23/src/s23_rom.c */

/* the boot image (4 MB at 0x1FC00000): dh3vera.ic2 and .ic1 byte-interleaved, .ic2 first: the NMON monitor and, at 0x20000, the game's ELF
 * (tools/make_prog.py prints the layout) */
static int rom_boot(const char *dir, uint8_t *boot)
{
    uint8_t *a = malloc(0x200000), *b = malloc(0x200000);
    const int ok = a && b && s23_rom_read(dir, "dh3vera.ic1", a, 0x200000) && s23_rom_read(dir, "dh3vera.ic2", b, 0x200000);
    if (ok) for (uint32_t k = 0; k < 0x200000; k++) { boot[2 * k] = b[k]; boot[2 * k + 1] = a[k]; }
    free(a); free(b);
    return ok;
}
/* the sub-CPU's (H8/3002) program: dh3vera.ic3, word-swapped (MAME ROM_LOAD16_WORD_SWAP) */
static int rom_sub(const char *dir, uint8_t *dst, size_t n)
{
    if (n < 0x80000 || !s23_rom_read(dir, "dh3vera.ic3", dst, 0x80000)) return 0;
    for (uint32_t k = 0; k < 0x80000; k += 2) { const uint8_t t = dst[k]; dst[k] = dst[k + 1]; dst[k + 1] = t; }
    return 1;
}
/* the ASCA-3 I/O board's (H8/3334) program: the first n bytes of asc3_io-c.ic14 (128 KB; the code is the first 8 KB) */
static int rom_io(const char *dir, uint8_t *dst, size_t n)
{
    uint8_t *all = malloc(0x20000);
    const int ok = all && n <= 0x20000 && s23_rom_read(dir, "asc3_io-c.ic14", all, 0x20000);
    if (ok) memcpy(dst, all, n);
    free(all);
    return ok;
}

/* THE CONTROLS (MAME INPUT_PORTS_START(downhill)): the HANDLEBAR on A/D channel 6 (JVS analog input 7; 0x0000-0xFFFF, centre 0x8000; the A/D
 * takes the top 10 bits), the PEDALS on rotary encoder 1 (the board counts the crank's turns), two BRAKE levers (left = JVS button 9,
 * p1 bit 9; right = button 8, p1 bit 10), START (button 10, p1 bit 8) and the operator's Select Up / Down / Enter (p1 bits 5, 4, 1). */
#include <SDL2/SDL.h>
#include "ss22_input.h"
extern uint16_t s23_inputs;
extern uint32_t rr_frame;
#define DH_IN_COIN      0x0001
#define DH_IN_PEDAL     0x0002
#define DH_IN_SERVICE   0x0004
#define DH_IN_TEST      0x0008
#define DH_IN_START     0x0010
#define DH_IN_BRAKE_L   0x0020
#define DH_IN_BRAKE_R   0x0040
#define DH_IN_SEL_UP    0x0080
#define DH_IN_SEL_DOWN  0x0100
#define DH_IN_ENTER     0x0200
#define DH_IN_LEFT      0x0400
#define DH_IN_RIGHT     0x0800

/* MAME namco_asca_1_device::in_r with Downhill Bikers' JVS_PLAYER1 */
static uint8_t dh_asca_in(int n, int test_on)
{
    const uint16_t in = s23_inputs;
    switch (n) {
    case 0: return (uint8_t)~((!!(in & DH_IN_ENTER)) << 3 | (!!(in & DH_IN_SEL_DOWN)) << 2 | (!!(in & DH_IN_SEL_UP)) << 1 | test_on);
    case 1: return (uint8_t)~((!!(in & DH_IN_START)) << 1 | (!!(in & DH_IN_BRAKE_L)) << 0);
    case 2: return (uint8_t)~((!!(in & DH_IN_COIN)) << 3 | (!!(in & DH_IN_BRAKE_R)) << 2);
    default: return (uint8_t)~0x03;                 /* both coin counters connected */
    }
}

/* the handlebar: a pad's left stick as it is (past a small dead zone); a key turns it steadily to that side and it springs back to the centre
 * at the same pace when let go. The pedals: a held key (or a pad button) pedals at a steady cadence; a pad's right trigger sets the cadence. */
static SDL_GameController *pad;
static int pad_x, pad_rt;                          /* the stick (-32768..32767) and the right trigger (0..32767) */
static uint16_t bar = 0x8000;
static int dh_pad_axis(SDL_GameControllerAxis ax)
{
    if (!pad || !SDL_GameControllerGetAttached(pad)) {
        pad = NULL;
        int s23_pad_choice(void); const int only = s23_pad_choice();
        if (only == -3) return 0;
        for (int j = 0, gi = -1; j < SDL_NumJoysticks(); j++) if (SDL_IsGameController(j) && ++gi == (only >= 0 ? only : 0)) { pad = SDL_GameControllerOpen(j); break; }
        if (!pad) return 0;
    }
    const int v = SDL_GameControllerGetAxis(pad, ax);
    if (ax == SDL_CONTROLLER_AXIS_TRIGGERRIGHT) return v > 2000 ? v : 0;
    const int dz = 2500;
    if (v > -dz && v < dz) return 0;
    return v > 0 ? (int)((long)(v - dz) * 32767 / (32767 - dz)) : -(int)((long)(-v - dz) * 32767 / (32768 - dz));
}
extern uint16_t s23_analog_in[8];
extern uint16_t s23_rotary_in[2];
static void dh_controls_frame(void)
{
    static int scripted = -1; if (scripted < 0) scripted = getenv("S23_ANALOG") != NULL;
    const uint16_t in = s23_inputs;
    if (!scripted) {
        const int l = (in & DH_IN_LEFT) != 0, r = (in & DH_IN_RIGHT) != 0;
        int target = l != r ? (l ? 0x0000 : 0xFFFF) : pad_x ? 0x8000 + (int)((long)pad_x * 0x7FFF / 32767) : 0x8000;
        if (target < 0) target = 0;
        if (target > 0xFFFF) target = 0xFFFF;
        if (pad_x && l == r) bar = (uint16_t)target;
        else { int v = bar; const int step = 0x0C00; v += target > v ? (target - v < step ? target - v : step) : -(v - target < step ? v - target : step); bar = (uint16_t)v; }
        s23_analog_in[6] = bar;
    }
    /* the crank: counts a frame (the encoder's running count, 8 bits on the board) */
    int cadence = (in & DH_IN_PEDAL) ? 6 : 0;
    if (pad_rt) cadence = 1 + pad_rt * 9 / 32767;
    static int env = -2; if (env == -2) { const char *e = getenv("DH_PEDAL"); env = e ? atoi(e) : -1; }   /* tests: a fixed cadence */
    if (env >= 0) cadence = env;
    s23_rotary_in[0] = (uint16_t)(s23_rotary_in[0] + cadence);
}
static uint16_t dh_analog(int ch)
{
    static uint32_t last = ~0u;
    if (rr_frame != last) { last = rr_frame; dh_controls_frame(); }   /* once a frame, so a replay repeats it */
    return (uint16_t)((s23_analog_in[ch & 7] & 0xFFC0) >> 6);
}
extern uint16_t (*s23_asca_analog)(int ch);
__attribute__((constructor)) static void dh_controls_init(void) { s23_asca_analog = dh_analog; }

static const ss22_action dh_actions[] = {
    { "Insert coin",      "coin",     SDL_SCANCODE_5,        SDL_SCANCODE_6,       DH_IN_COIN,     0, SS22_PAD(SDL_CONTROLLER_BUTTON_BACK) },
    { "Start",            "start",    SDL_SCANCODE_1,        SDL_SCANCODE_RETURN,  DH_IN_START,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_START) },
    { "Pedal",            "pedal",    SDL_SCANCODE_UP,       SDL_SCANCODE_W,       DH_IN_PEDAL,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_A) },
    { "Handlebar left",   "left",     SDL_SCANCODE_LEFT,     SDL_SCANCODE_A,       DH_IN_LEFT,     0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_LEFT) },
    { "Handlebar right",  "right",    SDL_SCANCODE_RIGHT,    SDL_SCANCODE_D,       DH_IN_RIGHT,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_RIGHT) },
    { "Left brake",       "brake_l",  SDL_SCANCODE_Z,        SDL_SCANCODE_DOWN,    DH_IN_BRAKE_L,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_LEFTSHOULDER) },
    { "Right brake",      "brake_r",  SDL_SCANCODE_X,        SDL_SCANCODE_DOWN,    DH_IN_BRAKE_R,  0, SS22_PAD(SDL_CONTROLLER_BUTTON_RIGHTSHOULDER) },
    { "Select up",        "sel_up",   SDL_SCANCODE_PAGEUP,   SDL_SCANCODE_UNKNOWN, DH_IN_SEL_UP,   0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_UP) },
    { "Select down",      "sel_down", SDL_SCANCODE_PAGEDOWN, SDL_SCANCODE_UNKNOWN, DH_IN_SEL_DOWN, 0, SS22_PAD(SDL_CONTROLLER_BUTTON_DPAD_DOWN) },
    { "Enter",            "enter",    SDL_SCANCODE_HOME,     SDL_SCANCODE_UNKNOWN, DH_IN_ENTER,    0, SS22_PAD(SDL_CONTROLLER_BUTTON_B) },
    { "Service",          "service",  SDL_SCANCODE_9,        SDL_SCANCODE_UNKNOWN, DH_IN_SERVICE,  0, 0 },
    { "Test",             "test",     SDL_SCANCODE_F2,       SDL_SCANCODE_UNKNOWN, DH_IN_TEST,     0, 0 },
};
static void dh_send_inputs(uint16_t pressed, unsigned wheel, unsigned p1, unsigned p2)
{
    (void)wheel; (void)p1; (void)p2;
    static int scripted = -1;
    if (scripted < 0) scripted = getenv("S23_PRESS") != NULL || getenv("S23_REPLAY") != NULL;
    pad_x = dh_pad_axis(SDL_CONTROLLER_AXIS_LEFTX);
    pad_rt = dh_pad_axis(SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
    if (pad && dh_pad_axis(SDL_CONTROLLER_AXIS_TRIGGERLEFT) > 16000) pressed |= DH_IN_BRAKE_L | DH_IN_BRAKE_R;   /* left trigger: both brakes */
    if (!scripted) s23_inputs = pressed;
}
static const ss22_input_game dh_input = {
    .actions = dh_actions, .n = (int)(sizeof dh_actions / sizeof *dh_actions),
    .wheel_min = 0x001, .wheel_max = 0x3FF, .wheel_key_span = 0x1FF, .wheel_step = 12,
    .notes = { "Enter, then press the new key (Esc cancels)",
               "Left / Right = handlebar, Up = pedal, Z / X = left / right brake (Down: both), 1 = START, 5 = coin",
               "Pads: left stick = handlebar, right trigger (or A) = pedal, LB / RB = brakes (left trigger: both), Start = START" },
    .send = dh_send_inputs, .test_bit = DH_IN_TEST, .service_bit = DH_IN_SERVICE,
    .light_gun = false,
};

/* THE HUD GLYPH UPLOAD'S TIME BUDGET. The HUD (speed, start / finish, the course-preview box's messages, COURSE PREVIEW) is the C361 text
 * layer: before an item is drawn its glyph set is copied into character RAM by 0x800030E4, a slice a frame, as many halfwords as
 * (500 - the beam register) * 25 -- the time left before line 250. It reads the beam (0x0682000A, line * 2) through 0x8000E69C, called
 * from 0x80003168 (return address 0x80003170), after the frame's work. On the board that work runs past vblank and the beam is low; here
 * the CPU finishes the attract frame inside vblank, the register reads 0x3C1..0x3E7, the budget is 0 and the copy never starts -- the
 * course preview had no text at all (the race, whose frame runs longer, got its slices). That one read sees line 0: the whole budget. */
static int dh_mem_read(uint32_t p, int size, uint32_t *v)
{
    if (p != 0x0682000Au || size != 2) return 0;
    extern uint8_t R[];
    const uint32_t ra = (uint32_t)R[0xFC] << 24 | (uint32_t)R[0xFD] << 16 | (uint32_t)R[0xFE] << 8 | R[0xFF];   /* $ra (Ghidra's 0xF8, BE) */
    if (ra != 0x80003170u) return 0;
    int s23_screen_read(uint32_t, int, uint32_t *);
    if (!s23_screen_read(p, size, v)) return 0;
    if (*v >= 0x1F4u) *v = 0;
    return 1;
}

/* the race HUD's section label (7200 "Sprint Section", 7201 "Downhill Section") sits on the text layer's dithered shadow strip above the
 * course bar: on the board (the attract video) the label is over that strip, not dimmed by it */
static int dh_over_text(int model) { return model == 7200 || model == 7201; }

const s23_game_t s23_game = {
    .title = "Downhill Bikers", .tag = "downhill", .window_title = "Downhill Bikers", .mame_set = "downhillu",
    .zip_help = "MAME's downhillu.zip (Downhill Bikers, US DH3 Ver. A) with its parent downhill.zip, and namco_asca3.zip",
    .zips = zips, .nzips = (int)(sizeof zips / sizeof zips[0]), .chips = chips, .nchips = (int)(sizeof chips / sizeof chips[0]),
    .io_asca3 = 1, .asca_in = dh_asca_in, .input = &dh_input, .mem_read = dh_mem_read, .over_text = dh_over_text,
    .irq_prompt = 1,
    .twin = 0, .evo2 = 0,
    .clock = 169344000u,                          /* BUSCLOCK * 5 */
    .px_num = 200, .px_den = 1323,                /* 25.6 MHz / 169.344 MHz */
    .sub_div = 10,                                /* 16.9344 MHz */
    .io_num = 64, .io_den = 735,                  /* 14.7456 MHz */
    .c352_div = 1920,                             /* 88200 Hz */
    /* MAME namcos23_state::machine_start: IRQ0 vblank, IRQ1 C361 + sub-CPU, IRQ2 C435, IRQ3 C422, IRQ5 RS-232 */
    .ip = { [S23_SRC_VBLANK] = S23_IP(0), [S23_SRC_C361] = S23_IP(1), [S23_SRC_SUB] = S23_IP(1), [S23_SRC_C435] = S23_IP(2),
            [S23_SRC_C422] = S23_IP(3), [S23_SRC_TIMER] = S23_IP(5) },
    .boot_size = 0x400000, .rom_boot = rom_boot, .rom_sub = rom_sub, .rom_io = rom_io, .io_rom_size = 0x4000, .c417_status = 0x008E, .depth_default = 0,
    .data_rom = { { "dh1mtah.2j", "dh1mtal.2h" }, { NULL, NULL } },
    .npt = 3, .pt_rom = { { "dh1pt0h.7a", "dh1pt0l.7c" }, { "dh1pt1h.5a", "dh1pt1l.5c" }, { "dh1pt2h.4a", "dh1pt2l.4c" } },
    .ccrl = "dh1ccrl.7f", .ccrh = "dh1ccrh.7e", .cg = { "dh1cgll.4m", "dh1cglm.4k", "dh1cgum.4j", "dh1cguu.5f" },
    .wave = { "dh1wavel.2c", "dh1waveh.2a" },
};
