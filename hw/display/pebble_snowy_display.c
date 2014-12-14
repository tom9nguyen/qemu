/*-
 * Copyright (c) 2014
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

/*
 * 8-bit color display connected through SPI bus. The 8 bits are organized as (starting from MSB):
 *  2 bits red, 2 bits green, 2 bits blue, 2 bits of 0.
 *
 * This display is used in the Pebble Snowy platform and actually represents an FPGA connected
 * to a LPM012A220A display. The FPGA implements the SPI interface.
 * 
 * Some example colors:
 *    black: 0x00
 *    white: 0xFC
 *    red:   0xC0
 *    green: 0x30
 *    blue:  0x0C
 *
 * This display expects columns to be sent through the SPI bus, from bottom to top. So, when we 
 *  get a line of data from the SPI bus, the first byte is the column index and the remaining bytes
 *  are the bytes in the column, starting from the bottom.
 *
 * This display expects 206 bytes to be sent per line (column). Organized as follows:
 *  uint8_t column_index
 *  uint8_t padding[16]    // SNOWY_ROWS_SKIPPED_AT_BOTTOM
 *  uint8_t column_data[172]
 *  uint8_t padding[17]    // SNOWY_ROWS_SKIPPED_AT_TOP
 */

/*
 * TODO:
 * Add part number attribute and set ROWS/COLS appropriately.
 * Add attribute for 'off' bit colour for simulating backlight.
 * Add display rotation attribute.
 * Handle 24bpp host displays.
 */

#include "qemu-common.h"
#include "ui/console.h"
#include "ui/pixel_ops.h"
#include "hw/ssi.h"

//#define DEBUG_PEBBLE_SNOWY_DISPLAY
#ifdef DEBUG_PEBBLE_SNOWY_DISPLAY
// NOTE: The usleep() helps the MacOS stdout from freezing when we have a lot of print out
#define DPRINTF(fmt, ...)                                       \
    do { printf("PEBBLE_SNOWY_DISPLAY: " fmt , ## __VA_ARGS__); \
         usleep(1000); \
    } while (0)
#else
#define DPRINTF(fmt, ...)
#endif


#define SNOWY_NUM_ROWS        172
#define SNOWY_NUM_COLS        148
#define SNOWY_BYTES_PER_ROW   SNOWY_NUM_COLS

#define SNOWY_ROWS_SKIPPED_AT_TOP     17
#define SNOWY_ROWS_SKIPPED_AT_BOTTOM  16
#define SNOWY_LINE_DATA_LEN   (SNOWY_ROWS_SKIPPED_AT_TOP + SNOWY_NUM_ROWS \
                               + SNOWY_ROWS_SKIPPED_AT_BOTTOM)

#define SNOWY_COLOR_BLACK     0
#define SNOWY_COLOR_WHITE     0xFC
#define SNOWY_COLOR_RED       0xC0
#define SNOWY_COLOR_GREEN     0x30
#define SNOWY_COLOR_BLUE      0x0C


/* Various states the Display can be in */
typedef enum {
    PSDISPLAYSTATE_PROGRAMMING,
    PSDISPLAYSTATE_ACCEPTING_CMD,
    PSDISPLAYSTATE_ACCEPTING_PARAM,
    PSDISPLAYSTATE_ACCEPTING_SCENE_BYTE,

    PSDISPLAYSTATE_ACCEPTING_LINENO,
    PSDISPLAYSTATE_ACCEPTING_DATA,

} PSDisplayState;


// Which command set the FPGA is implementing
typedef enum {
    PSDISPLAY_CMD_SET_UNKNOWN,
    PSDISPLAY_CMD_SET_0,   // Boot ROM built on Dec 10, 2014
    PSDISPLAY_CMD_SET_1,   // FW ROM built on Sep 12, 2014
} PDisplayCmdSet;


/* Commands for PSDISPLAY_CMD_SET_0. We accept these while in the 
 * PSDISPLAYSTATE_ACCEPTING_CMD state. These are implemented in the first boot
 * ROM built Dec 2014 */
