/* Input: the board's buttons and PC-keyboard-over-serial, delivered in the
 * exact shape MGS's pad task expects.
 *
 * mts_pad.c registers two PAD_RECV_BUF buffers through PadInitDirect() and
 * every vsync parses them as raw SIO pad frames: byte 0 result (0 = ok),
 * byte 1 terminal type / size (0x41 = digital pad, one halfword), bytes 2-3
 * the button state, ACTIVE LOW. PadGetState() drives its discovery state
 * machine: PadStateFindCTP1 tells it "a normal controller is here".
 *
 * Bit order (button_hi << 8 | button_lo, from libgv.h's PAD_* constants):
 *   hi: 0x80 LEFT  0x40 DOWN  0x20 RIGHT  0x10 UP
 *       0x08 START 0x04 R3    0x02 L3     0x01 SELECT
 *   lo: 0x80 SQUARE 0x40 CROSS 0x20 CIRCLE 0x10 TRIANGLE
 *       0x08 R1     0x04 L1    0x02 R2     0x01 L2
 *
 * Same two input sources as the SOTN/OpenLara/Descent ports on this board:
 * the P1-header buttons (active low, pull-ups), and serial characters where
 * each received key presses its button for a few frames so the keyboard's
 * auto-repeat sustains holds.
 */

#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "freertos/FreeRTOS.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "driver/uart_vfs.h"
#include "board_pins.h"

#define PadStateFindCTP1 2

static unsigned char* sPadBuf[2];
static unsigned sButtonUnwired;
/* set once the serial drivers exist; read from the vblank task */
static volatile int sSerialReady;

#ifdef MGS_BOARD_XIAO
#include "esp_input_xiao.inc.c"
#else

/* {gpio, bit in the 16-bit pad word} */
static const struct {
    int gpio;
    unsigned short bit;
} sButtons[] = {
    {PIN_BTN_UP, 0x1000},    {PIN_BTN_DOWN, 0x4000},
    {PIN_BTN_LEFT, 0x8000},  {PIN_BTN_RIGHT, 0x2000},
    /* the A pin reads low at boot on this board (unwired) and gets masked
     * out, which would leave the game without its primary action button --
     * so B carries CIRCLE (confirm / action) and CROSS moves to L */
    {PIN_BTN_A, 0x0020},     /* CIRCLE (masked out if unwired) */
    {PIN_BTN_B, 0x0020},     /* CIRCLE: confirm/action */
    {PIN_BTN_L, 0x0004},     /* L1 */
    {PIN_BTN_R, 0x0008},     /* R1 */
    {PIN_BTN_START, 0x0800}, {PIN_BTN_SELECT, 0x0100},
};
#endif /* MGS_BOARD_XIAO */

/* How long one keystroke holds its button, counted in GAME FRAMES.
 *
 * Counting vblanks cannot work, and both ways of getting it wrong were seen on
 * the board. The frame loop no longer runs at a fixed rate: an open room takes
 * four or five vblanks per frame where a corridor takes two. Hold for many
 * vblanks and one tap walks Snake through several frames, so he ends up past
 * where the player aimed; hold for few and the press can be raised before the
 * game ever reads the pad -- it reads once per frame, and at 12 fps that is
 * every 80 ms against a 48 ms hold. Presses vanished at random, which is what
 * made the movement erratic.
 *
 * So the countdown is driven by the game itself: Mgs_PadConsumed() is called
 * from the frame loop right after GV_UpdatePadSystem, so a hold of 2 means two
 * frames the game actually saw, whatever the frame rate is doing. */
#define SERIAL_HOLD_FRAMES 2
static unsigned char sHold[16];

/* Absolute button state, the way a real pad reports it.
 *
 * Taps cannot express walking. A keyboard sends a character on press and then
 * nothing until auto-repeat kicks in half a second later, and it never says
 * anything on release -- so holding W gave one step, a long pause, and only
 * then a walk. That IS the erratic movement, and no amount of tuning the hold
 * length fixes it, because the information is simply not in the stream.
 *
 * So play.py polls the real key state and sends the whole 16-bit mask whenever
 * it changes, framed as FF hi lo. FF cannot collide with the tap keys, which
 * are all printable ASCII and still work for anyone on a plain terminal.
 *
 * The watchdog matters: with absolute state, a sender that dies mid-walk would
 * leave a direction held forever. Any gap longer than a second clears it. */
#define PAD_PACKET_LEAD  0xFFu
#define PAD_STATE_TIMEOUT_TICKS 60   /* vblank ticks ~= 1 s */

static unsigned sState;
static unsigned sStateAge = PAD_STATE_TIMEOUT_TICKS;
static unsigned char sPktBuf[2];
static unsigned char sPktLen;   /* 0 = idle, 1 = seen FF, 2 = seen FF hi */

static int keyToPadBit(char c) {
    switch (c) {
    case 'w': return 12; /* UP     0x1000 */
    case 's': return 14; /* DOWN   0x4000 */
    case 'a': return 15; /* LEFT   0x8000 */
    case 'd': return 13; /* RIGHT  0x2000 */
    case 'e': return 5;  /* CIRCLE 0x0020: confirm */
    case 'x': return 6;  /* CROSS  0x0040 */
    case 'q': return 4;  /* TRIANGLE 0x0010 */
    case 'z': return 7;  /* SQUARE 0x0080 */
    case '1': return 2;  /* L1 */
    case '2': return 3;  /* R1 */
    case '3': return 0;  /* L2 */
    case '4': return 1;  /* R2 */
    case '\r':
    case '\n': return 11; /* START 0x0800 */
    case ' ': return 8;   /* SELECT 0x0100 */
    }
    return -1;
}

