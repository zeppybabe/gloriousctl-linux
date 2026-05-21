#include <fcntl.h>
#include <getopt.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
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

struct supported_device {
    uint16_t vid;
    uint16_t pid;
    const char *name;
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
    { .vid = 0x258a, .pid = 0x27, .name =  "Dream Machines DM5" },
    { .vid = 0x258a, .pid = 0x33, .name =  "Glorious Model D" },
    { .vid = 0x258a, .pid = 0x36, .name =  "Glorious Model O/O-" }, // probably works
    { .vid = 0x093a, .pid = 0x821d, .name = "Glorious Model I 2 Wireless" },								   
};

#pragma pack(push, 1)

typedef struct {
    uint8_t r, g, b;
} RGB8;

enum pixart_rgb_effect {
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
    uint8_t  speed;               // [9] Speed (0-20)
    uint8_t  modifier;            // [10] Tracks with effect variations
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
    uint8_t  unk_pad1;       // [4] 0x01
    uint8_t  active_stage;   // [5] 0x00 to 0x05 (Zero-indexed stage)
    uint8_t  total_stages;   // [6] 0x04, 0x05, or 0x06
    uint8_t  lod;            // [7] 0x01 (1mm) or 0x02 (2mm)
    uint8_t  debounce;       // [8] 0x00 to 0x10 (Even numbers only)
    uint8_t  polling;        // [9] 0x01(1k), 0x02(125), 0x03(250), 0x04(500)
    uint8_t  pad_zero;       // [10] 0x00
    uint16_t dpi_stage1;     // [11-12] (DPI / 50) Little-Endian
    RGB8     color_stage1;   // [13-15]
    uint8_t  f1_padding[48]; // [16-63] Blank

    // --- Fragment 2 (Stages 2 & 3) ---
    uint8_t  report_id2;     // [64] Always 0x03
    uint8_t  cmd2[2];        // [65-66] 0x04 0xfb
    uint8_t  seq2;           // [67] 0x01
    uint8_t  unk_pad2;       // [68] 0x01
    uint16_t dpi_stage2;     // [69-70] (DPI / 50)
    RGB8     color_stage2;   // [71-73]
    uint16_t dpi_stage3;     // [74-75] (DPI / 50)
    RGB8     color_stage3;   // [76-78]
    uint8_t  f2_padding[49]; // [79-127] Blank

    // --- Fragment 3 (Stages 4 & 5) ---
    uint8_t  report_id3;     // [128] Always 0x03
    uint8_t  cmd3[2];        // [129-130] 0x04 0xfb
    uint8_t  seq3;           // [131] 0x02
    uint8_t  unk_pad3;       // [132] 0x01
    uint16_t dpi_stage4;     // [133-134] (DPI / 50)
    RGB8     color_stage4;   // [135-137]
    uint16_t dpi_stage5;     // [138-139] (DPI / 50)
    RGB8     color_stage5;   // [140-142]
    uint8_t  f3_padding[49]; // [143-191] Blank

    // --- Fragment 4 (Stage 6) ---
    uint8_t  report_id4;     // [192] Always 0x03
    uint8_t  cmd4[2];        // [193-194] 0x04 0xfb
    uint8_t  seq4;           // [195] 0x03
    uint8_t  unk_pad4;       // [196] 0x01
    uint16_t dpi_stage6;     // [197-198] (DPI / 50)
    RGB8     color_stage6;   // [199-201]
    uint8_t  f4_padding[54]; // [202-255] Blank
};

// The Master Linux Cache
struct glorious_state {
    struct pixart_lighting_payload lighting;
    struct pixart_settings_payload settings;
};

#pragma pack(pop)

static RGB8 int_to_rgb(unsigned int value)
{
    RGB8 rgb;
    rgb.r = value >> 16U;
    rgb.g = (value >> 8U) & 0xFF;
    rgb.b = value & 0xFF;
    return rgb;
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
        if(device->interface_number == 1) {
            path = strdup(device->path);
            break;
        }
        device = device->next;
    }

    hid_free_enumeration(devices);
    return path;
}