typedef enum {
    PSDISPLAYCMD0_NULL = 0,
    PSDISPLAYCMD0_SET_PARAMETER = 1,
    PSDISPLAYCMD0_DISPLAY_OFF = 2,
    PSDISPLAYCMD0_DISPLAY_ON = 3,
    PSDISPLAYCMD0_DRAW_SCENE = 4
} PDisplayCmd0;


/* Commands for PSDISPLAY_CMD_SET_1. We accept these while in the
 * PSDISPLAYSTATE_ACCEPTING_CMD state. These are implemented in the early firmware
 * ROM buit Sep 2014 */
typedef enum {
    PSDISPLAYCMD1_FRAME_BEGIN = 0,
    PSDISPLAYCMD1_FRAME_DATA = 1,
    PSDISPLAYCMD1_FRAME_END = 2
} PDisplayCmd1;


// Scene numbers put into cmd_parameter and used by the PSDISPLAYCMD0_DRAW_SCENE command.
typedef enum {
    PSDISPLAYSCENE_BLACK = 0,
    PSDISPLAYSCENE_SPLASH = 1,    // splash screen
    PSDISPLAYSCENE_UPDATE = 2,    // firmware update
    PSDISPLAYSCENE_ERROR = 3      // display error code
} PDisplayScene;


typedef struct {
    SSISlave ssidev;

    /* Properties */
    union {
        void *vdone_output;
        qemu_irq done_output;
    };

    // This output line gets asserted (low) when we are done processing a drawing command.
    // It is generally to an IRQ
    union {
        void *vintn_output;
        qemu_irq intn_output;
    };

    QemuConsole   *con;
    bool          redraw;
    uint8_t       framebuffer[SNOWY_NUM_ROWS * SNOWY_BYTES_PER_ROW];
    int           col_index;
    int           row_index;

    /* State variables */
    PSDisplayState  state;
    uint8_t         cmd;
    uint32_t        parameter;
    uint32_t        parameter_byte_offset;
    PDisplayScene   scene;

    bool      sclk_value;
    bool      cs_value;                 // low means asserted
    uint32_t  sclk_count_with_cs_high;

    /* We capture the first 256 bytes of the programming and inspect it to try and figure 
     * out which command set to expect */
    uint8_t       prog_header[256];
    uint32_t      prog_byte_offset;

    // Which command set we are emulating
    PDisplayCmdSet cmd_set;

} PSDisplayGlobals;


typedef struct {
  uint8_t red, green, blue;
} PSDisplayPixelColor;


static uint8_t *ps_display_get_pebble_logo(void);



// -----------------------------------------------------------------------------
static void ps_display_set_pixel(PSDisplayGlobals *s, uint32_t x, uint32_t y,
                            uint8_t pixel_byte) {
    s->framebuffer[y * SNOWY_BYTES_PER_ROW + x] = pixel_byte;
}


// -----------------------------------------------------------------------------
static void ps_display_draw_bitmap(PSDisplayGlobals *s, uint8_t *bits,
                      uint32_t x_offset, uint32_t y_offset, uint32_t width, uint32_t height) {
  uint32_t pixels = width * height;

  for (int i = 0; i < pixels; ++i) {
      bool value = bits[i / 8] & (1 << (i % 8));
      uint8_t x = x_offset + (i % width);
      uint8_t y = y_offset + (i / width);
      if (value) {
          ps_display_set_pixel(s, x, y, 0xC0 /* red */);
      } else {
          ps_display_set_pixel(s, x, y, 0x00 /* black */);
      }
  }
}


/* -----------------------------------------------------------------------------
 Scan through the first part of the programming data and try and determine which
 command set the FPGA is implementing. Here is an example of the data comprising
 the programming for PSDISPLAY_CMD_SET_BOOT_0:

  39F0:       FF 00 4C 61 74 74 69 63 65 00 69 43 45 63      pG..Lattice.iCEc
  3A00: 75 62 65 32 20 32 30 31 34 2E 30 38 2E 32 36 37      ube2 2014.08.267
  3A10: 32 33 00 50 61 72 74 3A 20 69 43 45 34 30 4C 50      23.Part: iCE40LP
  3A20: 31 4B 2D 43 4D 33 36 00 44 61 74 65 3A 20 44 65      1K-CM36.Date: De
  3A30: 63 20 31 30 20 32 30 31 34 20 30 38 3A 33 30 3A      c 10 2014 08:30:
  3A40: 00 FF 31 38 00 7E AA 99 7E 51 00 01 05 92 00 20      ..18.~..~Q.....
  3A50: 62 01 4B 72 00 90 82 00 00 11 00 01 01 00 00 00      b.Kr...........  */
