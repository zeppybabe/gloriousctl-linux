#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/utsname.h>
#include <dirent.h>
#include <pwd.h>
#include <unistd.h>
#include <hidapi/hidapi.h>

#define MAX_STR 255

/* #define FREE(var) free(var); var = 0;  This was removed to add the line below it 
 * wrapping macro execution in a do { ... } while (0) loop to ensure it behaves safely
 * exactly like a single C statement, regardless of surrounding braces. Also, setting pointers to NULL is preferred over 0 for clarity. */

#define FREE(var) do { free(var); var = NULL; } while(0)

/* Pixart Model I 2 Wireless / Wired Support added by Steven, 2026, with help of 
 * the original poster, enkore, and the entire community that helped build that repo
 * for older Glorious Mouse models */

void hexDump (const char * desc, const void * addr, const int len);

/* Which wire protocol a device actually speaks. This fork only implements
 * PROTO_PIXART. Anything else is recognised for reporting purposes but is
 * never written to -- sending Pixart fragments to a SinoWealth flash
 * controller is how you brick someone's mouse. */
enum device_proto {
    PROTO_UNSUPPORTED = 0,   /* detected, refuse to write */
    PROTO_PIXART,            /* 64-byte fragments, report ID 0x03 */
    PROTO_SINOWEALTH,        /* 520-byte feature report ID 0x04, cmd on 0x05 */
};

struct supported_device {
    uint16_t vid;
    uint16_t pid;
    const char *name;
    enum device_proto proto;
    /* HID interface carrying the vendor config page.
     * -1 means "unknown, try every interface". */
    int iface;
    /* Sensor DPI limits for THIS device, not for the protocol. Both 0 means
     * "unknown" and the range check is skipped -- adding a device whose specs
     * you cannot verify must not require inventing numbers. */
    int dpi_min, dpi_max;
};

/* these mice are clearly based on sinowealth's design
 * and there are a whole bunch others
 * one of the easiest ways to figure out which, google for glorious mice
 * anti-cheat bans, and you'll find a bunch of similarly specced mice that
 * also got banned for using the same VID/PID.
 * some of these have screenshots of their control software, which clearly
 * is the same as the glorious software.
 */
static struct supported_device supported_devices[] = {
    // G-Wolves Hati has the same PID, but uses a different report ID for changing configuration (0x6).
    /* The vendor config page sits on interface 1, which the descriptor
     * misleadingly declares as a Boot Keyboard. */
    { .vid = 0x258a, .pid = 0x27, .name = "Dream Machines DM5",
      .proto = PROTO_SINOWEALTH, .iface = 1 },
    { .vid = 0x258a, .pid = 0x33, .name = "Glorious Model D",
      .proto = PROTO_SINOWEALTH, .iface = 1 },
    { .vid = 0x258a, .pid = 0x36, .name = "Glorious Model O/O-",
      .proto = PROTO_SINOWEALTH, .iface = 1 },
    /* dpi_min/max from the published spec sheet, NOT confirmed against
     * firmware -- see the open hardware questions in the porting notes. */
    { .vid = 0x093a, .pid = 0x821d, .name = "Glorious Model I 2 Wireless",
      .proto = PROTO_PIXART, .iface = 1,
      .dpi_min = 100, .dpi_max = 26000 },
    /* Reported working config unknown -- probe only, do not write.
     * See issue thread: SINOWEALTH "Model O Eternal". Retailers disagree on
     * whether this is a 12K or 19K sensor, so the bounds stay unknown. */
    { .vid = 0x3794, .pid = 0xa000, .name = "Glorious Model O Eternal",
      .proto = PROTO_UNSUPPORTED, .iface = -1 },
};

#pragma pack(push, 1)

typedef struct {
    uint8_t r, g, b;
} RGB8;

enum pixart_rgb_effect {
    /* OFF confirmed on a Model I 2 Wireless: --set-effect off turned the LEDs
     * dark. It is the only real "off" -- a zero brightness byte makes the
     * firmware revert to its stored profile instead. */
    PIXART_RGB_OFF = 0x00,
    PIXART_RGB_GLORIOUS = 0x01,
    PIXART_RGB_SEAMLESS_BREATHING = 0x02,
    PIXART_RGB_BREATHING = 0x03,
    PIXART_RGB_NORMALLY_ON = 0x04,
    PIXART_RGB_BREATHING_SINGLE = 0x05,
    PIXART_RGB_TAIL = 0x06,
    PIXART_RGB_RAVE = 0x07,
    PIXART_RGB_WAVE = 0x08
};

/* Sensor granularity, kept only as a rounding hint for user input. The wire
 * format stores the RAW DPI value little-endian -- confirmed on a Model I 2
 * Wireless, where /50-encoded values (8, 128, ...) fell below the sensor
 * minimum and were silently ignored, while raw values (0x0320 = 800) moved
 * the cursor as expected. The datasheet's DPI/50 claim is wrong for this
 * device. Per-device sensor limits live in supported_device.dpi_min/dpi_max. */
#define PIXART_DPI_STEP 50

/* Inter-fragment delay for wireless busy-drop protection; see the comment in
 * pixart_send_payload for the observations behind it. */
#define PIXART_FRAGMENT_GAP_MS 120

// 192-Byte Payload for Lighting (3 Packets)
struct pixart_lighting_payload {
    // --- Fragment 1 (Main Settings) ---
    uint8_t  report_id1;          // [0] Always 0x03
    uint8_t  cmd1[2];             // [1-2] 0x02 0xfb
    uint8_t  seq1;                // [3] 0x00
    uint8_t  unk1;                // [4] 0x01
    uint8_t  effect_id;           // [5] enum pixart_rgb_effect
    uint8_t  brightness_wl;       // [6] Wireless Brightness (0-20)
    uint8_t  brightness_w;        // [7] Wired Brightness (0-20)
    uint8_t  color_count;         // [8] 0x02 or 0x07 depending on mode
    /* [9] Labelled "Speed (0-20)" in the original datasheet, but unconfirmed:
     * on a Model I 2 Wireless, poking this byte to 0x01 and 0x14 with the wave
     * effect active produced no observable rate change. Where speed actually
     * lives is still an open question; this byte keeps being written because
     * the captures did. */
    uint8_t  speed;               // [9]
    /* [10] Master brightness, working scale 0x01-0x14. Confirmed on a Model I
     * 2 Wireless: 0x05 dimmed the LEDs, and 0x01 dimmed further with the
     * chosen colour intact. 0x00 does NOT mean off -- the firmware reverts to
     * its stored default profile (same fallback seen when [6]/[7] are zeroed).
     * brightness_wl/brightness_w above had no observable effect in the same
     * test; their purpose is still unknown, so they keep being written. */
    uint8_t  brightness_master;   // [10]
    RGB8     main_color;          // [11-13]
    uint8_t  f1_padding[50];      // [14-63] Blank

    // --- Fragment 2 (Color Array Part 1) ---
    uint8_t  report_id2;          // [64] Always 0x03
    uint8_t  cmd2[2];             // [65-66] 0x02 0xfb
    uint8_t  seq2;                // [67] 0x01
    uint8_t  unk2;                // [68] 0x01
    uint8_t  effect_id2;          // [69] Repeats effect ID
    RGB8     color_array_1[6];    // [70-87] Multi-color cycle values
    uint8_t  f2_padding[40];      // [88-127] Blank

    // --- Fragment 3 (Color Array Part 2) ---
    uint8_t  report_id3;          // [128] Always 0x03
    uint8_t  cmd3[2];             // [129-130] 0x02 0xfb
    uint8_t  seq3;                // [131] 0x02
    uint8_t  unk3;                // [132] 0x01
    uint8_t  effect_id3;          // [133] Repeats effect ID
    uint8_t  f3_padding[58];      // [134-191] Blank
};

// 256-Byte Payload for Settings/DPI (4 Packets)
struct pixart_settings_payload {
    // --- Fragment 1 (Global + Stage 1) ---
    uint8_t  report_id1;     // [0] Always 0x03
    uint8_t  cmd1[2];        // [1-2] 0x04 0xfb
    uint8_t  seq1;           // [3] 0x00
    uint8_t  unk_pad1;       // [4] 0x01 (datasheet; never poked, fragment 1 works with it)
    /* [5] Zero-indexed stage. Confirmed live on a Model I 2 Wireless:
     * --set-active-stage switches the mouse's DPI indicator LED to the colour
     * programmed for that stage, so this payload is accepted by the firmware
     * (intermittently -- a sleeping wireless mouse drops it). */
    uint8_t  active_stage;   // [5] 0x00 to 0x05
    uint8_t  total_stages;   // [6] 0x04-0x06 (confirmed: 4<->6 switches the button cycle live)
    /* [7] Lift-off distance. Weakly confirmed on a Model I 2 Wireless: 0x02
     * tracked when lifted slightly, 0x01 did not -- a feel test, not a
     * measurement, so treat as likely rather than proven. */
    uint8_t  lod;            // [7] 0x01 (1mm) or 0x02 (2mm)
    /* [8] Debounce, ms. Sent as documented; NOT verifiable on this hardware
     * (no read path, and 4 vs 16 ms is below what a human can feel). */
    uint8_t  debounce;       // [8] 0x00 to 0x10 (Even numbers only)
    /* [9] Polling rate. The datasheet's map (0x01=1000, 0x02=125, 0x03=250,
     * 0x04=500) is WRONG: measured with evhz on a Model I 2 Wireless, 0x01
     * gave 125 Hz and 0x02 gave ~235 Hz (250). The scale is therefore linear:
     * 0x01=125, 0x02=250, 0x03=500 (all three measured). 0x04 read as a
     * jittery 500-1000 on a browser-based meter, wired and wireless alike,
     * and 0x05 did nothing more -- consistent with 1000 Hz being delivered
     * and the meter (which sees compositor-coalesced events) being the
     * ceiling. Mapped as 1000; an evdev-level measurement (evhz) would
     * make it exact. */
    uint8_t  polling;        // [9] 0x01=125, 0x02=250, 0x03=500, 0x04=1000 Hz
    /* [10-63] Stage 1 record + blank. Per-stage bytes are addressed through
     * pixart_stage_off[] and the pixart_stage_* accessors, not named fields:
     * see the table below the struct for why. */
    uint8_t  stage_bytes1[54];

    // --- Fragment 2 (Stages 2 & 3) ---
    uint8_t  report_id2;     // [64] Always 0x03
    uint8_t  cmd2[2];        // [65-66] 0x04 0xfb
    uint8_t  seq2;           // [67] 0x01
    uint8_t  stage_bytes2[60]; // [68-127] Stages 2 and 3 + blank. NO 0x01 pad at [68].

    // --- Fragment 3 (Stages 4 & 5) ---
    uint8_t  report_id3;     // [128] Always 0x03
    uint8_t  cmd3[2];        // [129-130] 0x04 0xfb
    uint8_t  seq3;           // [131] 0x02
    uint8_t  stage_bytes3[60]; // [132-191] Stages 4 and 5 + blank. NO 0x01 pad at [132].

    // --- Fragment 4 (Stage 6) ---
    uint8_t  report_id4;     // [192] Always 0x03
    uint8_t  cmd4[2];        // [193-194] 0x04 0xfb
    uint8_t  seq4;           // [195] 0x03
    uint8_t  stage_bytes4[60]; // [196-255] Stage 6 + blank. NO 0x01 pad at [196].
};

