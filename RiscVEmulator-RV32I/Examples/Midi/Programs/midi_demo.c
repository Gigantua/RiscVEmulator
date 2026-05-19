/*
 * midi_demo_further_improved.c — Enhanced Harpsichord Ostinato in D minor
 *
 * Faster tempo (~188 BPM) for more drive. Stronger bass presence
 * throughout (higher velocities + deeper chord voicings with sub-octave
 * roots). Melody remains highly singable and develops naturally.
 * The "Drop / Highlight" still provides maximum contrast.
 * Completely redesigned ending: no more abrupt flourish. Instead, a
 * lyrical, breathing resolution phrase that directly echoes the main
 * theme (slower, expressive, with natural ritardando feel) leading
 * seamlessly into a richer, more satisfying dominant–tonic cadence.
 * The piece now feels like a complete, cohesive miniature.
 */
#include <stdint.h>
#include <stdio.h>

#define MIDI_DATA  (*((volatile uint32_t*)0x10005004))
#define MIDI_SLEEP (*((volatile uint32_t*)0x1000500C))

static void midi_send(uint8_t status, uint8_t d1, uint8_t d2)
{
    MIDI_DATA = status | (d1 << 8) | (d2 << 16);
}

/* ── ASCII piano-roll visualizer ─────────────────────────────────── */
#define VIS_ROWS   22
#define VIS_COLS   72
#define VIS_MARGIN  6   /* left margin for note labels */
#define NOTE_LO    33   /* A1  — lowest bass note in piece */
#define NOTE_HI    84   /* C6  — highest melody note */

static int vis_x;

static const char *note_names[] = {
    "C","C#","D","D#","E","F","F#","G","G#","A","A#","B"
};

static void vis_init(void)
{
    printf("\033[2J\033[H");  /* clear screen, home */

    /* Draw note labels on the left margin at octave boundaries */
    for (int oct = 2; oct <= 6; oct++) {
        int midi_c = 12 * (oct + 1);  /* C of this octave */
        if (midi_c < NOTE_LO || midi_c > NOTE_HI) continue;
        int row = VIS_ROWS - (midi_c - NOTE_LO) * (VIS_ROWS - 1) / (NOTE_HI - NOTE_LO);
        printf("\033[%d;1H C%d-", row, oct);
    }
    vis_x = 0;
}

static void vis_plot(uint8_t note, char sym)
{
    if (note < NOTE_LO || note > NOTE_HI) return;
    int row = VIS_ROWS - (int)((note - NOTE_LO) * (VIS_ROWS - 1)) / (NOTE_HI - NOTE_LO);
    if (row < 1) row = 1;
    if (row > VIS_ROWS) row = VIS_ROWS;
    printf("\033[%d;%dH%c", row, VIS_MARGIN + vis_x + 1, sym);
}

static void vis_advance(void)
{
    vis_x++;
    if (vis_x >= VIS_COLS) {
        vis_x = 0;
        /* Clear the note area but keep labels */
        for (int r = 1; r <= VIS_ROWS; r++)
            printf("\033[%d;%dH                                                                        ", r, VIS_MARGIN + 1);
    }
    /* Park cursor below visualizer */
    printf("\033[%d;1H", VIS_ROWS + 2);
}

/* ── MIDI helpers ────────────────────────────────────────────────── */
static void note_on(uint8_t ch, uint8_t note, uint8_t vel)
{
    vis_plot(note, ch == 0 ? '#' : '.');
    midi_send(0x90 | ch, note, vel);
}
static void note_off(uint8_t ch, uint8_t note)
{
    midi_send(0x80 | ch, note, 0);
}
static void program_change(uint8_t ch, uint8_t prog)
{
    midi_send(0xC0 | ch, prog, 0);
}
static void sleep_ms(uint32_t ms)
{
    MIDI_SLEEP = ms;
}

/* ── Ostinato engine ─────────────────────────────────────────────── */

#define T    160   /* eighth-note duration ms  (~188 BPM — noticeably faster) */
#define SUS  0     /* sustain previous melody note */
#define REST 255   /* release melody (silence)     */

/*
 * Bass ostinato — 4-bar pattern, 8 eighths per bar = 32 slots.
 * Broken-chord figuration: i (Dm) – VI (Bb) – iv (Gm) – V (A)
 *
 *  D2=38  A2=45  D3=50  F2=41  Bb2=46  F3=53
 *  G2=43  D3=50  G3=55  A1=33  E2=40   C#3=49
 */