static void ps_display_determine_command_set(PSDisplayGlobals *s) {
    // Table of programming header dates and command sets
    typedef struct {
        const char *date_str;
        PDisplayCmdSet cmd_set;
    } CommandSetInfo;
    static const CommandSetInfo cmd_sets[] = {
      {"Date: Dec 10 2014 08:30", PSDISPLAY_CMD_SET_0},
      {"Date: Sep 12 2014 16:56:21", PSDISPLAY_CMD_SET_1},
    };


    // Make sure prog_header is null terminated
    if (s->prog_byte_offset >= sizeof(s->prog_header)) {
        s->prog_byte_offset = sizeof(s->prog_header) - 1;
    }
    s->prog_header[s->prog_byte_offset] = 0;

    // Default one to use
    s->cmd_set = PSDISPLAY_CMD_SET_1;

    // Skip first two bytes which contain 0xFF 00
    for (int i=2; i<s->prog_byte_offset; i++) {
        const char *str_p = (const char *)s->prog_header + i;

        DPRINTF("%s: found '%s' string in programming header\n", __func__, str_p);

        // Look for a string that starts with "Date:"
        if (!strncmp(str_p, "Date:", 5)) {
            int n_cmd_sets = sizeof(cmd_sets) / sizeof(cmd_sets[0]);
            for (int n=0; n<n_cmd_sets; n++) {
                if (!strncmp(str_p, cmd_sets[n].date_str, strlen(cmd_sets[n].date_str))) {
                    s->cmd_set = cmd_sets[n].cmd_set;
                    DPRINTF("%s: determined command set as %d\n", __func__, s->cmd_set);
                    return;
                }
            }

            // We didn't find the command set, bail
            fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Unknown FPGA programming with a date"
                    " stamp of '%s'. Defaulting to command set %d\n", str_p, s->cmd_set);
            return;
        } else {
            // Skip this string
            i += strlen(str_p);
        }

    }

    // Couldn't find the "Date:" string
    fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Error parsing FPGA programming data to"
            " determine command set. Defaulting to command set %d\n", s->cmd_set);
    return;
}


// -----------------------------------------------------------------------------
static void
ps_display_reset_state(PSDisplayGlobals *s, bool assert_done) {

    // If we are resetting because we done with the previous command, assert done
    if (assert_done) {
        DPRINTF("Asserting done interrupt\n");
        qemu_set_irq(s->intn_output, false);
    }

    DPRINTF("Resetting state to accept command\n");
    s->state = PSDISPLAYSTATE_ACCEPTING_CMD;
    s->parameter_byte_offset = 0;
}