/* Per-stage byte offsets into the 256-byte settings payload.
 *
 * Every DPI offset below was confirmed on a Model I 2 Wireless with a crawl
 * test: 200 DPI (0x00c8) written into the candidate made that stage crawl at
 * half the speed of a 400 DPI stage, while the same value written one byte
 * later (the datasheet's address) did not. The datasheet is off by one for
 * ALL six stages -- it documents a 0x01 pad byte after each fragment header
 * that does not exist; that byte is the DPI low byte. Sending 0x01 there is
 * exactly why stages 2-6 sat pinned at max sensitivity for so long
 * (0x01 | low<<8 >= 0x8001 for any DPI with bit 7 of its low byte set, and
 * anything below the sensor minimum is silently discarded).
 *
 * DPI is the raw value, little-endian. Not DPI/50.
 *
 * Colour offsets are DPI+3 for every stage. Stage 1 (red/magenta at [13-15])
 * and stage 3 (magenta at [76-78], where the datasheet's DPI-adjacent [75-77]
 * would have shown green) are solid; stage 4 is solid via [137]: zeroing it
 * turned a white stage yellow. Stages 2, 5 and 6 follow the pattern and are
 * not independently confirmed.
 *
 * CONSEQUENCE, and the reason this is a table rather than a struct: within a
 * fragment the DPI fields are 5 bytes apart but a record is 6 bytes long, so
 * the BLUE byte of stage 2 ([73]) is also the DPI low byte of stage 3, and
 * the BLUE byte of stage 4 ([137]) is also the DPI low byte of stage 5. Both
 * halves of that overlap were observed on hardware (crawl at [73]/[137];
 * yellow tint from [137]). The tool applies colours first and DPI second so
 * a DPI setting always wins; the blue channel of stages 2 and 4 is therefore
 * not fully controllable and --info reports what will actually be sent. */
struct pixart_stage_offsets { uint8_t dpi; uint8_t rgb; };
static const struct pixart_stage_offsets pixart_stage_off[6] = {
    { 10,  13 },   /* stage 1: fragment 1, after the five global bytes */
    { 68,  71 },   /* stage 2: fragment 2 */
    { 73,  76 },   /* stage 3: fragment 2; [73] shared with stage 2 blue */
    { 132, 135 },  /* stage 4: fragment 3 */
    { 137, 140 },  /* stage 5: fragment 3; [137] shared with stage 4 blue */
    /* Stage 6 colour follows the DPI+3 pattern: with a fresh cache (every
     * other byte zero) 0xff at [201] alone lit the indicator deep blue, and
     * white across [198-203] lit it white. One earlier run where green at
     * [199-201] showed nothing is attributed to a dropped fragment 4 -- the
     * DPI in that same fragment also needed a second cycle to land. */
    { 196, 199 },  /* stage 6: fragment 4 */
};
#define PIXART_MAX_STAGES 6

/* Measured scale, see the polling comment in the struct. 0 = unknown code. */
static int pixart_polling_hz(uint8_t code)
{
    switch (code) {
    case 0x01: return 125;
    case 0x02: return 250;
    case 0x03: return 500;
    case 0x04: return 1000;
    default:   return 0;
    }
}

/* The cache file is read straight back into these payloads and then written to
 * the mouse, so an unversioned file is a footgun: add one field to either
 * payload and every existing ~/.gloriousctl_state.bin becomes garbage that
 * gets transmitted verbatim. Magic + version means a stale file is simply
 * ignored and regenerated from defaults. */
#define STATE_MAGIC   0x474C5253u   /* "GLRS" */
/* v2: dpi_stage1 moved from [11-12] to [10-11] and every dpi_stage* field
 * changed meaning from DPI/50 to the raw DPI value.
 * v3: dpi_stage4 moved from [133-134] to [132-133], the 0x01 pad at [132]
 * became stage 4's DPI low byte.
 * v4: all per-stage fields replaced by the pixart_stage_off table (every
 * stage's DPI one byte earlier than the datasheet, no pads at [68]/[132]/[196]).
 * An older cache replayed
 * through this layout would transmit garbage, so older files are discarded. */
#define STATE_VERSION 4

// The Master Linux Cache
struct glorious_state {
    uint32_t magic;
    uint32_t version;
    struct pixart_lighting_payload lighting;
    struct pixart_settings_payload settings;
};

#pragma pack(pop)

/* ---------- SinoWealth (0x258a family) ----------
 * Ported from enkore/gloriousctl, EUPL. Constants derived from the
 * 258a:0033 report descriptor; see the table in the porting notes.
 */

#define SW_CMD_CONFIG        0x11
#define SW_CMD_DEBOUNCE      0x1a
#define SW_CONFIG_SIZE       520
#define SW_CONFIG_SIZE_USED  131
#define SW_NUM_DPIS          6
#define SW_REPORT_ID_CMD     0x5
#define SW_REPORT_ID_CONFIG  0x4
#define SW_XY_INDEPENDENT    0x80

/* Encoding is (DPI/100)-1 in a uint8_t, so 100 is the lowest representable
 * value and 25600 the highest. Protocol limits, not sensor limits -- these are
 * derivable from the wire format, unlike the per-device dpi_min/dpi_max.
 * Note the step is 100 here and 50 on Pixart; --set-dpi 450 therefore rounds
 * differently per backend, which is why both paths report the rounding. */
#define SW_DPI_STEP    100
#define SW_DPI_MIN     100
#define SW_DPI_MAX   25600

#pragma pack(push, 1)

/* wire order is R, B, G -- this member order is deliberate, do not "fix" it */
typedef struct {
    uint8_t r, b, g;
} RBG8;

enum sw_rgb_effect {
    SW_RGB_OFF        = 0x0,
    SW_RGB_GLORIOUS   = 0x1,
    SW_RGB_SINGLE     = 0x2,
    SW_RGB_BREATHING7 = 0x3,
    SW_RGB_TAIL       = 0x4,
    SW_RGB_BREATHING  = 0x5,
    SW_RGB_RAVE       = 0x7,
    SW_RGB_WAVE       = 0x9,
    SW_RGB_BREATHING1 = 0xa,
};

struct sw_config {
    uint8_t report_id;
    uint8_t command_id;
    uint8_t unk1;
    /* 0x0 = read, SW_CONFIG_SIZE_USED-8 = write */
    uint8_t config_write;
    uint8_t unk2[6];
    uint8_t config1;              /* 0x80 = XY DPI independent */
    uint8_t dpi_count:4;
    uint8_t active_dpi:4;
    uint8_t dpi_enabled;          /* bit SET = disabled */
    uint8_t dpi[16];              /* (DPI/100)-1, or X,Y pairs if independent */
    RGB8    dpi_color[8];
    uint8_t rgb_effect;
    uint8_t glorious_mode;
    uint8_t glorious_direction;
    uint8_t single_mode;
    RBG8    single_color;
    uint8_t breathing7_mode;
    uint8_t breathing7_colorcount;
    RBG8    breathing7_colors[7];
    uint8_t tail_mode;
    uint8_t unk4[33];
    uint8_t rave_mode;
    RBG8    rave_colors[2];
    uint8_t wave_mode;
    uint8_t breathing1_mode;
    RBG8    breathing1_color;
    uint8_t unk5;
    uint8_t lift_off_distance;    /* 0x1 = 2mm, 0x2 = 3mm */
};

struct sw_change_report {
    uint8_t report_id;            /* = 7 */
    uint8_t unk1;
    uint8_t active_dpi:4;
    uint8_t unk2:4;
    uint8_t dpi_x;
    uint8_t dpi_y;
    uint8_t unk3[3];
};

#pragma pack(pop)

_Static_assert(sizeof(struct sw_config) == SW_CONFIG_SIZE_USED,
               "sw_config must be exactly 131 bytes");

/* Parsed --set-* options, passed to whichever backend claims the device. */
struct set_opts {
    const char *debounce;
    const char *dpi;
    const char *dpi_color;
    const char *effect;
    const char *colors;
    const char *brightness;
    const char *speed;
    const char *active_stage;
    const char *stages;
    const char *polling;
    const char *lod;
    /* Raw byte pokes, applied last. These exist because the payload layout
     * came from USB captures: when a documented field has no observable
     * effect, the only way forward is to try neighbouring offsets without
     * recompiling. Format: "offset=value,offset=value" (strtol base 0). */
    const char *raw_lighting;
    const char *raw_settings;
};

static RGB8 int_to_rgb(unsigned int value)
{
    RGB8 rgb;
    rgb.r = value >> 16U;
    rgb.g = (value >> 8U) & 0xFF;
    rgb.b = value & 0xFF;
    return rgb;
}

/* Per-stage accessors for the settings payload. `stage` is 0-based (0-5).
 * These are the only code allowed to touch stage bytes; see the offset table
 * next to pixart_stage_off for the layout evidence and the [73]/[137] overlap. */
static uint16_t pixart_stage_get_dpi(const struct pixart_settings_payload *s, int stage)
{
    const uint8_t *b = (const uint8_t *)s + pixart_stage_off[stage].dpi;
    return (uint16_t)(b[0] | (b[1] << 8));
}

static void pixart_stage_set_dpi(struct pixart_settings_payload *s, int stage, uint16_t dpi)
{
    uint8_t *b = (uint8_t *)s + pixart_stage_off[stage].dpi;
    b[0] = dpi & 0xFF;
    b[1] = dpi >> 8;
}

static RGB8 pixart_stage_get_color(const struct pixart_settings_payload *s, int stage)
{
    const uint8_t *b = (const uint8_t *)s + pixart_stage_off[stage].rgb;
    RGB8 rgb = { b[0], b[1], b[2] };
    return rgb;
}

static void pixart_stage_set_color(struct pixart_settings_payload *s, int stage, RGB8 rgb)
{
    uint8_t *b = (uint8_t *)s + pixart_stage_off[stage].rgb;
    b[0] = rgb.r;
    b[1] = rgb.g;
    b[2] = rgb.b;
}

/* True when this stage's blue byte is the next stage's DPI low byte. */
static int pixart_stage_blue_is_shared(int stage)
{
    return stage + 1 < PIXART_MAX_STAGES &&
           pixart_stage_off[stage].rgb + 2 == pixart_stage_off[stage + 1].dpi;
}

static const char *pixart_effect_name(uint8_t id)
{
    switch (id) {
        case PIXART_RGB_OFF:                return "off";
        case PIXART_RGB_GLORIOUS:           return "glorious";
        case PIXART_RGB_SEAMLESS_BREATHING: return "seamless";
        case PIXART_RGB_BREATHING:          return "breathing";
        case PIXART_RGB_NORMALLY_ON:        return "normally_on";
        case PIXART_RGB_BREATHING_SINGLE:   return "breathing_single";
        case PIXART_RGB_TAIL:               return "tail";
        case PIXART_RGB_RAVE:               return "rave";
        case PIXART_RGB_WAVE:               return "wave";
        default:                            return "unknown";
    }
}

static
void print_color(RGB8 color)
{
    printf("\e[38;2;%d;%d;%dm", color.r, color.g, color.b);
    printf("#%02X%02X%02X", color.r, color.g, color.b);
    printf("\e[39m");
}

static
unsigned int clamp(unsigned int value, unsigned int lower, unsigned int upper)
{
    if(value < lower) {
        return lower;
    }
    if(value > upper) {
        return upper;
    }

    return value;
}