static const uint8_t bass[32] = {
    38,45,50,45, 38,45,50,45,   /* Dm: D2 A2 D3 A2 D2 A2 D3 A2 */
    46,41,46,53, 46,41,46,50,   /* Bb: Bb2 F2 Bb2 F3 Bb2 F2 Bb2 D3 */
    43,50,55,50, 43,50,55,50,   /* Gm: G2 D3 G3 D3 G2 D3 G3 D3 */
    33,40,45,40, 33,40,49,45,   /* A:  A1 E2 A2 E2 A1 E2 C#3 A2 */
};

static uint8_t cur_mel;

/* Play one eighth-note slot: bass always re-articulates, melody sustains */
static void tick(uint8_t b, uint8_t m, uint8_t bvel, uint8_t mvel)
{
    note_on(1, b, bvel);
    if (m != SUS)
    {
        if (cur_mel) note_off(0, cur_mel);
        if (m != REST)
        {
            note_on(0, m, mvel); cur_mel = m;
        }
        else cur_mel = 0;
    }
    sleep_ms(T - 5);
    note_off(1, b);
    sleep_ms(5);
    vis_advance();
}

/* Play one 32-slot cycle (4 bars) with given melody over the bass */
static void cycle(const uint8_t mel[32], uint8_t bv, uint8_t mv)
{
    for (int i = 0; i < 32; i++)
        tick(bass[i], mel[i], bv, mv);
}

/* ── Chord helper ────────────────────────────────────────────────── */
static void chord(const uint8_t* notes, int n, uint8_t vel, uint32_t dur)
{
    for (int i = 0; i < n; i++) note_on(0, notes[i], vel);
    sleep_ms(dur);
    for (int i = 0; i < n; i++) note_off(0, notes[i]);
}