// -----------------------------------------------------------------------------
// Implements command PSDISPLAY_CMD_SET_0, used in the first boot ROM, built Dec 2014
static void
ps_display_execute_current_cmd_set0(PSDisplayGlobals *s) {
    switch (s->cmd) {
    case PSDISPLAYCMD0_NULL:
        DPRINTF("Executing command: NULL\n");
        ps_display_reset_state(s, true /*assert_done*/);
        break;

    case PSDISPLAYCMD0_SET_PARAMETER:
        DPRINTF("Executing command: SET_PARAMETER\n");
        s->state = PSDISPLAYSTATE_ACCEPTING_PARAM;
        s->parameter_byte_offset = 0;
        break;

    case PSDISPLAYCMD0_DISPLAY_OFF:
        DPRINTF("Executing command: DISPLAY_OFF\n");
        ps_display_reset_state(s, true /*assert_done*/);
        break;

    case PSDISPLAYCMD0_DISPLAY_ON:
        DPRINTF("Executing command: DISPLAY_ON\n");
        ps_display_reset_state(s, true /*assert_done*/);
        break;

    case PSDISPLAYCMD0_DRAW_SCENE:
        if (s->state == PSDISPLAYSTATE_ACCEPTING_CMD) {
            s->state = PSDISPLAYSTATE_ACCEPTING_SCENE_BYTE;
        } else if (s->state == PSDISPLAYSTATE_ACCEPTING_SCENE_BYTE) {
            DPRINTF("Executing command: DRAW_SCENE: %d\n", s->scene);
            switch (s->scene) {
            case PSDISPLAYSCENE_BLACK:
                memset(s->framebuffer, SNOWY_COLOR_BLACK, sizeof(s->framebuffer));
                break;
            case PSDISPLAYSCENE_SPLASH:
                ps_display_draw_bitmap(s, ps_display_get_pebble_logo(), 8, 68, 128, 32);
                break;
            case PSDISPLAYSCENE_UPDATE:
                memset(s->framebuffer, SNOWY_COLOR_GREEN, sizeof(s->framebuffer));
                break;
            case PSDISPLAYSCENE_ERROR:
                memset(s->framebuffer, SNOWY_COLOR_BLUE, sizeof(s->framebuffer));
                break;
            default:
                fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Unsupported scene: %d\n", s->scene);
                break;
            }
            ps_display_reset_state(s, true /*assert_done*/);
            s->redraw = true;
        } else {
            fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Tried to execute draw scene in "
                      "wrong state: %d\n", s->state);
            ps_display_reset_state(s, true /*assert_done*/);
        }
        break;

    default:
        fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Unsupported cmd: %d\n", s->cmd);
        ps_display_reset_state(s, true /*assert_done*/);
        break;
    }
}

// -----------------------------------------------------------------------------
// Implements command set PSDISPLAY_CMD_SET_1, used in the development firmware, built Sep 2014
static void
ps_display_execute_current_cmd_set1(PSDisplayGlobals *s) {

    switch (s->cmd) {
    case PSDISPLAYCMD1_FRAME_BEGIN:
        DPRINTF("Executing command: FRAME_BEGIN\n");
        // Basically ignore this, wait for the FRAME_DATA command to be sent
        break;

    case PSDISPLAYCMD1_FRAME_DATA:
        DPRINTF("Executing command: FRAME_DATA\n");
        s->state = PSDISPLAYSTATE_ACCEPTING_LINENO;
        break;

    case PSDISPLAYCMD1_FRAME_END:
        DPRINTF("Executing command: FRAME_END\n");
        // Go back to accepting command. This will also assert the done interrupt.
        s->redraw = true;
        ps_display_reset_state(s, true /*assert_done*/);
        break;

    default:
        fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Unsupported cmd: %d\n", s->cmd);
        ps_display_reset_state(s, true /*assert_done*/);
        break;
    }
}