static
char *find_device(const struct supported_device *dev)
{
    struct hid_device_info *devices = hid_enumerate(dev->vid, dev->pid);
    struct hid_device_info *device = devices;
    char *path = NULL;
    while(device) {
        if(dev->iface < 0 || device->interface_number == dev->iface) {
            path = strdup(device->path);
            break;
        }
        device = device->next;
    }

    hid_free_enumeration(devices);
    return path;
}

static
char *detect_device(const struct supported_device **matched)
{
    hid_init();

    char *path = NULL;
    if(matched) {
        *matched = NULL;
    }
    for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
        path = find_device(&supported_devices[i]);
        if(path) {
            fprintf(stderr, "Detected %s (VID %04x PID %04x)\n",
                    supported_devices[i].name,
                    supported_devices[i].vid, supported_devices[i].pid);
            if(matched) {
                *matched = &supported_devices[i];
            }
            return path;
        }
    }
    return NULL;
}

static
void print_hid_error(hid_device *handle, const char *operation)
{
    char err[200];
    const wchar_t *werr = hid_error(handle);
    if(werr) {
        wcstombs(err, werr, sizeof(err));
    } else {
        strcpy(err, "Unknown error");
    }
    fprintf(stderr, "%s: %s\n", operation, err);
}

static
const char *sw_effect_to_name(uint8_t e)
{
    switch(e) {
    case SW_RGB_OFF:        return "Off";
    case SW_RGB_GLORIOUS:   return "Glorious mode";
    case SW_RGB_SINGLE:     return "Single color";
    case SW_RGB_BREATHING:  return "RGB breathing";
    case SW_RGB_BREATHING7: return "Seven-color breathing";
    case SW_RGB_BREATHING1: return "Single color breathing";
    case SW_RGB_TAIL:       return "Tail effect";
    case SW_RGB_RAVE:       return "Two-color rave";
    case SW_RGB_WAVE:       return "Wave effect";
    default:                return "Unknown";
    }
}

/* 0 and *out filled on success, -1 if the name is not a SinoWealth effect.
 * The old version fell back to SW_RGB_OFF, so a Pixart effect name typed at a
 * Model D silently switched the lighting off instead of reporting the typo.
 * "off" must therefore be matched explicitly, not left to the fallback. */
static
int sw_name_to_effect(const char *name, uint8_t *out)
{
    if(!strcmp(name, "off"))        { *out = SW_RGB_OFF;        return 0; }
    if(!strcmp(name, "glorious"))   { *out = SW_RGB_GLORIOUS;   return 0; }
    if(!strcmp(name, "single"))     { *out = SW_RGB_SINGLE;     return 0; }
    if(!strcmp(name, "breathing"))  { *out = SW_RGB_BREATHING;  return 0; }
    if(!strcmp(name, "breathing7")) { *out = SW_RGB_BREATHING7; return 0; }
    if(!strcmp(name, "breathing1")) { *out = SW_RGB_BREATHING1; return 0; }
    if(!strcmp(name, "tail"))       { *out = SW_RGB_TAIL;       return 0; }
    if(!strcmp(name, "rave"))       { *out = SW_RGB_RAVE;       return 0; }
    if(!strcmp(name, "wave"))       { *out = SW_RGB_WAVE;       return 0; }
    return -1;
}

static
RBG8 int_to_rbg(unsigned int value)
{
    RBG8 c;
    c.r = value >> 16U;
    c.g = (value >> 8U) & 0xFF;
    c.b = value & 0xFF;
    return c;
}

static int sw_config_to_dpi(int v) { return (v + 1) * 100; }
static int sw_dpi_to_config(int d) { return d / 100 - 1; }

static
int sw_print_firmware_version(hid_device *dev)
{
    uint8_t version[6] = {SW_REPORT_ID_CMD, 0x1};
    int res = hid_send_feature_report(dev, version, sizeof(version));
    if(res != sizeof(version)) {
        print_hid_error(dev, "get firmware version command");
        return -1;
    }
    res = hid_get_feature_report(dev, version, sizeof(version));
    if(res != sizeof(version)) {
        print_hid_error(dev, "read firmware version");
        return -1;
    }
    printf("Firmware version: %.4s\n", version + 2);
    return 0;
}

/* Returns milliseconds, or -1 on error. (Upstream returned 1, which is
 * indistinguishable from a real 2 ms reading.) */
static
int sw_get_debounce_time(hid_device *dev)
{
    uint8_t debounce[6] = {SW_REPORT_ID_CMD, SW_CMD_DEBOUNCE};
    int res = hid_send_feature_report(dev, debounce, sizeof(debounce));
    if(res != sizeof(debounce)) {
        print_hid_error(dev, "get debounce time command");
        return -1;
    }
    res = hid_get_feature_report(dev, debounce, sizeof(debounce));
    if(res != sizeof(debounce)) {
        print_hid_error(dev, "read debounce time");
        return -1;
    }
    return debounce[2] * 2;
}

static
void sw_dump_config(const struct sw_config *cfg)
{
    int xy_independent = (cfg->config1 & SW_XY_INDEPENDENT) == SW_XY_INDEPENDENT;
    printf("XY DPI independent: %s\n", xy_independent ? "yes" : "no");
    for(unsigned int i = 0; i < SW_NUM_DPIS; i++) {
        if(cfg->dpi_enabled & (1U << i)) {
            printf("[ ] ");
        } else if(cfg->active_dpi == i) {
            printf("\e[1m[x]\e[0m ");
        } else {
            printf("[x] ");
        }
        printf("DPI setting %d: ", i + 1);
        if(xy_independent) {
            printf("%d/%d DPI\t", sw_config_to_dpi(cfg->dpi[i*2]),
                                  sw_config_to_dpi(cfg->dpi[i*2+1]));
        } else {
            printf("%d DPI\t", sw_config_to_dpi(cfg->dpi[i]));
        }
        print_color(cfg->dpi_color[i]);
        printf("\n");
    }
    printf("\nRGB mode: %s\n", sw_effect_to_name(cfg->rgb_effect));
    printf("Lift-off distance: %d mm\n", cfg->lift_off_distance + 1);
}

/* Caller owns the returned buffer (SW_CONFIG_SIZE bytes). NULL on failure. */
static
struct sw_config *sw_read_config(hid_device *dev)
{
    uint8_t cmd[6] = {SW_REPORT_ID_CMD, SW_CMD_CONFIG};
    if(hid_send_feature_report(dev, cmd, sizeof(cmd)) != sizeof(cmd)) {
        print_hid_error(dev, "get config command");
        return NULL;
    }

    struct sw_config *cfg = calloc(1, SW_CONFIG_SIZE);
    if(!cfg) {
        return NULL;
    }
    cfg->report_id = SW_REPORT_ID_CONFIG;

    int res = hid_get_feature_report(dev, (uint8_t*)cfg, SW_CONFIG_SIZE);
    if(res < SW_CONFIG_SIZE_USED) {
        print_hid_error(dev, "read config");
        free(cfg);
        return NULL;
    }
    return cfg;
}

static
int sw_write_config(hid_device *dev, struct sw_config *cfg)
{
    cfg->config_write = SW_CONFIG_SIZE_USED - 8;
    int res = hid_send_feature_report(dev, (uint8_t*)cfg, SW_CONFIG_SIZE);
    if(res == -1) {
        print_hid_error(dev, "write config");
        return -1;
    }
    return 0;
}



/* Dump everything needed to port a new device, so a bug report contains it
 * on the first round trip instead of the fourth. */
static
int do_probe(void)
{
    hid_init();

    struct hid_device_info *all = hid_enumerate(0x0, 0x0);
    struct hid_device_info *cur = all;
    int found = 0;

    printf("--- HID enumeration ---\n");
    while(cur) {
        int known = 0;
        for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
            if(cur->vendor_id == supported_devices[i].vid &&
               cur->product_id == supported_devices[i].pid) {
                known = 1;
                break;
            }
        }
        /* Only print mice-ish or vendor-page devices to keep output readable. */
        if(known || cur->usage_page >= 0xff00) {
            printf("%04x:%04x  iface=%d  usage_page=0x%04x  usage=0x%04x  %ls / %ls\n",
                   cur->vendor_id, cur->product_id, cur->interface_number,
                   cur->usage_page, cur->usage,
                   cur->manufacturer_string ? cur->manufacturer_string : L"?",
                   cur->product_string ? cur->product_string : L"?");
            /* Tagged so tooling (install.sh --verify) can pick the node that
             * belongs to a supported mouse instead of any vendor-page device
             * that happened to enumerate. */
            printf("            path=%s%s\n", cur->path, known ? "  supported=yes" : "");
            found++;
        }
        cur = cur->next;
    }
    hid_free_enumeration(all);

    if(!found) {
        printf("No candidate devices found. Try sudo.\n");
    }
    printf("\nAlso attach: sudo usbhid-dump -d VID:PID\n");

    hid_exit();
    return 0;
}

/* Everything a maintainer needs to add support for an unrecognised Glorious
 * mouse, gathered locally in one shot. Read-only: opens no device for
 * writing, sends nothing to the mouse, and never touches the network -- the
 * user decides where the output goes (attach it to a GitHub issue). */
static
int do_collect(void)
{
    printf("=== gloriousctl hardware report ===\n");
    printf("Attach this whole output to a GitHub issue to request support for a\n");
    printf("new device. Everything below was gathered locally; this tool sent\n");
    printf("nothing anywhere.\n\n");

    struct utsname un;
    if (uname(&un) == 0)
        printf("kernel: %s %s (%s)\n\n", un.sysname, un.release, un.machine);

    /* Section 1: the same enumeration --probe prints. */
    do_probe();

    /* Section 2: raw HID report descriptors from sysfs. hidapi has no API for
     * these, and they are the ground truth for what the device can speak. */
    printf("\n--- HID report descriptors (sysfs) ---\n");
    DIR *d = opendir("/sys/class/hidraw");
    if (!d) {
        printf("cannot open /sys/class/hidraw: %s\n", strerror(errno));
        return 1;
    }
    struct dirent *ent;
    int dumped = 0;
    while ((ent = readdir(d))) {
        if (strncmp(ent->d_name, "hidraw", 6) != 0)
            continue;
        char path[512];
        char uevent[1024] = {0};
        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/uevent", ent->d_name);
        FILE *f = fopen(path, "r");
        if (f) {
            size_t got = fread(uevent, 1, sizeof uevent - 1, f);
            uevent[got] = '\0';
            fclose(f);
        }

        /* HID_ID=0003:0000093A:0000821D. Keep any device whose vendor id the
         * table knows, whatever the product id -- unknown products from known
         * vendors are exactly what this report exists to capture. */
        unsigned int bus = 0, vid = 0, pid = 0;
        const char *idline = strstr(uevent, "HID_ID=");
        if (idline)
            sscanf(idline, "HID_ID=%x:%x:%x", &bus, &vid, &pid);
        int vendor_known = 0;
        for (unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++)
            if (supported_devices[i].vid == vid)
                vendor_known = 1;
        /* Some Glorious receivers enumerate under other vendor ids, so a name
         * match keeps them in the report too. */
        if (!vendor_known && !strstr(uevent, "Glorious") && !strstr(uevent, "GLORIOUS"))
            continue;

        printf("\n[%s] %04x:%04x\n", ent->d_name, vid, pid);
        const char *nameline = strstr(uevent, "HID_NAME=");
        if (nameline) {
            char name[256] = {0};
            sscanf(nameline, "HID_NAME=%255[^\n]", name);
            printf("name: %s\n", name);
        }

        snprintf(path, sizeof path, "/sys/class/hidraw/%s/device/report_descriptor", ent->d_name);
        f = fopen(path, "rb");
        if (!f) {
            printf("report_descriptor unreadable: %s (try sudo)\n", strerror(errno));
            continue;
        }
        uint8_t desc[4096];
        size_t n = fread(desc, 1, sizeof desc, f);
        fclose(f);
        printf("report descriptor, %zu bytes:\n", n);
        hexDump(NULL, desc, (int)n);
        dumped++;
    }
    closedir(d);
    if (!dumped)
        printf("no Glorious hidraw nodes found.\n");

    printf("\n--- what this report cannot contain ---\n");
    printf("The Pixart protocol is write-only, so the meaning of most payload\n");
    printf("bytes can only come from capturing the Windows Glorious CORE\n");
    printf("software: run it in a Windows VM with USB passthrough, record the\n");
    printf("traffic with Wireshark + USBPcap, and attach the .pcapng as well.\n");
    return 0;
}