static
char *detect_device()
{
    hid_init();

    char *path = NULL;
    for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
        path = find_device(&supported_devices[i]);
        if(path) {
            fprintf(stderr, "Detected %s\n", supported_devices[i].name);
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
            " gloriousctl [--set-...]\n"
            "\tChange persistent mouse settings.\n"
            "\n"
            "Available settings:\n"
            " --set-debounce-time 4-16\n"
            "\tChange click debounce time in milliseconds. Only use even numbers.\n"
            " --set-dpi DPI1,...\n"
            "\tUp to six DPIs can be configured.\n"
            " --set-dpi-color RRGGBB,...\n"
            "\tFor each DPI the RGB color can be set.\n"
            " --set-effect effect-name\n"
            "\tAvailable RGB effects: off, glorious, breathing, wave, tail,\n"
            "\tsingle, breathing7, breathing1, rave\n"
            "\tsingle and breathing1 use one color, breathing7 seven, rave two.\n"
            " --set-colors RRGGBB,...\n"
            "\tSet the color(s) of the effect. Only effective with --set-effect.\n"
            " --set-brightness 0-4\n"
            "\tSet the brightness of the effect. Only effective with --set-effect.\n"
            " --set-speed 0-3\n"
            "\tSet the speed of the effect. Only effective with --set-effect.\n"
            "\n"
        );

    fprintf(stderr, "Supported mice:\n");
    for(unsigned int i = 0; i < sizeof(supported_devices)/sizeof(supported_devices[0]); i++) {
        struct supported_device dev = supported_devices[i];
        fprintf(stderr, " - %s (VID %04x PID %04x)\n", dev.name, dev.vid, dev.pid);
    }

    return 0;
}

// Safely chunks our massive structs into 64-byte packets and sends them in sequence
static int pixart_send_payload(hid_device *dev, void *payload, size_t total_size)
{
    int res = 0;
    int chunks = total_size / 64;
    uint8_t *byte_ptr = (uint8_t *)payload;

    for (int i = 0; i < chunks; i++) {
        // hid_send_feature_report requires the first byte of the buffer to be the Report ID.
        // Because we built our structs with report_id (0x03) at the exact 64-byte boundaries,
        // we can just pass a pointer to the start of each fragment directly!

        res = hid_send_feature_report(dev, byte_ptr + (i * 64), 64);

        if (res < 0) {
            print_hid_error(dev, "Failed to send payload fragment");
            return -1;
        }
    }

    printf("Successfully transmitted %d bytes to the Pixart chip.\n", (chunks * 64));
    return 0;
}

static void get_state_path(char *path_buffer, size_t max_len) {
    const char *homedir = getenv("HOME");
    if (!homedir) homedir = getpwuid(getuid())->pw_dir;
    snprintf(path_buffer, max_len, "%s/.gloriousctl_state.bin", homedir);
}

static void load_state(struct glorious_state *state) {
    char path[512];
    get_state_path(path, sizeof(path));

    FILE *f = fopen(path, "rb");
    if (f) {
        fread(state, sizeof(struct glorious_state), 1, f);
        fclose(f);
    } else {
        printf("No existing config found. Generating safe factory defaults...\n");
        memset(state, 0, sizeof(struct glorious_state));

        // Setup Lighting Defaults (Glorious Mode)
        state->lighting.report_id1 = 0x03; state->lighting.cmd1[0] = 0x02; state->lighting.cmd1[1] = 0xfb; state->lighting.seq1 = 0x00;
        state->lighting.report_id2 = 0x03; state->lighting.cmd2[0] = 0x02; state->lighting.cmd2[1] = 0xfb; state->lighting.seq2 = 0x01;
        state->lighting.report_id3 = 0x03; state->lighting.cmd3[0] = 0x02; state->lighting.cmd3[1] = 0xfb; state->lighting.seq3 = 0x02;
        state->lighting.effect_id = PIXART_RGB_GLORIOUS;
        state->lighting.brightness_w = 0x14; // 100%
        state->lighting.brightness_wl = 0x14; // 100%
        state->lighting.speed = 0x0a; // 50%

        // Setup Settings Defaults (4 Stages: 400, 800, 1600, 3200)
        state->settings.report_id1 = 0x03; state->settings.cmd1[0] = 0x04; state->settings.cmd1[1] = 0xfb; state->settings.seq1 = 0x00;
        state->settings.report_id2 = 0x03; state->settings.cmd2[0] = 0x04; state->settings.cmd2[1] = 0xfb; state->settings.seq2 = 0x01;
        state->settings.report_id3 = 0x03; state->settings.cmd3[0] = 0x04; state->settings.cmd3[1] = 0xfb; state->settings.seq3 = 0x02;
        state->settings.report_id4 = 0x03; state->settings.cmd4[0] = 0x04; state->settings.cmd4[1] = 0xfb; state->settings.seq4 = 0x03;

        state->settings.active_stage = 0x00; // Stage 1
        state->settings.total_stages = 0x04;
        state->settings.polling = 0x01; // 1000Hz
        state->settings.debounce = 0x0a; // 10ms
        state->settings.lod = 0x01; // 1mm

        // Default DPIs (Target / 50)
        state->settings.dpi_stage1 = 400 / 50;
        state->settings.dpi_stage2 = 800 / 50;
        state->settings.dpi_stage3 = 1600 / 50;
        state->settings.dpi_stage4 = 3200 / 50;

        // Default Colors (Red, Blue, Green, Yellow)
        state->settings.color_stage1 = int_to_rgb(0xFF0000);
        state->settings.color_stage2 = int_to_rgb(0x0000FF);
        state->settings.color_stage3 = int_to_rgb(0x00FF00);
        state->settings.color_stage4 = int_to_rgb(0xFFFF00);
    }
}