void PadInitDirect(unsigned char* pad1, unsigned char* pad2) {
    sPadBuf[0] = pad1;
    sPadBuf[1] = pad2;

#ifdef MGS_BOARD_XIAO
    xiaoInputInit();
#else
    uint64_t mask = 0;
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        mask |= 1ULL << sButtons[i].gpio;
    }
    gpio_config_t cfg = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cfg);

    /* a pin already LOW at boot has nothing wired to it and would read as a
     * button jammed down forever -- mask it out (SOTN hit exactly this) */
    for (unsigned i = 0; i < sizeof(sButtons) / sizeof(sButtons[0]); i++) {
        if (!gpio_get_level(sButtons[i].gpio)) {
            sButtonUnwired |= 1u << i;
        }
    }
#endif

    usb_serial_jtag_driver_config_t usb_cfg = {
        .tx_buffer_size = 1024,
        .rx_buffer_size = 256,
    };
    usb_serial_jtag_driver_install(&usb_cfg);
    if (!uart_is_driver_installed(UART_NUM_0)) {
        uart_driver_install(UART_NUM_0, 256, 0, 0, NULL, 0);
        /* deliberately NOT routing the console VFS through the driver: the
         * console keeps stdout, we only siphon raw RX bytes */
    }
    /* Last, and only once both drivers exist. The vblank tick is already
     * running by now and calls Mgs_PadsUpdate from its own task, so without
     * this the first tick can reach usb_serial_jtag_read_bytes before the
     * driver has its ring buffer and dereference a null one: LoadProhibited at
     * boot, roughly one start in three. */
    sSerialReady = 1;

    printf("[pad] buttons up (unwired mask %04x), serial keys live\n",
           sButtonUnwired);
}

int PadGetState(int port) {
    /* "a plain controller is present" -- both ports, every poll; mts's
     * discovery machine settles into IDENTIFIED and parses the buffers */
    (void)port;
    return PadStateFindCTP1;
}

void PadSetAct(int port, unsigned char* data, int len) {
    (void)port; (void)data; (void)len; /* no rumble hardware */
}

int PadSetActAlign(int port, char* data) {
    (void)port; (void)data;
    return 0;
}

void PadStartCom(void) {}

/* called by the vblank tick (esp32_vblank.c) right before the mts callback,
 * so the frame the game parses this field is already this field's state */
void Mgs_PadsUpdate(void) {
    unsigned pressed = 0;
    unsigned char rx[16];
    int n, i;

    if (!sPadBuf[0]) {
        return;
    }

    /* zero-timeout raw reads ONLY. This runs inside the vblank tick, and a
     * read that can block -- read(0) through the console VFS was one --
     * freezes the tick, which freezes every mts task with it: the game
     * "wedged" the moment input was added, and it was this. */
    n = 0;
    if (sSerialReady) {
        n = usb_serial_jtag_read_bytes(rx, sizeof(rx), 0);
        if (n <= 0) {
            n = uart_read_bytes(UART_NUM_0, rx, sizeof(rx), 0);
        }
    }
    for (i = 0; i < n; i++) {
        unsigned char c = rx[i];

        if (sPktLen == 0) {
            if (c == PAD_PACKET_LEAD) {
                sPktLen = 1;
            } else {
                int bit = keyToPadBit((char)c);
                if (bit >= 0) {
                    sHold[bit] = SERIAL_HOLD_FRAMES;
                }
            }
            continue;
        }
        sPktBuf[sPktLen - 1] = c;
        if (++sPktLen == 3) {
            sState = ((unsigned)sPktBuf[0] << 8) | sPktBuf[1];
            sStateAge = 0;
            sPktLen = 0;
        }
    }

    /* Held-down state from the state packets, plus whatever a tap still owes.
     * Mgs_PadConsumed ages the taps, once per frame the game reads, so a press
     * cannot expire between two of its reads. */
    if (sStateAge < PAD_STATE_TIMEOUT_TICKS) {
        sStateAge++;
        pressed |= sState;
    } else {
        sState = 0;
    }
    for (i = 0; i < 16; i++) {
        if (sHold[i]) {
            pressed |= 1u << i;
        }
    }
#ifdef MGS_BOARD_XIAO
    pressed |= xiaoReadPad();
#else
    for (unsigned b = 0; b < sizeof(sButtons) / sizeof(sButtons[0]); b++) {
        if ((sButtonUnwired & (1u << b)) == 0 &&
            !gpio_get_level(sButtons[b].gpio)) {
            pressed |= sButtons[b].bit;
        }
    }
#endif

    {
        static unsigned last;
        static int budget = 40;
        if (pressed != last && budget > 0) {
            budget--;
            printf("[pad] pressed %04x\n", pressed);
            last = pressed;
        }
    }

    /* port 0: digital pad frame, active low */
    sPadBuf[0][0] = 0x00;
    sPadBuf[0][1] = 0x41;
    sPadBuf[0][2] = (unsigned char)(~(pressed >> 8) & 0xFF);
    sPadBuf[0][3] = (unsigned char)(~pressed & 0xFF);

    /* port 1: nothing connected */
    if (sPadBuf[1]) {
        sPadBuf[1][0] = 0xFF;
    }
}

/* Called once per frame from the game's own loop (libdg/dgd.c, right after
 * GV_UpdatePadSystem), which is the only place that knows a frame has really
 * consumed the pad state. See SERIAL_HOLD_FRAMES for why the vblank tick is
 * the wrong clock for this. */
void Mgs_PadConsumed(void) {
    int i;
    for (i = 0; i < 16; i++) {
        if (sHold[i]) {
            sHold[i]--;
        }
    }
}