static
int print_help()
{
    fprintf(stderr,
            "gloriousctl\n"
            "A utility to adjust the settings of Model O/D mice\n"
            "Copyright (c) 2020 Marian Beermann under the EUPL license\n"
            "\n"
            "Usage:\n"
            " gloriousctl --help\n"
            "\tShow this help text.\n"
            " gloriousctl --info\n"
            "\tShow the current configuration of the mouse.\n"
            "\tFor wireless variants, also shows battery status.\n"
            " gloriousctl --listen\n"
            "\tListen for and show DPI profile changes.\n"
            " gloriousctl --probe\n"
            "\tDump HID enumeration details. Attach this to bug reports.\n"
            " gloriousctl --collect\n"
            "\tGather everything needed to add support for an unrecognised\n"
            "\tGlorious mouse: enumeration, kernel info, and raw HID report\n"
            "\tdescriptors. Read-only and local; redirect to a file and attach\n"
            "\tit to a GitHub issue yourself. Nothing is sent anywhere.\n"
            " gloriousctl --udev-rules\n"
            "\tPrint udev rules for all supported mice to stdout.\n"
            " gloriousctl --dump-payload [--set-...]\n"
            "\tPrint the exact bytes a --set-... would transmit, as hex, and\n"
            "\tsend nothing. Pixart only. Opens no device, needs no permissions,\n"
            "\tleaves the cache untouched. Use it to diff against a USB capture.\n"
            " gloriousctl [--set-...]\n"
            "\tChange persistent mouse settings.\n"
            "\n"
            "Available settings:\n"
            " --set-active-stage 1-6\n"
            "\tPixart only. Select which DPI stage is active.\n"
            " --set-stages 4-6\n"
            "\tPixart only. Number of DPI stages the button cycles through.\n"
            "\tStages above the count keep their values but are skipped.\n"
            " --set-debounce-time 4-16\n"
            "\tChange click debounce time in milliseconds. Only use even numbers.\n"
            "\tPixart: sent as documented but not verifiable (no read path).\n"
            " --set-polling-rate 125|250|500|1000\n"
            "\tPixart only. 125/250/500 measured with a browser meter; 1000 read as\n"
            "\t500-1000 on the same meter (likely meter-limited; use evhz to confirm).\n"
            " --set-lod 1|2\n"
            "\tPixart only. Lift-off distance in mm.\n"
            " --state\n"
            "\tPixart only. Cached settings as key=value lines (for scripts/GUI).\n"
            " --set-dpi DPI1,...\n"
            "\tUp to six DPIs can be configured.\n"
            "\tSinoWealth: 100-25600, multiples of 100.\n"
            "\tPixart: multiples of 50; upper limit depends on the sensor.\n"
            " --set-dpi-color RRGGBB,...\n"
            "\tFor each DPI the RGB color can be set.\n"
            " --set-effect effect-name\n"
            "\tSinoWealth (Model O/O-, Model D, DM5):\n"
            "\t  off, glorious, breathing, breathing7, breathing1,\n"
            "\t  single, tail, rave, wave\n"
            "\tPixart (Model I 2):\n"
            "\t  off, glorious, seamless, breathing, normally_on,\n"
            "\t  breathing_single, tail, rave, wave\n"
            "\tsingle and breathing1 use one color, breathing7 seven, rave two.\n"
            " --set-colors RRGGBB,...\n"
            "\tSet the color(s) of the effect. Only effective with --set-effect.\n"
            " --set-brightness 1-4\n"
            "\tSet the brightness of the effect. Only effective with --set-effect.\n"
            "\t(0 is rejected: the firmware treats a zero brightness byte as a\n"
            "\tbad payload and reverts to its stored profile. Use --set-effect off.)\n"
            " --set-speed 0-3\n"
            "\tSet the speed of the effect. Only effective with --set-effect.\n"
            "\n"
            "Experimental (Pixart only, for mapping the captured payload):\n"
            " --raw-lighting off=val,...   --raw-settings off=val,...\n"
            "\tOverwrite payload bytes by offset after every other field is\n"
            "\tapplied. Offsets match --dump-payload: fragment N byte M is\n"
            "\toffset (N-1)*64+M. Use with --dump-payload first.\n"
            " --reset-cache\n"
            "\tPut ~/.gloriousctl_state.bin back to defaults. Sends nothing.\n"
            "\tEvery run edits this cache and transmits the whole payload, so\n"
            "\tvalues from earlier commands persist until they are overwritten.\n"
            "\n"
        );

    fprintf(stderr, "Supported mice:\n");
    for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
        struct supported_device dev = supported_devices[i];
        fprintf(stderr, " - %s (VID %04x PID %04x)%s\n", dev.name, dev.vid, dev.pid,
                dev.proto == PROTO_UNSUPPORTED ? " [detected, read-only]" : "");
    }

    return 0;
}

/* Emit the udev rules straight from supported_devices[] so permissions can
 * never drift from the table. Prints only; touches no device, needs no root.
 *
 * Install as /etc/udev/rules.d/60-glorious.rules. The name matters: systemd's
 * 73-seat-late.rules is what turns TAG+="uaccess" into an ACL
 * (TAG=="uaccess" ... RUN{builtin}+="uaccess"), and udev evaluates rule files
 * in lexical order. A 99- prefix sets the tag after that line has already run,
 * so the device ends up tagged with no ACL ever applied -- observed on this
 * hardware: CURRENT_TAGS=:uaccess: and getfacl empty. */
int print_udev_rules()
{
    printf("# Generated by 'gloriousctl --udev-rules'. Do not edit by hand.\n");
    printf("# Grants the seat-local user access so gloriousctl does not need root.\n");
    printf("# Must be installed under a name sorting before 73-seat-late.rules\n");
    printf("# (e.g. 60-glorious.rules), or the uaccess tag is set too late to\n");
    printf("# produce an ACL.\n");
    /* PROTO_UNSUPPORTED rows are included on purpose: running --probe on an
     * unknown device still requires hidraw access. */
    for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
        struct supported_device dev = supported_devices[i];
        printf("# %s\n", dev.name);
        printf("KERNEL==\"hidraw*\", ATTRS{idVendor}==\"%04x\", "
               "ATTRS{idProduct}==\"%04x\", TAG+=\"uaccess\"\n",
               dev.vid, dev.pid);
    }

    return 0;
}

/* Apply "offset=value[,offset=value...]" to a payload before it is sent.
 *
 * Offsets index the whole multi-fragment payload, matching what --dump-payload
 * prints: fragment N, byte M is offset (N-1)*64 + M. Offsets and values accept
 * any strtol base-0 form, so 0x0a and 10 are the same. Nothing is applied
 * unless every entry parses and fits, so a typo cannot half-write a payload.
 *
 * This exists because the layout came from USB captures rather than a
 * datasheet: when a documented field has no observable effect on hardware, the
 * next step is trying neighbouring offsets, and that should not need a
 * recompile per attempt. Returns 0 on success, -1 on a bad spec. */
static int pixart_apply_raw(const char *spec, void *payload, size_t size,
                            const char *label)
{
    long offs[64], vals[64];
    int n = 0;
    const char *p = spec;

    while (*p) {
        char *end;
        long off, val;

        off = strtol(p, &end, 0);
        if (end == p || *end != '=') {
            fprintf(stderr, "Malformed --raw-%s entry at '%s' (want offset=value).\n",
                    label, p);
            return -1;
        }
        p = end + 1;
        val = strtol(p, &end, 0);
        if (end == p) {
            fprintf(stderr, "Malformed --raw-%s value at '%s'.\n", label, p);
            return -1;
        }
        p = end;
        if (*p == ',') p++;
        else if (*p) {
            fprintf(stderr, "Unexpected character in --raw-%s at '%s'.\n", label, p);
            return -1;
        }

        if (off < 0 || (size_t)off >= size) {
            fprintf(stderr, "--raw-%s offset %ld is outside the %zu-byte payload.\n",
                    label, off, size);
            return -1;
        }
        if (val < 0 || val > 0xff) {
            fprintf(stderr, "--raw-%s value %ld is not a byte (0-255).\n", label, val);
            return -1;
        }
        if (n == 64) {
            fprintf(stderr, "--raw-%s takes at most 64 entries.\n", label);
            return -1;
        }
        offs[n] = off; vals[n] = val; n++;
    }

    if (n == 0) {
        fprintf(stderr, "--raw-%s was given nothing to apply.\n", label);
        return -1;
    }

    for (int i = 0; i < n; i++) {
        uint8_t *bytes = (uint8_t *)payload;
        long off = offs[i];
        printf("raw %s: [%ld] (fragment %ld byte %ld) 0x%02x -> 0x%02lx%s\n",
               label, off, off / 64 + 1, off % 64, bytes[off], vals[i],
               (off % 64) < 4 ? "  <-- fragment header byte" : "");
        bytes[off] = (uint8_t)vals[i];
    }
    return 0;
}

/* Print a payload exactly as it would go on the wire, one 64-byte fragment per
 * block, and send nothing. Diffing this against a USB capture of the vendor
 * software is the only way to settle whether a field is mis-mapped, since
 * Pixart offers no read path to compare against. */
static void pixart_dump_payload(const char *label, const void *payload, size_t total_size)
{
    size_t chunks = total_size / 64;
    const uint8_t *bytes = (const uint8_t *)payload;

    printf("\n%s -- %zu bytes, %zu fragments of 64\n", label, total_size, chunks);
    for (size_t i = 0; i < chunks; i++) {
        char title[80];
        snprintf(title, sizeof(title), "  fragment %zu/%zu (report ID 0x%02x)",
                 i + 1, chunks, bytes[i * 64]);
        hexDump(title, bytes + (i * 64), 64);
    }
}