// ----------------------------------------------------------------------------- 
static uint32_t
ps_display_transfer(SSISlave *dev, uint32_t data)
{
    PSDisplayGlobals *s = FROM_SSI_SLAVE(PSDisplayGlobals, dev);
    uint32_t data_byte = data & 0x00FF;

    //DPRINTF("rcv byte: 0x%02x\n", data_byte);
    //DPRINTF("Got %d sclocks\n", s->sclk_count_with_cs_high);

    /* Ignore incoming data if our chip select is not asserted */
    if (s->cs_value) {
        if (s->state != PSDISPLAYSTATE_PROGRAMMING) {
            fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: received data without CS asserted");
        }
        return 0;
    }

    switch(s->state) {
    case PSDISPLAYSTATE_PROGRAMMING:
        // Capture the start of the programming data
        if (s->prog_byte_offset < sizeof(s->prog_header)) {
            s->prog_header[s->prog_byte_offset++] = data_byte;
        }
        break;

    case PSDISPLAYSTATE_ACCEPTING_CMD:
        s->cmd = data_byte;
        DPRINTF("received command %d, deasserting done interrupt\n", s->cmd);

        // Start of a command. Deassert done interrupt, it will get asserted again when
        // ps_display_reset_state() is called at the end of the command
        qemu_set_irq(s->intn_output, true);

       if (s->cmd_set == PSDISPLAY_CMD_SET_0) {
            ps_display_execute_current_cmd_set0(s);
        } else if (s->cmd_set == PSDISPLAY_CMD_SET_1) {
            ps_display_execute_current_cmd_set1(s);
        } else {
            fprintf(stderr, "Unimplemeneted command set\n");
            abort();
        }
        break;

    case PSDISPLAYSTATE_ACCEPTING_PARAM:

        DPRINTF("received param byte %d\n", data_byte);
        /* Params are sent low byte first */
        if (s->parameter_byte_offset == 0) {
            s->parameter = (s->parameter & 0xFFFFFF00) | data_byte;
        } else if (s->parameter_byte_offset == 1) {
            s->parameter = (s->parameter & 0xFFFF00FF) | (data_byte << 8);
        } else if (s->parameter_byte_offset == 2) {
            s->parameter = (s->parameter & 0xFF00FFFF) | (data_byte << 16);
        } else if (s->parameter_byte_offset == 3) {
            s->parameter = (s->parameter & 0x00FFFFFF) | (data_byte << 24);
        } else {
            fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: received more than 4 bytes of parameter");
        }

        s->parameter_byte_offset++;
        if (s->parameter_byte_offset >= 4) {
            DPRINTF("assembled complete param value of %d\n", s->parameter);
            ps_display_reset_state(s, true /*assert_done*/);
        }
        break;

    case PSDISPLAYSTATE_ACCEPTING_SCENE_BYTE:
        s->scene = data_byte;
        DPRINTF("received scene ID: %d\n", s->scene);
        ps_display_execute_current_cmd_set0(s);
        break;

    case PSDISPLAYSTATE_ACCEPTING_LINENO:
        /* Check for invalid column index */
        if (data >= SNOWY_NUM_COLS) {
            fprintf(stderr, "PEBBLE_SNOWY_DISPLAY: Invalid column index %d received", data);
            data = 0;
        }
        s->col_index = data;

        /* The column data is sent from the bottom up */
        s->row_index = SNOWY_NUM_ROWS + SNOWY_ROWS_SKIPPED_AT_BOTTOM - 1;
        s->state = PSDISPLAYSTATE_ACCEPTING_DATA;

        // We are not done, deassert the interrupt
        qemu_set_irq(s->intn_output, true);
        //DPRINTF("  new column no: %d\n", data);
        break;

    case PSDISPLAYSTATE_ACCEPTING_DATA:
        //DPRINTF("0x%02X, row %d\n", data, s->row_index);
        if (s->row_index >= SNOWY_NUM_ROWS) {
          /* If this row index is in the bottom padding area, ignore it */
          s->row_index--;
        } else if (s->row_index >= 0) {
          /* If this row index is in the viewable area, save to our frame buffer */
          s->framebuffer[s->row_index * SNOWY_BYTES_PER_ROW + s->col_index] = data;
          s->row_index--;
        } else if (s->row_index > -SNOWY_ROWS_SKIPPED_AT_TOP) {
          /* If this row index is in the top padding area, ignore it */
          s->row_index--;
        } else {
          /* We just received the last byte in the line, change state */
          s->state = PSDISPLAYSTATE_ACCEPTING_LINENO;
          // We are done with this line, assert the interrupt
          qemu_set_irq(s->intn_output, false);
        }
        break;
    }
    return 0;
}


// ----------------------------------------------------------------------------- 
// This function maps an 8 bit value from the frame buffer into red, green, and blue
// components
static PSDisplayPixelColor ps_display_get_rgb(uint8_t pixel_value) {

  PSDisplayPixelColor c;
  c.red = ((pixel_value & 0xC0) >> 6) * 255 / 3;
  c.green = ((pixel_value & 0x30) >> 4) * 255 / 3;
  c.blue = ((pixel_value & 0x0C) >> 2) * 255 / 3;
  return c;

  /*
  static PSDisplayPixelColors[256] = {
    {0, 0, 0 },
  };

  return PSDisplayPixelColors[pixel_value];
  */
}