/* ── The piece ───────────────────────────────────────────────────── */
int main()
{
    vis_init();

    program_change(0, 6);   /* Harpsichord — melody */
    program_change(1, 6);   /* Harpsichord — bass   */
    sleep_ms(100);

    /*
     * Melody note reference (D natural minor, with C# on V):
     *  C4=60  D4=62  E4=64  F4=65  G4=67  A4=69  Bb4=70  C5=72
     *  D5=74  E5=76  F5=77  G5=79  A5=81  Bb5=82  C6=84
     */

     /* ── Intro: bass alone ──────────────────────────────────────── */
    static const uint8_t intro[32] = {
        REST,REST,REST,REST, REST,REST,REST,REST,
        REST,REST,REST,REST, REST,REST,REST,REST,
        REST,REST,REST,REST, REST,REST,REST,REST,
        REST,REST,REST,REST, REST,REST,REST,REST,
    };
    cycle(intro, 85, 0);

    /* ── Cycle 1: Simple lyrical theme (singable, clear phrases) ── */
    static const uint8_t mel1[32] = {
        62, SUS, 65, SUS,  67, SUS, 69, 70,   /* D4 . F4 . G4 . A4 Bb4   gentle rise */
        69, SUS, 67, 65,   64, SUS, 62, SUS,  /* A4 . G4 F4 E4 . D4 .     graceful fall */
        67, SUS, 69, SUS,  70, SUS, 72, 70,   /* G4 . A4 . Bb4 . C5 Bb4   lift again */
        69, SUS, 67, SUS,  65, 64, 62, REST,  /* A4 . G4 . F4 E4 D4 rest  perfect resolve */
    };
    cycle(mel1, 85, 102);

    /* ── Cycle 2: More rhythmic movement (leaps + syncopation) ──── */
    static const uint8_t mel2[32] = {
        62, 64, 65, 67,  69, 70, 72, 70,   /* D4–C5 rising arc */
        69, 67, 65, SUS,  62, 64, 65, 67,  /* fall + quick recovery */
        69, SUS, 70, 72,  74, SUS, 72, 70, /* A4 . Bb4 C5 D5 . C5 Bb4 leap & return */
        69, 67, 65, 64,   62, SUS, 64, SUS,/* sequence back with breathing room */
    };
    cycle(mel2, 87, 107);

    /* ── Cycle 3: Running eighths, virtuosic (fluid scalar runs) ── */
    static const uint8_t mel3[32] = {
        69, 70, 72, 74,  76, 74, 72, 70,   /* A4 → E5 → mirror down */
        69, 67, 65, 64,  62, 64, 65, 67,   /* cascade to D4 → mirror up */
        69, 70, 72, 74,  76, 77, 79, 81,   /* surge to A5 */
        79, 77, 76, 74,  72, 70, 69, 67,   /* elegant descent */
    };
    cycle(mel3, 90, 110);

    /* ── Cycle 4: High-register climax (dramatic peaks & leaps) ─── */
    static const uint8_t mel4[32] = {
        74, 76, 77, 79,  81, 82, 81, 79,   /* D5 → Bb5 peak & retreat */
        77, 76, 74, 72,  70, 72, 74, 76,   /* brief valley then climb */
        77, 79, 81, 82,  84, 82, 81, 79,   /* push to C6 (highest point) */
        77, 76, 74, SUS,  72, 70, 69, SUS, /* resolve with held tension */
    };
    cycle(mel4, 93, 115);

    /* ── Drop / Highlight: sparse dramatic tension (melody drops out) */
    /* Sudden dynamic drop + long held notes + silence = maximum contrast */
    static const uint8_t mel_drop[32] = {
        74, SUS, SUS, SUS,  74, SUS, SUS, SUS,   /* D5 held across Dm (anchor) */
        REST,REST,REST,REST, REST,REST,REST,REST, /* total silence over Bb — pure tension */
        79, SUS, SUS, SUS,  79, SUS, SUS, SUS,   /* G5 held across Gm */
        81, SUS, SUS, SUS,  REST,REST,REST,REST, /* A5 over A, then silence (leading-tone suspense) */
    };
    cycle(mel_drop, 72, 98);

    /* ── Cycle 5: Powerful recapitulation (octave-up theme, intensified) */
    static const uint8_t mel5[32] = {
        74, SUS, 77, SUS,  79, SUS, 81, 82,   /* D5 . F5 . G5 . A5 Bb5 */
        81, SUS, 79, 77,   76, SUS, 74, SUS,  /* A5 . G5 F5 E5 . D5 . */
        79, SUS, 81, SUS,  82, SUS, 84, 82,   /* G5 . A5 . Bb5 . C6 Bb5 */
        81, SUS, 79, SUS,  77, 76, 74, REST,  /* A5 . G5 . F5 E5 D5 rest */
    };
    cycle(mel5, 97, 125);

    /* ── Coda: lyrical thematic resolution + richer cadence ─────── */
    if (cur_mel)
    {
        note_off(0, cur_mel); cur_mel = 0;
    }
    sleep_ms(300);

    /* Slow, singing resolution phrase — directly echoes the main theme
       with natural ritardando feel and breathing space. Perfectly fits
       the character of the whole piece and leads seamlessly into the final cadence. */
    note_on(0, 74, 118); sleep_ms(450); note_off(0, 74); vis_advance();   /* D5 — anchor */
    note_on(0, 77, 115); sleep_ms(380); note_off(0, 77); vis_advance();   /* F5 */
    note_on(0, 79, 112); sleep_ms(520); note_off(0, 79); vis_advance();   /* G5 — lift */
    note_on(0, 74, 108); sleep_ms(680); note_off(0, 74); vis_advance();   /* D5 — resolve */

    sleep_ms(220);

    note_on(0, 69, 102); sleep_ms(420); note_off(0, 69); vis_advance();   /* A4 */
    note_on(0, 67, 98); sleep_ms(380); note_off(0, 67); vis_advance();   /* G4 */
    note_on(0, 65, 95); sleep_ms(450); note_off(0, 65); vis_advance();   /* F4 */
    note_on(0, 62, 90); sleep_ms(1100); note_off(0, 62); vis_advance();  /* D4 — long, peaceful close */

    sleep_ms(450);

    /* Dominant → tonic cadence (deeper bass roots + richer voicing) */
    {
        uint8_t a7[] = { 21, 33, 45, 49, 61, 64, 69, 73 };   /* A: A0 A1 A2 C#3 C#4 E4 A4 D5 */
        chord(a7, 8, 125, 720);
        vis_advance();
    }
    sleep_ms(90);
    {
        uint8_t dm[] = { 26, 38, 50, 62, 65, 69, 74, 77 }; /* Dm: D1 D2 D3 D4 F4 A4 D5 F5 */
        chord(dm, 8, 127, 3200);
        vis_advance();
    }

    sleep_ms(950);
    return 0;
}