// Safely chunks our massive structs into 64-byte packets and sends them in sequence
static int pixart_send_payload(hid_device *dev, void *payload, size_t total_size)
{
    int chunks = total_size / 64;
    uint8_t *byte_ptr = (uint8_t *)payload;
    int sent_total = 0;

    for (int i = 0; i < chunks; i++) {
        // hid_send_feature_report requires the first byte of the buffer to be the Report ID.
        // Because we built our structs with report_id (0x03) at the exact 64-byte boundaries,
        // we can just pass a pointer to the start of each fragment directly!

        int res = hid_send_feature_report(dev, byte_ptr + (i * 64), 64);

        if (res < 0) {
            char msg[80];
            snprintf(msg, sizeof(msg), "Failed to send payload fragment %d/%d",
                     i + 1, chunks);
            print_hid_error(dev, msg);
            fprintf(stderr, "%d of %d bytes were accepted before the failure; "
                            "the device is now in a partially written state.\n",
                    sent_total, chunks * 64);
            return -1;
        }

        /* Pause between fragments. Empirical, from a Model I 2 Wireless:
         * with the fragments sent back-to-back, the 4-fragment settings
         * payload only ever half-landed -- one run changed just stage 1
         * (fragment 1), the next changed just stage 4 (fragment 3), and the
         * stages in fragment 2 never changed at all. That pattern is single
         * fragments surviving out of a burst, which points at the wireless
         * firmware being busy storing one fragment while the next arrives.
         * The dongle ACKs either way, so the drop is invisible to hidapi.
         * 50ms made most runs land but one later test still needed three
         * attempts before fragment 3 stuck, so it was raised to 120ms. Cost
         * is under half a second per command. */
        if (i + 1 < chunks)
            usleep(PIXART_FRAGMENT_GAP_MS * 1000);

        /* hidapi returns the byte count it wrote, report ID included, so a
         * whole fragment is exactly 64. Anything smaller is a short write.
         * The old code tested only res < 0 and then printed chunks * 64 --
         * a figure taken from the struct, never from the device -- so a short
         * or discarded write reported as complete success. */
        sent_total += res;
        if (res != 64) {
            fprintf(stderr, "Short write on fragment %d/%d: device took %d of 64 bytes "
                            "(%d of %d total).\n",
                    i + 1, chunks, res, sent_total, chunks * 64);
            return -1;
        }
    }

    printf("Device accepted %d of %d bytes.\n", sent_total, chunks * 64);
    /* Transport-level only. There is no Feature report to read back, so an
     * accepted write is not evidence that the settings took effect. */
    printf("(Transport acknowledgement only -- Pixart has no read path, so this "
           "does not confirm the mouse applied the config.)\n");
    /* Same busy-drop protection for the gap between two payloads: a combined
     * --set-effect + --set-dpi run sends lighting then settings, and the
     * settings' first fragment must not collide with the lighting store. */
    usleep(PIXART_FRAGMENT_GAP_MS * 1000);
    return 0;
}

/* Under sudo, HOME is /root: a privileged --set-* wrote /root/.gloriousctl_state.bin
 * while an unprivileged --info read the user's copy, and the two silently
 * disagreed. Observed directly. Prefer the invoking user whenever SUDO_USER is
 * set, so both paths land on one file. */
static void get_state_path(char *path_buffer, size_t max_len) {
    const char *homedir = NULL;
    const char *sudo_user = getenv("SUDO_USER");

    if (sudo_user && *sudo_user) {
        struct passwd *pw = getpwnam(sudo_user);
        if (pw) homedir = pw->pw_dir;
    }
    if (!homedir) homedir = getenv("HOME");
    if (!homedir) {
        struct passwd *pw = getpwuid(getuid());
        if (pw) homedir = pw->pw_dir;
    }
    if (!homedir) homedir = ".";

    snprintf(path_buffer, max_len, "%s/.gloriousctl_state.bin", homedir);
}

/* A file root just wrote into the user's home stays root-owned, which breaks
 * the next non-sudo run. Hand it back. */
static void fixup_state_owner(const char *path) {
    const char *sudo_user = getenv("SUDO_USER");
    struct passwd *pw;

    if (geteuid() != 0 || !sudo_user || !*sudo_user) return;
    pw = getpwnam(sudo_user);
    if (!pw) return;
    if (chown(path, pw->pw_uid, pw->pw_gid) != 0)
        fprintf(stderr, "Warning: could not give %s back to %s\n", path, sudo_user);
}

static void load_state(struct glorious_state *state) {
    char path[512];
    get_state_path(path, sizeof(path));

    int loaded = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        /* A short read, a bad magic or a version bump are all treated the same
         * as a missing file: fall through to defaults rather than transmit
         * whatever happened to be in the buffer. */
        if (fread(state, sizeof(struct glorious_state), 1, f) == 1 &&
            state->magic == STATE_MAGIC &&
            state->version == STATE_VERSION) {
            loaded = 1;
        } else {
            printf("Config at %s is unreadable or from an older version. Ignoring it.\n", path);
        }
        fclose(f);
    }

    if (!loaded) {
        printf("No usable config found. Generating safe factory defaults...\n");
        memset(state, 0, sizeof(struct glorious_state));
        state->magic = STATE_MAGIC;
        state->version = STATE_VERSION;

        // Setup Lighting Defaults (Glorious Mode)
        state->lighting.report_id1 = 0x03; state->lighting.cmd1[0] = 0x02; state->lighting.cmd1[1] = 0xfb; state->lighting.seq1 = 0x00;
        state->lighting.report_id2 = 0x03; state->lighting.cmd2[0] = 0x02; state->lighting.cmd2[1] = 0xfb; state->lighting.seq2 = 0x01;
        state->lighting.report_id3 = 0x03; state->lighting.cmd3[0] = 0x02; state->lighting.cmd3[1] = 0xfb; state->lighting.seq3 = 0x02;
        state->lighting.effect_id = PIXART_RGB_GLORIOUS;
        state->lighting.brightness_w = 0x14; // 100%
        state->lighting.brightness_wl = 0x14; // 100%
        state->lighting.brightness_master = 0x14; // [10], the one that acts
        state->lighting.speed = 0x0a; // 50%
        /* memset left color_count at 0 and every colour black. 0 is outside the
         * documented 0x02/0x07 range, and an all-black palette is a poor
         * default for an effect that cycles colours. The count matches the
         * default GLORIOUS (cycle) effect; the seven colours below are chosen,
         * not captured -- replace them if a USB capture shows what CORE sends. */
        state->lighting.color_count = 0x07;
        state->lighting.main_color = int_to_rgb(0xFF0000);
        state->lighting.color_array_1[0] = int_to_rgb(0xFF7F00);
        state->lighting.color_array_1[1] = int_to_rgb(0xFFFF00);
        state->lighting.color_array_1[2] = int_to_rgb(0x00FF00);
        state->lighting.color_array_1[3] = int_to_rgb(0x00FFFF);
        state->lighting.color_array_1[4] = int_to_rgb(0x0000FF);
        state->lighting.color_array_1[5] = int_to_rgb(0xFF00FF);

        // Setup Settings Defaults (4 Stages: 400, 800, 1600, 3200)
        state->settings.report_id1 = 0x03; state->settings.cmd1[0] = 0x04; state->settings.cmd1[1] = 0xfb; state->settings.seq1 = 0x00;
        state->settings.report_id2 = 0x03; state->settings.cmd2[0] = 0x04; state->settings.cmd2[1] = 0xfb; state->settings.seq2 = 0x01;
        state->settings.report_id3 = 0x03; state->settings.cmd3[0] = 0x04; state->settings.cmd3[1] = 0xfb; state->settings.seq3 = 0x02;
        state->settings.report_id4 = 0x03; state->settings.cmd4[0] = 0x04; state->settings.cmd4[1] = 0xfb; state->settings.seq4 = 0x03;

        state->settings.active_stage = 0x00; // Stage 1
        state->settings.total_stages = 0x04;
        /* 0x04 = 1000 Hz on the measured linear scale (0x01 was 125 Hz, which
         * the old datasheet-derived default silently ran the mouse at). 0x04
         * itself is inferred from the pattern; an unknown value is harmless
         * because the firmware keeps its stored rate. */
        state->settings.polling = 0x04;
        state->settings.debounce = 0x0a; // 10ms
        state->settings.lod = 0x01; // 1mm

        /* Default colours first, DPIs second: the blue byte of stages 2 and 4
         * is the DPI low byte of stages 3 and 5, and DPI must win. */
        static const unsigned int def_rgb[4] = { 0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00 };  /* stages 2 and 4 avoid blue: their blue byte is shared */
        static const uint16_t def_dpi[4] = { 400, 800, 1600, 3200 };
        for (int i = 0; i < 4; i++)
            pixart_stage_set_color(&state->settings, i, int_to_rgb(def_rgb[i]));
        for (int i = 0; i < 4; i++)
            pixart_stage_set_dpi(&state->settings, i, def_dpi[i]);
    }
}

static void save_state(struct glorious_state *state) {
    char path[512];
    get_state_path(path, sizeof(path));

    /* Stamp unconditionally -- the header must always match what this binary
     * writes, whatever the in-memory struct came from. */
    state->magic = STATE_MAGIC;
    state->version = STATE_VERSION;

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(state, sizeof(struct glorious_state), 1, f);
        fclose(f);
        fixup_state_owner(path);
    } else {
        fprintf(stderr, "Warning: Could not save state to %s\n", path);
    }
}

/* ---------- Pixart backend ---------- */

/* Every run edits the cache and transmits the whole payload, so an experiment
 * only means something if you know what it started from. This puts it back to
 * a known state without touching the mouse. */
static int pixart_reset_cache(void)
{
    char path[512];
    struct glorious_state state;

    get_state_path(path, sizeof(path));
    if (unlink(path) != 0 && errno != ENOENT) {
        fprintf(stderr, "Could not remove %s: %s\n", path, strerror(errno));
        return 1;
    }
    load_state(&state);   /* file is gone, so this regenerates the defaults */
    save_state(&state);
    printf("Cache reset to defaults at %s. Nothing was sent to the mouse.\n", path);
    return 0;
}

/* Machine-readable cache dump for scripts and the GUI. One key=value per
 * line, colours as RRGGBB exactly as they will be sent (shared bytes included). */
static int pixart_state(void)
{
    struct glorious_state state;
    load_state(&state);
    printf("stages=%d\n", state.settings.total_stages);
    printf("active=%d\n", state.settings.active_stage + 1);
    printf("polling=%d\n", pixart_polling_hz(state.settings.polling));
    printf("debounce=%d\n", state.settings.debounce);
    printf("lod=%d\n", state.settings.lod);
    for (int i = 0; i < PIXART_MAX_STAGES; i++) {
        RGB8 c = pixart_stage_get_color(&state.settings, i);
        printf("dpi%d=%d\n", i + 1, pixart_stage_get_dpi(&state.settings, i));
        printf("color%d=%02X%02X%02X\n", i + 1, c.r, c.g, c.b);
        printf("shared_blue%d=%d\n", i + 1, pixart_stage_blue_is_shared(i));
    }
    printf("effect=%s\n", pixart_effect_name(state.lighting.effect_id));
    printf("effect_id=%d\n", state.lighting.effect_id);
    /* Reverse of the 1-4 -> 0x05/0x0a/0x0f/0x14 map in pixart_set; 0 when the
     * byte holds something the CLI never writes (raw poke or older cache). */
    {
        int b = state.lighting.brightness_master, level = 0;
        if (b == 0x05) level = 1; else if (b == 0x0a) level = 2;
        else if (b == 0x0f) level = 3; else if (b == 0x14) level = 4;
        printf("brightness=%d\n", level);
        printf("brightness_raw=%d\n", b);
        int sp = state.lighting.speed, slevel = -1;
        if (sp == 0x05) slevel = 0; else if (sp == 0x0a) slevel = 1;
        else if (sp == 0x0f) slevel = 2; else if (sp == 0x14) slevel = 3;
        printf("speed=%d\n", slevel);
    }
    printf("color_count=%d\n", state.lighting.color_count);
    {
        RGB8 c = state.lighting.main_color;
        printf("effect_color1=%02X%02X%02X\n", c.r, c.g, c.b);
        for (int i = 0; i < 6; i++) {
            c = state.lighting.color_array_1[i];
            printf("effect_color%d=%02X%02X%02X\n", i + 2, c.r, c.g, c.b);
        }
    }
    return 0;
}

