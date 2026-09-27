/* Controls for the XIAO ESP32S3 Sense handheld.
 *
 * Eleven pins, and the controls get four of them: two buttons with a pin each,
 * six more sharing one ADC pin through a resistor ladder, and an analog stick
 * on two more. Included by esp_input.c when MGS_BOARD_XIAO is set; the
 * Waveshare board's ten direct GPIOs live there instead.
 *
 * Every constant below was measured on the assembled unit with the shakedown
 * firmware in port/consola/test/, and the two that look arbitrary -- the dead
 * zone and the averaging count -- are the ones that took the longest to get
 * right. See board_pins_xiao.h for the numbers and why.
 */

#include "esp_adc/adc_oneshot.h"
#include "freertos/task.h"

static adc_oneshot_unit_handle_t sAdc;

/* Stick calibration, learned at boot and refined as the stick is used.
 *
 * A drone gimbal is not a console stick: it rests wherever its springs leave
 * it (2317 on this unit, not the 2048 a mid-scale assumption would give) and
 * its throw covers only part of the range (X ran 141..2888). Mapping the raw
 * reading straight to a direction therefore leaves the player drifting at rest
 * and unable to reach full speed. Centre is whatever the stick reads when the
 * board starts, and each half is scaled by the travel actually seen in that
 * direction -- so it self-corrects as the player moves. */
static struct {
    int cx, cy;           /* rest position, sampled once at boot */
    int lo_x, hi_x;       /* travel seen so far, per axis          */
    int lo_y, hi_y;
    int ready;
} sCal;

/* The ADC is sampled on its own task, never from the vblank tick.
 *
 * This is the trap esp_input.c already warns about a few lines further down,
 * and it caught this file too: adc_oneshot_read takes a driver mutex and waits
 * for the conversion, and Mgs_PadsUpdate runs inside the vblank tick. Twenty
 * four conversions per tick froze the tick, and a frozen tick freezes every
 * mts task with it -- the game booted, mounted the card, found all 96 stages,
 * and then sat there with `[tick] all 6000 fired 1 active 0` and mts time
 * stopped at 124. Nothing crashed; it simply stopped.
 *
 * So a plain task does the sampling at its own pace and publishes three
 * integers. The tick only reads them, which cannot block. */
static volatile int sRawX, sRawY, sRawLadder;
/* raw x, raw y, centre x, centre y, deflection x, deflection y,
 * ladder, resulting pad word -- printed by the vblank tick */
int mgs_stick_dbg[8];

/* Counted every loop so the tick can show whether this task is alive at all.
 * Without it, "every reading is zero" has two very different causes that look
 * identical: a task that never started, and a task running but reading zero. */
int mgs_adc_loops;