// ----------------------------------------------------------------------------- 
static void ps_display_update_display(void *arg)
{
    PSDisplayGlobals *s = arg;
    DisplaySurface *surface = qemu_console_surface(s->con);

    uint8_t *d;
    int x, y, bpp, rgb_value;

    if (!s->redraw) {
        return;
    }

    bpp = surface_bits_per_pixel(surface);
    d = surface_data(surface);

    for (y = 0; y < SNOWY_NUM_ROWS; y++) {
        for (x = 0; x < SNOWY_NUM_COLS; x++) {
            uint8_t pixel = s->framebuffer[y * SNOWY_BYTES_PER_ROW + x];

            PSDisplayPixelColor color = ps_display_get_rgb(pixel);

            switch(bpp) {
            case 8:
                *((uint8_t *)d) = rgb_to_pixel8(color.red, color.green, color.blue);
                d++;
                break;
            case 15:
                *((uint16_t *)d) = rgb_to_pixel15(color.red, color.green, color.blue);;
                d += 2;
                break;
            case 16:
                *((uint16_t *)d) = rgb_to_pixel16(color.red, color.green, color.blue);;
                d += 2;
                break;
            case 24:
                rgb_value = rgb_to_pixel24(color.red, color.green, color.blue);
                *d++ = (rgb_value & 0x00FF0000) >> 16;
                *d++ = (rgb_value & 0x0000FF00) >> 8;
                *d++ = (rgb_value & 0x000000FF);
                break;
            case 32:
                *((uint32_t *)d) = rgb_to_pixel32(color.red, color.green, color.blue);;
                d += 4;
                break;
            }
        }
    }

    dpy_gfx_update(s->con, 0, 0, SNOWY_NUM_COLS, SNOWY_NUM_ROWS);
    s->redraw = false;
}

// ----------------------------------------------------------------------------- 
static int ps_display_set_cs(SSISlave *dev, bool value)
{
    PSDisplayGlobals *s = FROM_SSI_SLAVE(PSDisplayGlobals, dev);

    DPRINTF("CS changed to %d\n", value);
    s->cs_value = value;

    // When CS goes up (unasserted), reset our state
    if (value && s->state != PSDISPLAYSTATE_PROGRAMMING) {
        DPRINTF("Resetting state because CS was unasserted\n");
        ps_display_reset_state(s, true /*assert_done*/);
    }

    return 0;
}

// ----------------------------------------------------------------------------- 
static void ps_display_set_reset_pin_cb(void *opaque, int n, int level)
{
    PSDisplayGlobals *s = FROM_SSI_SLAVE(PSDisplayGlobals, opaque);
    bool value = !!level;
    assert(n == 0);

    DPRINTF("RESET changed to %d\n", value);
    qemu_set_irq(s->done_output, false);


    // When reset is asserted, reset our state
    if (!value) {
        // After a reset, we are not done. Deassert our interrupt (asserted low).
        qemu_set_irq(s->intn_output, true);
        s->sclk_count_with_cs_high = 0;
        s->state = PSDISPLAYSTATE_PROGRAMMING;
        s->prog_byte_offset = 0;
    }
}

// ----------------------------------------------------------------------------- 
static void ps_display_set_sclk_pin_cb(void *opaque, int n, int level)
{
    PSDisplayGlobals *s = FROM_SSI_SLAVE(PSDisplayGlobals, opaque);
    assert(n == 0);

    bool new_value = !!level;

    /* Count number of clocks received when CS is held high, this tells us when we are
     * done receiving programming */
    if (s->cs_value) {
        if (new_value && new_value != s->sclk_value) {
            s->sclk_count_with_cs_high++;
        }

        /* After enough cycles of sclck, say we are done with programming mode */
        if (s->sclk_count_with_cs_high > 50) {
            qemu_set_irq(s->done_output, true);
            if (s->state == PSDISPLAYSTATE_PROGRAMMING) {
                DPRINTF("Got %d sclocks, exiting programming mode\n",
                              s->sclk_count_with_cs_high);
                ps_display_reset_state(s, true);

                // Try and figure out which command set the FPGA expects by parsing the
                // programming data
                ps_display_determine_command_set(s);
            }
        }
    }

    /* Save new value */
    s->sclk_value = new_value;
}