/* Pixart is write-only, so --info reports the local cache rather than the
 * device. It needs no open handle. */
static
int pixart_info(void)
{
    struct glorious_state state;
    load_state(&state); // Pulls from your hard drive, not the mouse!

    printf("\n====================================\n");
    printf("  CACHED MOUSE SETTINGS (LINUX)       \n");
    printf("====================================\n");

    int hz = pixart_polling_hz(state.settings.polling);

    printf("Polling Rate:      %d Hz\n", hz);
    printf("Debounce Time:     %d ms\n", state.settings.debounce);
    printf("Lift-Off Distance: %d mm\n", state.settings.lod);
    printf("Active DPI Stage:  %d (Total: %d)\n", state.settings.active_stage + 1, state.settings.total_stages);

    printf("\n--- DPI Stages ---\n");
    {
        int shown = state.settings.total_stages;
        if (shown < 4) shown = 4;
        if (shown > PIXART_MAX_STAGES) shown = PIXART_MAX_STAGES;
        int shared_note = 0;
        for (int i = 0; i < shown; i++) {
            printf("Stage %d: %d DPI\t", i + 1, pixart_stage_get_dpi(&state.settings, i));
            print_color(pixart_stage_get_color(&state.settings, i));
            if (pixart_stage_blue_is_shared(i) && i + 1 < shown) {
                printf("  *");
                shared_note = 1;
            }
            printf("\n");
        }
        if (shared_note)
            printf("  * blue channel is the same wire byte as the next stage's DPI low byte;\n"
                   "    the colour shown is what the mouse actually receives.\n");
    }

    /* The lighting block was missing here, and its absence is what made the
     * colour results look order-dependent: every run loads this cache, edits
     * only the fields it was given and transmits the whole payload, so a
     * colour left over from an earlier command still ships. Printing it makes
     * the carry-over visible instead of surprising. */
    printf("\n--- Lighting (raw payload values) ---\n");
    printf("Effect:            %s (0x%02x)\n",
           pixart_effect_name(state.lighting.effect_id), state.lighting.effect_id);
    printf("Brightness:        wired 0x%02x / wireless 0x%02x\n",
           state.lighting.brightness_w, state.lighting.brightness_wl);
    printf("Speed:             0x%02x\n", state.lighting.speed);
    printf("Brightness [10]:   0x%02x  (the byte the mouse acts on)\n",
           state.lighting.brightness_master);
    printf("Colour count:      %d\n", state.lighting.color_count);
    printf("Main colour:       "); print_color(state.lighting.main_color); printf("\n");
    for (int i = 0; i < 6; i++) {
        printf("Cycle colour %d:    ", i + 2);
        print_color(state.lighting.color_array_1[i]);
        printf("\n");
    }
    printf("====================================\n");

    // Re-save it just to ensure the file exists for future writes
    save_state(&state);
    return 0;
}

/* Receives an already-open handle; main owns open/close. `matched` carries the
 * per-device sensor limits used to validate --set-dpi. */
/* dump_only: build the payloads, print them, transmit nothing and leave the
 * cache alone. In that mode dev may be NULL and matched may be NULL -- neither
 * is dereferenced on any path that dumping reaches. */
static
int pixart_set(hid_device *dev, const struct supported_device *matched,
               const struct set_opts *opts, int dump_only)
{
    struct glorious_state state;
    load_state(&state);
    int settings_changed = 0;
    int lighting_changed = 0;

    // --- CACHE SELF-HEAL ---
    // Automatically fixes any missing padding bytes or desynced effect IDs
    state.lighting.unk1 = 0x01; state.lighting.unk2 = 0x01; state.lighting.unk3 = 0x01;
    state.settings.unk_pad1 = 0x01;
    /* Only fragment 1 has a 0x01 byte after its header. The datasheet's pads
     * at [68], [132] and [196] are the DPI low bytes of stages 2, 4 and 6
     * (crawl-tested) and must not be "healed". */
    state.lighting.effect_id2 = state.lighting.effect_id;
    state.lighting.effect_id3 = state.lighting.effect_id;
    /* Nothing heals [10] any more. It was pinned to 0x14 on every run, which
     * silently reset brightness to maximum; then it was healed only when zero,
     * which resurrected a deliberate --set-brightness 0 on the next command.
     * load_state seeds it, so an unwritten cache is never 0 by accident. */
    /* The struct's own note says this field is 0x02 or 0x07; caches written by
     * earlier builds hold 0 or 1, neither of which is documented. Lift it to
     * the lower documented value rather than transmit an undocumented one.
     * Provenance: the 0x02/0x07 note came from USB captures, not a datasheet,
     * so other counts may well be legal -- this is a lead, not a spec. */
    if (state.lighting.color_count < 0x02)
        state.lighting.color_count = 0x02;

    if (opts->debounce) {
        int val = atoi(opts->debounce);
        val = clamp(val, 2, 16);
        if (val % 2 != 0) val -= 1;
        state.settings.debounce = val;
        settings_changed = 1;
        printf("Set Debounce Time: %d ms\n", val);
    }

    /* Colours before DPI on purpose: stage 2's and stage 4's blue byte is the
     * DPI low byte of stages 3 and 5 (see pixart_stage_off), and DPI wins. */
    uint16_t cached_dpi[PIXART_MAX_STAGES];
    for (int i = 0; i < PIXART_MAX_STAGES; i++)
        cached_dpi[i] = pixart_stage_get_dpi(&state.settings, i);

    if (opts->dpi_color) {
        unsigned int colors[6] = {0};
        int num = sscanf(opts->dpi_color, "%x,%x,%x,%x,%x,%x",
                         &colors[0], &colors[1], &colors[2], &colors[3], &colors[4], &colors[5]);
        for (int i = 0; i < num && i < PIXART_MAX_STAGES; i++) {
            RGB8 rgb = int_to_rgb(colors[i]);
            pixart_stage_set_color(&state.settings, i, rgb);
            /* This stage's blue byte is the next stage's DPI low byte on the
             * wire. Colour is applied before --set-dpi so an explicit DPI
             * still wins; without --set-dpi in the same run, re-assert the
             * cached DPI so a colour change never silently moves the sensor. */
            if (pixart_stage_blue_is_shared(i) && !opts->dpi) {
                pixart_stage_set_dpi(&state.settings, i + 1, cached_dpi[i + 1]);
                if (rgb.b != (cached_dpi[i + 1] & 0xFF))
                    printf("Note: stage %d blue shares a byte with stage %d DPI; "
                           "blue sent as 0x%02x, not 0x%02x.\n",
                           i + 1, i + 2, cached_dpi[i + 1] & 0xFF, rgb.b);
            }
        }
        settings_changed = 1;
        printf("Set DPI Colors updated.\n");
    }

    if (opts->dpi) {
        int dpis[6] = {0};
        int num = sscanf(opts->dpi, "%d,%d,%d,%d,%d,%d",
                         &dpis[0], &dpis[1], &dpis[2], &dpis[3], &dpis[4], &dpis[5]);
        if (num > 0) {
            /* Seeded from the cache, not zeroed: `--set-dpi 4200` used to
             * repeat the last supplied value into every remaining stage, so
             * one argument rewrote all four stages to 4200. Only the stages
             * actually named are overwritten now. */
            uint16_t enc[6];
            for (int i = 0; i < PIXART_MAX_STAGES; i++)
                enc[i] = pixart_stage_get_dpi(&state.settings, i);
            /* Both zero means the sensor's limits are unknown for this device;
             * skip the range check rather than guess. */
            int have_bounds = (matched && matched->dpi_min > 0 && matched->dpi_max > 0);
            for (int i = 0; i < num; i++) {
                if (dpis[i] < PIXART_DPI_STEP) {
                    fprintf(stderr, "DPI too low to encode (minimum %d): %d\n",
                            PIXART_DPI_STEP, dpis[i]);
                    return 1;
                }
                if (have_bounds &&
                    (dpis[i] < matched->dpi_min || dpis[i] > matched->dpi_max)) {
                    fprintf(stderr,
                        "DPI out of range for %s (%d-%d): %d\n"
                        "These bounds come from the published specs and are not "
                        "confirmed against firmware. If your mouse supports this "
                        "value, please open an issue.\n",
                        matched->name, matched->dpi_min, matched->dpi_max, dpis[i]);
                    return 1;
                }
                if (dpis[i] % PIXART_DPI_STEP != 0) {
                    printf("Note: %d DPI is not a multiple of %d; using %d.\n",
                           dpis[i], PIXART_DPI_STEP,
                           (dpis[i] / PIXART_DPI_STEP) * PIXART_DPI_STEP);
                }
                /* Wire format is the raw DPI value (LE), not DPI/50 --
                 * confirmed on a Model I 2 Wireless; see PIXART_DPI_STEP.
                 * Rounding to the step is kept as a granularity hint only. */
                enc[i] = (dpis[i] / PIXART_DPI_STEP) * PIXART_DPI_STEP;
            }

            /* Device appears to require 4-6 stages (captures of Glorious CORE
             * never offer fewer -- unverified against firmware). Keep whatever
             * the cache already had, only widening if the user named more
             * stages than are currently configured. */
            int stages = state.settings.total_stages;
            if (stages < 4) stages = 4;
            if (num > stages) stages = num;
            if (stages > 6) stages = 6;

            /* A stage that has never been set holds 0, which encodes as 0 DPI.
             * Fill those from the last named value rather than shipping zeros. */
            for (int i = 0; i < stages; i++) {
                if (enc[i] == 0) enc[i] = enc[num - 1];
            }

            for (int i = 0; i < stages; i++)
                pixart_stage_set_dpi(&state.settings, i, enc[i]);
            state.settings.total_stages = stages;
            settings_changed = 1;

            if (num < stages)
                printf("Set DPI: stages 1-%d updated; stages %d-%d kept "
                       "(%d total).\n", num, num + 1, stages, stages);
            else
                printf("Set DPI: stages 1-%d updated (%d total).\n", num, stages);
        }
    }

    if (opts->polling) {
        int hz = atoi(opts->polling);
        uint8_t code = 0;
        for (uint8_t c = 0x01; c <= 0x04; c++)
            if (pixart_polling_hz(c) == hz) code = c;
        if (!code) {
            fprintf(stderr, "Polling rate must be 125, 250, 500 or 1000: %s\n", opts->polling);
            return 1;
        }
        state.settings.polling = code;
        settings_changed = 1;
        printf("Set Polling Rate: %d Hz%s\n", hz,
               hz == 1000 ? " (browser meters may show 500-1000; measure with evhz)" : "");
    }

    if (opts->lod) {
        int mm = atoi(opts->lod);
        if (mm < 1 || mm > 2) {
            fprintf(stderr, "Lift-off distance must be 1 or 2: %s\n", opts->lod);
            return 1;
        }
        state.settings.lod = (uint8_t)mm;
        settings_changed = 1;
        printf("Set Lift-Off Distance: %d mm\n", mm);
    }

    if (opts->stages) {
        int n = atoi(opts->stages);
        /* 4-6: the firmware's button cycle switched live between 4 and 6
         * on a Model I 2 Wireless; fewer than 4 was never offered by Glorious
         * CORE and is untested, so it is refused rather than guessed. */
        if (n < 4 || n > PIXART_MAX_STAGES) {
            fprintf(stderr, "Stage count out of range (4-%d): %s\n",
                    PIXART_MAX_STAGES, opts->stages);
            return 1;
        }
        state.settings.total_stages = n;
        if (state.settings.active_stage >= n)
            state.settings.active_stage = 0;
        settings_changed = 1;
        printf("Set DPI stage count: %d\n", n);
    }

    if (opts->active_stage) {
        int st = atoi(opts->active_stage);
        if (st < 1 || st > 6) {
            fprintf(stderr, "Active stage out of range (1-6): %s\n", opts->active_stage);
            return 1;
        }
        if (st > state.settings.total_stages) {
            fprintf(stderr, "Stage %d is above the configured stage count (%d).\n",
                    st, state.settings.total_stages);
            return 1;
        }
        state.settings.active_stage = st - 1;   /* field is zero-indexed */
        settings_changed = 1;
        printf("Set Active DPI Stage: %d\n", st);
    }

    if (opts->effect) {
        /* Seed with the cached effect so the "Keeping current" path below
         * actually keeps it -- this used to fall through to GLORIOUS. */
        uint8_t effect = state.lighting.effect_id;
        int known = 1;
        if (!strcmp(opts->effect, "off")) effect = PIXART_RGB_OFF;
        else if (!strcmp(opts->effect, "glorious")) effect = PIXART_RGB_GLORIOUS;
        else if (!strcmp(opts->effect, "seamless")) effect = PIXART_RGB_SEAMLESS_BREATHING;
        else if (!strcmp(opts->effect, "breathing")) effect = PIXART_RGB_BREATHING;
        else if (!strcmp(opts->effect, "normally_on")) effect = PIXART_RGB_NORMALLY_ON;
        else if (!strcmp(opts->effect, "breathing_single")) effect = PIXART_RGB_BREATHING_SINGLE;
        else if (!strcmp(opts->effect, "tail")) effect = PIXART_RGB_TAIL;
        else if (!strcmp(opts->effect, "rave")) effect = PIXART_RGB_RAVE;
        else if (!strcmp(opts->effect, "wave")) effect = PIXART_RGB_WAVE;
        else { printf("Unknown effect '%s'! Keeping current.\n", opts->effect); known = 0; }

        /* A typo must not claim success or trigger a transmit. */
        if (known) {
            state.lighting.effect_id = effect;
            state.lighting.effect_id2 = effect; // Synchronize across packets
            state.lighting.effect_id3 = effect; // Synchronize across packets

            lighting_changed = 1;
            printf("Set Lighting Effect: %s\n", opts->effect);
        }
    }

    if (opts->colors && strlen(opts->colors) > 0) {
        unsigned int colors[7] = {0};
        int num = sscanf(opts->colors, "%x,%x,%x,%x,%x,%x,%x",
                         &colors[0], &colors[1], &colors[2], &colors[3], &colors[4], &colors[5], &colors[6]);
        if (num > 0) {
            /* The datasheet documents color_count as "0x02 or 0x07", but
             * count=1 is accepted: confirmed on a Model I 2 Wireless, where
             * --raw-lighting 8=0x01 with one colour gave clean single-colour
             * breathing, while the old forced count of 2 made the firmware
             * cycle in a second colour from its own stored profile (the
             * ghost blue). The count now always matches what the user gave. */
            state.lighting.color_count = num;

            /* Every slot is rewritten, cycling through what the user gave.
             * Partial fills left older colours in the untouched slots and the
             * mouse cycled them anyway -- that is where the stray blue and
             * green between breathing cycles came from. Repeating the list
             * makes the visible cycle equal the requested colours whether or
             * not the firmware honours color_count. */
            state.lighting.main_color = int_to_rgb(colors[0]);
            for (int i = 0; i < 6; i++)
                state.lighting.color_array_1[i] = int_to_rgb(colors[(i + 1) % num]);

            lighting_changed = 1;
            printf("Set Effect Colors: %d colors updated (color_count=%d).\n",
                   num, state.lighting.color_count);
        }
    }

    if (opts->brightness) {
        int br = atoi(opts->brightness);
        /* Was a silent clamp: --set-brightness 9 printed "4/4" with no sign the
         * value had been replaced. Validation happens before any device write,
         * so returning here leaves nothing half-applied. */
        if (br < 1 || br > 4) {
            /* 0 is refused, not mapped: observed on a Model I 2 Wireless that a
             * zeroed [10] (and zeroed [6]/[7]) makes the firmware discard the
             * payload and fall back to its stored default profile -- it does
             * not mean "LEDs off". 0x01 in [10] dims correctly with the chosen
             * colour kept, so the working scale is 0x01-0x14. */
            fprintf(stderr, "Brightness out of range (1-4): %s\n", opts->brightness);
            if (br == 0)
                fprintf(stderr, "0 makes the firmware revert to its stored default "
                                "profile. To turn the LEDs off use --set-effect off.\n");
            return 1;
        }
        int br_map[] = {0x00, 0x05, 0x0a, 0x0f, 0x14}; // index 0 unreachable; 1-4 used
        /* [10] is the byte the hardware acts on; [6]/[7] are written with the
         * same value because the captures did, not because they were observed
         * to do anything. */
        state.lighting.brightness_master = br_map[br];
        state.lighting.brightness_w = br_map[br];
        state.lighting.brightness_wl = br_map[br];
        lighting_changed = 1;
        printf("Set Brightness: %d/4\n", br);
    }

    if (opts->speed) {
        int sp = atoi(opts->speed);
        /* Was a silent clamp: --set-speed 4 printed "Set Speed: 3/3". */
        if (sp < 0 || sp > 3) {
            fprintf(stderr, "Speed out of range (0-3): %s\n", opts->speed);
            return 1;
        }
        int sp_map[] = {0x05, 0x0a, 0x0f, 0x14}; // Exact hex mapping
        state.lighting.speed = sp_map[sp];
        lighting_changed = 1;
        printf("Set Speed: %d/3\n", sp);
    }

    /* Applied last so a poke always wins over the field that owns that byte. */
    if (opts->raw_lighting) {
        if (pixart_apply_raw(opts->raw_lighting, &state.lighting,
                             sizeof(state.lighting), "lighting") < 0)
            return 1;
        lighting_changed = 1;
    }
    if (opts->raw_settings) {
        if (pixart_apply_raw(opts->raw_settings, &state.settings,
                             sizeof(state.settings), "settings") < 0)
            return 1;
        settings_changed = 1;
    }

    if (!lighting_changed && !settings_changed) {
        if (!dump_only) {
            printf("Nothing to change.\n");
            return 0;
        }
        /* --dump-payload with no --set-*: show what the cache would send. */
        lighting_changed = settings_changed = 1;
    }

    if (dump_only) {
        if (lighting_changed)
            pixart_dump_payload("LIGHTING payload (0x02 0xfb)",
                                &state.lighting, sizeof(state.lighting));
        if (settings_changed)
            pixart_dump_payload("SETTINGS/DPI payload (0x04 0xfb)",
                                &state.settings, sizeof(state.settings));
        printf("\n--dump-payload: nothing was transmitted; the cache is unchanged.\n");
        return 0;
    }

    int rc = 0;
    if (lighting_changed) {
        printf("\nWriting Lighting Config to mouse...\n");
        if (pixart_send_payload(dev, &state.lighting, sizeof(state.lighting)) < 0) rc = 1;
    }
    if (settings_changed) {
        printf("\nWriting Settings/DPI Config to mouse...\n");
        if (pixart_send_payload(dev, &state.settings, sizeof(state.settings)) < 0) rc = 1;
    }

    save_state(&state);
    if (rc) {
        fprintf(stderr, "\nOne or more payloads failed to transmit.\n");
    } else {
        printf("\nConfiguration applied and saved locally. Done!\n");
    }
    return rc;
}