static void xiaoAdcTask(void* arg) {
    (void)arg;
    for (;;) {
        mgs_adc_loops++;
        int sx = 0, sy = 0, sl = 0;
        for (int i = 0; i < STICK_SAMPLES; i++) {
            int a = 0, b = 0, c = 0;
            adc_oneshot_read(sAdc, PIN_STICK_X - 1, &a);
            adc_oneshot_read(sAdc, PIN_STICK_Y - 1, &b);
            adc_oneshot_read(sAdc, PIN_BTN_LADDER - 1, &c);
            sx += a; sy += b; sl += c;
        }
        sRawX = sx / STICK_SAMPLES;
        sRawY = sy / STICK_SAMPLES;
        sRawLadder = sl / STICK_SAMPLES;
        /* 10 ms: faster than the game reads the pad at any frame rate it
         * manages here, and slow enough to cost nothing. */
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

static void xiaoInputInit(void) {
    adc_oneshot_unit_init_cfg_t ucfg = { .unit_id = ADC_UNIT_1 };
    ESP_ERROR_CHECK(adc_oneshot_new_unit(&ucfg, &sAdc));
    adc_oneshot_chan_cfg_t ccfg = { .atten = ADC_ATTEN_DB_12,
                                    .bitwidth = ADC_BITWIDTH_12 };
    /* ADC1 channel N is GPIO N+1 on the S3, which is why these pins had to be
     * D0/D1/D2: D6 and D7 have no ADC at all, and putting the ladder there was
     * an early mistake that cost a rewire. */
    adc_oneshot_config_channel(sAdc, PIN_STICK_X - 1, &ccfg);
    adc_oneshot_config_channel(sAdc, PIN_STICK_Y - 1, &ccfg);
    adc_oneshot_config_channel(sAdc, PIN_BTN_LADDER - 1, &ccfg);

    gpio_config_t io = {
        .pin_bit_mask = (1ULL << PIN_BTN_A) | (1ULL << PIN_BTN_B),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);

    /* Start the sampler. Pinned to core 1: the game and the vblank tick both
     * live on core 0 and this has no business competing with them.
     *
     * 4 KB of stack, and the result is CHECKED. Both matter. adc_oneshot_read
     * goes through the driver's locking and a short stack fails creation
     * outright -- and a silent failure here surfaces much later as every
     * control reading zero, which is indistinguishable from a wiring fault.
     * This very line went missing once and cost a round trip diagnosing
     * "the stick does not work" when the sampler simply did not exist. */
    BaseType_t ok = xTaskCreatePinnedToCore(xiaoAdcTask, "xiao_adc", 4096,
                                            NULL, 4, NULL, 1);
    printf("[pad] XIAO: ladder on D2, A=D5 B=D7, stick D0/D1 -- sampler %s\n",
           ok == pdPASS ? "active" : "FAILED TO START");
}

/* One axis, raw counts -> -256..+256, dead zone applied.
 *
 * Subtracting the dead zone from both sides of the fraction rather than just
 * clamping matters to how the stick feels: it makes motion start from zero as
 * the player leaves the centre instead of jumping to a step. */
static int xiaoAxis(int raw, int centre, int lo, int hi, int invert) {
    int d = raw - centre;
    int span = (d >= 0) ? (hi - centre) : (centre - lo);
    if (span < 500) span = 500;          /* before the stick has been swept */
    int a = d < 0 ? -d : d;
    if (a <= STICK_DEADZONE) return 0;
    int v = ((a - STICK_DEADZONE) * 256) / (span - STICK_DEADZONE);
    if (v > 256) v = 256;
    if (d < 0) v = -v;
    return invert ? -v : v;
}

/* Build the PSX pad word from this board's controls. */
static unsigned xiaoReadPad(void) {
    unsigned pressed = 0;

    /* --- six buttons on one ADC pin ---------------------------------- */
    int lad = sRawLadder;
    if      (lad <  LADDER_CIRCLE_MAX)   pressed |= 0x0020;  /* circle   */
    else if (lad <  LADDER_CROSS_MAX)    pressed |= 0x0040;  /* cross    */
    else if (lad <  LADDER_TRIANGLE_MAX) pressed |= 0x0010;  /* triangle */
    else if (lad <  LADDER_L1_MAX)       pressed |= 0x0004;  /* L1       */
    else if (lad <  LADDER_START_MAX)    pressed |= 0x0800;  /* START    */
    else if (lad <  LADDER_SELECT_MAX)   pressed |= 0x0100;  /* SELECT   */

    /* --- the two with a pin to themselves ----------------------------
     * These are square and R1 on purpose: a ladder can only report one press
     * at a time, so the pair a player holds while doing something else -- fire
     * and aim -- must not share it. */
    if (!gpio_get_level(PIN_BTN_A)) pressed |= 0x0080;       /* square   */
    if (!gpio_get_level(PIN_BTN_B)) pressed |= 0x0008;       /* R1       */

    /* --- stick -> the d-pad the game reads ---------------------------- */
    int rx = sRawX;
    int ry = sRawY;
    if (!rx && !ry) return pressed;   /* the sampler has not run yet */
    if (!sCal.ready) {
        sCal.cx = sCal.lo_x = sCal.hi_x = rx;
        sCal.cy = sCal.lo_y = sCal.hi_y = ry;
        sCal.ready = 1;
        printf("[pad] stick center: X %d  Y %d\n", rx, ry);
    }
    if (rx < sCal.lo_x) sCal.lo_x = rx;
    if (rx > sCal.hi_x) sCal.hi_x = rx;
    if (ry < sCal.lo_y) sCal.lo_y = ry;
    if (ry > sCal.hi_y) sCal.hi_y = ry;

    int ax = xiaoAxis(rx, sCal.cx, sCal.lo_x, sCal.hi_x, STICK_INVERT_X);
    int ay = xiaoAxis(ry, sCal.cy, sCal.lo_y, sCal.hi_y, STICK_INVERT_Y);

    /* The game reads a digital pad, so the analog reading becomes direction
     * bits. Half deflection is the threshold: low enough to turn without
     * fighting the stick, high enough that a diagonal needs real intent.
     * MGS's walk/run distinction comes from the pad's own timing, not from
     * how far the stick is pushed, so nothing is lost by quantising here. */
    if (ax >  128) pressed |= 0x2000;   /* right */
    if (ax < -128) pressed |= 0x8000;   /* left  */
    if (ay >  128) pressed |= 0x1000;   /* up    */
    if (ay < -128) pressed |= 0x4000;   /* down  */

    /* Published for the vblank tick to print. The game's own printfs are
     * dropped once the console saturates, so a plain printf here would be
     * invisible exactly when the stick is misbehaving; the tick's line is the
     * one that still gets out. Raw counts, the calibrated centre and the
     * resulting deflection together say which stage is wrong: a raw value that
     * never moves is wiring, a centre far from the raw is a bad calibration,
     * and a deflection that never passes the threshold is scaling. */
    mgs_stick_dbg[0] = rx;
    mgs_stick_dbg[1] = ry;
    mgs_stick_dbg[2] = sCal.cx;
    mgs_stick_dbg[3] = sCal.cy;
    /* Deflection is reported as the LARGEST seen since the last report, not
     * the value at the instant of printing. The report rides on a line that
     * appears every few seconds, and a stick is pushed and released in well
     * under that -- so sampling instantaneously showed a stick at rest every
     * single time and looked like a dead axis. The reader clears these. */
    if (ax > mgs_stick_dbg[4] || -ax > mgs_stick_dbg[4])
        mgs_stick_dbg[4] = ax < 0 ? -ax : ax;
    if (ay > mgs_stick_dbg[5] || -ay > mgs_stick_dbg[5])
        mgs_stick_dbg[5] = ay < 0 ? -ay : ay;
    mgs_stick_dbg[6] = lad;
    mgs_stick_dbg[7] |= (int)pressed;   /* any bit seen in the window */

    return pressed;
}