// ----------------------------------------------------------------------------- 
static void ps_display_invalidate_display(void *arg)
{
    PSDisplayGlobals *s = arg;
    s->redraw = true;
}

// ----------------------------------------------------------------------------- 
static const GraphicHwOps ps_display_ops = {
    .gfx_update = ps_display_update_display,
    .invalidate = ps_display_invalidate_display,
};

// ----------------------------------------------------------------------------- 
static int ps_display_init(SSISlave *dev)
{
    PSDisplayGlobals *s = FROM_SSI_SLAVE(PSDisplayGlobals, dev);

    s->con = graphic_console_init(DEVICE(dev), 0, &ps_display_ops, s);
    qemu_console_resize(s->con, SNOWY_NUM_COLS, SNOWY_NUM_ROWS);

    /* Create our inputs that will be connected to GPIOs from the STM32 */
    qdev_init_gpio_in_named(DEVICE(dev), ps_display_set_reset_pin_cb,
                            "pebble-snowy-display-reset", 1);

    /* Create our inputs that will be connected to GPIOs from the STM32 */
    qdev_init_gpio_in_named(DEVICE(dev), ps_display_set_sclk_pin_cb,
                            "pebble-snowy-display-sclk", 1);

    return 0;
}

// ----------------------------------------------------------------------------- 
static Property ps_display_init_properties[] = {
    DEFINE_PROP_PTR("done_output", PSDisplayGlobals, vdone_output),

    // NOTE: Also used as a "busy" flag. If unasserted (high), the MPU asssumes the
    //  display is busy.
    DEFINE_PROP_PTR("intn_output", PSDisplayGlobals, vintn_output),
    DEFINE_PROP_END_OF_LIST()
};


// ----------------------------------------------------------------------------- 
static void ps_display_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    SSISlaveClass *k = SSI_SLAVE_CLASS(klass);

    dc->props = ps_display_init_properties;
    k->init = ps_display_init;
    k->transfer = ps_display_transfer;
    k->cs_polarity = SSI_CS_LOW;
    k->set_cs = ps_display_set_cs;
}

// ----------------------------------------------------------------------------- 
static const TypeInfo ps_display_info = {
    .name          = "pebble-snowy-display",
    .parent        = TYPE_SSI_SLAVE,
    .instance_size = sizeof(PSDisplayGlobals),
    .class_init    = ps_display_class_init,
};

// ----------------------------------------------------------------------------- 
static void ps_display_register(void)
{
    type_register_static(&ps_display_info);
}

// ----------------------------------------------------------------------------- 
type_init(ps_display_register);