/* ---------- SinoWealth backend ---------- */

static
int sinowealth_info(hid_device *dev)
{
    if(sw_print_firmware_version(dev) < 0) {
        return 1;
    }

    struct sw_config *cfg = sw_read_config(dev);
    if(!cfg) {
        return 1;
    }

    int db = sw_get_debounce_time(dev);
    if(db >= 0) {
        printf("Click debounce time: %d ms\n", db);
    }
    sw_dump_config(cfg);

    free(cfg);
    return 0;
}

static
int sinowealth_set(hid_device *dev, const struct supported_device *matched,
                   const struct set_opts *opts)
{
    int dpi[SW_NUM_DPIS] = {0};
    int n_dpi = 0;

    /* Validate DPI before touching the device at all. The debounce command
     * below is a separate write that lands immediately, so validating late
     * would let a bad DPI leave the mouse half-configured. */
    if(opts->dpi) {
        n_dpi = sscanf(opts->dpi, "%d,%d,%d,%d,%d,%d",
                       &dpi[0], &dpi[1], &dpi[2], &dpi[3], &dpi[4], &dpi[5]);
        if(n_dpi < 0) n_dpi = 0;                          /* sscanf can return EOF */
        if(n_dpi > SW_NUM_DPIS) n_dpi = SW_NUM_DPIS;

        /* Both zero means this device's sensor limits are unknown; the
         * protocol limits above still apply. Same rule as the Pixart path. */
        int have_bounds = (matched->dpi_min > 0 && matched->dpi_max > 0);
        for(int i = 0; i < n_dpi; i++) {
            if(dpi[i] < SW_DPI_MIN || dpi[i] > SW_DPI_MAX) {
                fprintf(stderr, "DPI outside encodable range (%d-%d): %d\n",
                        SW_DPI_MIN, SW_DPI_MAX, dpi[i]);
                return 1;
            }
            if(have_bounds &&
               (dpi[i] < matched->dpi_min || dpi[i] > matched->dpi_max)) {
                fprintf(stderr,
                    "DPI out of range for %s (%d-%d): %d\n"
                    "These bounds come from the published specs and are not "
                    "confirmed against firmware. If your mouse supports this "
                    "value, please open an issue.\n",
                    matched->name, matched->dpi_min, matched->dpi_max, dpi[i]);
                return 1;
            }
            if(dpi[i] % SW_DPI_STEP != 0) {
                printf("Note: %d DPI is not a multiple of %d; using %d.\n",
                       dpi[i], SW_DPI_STEP,
                       (dpi[i] / SW_DPI_STEP) * SW_DPI_STEP);
            }
        }
    }

    struct sw_config *cfg = sw_read_config(dev);
    if(!cfg) {
        return 1;
    }

    if(opts->debounce) {
        unsigned int ms = 0;
        sscanf(opts->debounce, "%u", &ms);
        if(ms < 4 || ms > 16) {
            fprintf(stderr, "Debounce time out of range (4-16 ms): %u\n", ms);
            free(cfg);
            return 1;
        }
        uint8_t cmd[6] = {SW_REPORT_ID_CMD, SW_CMD_DEBOUNCE, ms / 2};
        if(hid_send_feature_report(dev, cmd, sizeof(cmd)) != sizeof(cmd)) {
            print_hid_error(dev, "set debounce time");
            free(cfg);
            return 1;
        }
        printf("Set debounce time: %u ms\n", ms);
    }

    if(opts->dpi) {
        cfg->dpi_enabled = 0xff;
        cfg->dpi_count = 0;
        for(int i = 0; i < n_dpi; i++) {
            cfg->dpi[i] = sw_dpi_to_config(dpi[i]);
            cfg->dpi_enabled &= ~(1U << i);
            cfg->dpi_count++;
        }
    }

    if(opts->dpi_color) {
        unsigned int c[SW_NUM_DPIS] = {0};
        int n = sscanf(opts->dpi_color, "%x,%x,%x,%x,%x,%x",
                       &c[0], &c[1], &c[2], &c[3], &c[4], &c[5]);
        if(n < 0) n = 0;
        if(n > SW_NUM_DPIS) n = SW_NUM_DPIS;
        for(int i = 0; i < n; i++) {
            cfg->dpi_color[i] = int_to_rgb(c[i]);
        }
    }

    if(opts->effect) {
        uint8_t effect = 0;
        if(sw_name_to_effect(opts->effect, &effect) < 0) {
            printf("Unknown effect '%s'! Keeping current.\n", opts->effect);
        } else {
            cfg->rgb_effect = effect;
        }

        unsigned int col[7] = {0};
        int n = 0;
        if(opts->colors) {
            n = sscanf(opts->colors, "%x,%x,%x,%x,%x,%x,%x",
                       &col[0], &col[1], &col[2], &col[3], &col[4], &col[5], &col[6]);
            if(n < 0) n = 0;
            if(n > 7) n = 7;
        }

        switch(cfg->rgb_effect) {
        case SW_RGB_SINGLE:
            cfg->single_color = int_to_rbg(col[0]);
            break;
        case SW_RGB_BREATHING7:
            for(int i = 0; i < n; i++) {
                cfg->breathing7_colors[i] = int_to_rbg(col[i]);
            }
            break;
        case SW_RGB_BREATHING1:
            cfg->breathing1_color = int_to_rbg(col[0]);
            break;
        case SW_RGB_RAVE:
            cfg->rave_colors[0] = int_to_rbg(col[0]);
            cfg->rave_colors[1] = int_to_rbg(col[1]);   /* upstream wrote [0] twice */
            break;
        }

        unsigned int brightness = 4, speed = 3;
        if(opts->brightness) sscanf(opts->brightness, "%u", &brightness);
        if(opts->speed)      sscanf(opts->speed, "%u", &speed);
        brightness = clamp(brightness, 0, 4);
        speed = clamp(speed, 0, 3);
        uint8_t mode = speed | (brightness << 4U);

        switch(cfg->rgb_effect) {
        case SW_RGB_GLORIOUS:   cfg->glorious_mode   = mode; break;
        case SW_RGB_SINGLE:     cfg->single_mode     = mode; break;
        case SW_RGB_BREATHING7: cfg->breathing7_mode = mode; break;
        case SW_RGB_BREATHING1: cfg->breathing1_mode = mode; break;
        case SW_RGB_TAIL:       cfg->tail_mode       = mode; break;
        case SW_RGB_RAVE:       cfg->rave_mode       = mode; break;
        case SW_RGB_WAVE:       cfg->wave_mode       = mode; break;
        }
    }

    sw_dump_config(cfg);

    int rc = 0;
    if(sw_write_config(dev, cfg) < 0) {
        /* Debounce is a separate feature report that has already landed, and
         * there is no transaction spanning the two writes. Nothing can roll it
         * back, so at least don't make the user discover the split by feel. */
        if(opts->debounce) {
            fprintf(stderr, "Note: debounce was already applied; only the "
                            "config write failed. Re-run to retry the rest.\n");
        }
        rc = 1;
    }
    free(cfg);
    return rc;
}