static void save_state(struct glorious_state *state) {
    char path[512];
    get_state_path(path, sizeof(path));

    FILE *f = fopen(path, "wb");
    if (f) {
        fwrite(state, sizeof(struct glorious_state), 1, f);
        fclose(f);
    } else {
        fprintf(stderr, "Warning: Could not save state to %s\n", path);
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

    const char *set_debounce_time = 0;
    const char *set_dpi = 0;
    const char *set_dpi_color = 0;
    const char *set_effect = 0;
    const char *set_effect_colors = "";
    const char *set_effect_brightness = "4";
    const char *set_effect_speed = "3";

    struct option options[] = {
        {"info", no_argument, &do_info, 1},
        {"help", no_argument, &do_help, 1},
        {"listen", no_argument, &do_listen, 1},
        {"set-dpi", required_argument, 0, 'a'},
        {"set-dpi-color", required_argument, 0, 'b'},
        {"set-effect", required_argument, 0, 'c'},
        {"set-colors", required_argument, 0, 'd'},
        {"set-brightness", required_argument, 0, 'e'},
        {"set-speed", required_argument, 0, 'f'},
        {"set-debounce-time", required_argument, 0, 'g'},
        {0}
    };

    while(1) {
        int c = getopt_long(argc, argv, "", options, NULL);
        if(c == -1) break;
        if(!c) continue;
        if(c == '?') return 1;

        do_set = 1;
        switch(c) {
            case 'a': set_dpi = optarg; break;
            case 'b': set_dpi_color = optarg; break;
            case 'c': set_effect = optarg; break;
            case 'd': set_effect_colors = optarg; break;
            case 'e': set_effect_brightness = optarg; break;
            case 'f': set_effect_speed = optarg; break;
            case 'g': set_debounce_time = optarg; break;
            default:;
        }
    }

    if(do_help) {
        return print_help();
    }

    if(do_info) {
        struct glorious_state state;
        load_state(&state); // Pulls from your hard drive, not the mouse!

        printf("\n====================================\n");
        printf("  CACHED MOUSE SETTINGS (LINUX)       \n");
        printf("====================================\n");

        int hz = 0;
        if (state.settings.polling == 1) hz = 1000;
        else if (state.settings.polling == 2) hz = 125;
        else if (state.settings.polling == 3) hz = 250;
        else if (state.settings.polling == 4) hz = 500;

        printf("Polling Rate:      %d Hz\n", hz);
        printf("Debounce Time:     %d ms\n", state.settings.debounce);
        printf("Lift-Off Distance: %d mm\n", state.settings.lod);
        printf("Active DPI Stage:  %d (Total: %d)\n", state.settings.active_stage + 1, state.settings.total_stages);

        printf("\n--- DPI Stages ---\n");
        printf("Stage 1: %d DPI\t", state.settings.dpi_stage1 * 50); print_color(state.settings.color_stage1); printf("\n");
        printf("Stage 2: %d DPI\t", state.settings.dpi_stage2 * 50); print_color(state.settings.color_stage2); printf("\n");
        printf("Stage 3: %d DPI\t", state.settings.dpi_stage3 * 50); print_color(state.settings.color_stage3); printf("\n");
        printf("Stage 4: %d DPI\t", state.settings.dpi_stage4 * 50); print_color(state.settings.color_stage4); printf("\n");
        if (state.settings.total_stages >= 5) {
            printf("Stage 5: %d DPI\t", state.settings.dpi_stage5 * 50); print_color(state.settings.color_stage5); printf("\n");
        }
        if (state.settings.total_stages == 6) {
            printf("Stage 6: %d DPI\t", state.settings.dpi_stage6 * 50); print_color(state.settings.color_stage6); printf("\n");
        }
        printf("====================================\n");

        // Re-save it just to ensure the file exists for future writes
        save_state(&state);
    }
    if(do_set) {
        struct glorious_state state;
        load_state(&state);
        int settings_changed = 0;
        int lighting_changed = 0;

        // --- CACHE SELF-HEAL ---
        // Automatically fixes any missing padding bytes or desynced effect IDs
        state.lighting.unk1 = 0x01; state.lighting.unk2 = 0x01; state.lighting.unk3 = 0x01;
        state.settings.unk_pad1 = 0x01; state.settings.unk_pad2 = 0x01;
        state.settings.unk_pad3 = 0x01; state.settings.unk_pad4 = 0x01;
        state.lighting.effect_id2 = state.lighting.effect_id;
        state.lighting.effect_id3 = state.lighting.effect_id;
        state.lighting.modifier = 0x14; // Global brightness max

        if (set_debounce_time) {
            int val = atoi(set_debounce_time);
            val = clamp(val, 2, 16);
            if (val % 2 != 0) val -= 1;
            state.settings.debounce = val;
            settings_changed = 1;
            printf("Set Debounce Time: %d ms\n", val);
        }

        if (set_dpi) {
            int dpis[6] = {0};
            int num = sscanf(set_dpi, "%d,%d,%d,%d,%d,%d",
                             &dpis[0], &dpis[1], &dpis[2], &dpis[3], &dpis[4], &dpis[5]);
            if (num > 0) {
                state.settings.total_stages = clamp(num, 4, 6);
                if (num >= 1) state.settings.dpi_stage1 = dpis[0] / 50;
                if (num >= 2) state.settings.dpi_stage2 = dpis[1] / 50;
                if (num >= 3) state.settings.dpi_stage3 = dpis[2] / 50;
                if (num >= 4) state.settings.dpi_stage4 = dpis[3] / 50;
                if (num >= 5) state.settings.dpi_stage5 = dpis[4] / 50;
                if (num >= 6) state.settings.dpi_stage6 = dpis[5] / 50;
                settings_changed = 1;
                printf("Set DPI Stages: %d total stages updated.\n", state.settings.total_stages);
            }
        }

        if (set_dpi_color) {
            unsigned int colors[6] = {0};
            int num = sscanf(set_dpi_color, "%x,%x,%x,%x,%x,%x",
                             &colors[0], &colors[1], &colors[2], &colors[3], &colors[4], &colors[5]);
            if (num >= 1) state.settings.color_stage1 = int_to_rgb(colors[0]);
            if (num >= 2) state.settings.color_stage2 = int_to_rgb(colors[1]);
            if (num >= 3) state.settings.color_stage3 = int_to_rgb(colors[2]);
            if (num >= 4) state.settings.color_stage4 = int_to_rgb(colors[3]);
            if (num >= 5) state.settings.color_stage5 = int_to_rgb(colors[4]);
            if (num >= 6) state.settings.color_stage6 = int_to_rgb(colors[5]);
            settings_changed = 1;
            printf("Set DPI Colors updated.\n");
        }

        if (set_effect) {
            uint8_t effect = PIXART_RGB_GLORIOUS;
            if (!strcmp(set_effect, "off")) effect = PIXART_RGB_OFF;
            else if (!strcmp(set_effect, "glorious")) effect = PIXART_RGB_GLORIOUS;
            else if (!strcmp(set_effect, "seamless")) effect = PIXART_RGB_SEAMLESS_BREATHING;
            else if (!strcmp(set_effect, "breathing")) effect = PIXART_RGB_BREATHING;
            else if (!strcmp(set_effect, "normally_on")) effect = PIXART_RGB_NORMALLY_ON;
            else if (!strcmp(set_effect, "breathing_single")) effect = PIXART_RGB_BREATHING_SINGLE;
            else if (!strcmp(set_effect, "tail")) effect = PIXART_RGB_TAIL;
            else if (!strcmp(set_effect, "rave")) effect = PIXART_RGB_RAVE;
            else if (!strcmp(set_effect, "wave")) effect = PIXART_RGB_WAVE;
            else printf("Unknown effect '%s'! Keeping current.\n", set_effect);

            state.lighting.effect_id = effect;
            state.lighting.effect_id2 = effect; // Synchronize across packets
            state.lighting.effect_id3 = effect; // Synchronize across packets

            lighting_changed = 1;
            printf("Set Lighting Effect: %s\n", set_effect);
        }

        if (set_effect_colors && strlen(set_effect_colors) > 0) {
            unsigned int colors[7] = {0};
            int num = sscanf(set_effect_colors, "%x,%x,%x,%x,%x,%x,%x",
                             &colors[0], &colors[1], &colors[2], &colors[3], &colors[4], &colors[5], &colors[6]);
            if (num > 0) {
                state.lighting.color_count = num;

                // Color 1 always goes to main_color
                state.lighting.main_color = int_to_rgb(colors[0]);

                // Colors 2-7 shift into the array
                if (num >= 2) state.lighting.color_array_1[0] = int_to_rgb(colors[1]);
                if (num >= 3) state.lighting.color_array_1[1] = int_to_rgb(colors[2]);
                if (num >= 4) state.lighting.color_array_1[2] = int_to_rgb(colors[3]);
                if (num >= 5) state.lighting.color_array_1[3] = int_to_rgb(colors[4]);
                if (num >= 6) state.lighting.color_array_1[4] = int_to_rgb(colors[5]);
                if (num >= 7) state.lighting.color_array_1[5] = int_to_rgb(colors[6]);

                lighting_changed = 1;
                printf("Set Effect Colors: %d colors updated.\n", num);
            }
        }

        if (set_effect_brightness) {
            int br = atoi(set_effect_brightness);
            br = clamp(br, 0, 4);
            int br_map[] = {0x00, 0x05, 0x0a, 0x0f, 0x14}; // Exact hex mapping
            state.lighting.brightness_w = br_map[br];
            state.lighting.brightness_wl = br_map[br];
            lighting_changed = 1;
            printf("Set Brightness: %d/4\n", br);
        }

        if (set_effect_speed) {
            int sp = atoi(set_effect_speed);
            sp = clamp(sp, 0, 3);
            int sp_map[] = {0x05, 0x0a, 0x0f, 0x14}; // Exact hex mapping
            state.lighting.speed = sp_map[sp];
            lighting_changed = 1;
            printf("Set Speed: %d/3\n", sp);
        }

        // --- TRANSMIT ---
        if (lighting_changed || settings_changed) {
            char *dev_path = detect_device();
            if(!dev_path) {
                fprintf(stderr, "No supported device found. Cannot apply settings.\n");
                return 1;
            }
            hid_device *dev = hid_open_path(dev_path);
            FREE(dev_path);
            if(!dev) {
                fprintf(stderr, "Failed to open HID device. Try using sudo.\n");
                return 1;
            }

            if (lighting_changed) {
                printf("\nWriting Lighting Config to mouse...\n");
                pixart_send_payload(dev, &state.lighting, sizeof(state.lighting));
            }
            if (settings_changed) {
                printf("\nWriting Settings/DPI Config to mouse...\n");
                pixart_send_payload(dev, &state.settings, sizeof(state.settings));
            }

            hid_close(dev);
            hid_exit();

            save_state(&state);
            printf("\nConfiguration applied and saved locally. Done!\n");
        }
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