static uint8_t *ps_display_get_pebble_logo(void) {

    static uint8_t logo[512] = {
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 0 - 16 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 16 - 32 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 32 - 48 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 48 - 64 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 64 - 80 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 80 - 96 */
      0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x38, 0x00, 0x00, 0x1c, 0x00, 0x00, 0x0e, 0x00, 0x00, 0x00, /* bytes 96 - 112 */
      0x00, 0x00, 0x07, 0x00, 0xe0, 0x01, 0x38, 0x70, 0x00, 0x1c, 0x38, 0x00, 0x0e, 0xc0, 0x03, 0x00, /* bytes 112 - 128 */
      0x80, 0xe3, 0x3f, 0x00, 0xfc, 0x0f, 0x38, 0xfe, 0x03, 0x1c, 0xff, 0x01, 0x0e, 0xf8, 0x1f, 0x00, /* bytes 128 - 144 */
      0x80, 0xf3, 0x7f, 0x00, 0xfe, 0x1f, 0x38, 0xff, 0x0f, 0x9c, 0xff, 0x07, 0x0e, 0xfc, 0x3f, 0x00, /* bytes 144 - 160 */
      0x80, 0x3b, 0xf0, 0x00, 0x0f, 0x3c, 0xb8, 0x03, 0x1f, 0xdc, 0x81, 0x0f, 0x0e, 0x1e, 0x78, 0x00, /* bytes 160 - 176 */
      0x80, 0x0f, 0xe0, 0x81, 0x03, 0x78, 0xf8, 0x01, 0x1c, 0xfc, 0x00, 0x0e, 0x0e, 0x07, 0xf0, 0x00, /* bytes 176 - 192 */
      0x80, 0x0f, 0xc0, 0x83, 0x03, 0x70, 0xf8, 0x00, 0x3c, 0x7c, 0x00, 0x1e, 0x0e, 0x07, 0xe0, 0x00, /* bytes 192 - 208 */
      0x80, 0x07, 0x80, 0xc3, 0x01, 0x70, 0x78, 0x00, 0x38, 0x3c, 0x00, 0x1c, 0x8e, 0x03, 0xe0, 0x00, /* bytes 208 - 224 */
      0x80, 0x03, 0x80, 0xc3, 0x01, 0x7e, 0x38, 0x00, 0x30, 0x1c, 0x00, 0x18, 0x8e, 0x03, 0xfc, 0x00, /* bytes 224 - 240 */
      0x80, 0x03, 0x00, 0xc7, 0xc1, 0x1f, 0x38, 0x00, 0x70, 0x1c, 0x00, 0x38, 0x8e, 0x83, 0x3f, 0x00, /* bytes 240 - 256 */
      0x80, 0x03, 0x00, 0xc7, 0xf9, 0x03, 0x38, 0x00, 0x70, 0x1c, 0x00, 0x38, 0x8e, 0xf3, 0x07, 0x00, /* bytes 256 - 272 */
      0x80, 0x03, 0x00, 0xc7, 0x7f, 0x00, 0x38, 0x00, 0x70, 0x1c, 0x00, 0x38, 0x8e, 0xff, 0x00, 0x00, /* bytes 272 - 288 */
      0x80, 0x03, 0x00, 0xc7, 0x0f, 0x00, 0x38, 0x00, 0x70, 0x1c, 0x00, 0x38, 0x8e, 0x1f, 0x00, 0x00, /* bytes 288 - 304 */
      0x80, 0x03, 0x80, 0xc3, 0x01, 0x00, 0x38, 0x00, 0x30, 0x1c, 0x00, 0x18, 0x8e, 0x03, 0x00, 0x00, /* bytes 304 - 320 */
      0x80, 0x07, 0x80, 0xc3, 0x01, 0x00, 0x78, 0x00, 0x38, 0x3c, 0x00, 0x1c, 0x8e, 0x03, 0x00, 0x00, /* bytes 320 - 336 */
      0x80, 0x0f, 0xc0, 0x83, 0x03, 0x00, 0xf8, 0x00, 0x38, 0x7c, 0x00, 0x1c, 0x0e, 0x07, 0x00, 0x00, /* bytes 336 - 352 */
      0x80, 0x0f, 0xc0, 0x81, 0x07, 0x70, 0xf8, 0x01, 0x1c, 0xfc, 0x00, 0x0e, 0x0e, 0x0f, 0xe0, 0x00, /* bytes 352 - 368 */
      0x80, 0x3f, 0xf0, 0x00, 0x0f, 0x78, 0xb8, 0x03, 0x1f, 0xdc, 0x81, 0x0f, 0x0e, 0x1e, 0xf0, 0x00, /* bytes 368 - 384 */
      0x80, 0xf3, 0x7f, 0x00, 0xfe, 0x3f, 0x38, 0xff, 0x07, 0x9c, 0xff, 0x03, 0x0e, 0xfc, 0x7f, 0x00, /* bytes 384 - 400 */
      0x80, 0xe3, 0x3f, 0x00, 0xf8, 0x0f, 0x38, 0xfe, 0x03, 0x1c, 0xff, 0x01, 0x0e, 0xf0, 0x1f, 0x00, /* bytes 400 - 416 */
      0x80, 0x03, 0x07, 0x00, 0xc0, 0x01, 0x00, 0x70, 0x00, 0x00, 0x38, 0x00, 0x00, 0x80, 0x03, 0x00, /* bytes 416 - 432 */
      0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bytes 432 - 448 */
      0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bytes 448 - 464 */
      0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bytes 464 - 480 */
      0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, /* bytes 480 - 496 */
      0x80, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    return logo;
}