static
int sinowealth_listen(hid_device *dev)
{
    struct sw_change_report report;
    while(1) {
        int res = hid_read_timeout(dev, (uint8_t*)&report, sizeof(report), -1);
        if(res != sizeof(report)) {
            print_hid_error(dev, "read input report");
            return 1;
        }
        printf("Active profile: %d, X DPI: %d, Y DPI: %d\n",
               report.active_dpi,
               sw_config_to_dpi(report.dpi_x),
               sw_config_to_dpi(report.dpi_y));
    }
}


int main(int argc, char* argv[])
{
    if(argc == 1)
        return print_help();

    int do_info = 0;
    int do_help = 0;
    int do_set = 0;
    int do_listen = 0;
    int do_probe_flag = 0;
    int do_udev_flag = 0;
    int do_dump_flag = 0;
    int do_reset_flag = 0;
    int do_collect_flag = 0;
    int do_state_flag = 0;

    /* All-NULL is load-bearing: non-NULL brightness/speed defaults used to make
     * every --set-* rewrite the lighting payload (Bug A). Do not "helpfully"
     * initialise these. */
    struct set_opts opts = {0};

    struct option options[] = {
        {"info", no_argument, &do_info, 1},
        {"help", no_argument, &do_help, 1},
        {"listen", no_argument, &do_listen, 1},
        {"probe", no_argument, &do_probe_flag, 1},
        {"udev-rules", no_argument, &do_udev_flag, 1},
        {"dump-payload", no_argument, &do_dump_flag, 1},
        {"reset-cache", no_argument, &do_reset_flag, 1},
        {"collect", no_argument, &do_collect_flag, 1},
        {"set-dpi", required_argument, 0, 'a'},
        {"set-dpi-color", required_argument, 0, 'b'},
        {"set-effect", required_argument, 0, 'c'},
        {"set-colors", required_argument, 0, 'd'},
        {"set-brightness", required_argument, 0, 'e'},
        {"set-speed", required_argument, 0, 'f'},
        {"set-debounce-time", required_argument, 0, 'g'},
        {"set-active-stage", required_argument, 0, 'i'},
        {"set-stages", required_argument, 0, 'l'},
        {"set-polling-rate", required_argument, 0, 'm'},
        {"set-lod", required_argument, 0, 'n'},
        {"state", no_argument, &do_state_flag, 1},
        {"raw-lighting", required_argument, 0, 'j'},
        {"raw-settings", required_argument, 0, 'k'},
        {0}
    };

    while(1) {
        int c = getopt_long(argc, argv, "", options, NULL);
        if(c == -1) break;
        if(!c) continue;
        if(c == '?') return 1;

        do_set = 1;
        switch(c) {
            case 'a': opts.dpi = optarg; break;
            case 'b': opts.dpi_color = optarg; break;
            case 'c': opts.effect = optarg; break;
            case 'd': opts.colors = optarg; break;
            case 'e': opts.brightness = optarg; break;
            case 'f': opts.speed = optarg; break;
            case 'g': opts.debounce = optarg; break;
            case 'i': opts.active_stage = optarg; break;
            case 'l': opts.stages = optarg; break;
            case 'm': opts.polling = optarg; break;
            case 'n': opts.lod = optarg; break;
            case 'j': opts.raw_lighting = optarg; break;
            case 'k': opts.raw_settings = optarg; break;
            default:;
        }
    }

    if(do_help) {
        return print_help();
    }

    if(do_udev_flag) {
        return print_udev_rules();
    }

    if(do_probe_flag) {
        return do_probe();
    }

    if(do_collect_flag) {
        return do_collect();
    }

    /* Cache-only, no device, no permissions. */
    if(do_reset_flag) {
        return pixart_reset_cache();
    }
    if(do_state_flag) {
        return pixart_state();
    }

    if(do_info || do_set || do_listen || do_dump_flag) {
        const struct supported_device *matched = NULL;
        char *dev_path = detect_device(&matched);

        /* Pixart --info reads the local cache and works with no device attached. */
        if(!dev_path) {
            if(do_dump_flag) {
                fprintf(stderr, "No device detected; dumping from the cache "
                                "(sensor DPI limits cannot be checked).\n");
                hid_exit();
                return pixart_set(NULL, NULL, &opts, 1);
            }
            if(do_info) {
                fprintf(stderr, "No device detected; showing cached Pixart state.\n");
                hid_exit();
                return pixart_info();
            }
            fprintf(stderr, "No supported device found.\n");
            hid_exit();
            return 1;
        }

        if(!matched || matched->proto == PROTO_UNSUPPORTED) {
            fprintf(stderr,
                "\n%s is detected, but its protocol is not implemented in this build.\n"
                "Run 'gloriousctl --probe' and open an issue with the output.\n",
                matched ? matched->name : "This device");
            FREE(dev_path);
            hid_exit();
            return 1;
        }

        if(do_listen && matched->proto != PROTO_SINOWEALTH) {
            fprintf(stderr, "--listen is only supported on SinoWealth devices.\n");
            FREE(dev_path);
            hid_exit();
            return 1;
        }

        /* --dump-payload transmits nothing, so it must not open the device
         * either: it stays usable when the uaccess rule has not been applied
         * yet. It also wins over any --set-*, so a dump never writes. */
        if(do_dump_flag) {
            int rc_dump;
            if(matched->proto != PROTO_PIXART) {
                fprintf(stderr, "--dump-payload is only implemented for Pixart devices.\n");
                rc_dump = 1;
            } else {
                rc_dump = pixart_set(NULL, matched, &opts, 1);
            }
            FREE(dev_path);
            hid_exit();
            return rc_dump;
        }

        /* Pixart --info is a pure cache read; don't require an open handle.
         * Opening first would make --info fail without root while the same
         * command succeeds with the mouse unplugged. */
        if(matched->proto == PROTO_PIXART && do_info) {
            FREE(dev_path);
            hid_exit();
            return pixart_info();
        }

        hid_device *dev = hid_open_path(dev_path);
        FREE(dev_path);
        if(!dev) {
            fprintf(stderr, "Failed to open HID device. Try sudo.\n");
            hid_exit();
            return 1;
        }

        int rc = 0;
        if(matched->proto == PROTO_SINOWEALTH) {
            if(do_listen)    rc = sinowealth_listen(dev);
            else if(do_info) rc = sinowealth_info(dev);
            else             rc = sinowealth_set(dev, matched, &opts);
        } else {
            rc = pixart_set(dev, matched, &opts, 0);
        }

        hid_close(dev);
        hid_exit();
        return rc;
    }


    return 0;
}


/* from https://stackoverflow.com/a/7776146/675646 */
void hexDump (const char * desc, const void * addr, const int len) {
    int i = 0;
    unsigned char buff[17];
    const unsigned char * pc = (const unsigned char *)addr;

    // Output description if given.

    if (desc != NULL)
        printf ("%s:\n", desc);

    // Length checks.

    if (len == 0) {
        printf("  ZERO LENGTH\n");
        return;
    }
    if (len < 0) {
        printf("  NEGATIVE LENGTH: %d\n", len);
        return;
    }

    // Process every byte in the data.

    for (i = 0; i < len; i++) {
        // Multiple of 16 means new line (with line offset).

        if ((i % 16) == 0) {
            // Don't print ASCII buffer for the "zeroth" line.

            if (i != 0)
                printf ("  %s\n", buff);

            // Output the offset.

            printf ("  %04x ", i);
        }

        // Now the hex code for the specific character.
        printf (" %02x", pc[i]);

        // And buffer a printable ASCII character for later.

        if ((pc[i] < 0x20) || (pc[i] > 0x7e)) // isprint() may be better.
            buff[i % 16] = '.';
        else
            buff[i % 16] = pc[i];
        buff[(i % 16) + 1] = '\0';
    }

    // Pad out last line if not exactly 16 characters.

    while ((i % 16) != 0) {
        printf ("   ");
        i++;
    }

    // And print the final ASCII buffer.

    printf ("  %s\n", buff);
}
