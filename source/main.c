/*
 * DMC5 PS4 "Turbo Mode" GoldHEN plugin - v7 (universal discovery)
 *
 * Port of the PC "Turbo Mode Injector" (dmc5.dll, Nexus mod 29/929):
 * the PC mod continuously pins the engine's Application speed float
 * (PC build: [Application+0x380]) to turbo.ini's [settings] speed.
 *
 * v6 hardcoded the DMC5 v1.00 addresses. v7 discovers them at runtime
 * so regional builds and game updates can work without a rebuild:
 *
 *   1. locate every reflection-native getter of the form
 *          mov  rax, [rip+slot]
 *          vmovss xmm0, [rax+field]      (AVX)
 *          vcvtss2sd xmm0, xmm0
 *          vmovsd [rdi+0x38], xmm0       (native result convention)
 *          ret
 *      (SSE encoding accepted as a fallback). Each hit yields a singleton
 *      slot address; the Application slot is one of them.
 *
 *   2. when a slot becomes non-null, probe the object for the Application
 *      constructor's default block:
 *          +0x00 = speed (1.0), +0x0C = 0.001, +0x18 = 60.0, +0x1C = 6000.0
 *      The matching offset is the speed field - no hardcoded field offset.
 *
 *   3. pin that float to turbo.ini's speed every 10 ms.
 *
 * If no getter can be found, the exact v1.00 profile is used, but only
 * after verifying the v1.00 signature at 0x3a9190. Anything else is left
 * untouched (fail safe).
 *
 * v1.00 reference facts (verified against the shipped eboot.bin):
 *   via.Application singleton pointer slot : vaddr 0xae9d078
 *   get_GlobalSpeed  (reflection)          : vaddr 0x3a9190
 *       -> vmovss xmm0, [app+0x280]        (float GlobalSpeed)
 *   set_GlobalSpeed  (reflection)          : vaddr 0x3a9620
 *       -> vmovss [app+0x280], xmm0
 *   engine consumers that apply it:
 *       0x36caacf: [obj+0x5C] = max(0,[obj+0x5C]) * app->GlobalSpeed
 *       0x393cb32: [obj+0x5C] = max(0,xmm0)       * app->GlobalSpeed
 *       0x45075bb: frameTimeMs * app->GlobalSpeed (when enabled)
 *
 * NOT the speed field: [app+0x278] is an 8-byte double computed in the
 * constructor as 1000000.0 / counter_frequency (ticks -> microseconds).
 *
 * Design:
 *   - find the main module base via sceKernelGetModuleList/Info (ASLR safe)
 *   - runtime scan described above, validated by the constructor defaults
 *   - re-read turbo.ini every 5 s so the speed can be tuned live over FTP
 *   - log to klog (printf) and /data/GoldHEN/plugins/turbo.log
 *
 * GoldHEN calls plugin_load()/plugin_unload() by dlsym; module_start/stop
 * stay hidden and empty, matching the proven DDMK plugin layout.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

#include <orbis/libkernel.h>
#include <orbis/Pad.h>
#include <orbis/UserService.h>
#include <Common.h>

#define TITLE_ID "DMC5"

#define ATTR_PUBLIC  __attribute__((visibility("default")))
#define ATTR_HIDDEN  __attribute__((weak, visibility("hidden")))
#define ATTR_REALIGN __attribute__((force_align_arg_pointer))

/* ---- v1.00 reference profile (fallback only) ---- */
#define APP_SLOT_VA       0xae9d078ULL /* via::Application* singleton slot */
#define GET_GLOBAL_VA     0x3a9190ULL  /* get_GlobalSpeed (float +0x280)    */
#define OFF_GLOBAL_SPEED  0x280        /* float: engine global speed        */
#define OFF_FRAME_TIME    0x288        /* float: frame time ms              */
#define OFF_FRAME_COUNT   0x2f8        /* int32: frame counter              */
#define OFF_TICK_SCALE    0x278        /* double: ticks -> us (read-only)   */

#define INI_PATH   "/data/GoldHEN/plugins/turbo.ini"
#define LOG_PATH   "/data/GoldHEN/plugins/turbo.log"

#define DEFAULT_SPEED   1.2f
#define MIN_SPEED       0.10f
#define MAX_SPEED       10.0f

#define PIN_INTERVAL_US 10000ULL    /* 10 ms */
#define INI_RELOAD_US   5000000ULL  /* 5 s   */
#define HEARTBEAT_US    5000000ULL  /* 5 s   */

/* runtime discovery limits */
#define SCAN_MAX_SLOTS  64
#define SCAN_PATTERN_LEN 25
#define PROBE_SIZE      0x300       /* constructor block search window */

/* exported metadata, shown by the GoldHEN plugin loader */
ATTR_PUBLIC const char    *g_pluginName    = "turbo";
ATTR_PUBLIC const char    *g_pluginDesc    = "DMC5 turbo mode (GlobalSpeed pin, hotkey control)";
ATTR_PUBLIC const char    *g_pluginAuth    = "port: jun | original mod: Turbo Mode Injector";
ATTR_PUBLIC const uint32_t g_pluginVersion = 0x00000800; /* 0.08 */

static volatile int g_running = 0;
static volatile int g_threadDone = 1;

static float    g_speed = DEFAULT_SPEED;
static uint64_t g_base = 0;         /* main module load base            */
static uint64_t g_app  = 0;         /* resolved Application*            */
static uint64_t g_slot_va = 0;      /* runtime address of its slot      */
static uint32_t g_off_speed = OFF_GLOBAL_SPEED; /* discovered field     */
static int      g_profile_mode = 0; /* 1 = v1.00 constants fallback     */
static int      g_idle = 0;         /* nothing compatible in this game  */
static uint64_t g_writes = 0;
static int      g_sites = 0;

static uint64_t g_slots[SCAN_MAX_SLOTS];
static int      g_slot_count = 0;

typedef struct {
    uint64_t addr;
    uint32_t size;
    int32_t  prot;
} mod_seg;

static mod_seg g_segs[4];
static int     g_seg_count = 0;

/* in-game adjustment: passive pad handle + hotkey state */
static int32_t  g_pad_handle = -1;
static int      g_pad_inited = 0;
static int      g_user_inited = 0;
static int32_t  g_pad_user = 0;
static uint64_t g_next_pad_try = 0;
static uint64_t g_next_pad_poll = 0;
static uint64_t g_next_repeat = 0;
static uint32_t g_prev_hot = 0;
static int      g_hint_sent = 0;

/* ------------------------------------------------------------------ log */

#define O_WRONLY_ 0x0001
#define O_CREAT_  0x0200
#define O_APPEND_ 0x0008
#define O_TRUNC_  0x0400

static void tlog(const char *fmt, ...)
{
    char body[640];
    char line[704];
    va_list ap;
    int n;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    n = snprintf(line, sizeof(line), "[turbo] %s\n", body);
    if (n <= 0)
        return;

    /* klog / TTY (GoldHEN redirects printf to the klog socket) */
    printf("%s", line);

    int fd = sceKernelOpen(LOG_PATH, O_WRONLY_ | O_CREAT_ | O_APPEND_, 0777);
    if (fd >= 0) {
        sceKernelLseek(fd, 0, 2 /* SEEK_END */);
        sceKernelWrite(fd, line, strlen(line));
        sceKernelClose(fd);
    }
}

/* ------------------------------------------------------------ addresses */

#define MAX_MODULES 256

static uint64_t find_main_base(void)
{
    OrbisKernelModule handles[MAX_MODULES];
    size_t available = 0;
    uint64_t base = 0;

    memset(handles, 0, sizeof(handles));
    if (sceKernelGetModuleList(handles, MAX_MODULES, &available) < 0)
        return 0;
    if (available > MAX_MODULES)
        available = MAX_MODULES;

    for (size_t i = 0; i < available; i++) {
        OrbisKernelModuleInfo info;
        memset(&info, 0, sizeof(info));
        info.size = sizeof(info);
        if (sceKernelGetModuleInfo(handles[i], &info) < 0)
            continue;
        if (strncmp(info.name, "eboot", 5) == 0) {
            base = (uint64_t)(uintptr_t)info.segmentInfo[0].address;

            g_seg_count = info.segmentCount > 4 ? 4 : (int)info.segmentCount;
            for (int s = 0; s < g_seg_count; s++) {
                g_segs[s].addr = (uint64_t)(uintptr_t)info.segmentInfo[s].address;
                g_segs[s].size = info.segmentInfo[s].size;
                g_segs[s].prot = info.segmentInfo[s].prot;
            }

            tlog("main module '%s' base=%#lx segs=%d",
                 info.name, (unsigned long)base, g_seg_count);
            for (int s = 0; s < g_seg_count; s++)
                tlog("  seg%d %#lx +%#x prot=%d",
                     s, (unsigned long)g_segs[s].addr,
                     g_segs[s].size, g_segs[s].prot);
            break;
        }
    }
    return base;
}

static int in_module(uint64_t a)
{
    for (int i = 0; i < g_seg_count; i++) {
        uint64_t lo = g_segs[i].addr;
        uint64_t hi = lo + g_segs[i].size;
        if (a >= lo && a < hi)
            return 1;
    }
    return 0;
}

/*
 * v1.00 signature: at 0x3a9190 the game must do
 * "mov rax, [rip+disp] -> label 0xae9d078" (the Application singleton).
 * Only used to justify the hardcoded fallback profile.
 */
static int eboot_matches(uint64_t base)
{
    const uint8_t *p = (const uint8_t *)(uintptr_t)(base + GET_GLOBAL_VA);
    int32_t disp;

    if (p[0] != 0x48 || p[1] != 0x8b || p[2] != 0x05)
        return 0;
    memcpy(&disp, p + 3, 4);
    return base + GET_GLOBAL_VA + 7 + (int64_t)disp == base + APP_SLOT_VA;
}

/* ------------------------------------------------- runtime discovery */

static int add_slot(uint64_t slot)
{
    if ((slot & 7) != 0 || !in_module(slot))
        return 0;
    for (int i = 0; i < g_slot_count; i++)
        if (g_slots[i] == slot)
            return 0;
    if (g_slot_count >= SCAN_MAX_SLOTS)
        return 0;
    g_slots[g_slot_count++] = slot;
    return 1;
}

/*
 * Reflection native getter:
 *   48 8B 05 disp32          mov rax,[rip+slot]
 *   C5 FA 10 80 off32        vmovss xmm0,[rax+field]      (AVX)
 *   C5 FA 5A C0              vcvtss2sd xmm0,xmm0
 *   C5 FB 11 47 38           vmovsd [rdi+0x38],xmm0
 *   C3                       ret
 * SSE flavour swaps C5 FA/5A/FB for F3 0F equivalents.
 */
static int match_getter(const uint8_t *p)
{
    if (p[0] != 0x48 || p[1] != 0x8B || p[2] != 0x05)
        return 0;

    if (p[7] == 0xC5 && p[8] == 0xFA && p[9] == 0x10 && p[10] == 0x80 &&
        p[15] == 0xC5 && p[16] == 0xFA && p[17] == 0x5A && p[18] == 0xC0 &&
        p[19] == 0xC5 && p[20] == 0xFB && p[21] == 0x11 &&
        p[22] == 0x47 && p[23] == 0x38 && p[24] == 0xC3)
        return 1;

    if (p[7] == 0xF3 && p[8] == 0x0F && p[9] == 0x10 && p[10] == 0x80 &&
        p[15] == 0xF3 && p[16] == 0x0F && p[17] == 0x5A && p[18] == 0xC0 &&
        p[19] == 0xF3 && p[20] == 0x0F && p[21] == 0x11 &&
        p[22] == 0x47 && p[23] == 0x38 && p[24] == 0xC3)
        return 1;

    return 0;
}

static void scan_getter_slots(void)
{
    g_slot_count = 0;
    g_sites = 0;

    for (int s = 0; s < g_seg_count; s++) {
        if (!(g_segs[s].prot & 4))     /* executable only */
            continue;

        const uint8_t *p = (const uint8_t *)(uintptr_t)g_segs[s].addr;
        uint32_t n = g_segs[s].size;

        for (uint32_t i = 0; i + SCAN_PATTERN_LEN <= n; i++) {
            if (p[i] != 0x48)
                continue;
            if (!match_getter(p + i))
                continue;

            int32_t disp;
            memcpy(&disp, p + i + 3, 4);
            uint64_t hit = (uint64_t)(uintptr_t)(p + i);
            uint64_t slot = hit + 7 + (int64_t)disp;

            g_sites++;
            add_slot(slot);
        }
    }
}

static int float_ok(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    return (b & 0x7f800000u) != 0x7f800000u; /* no NaN/Inf */
}

static int valid_ptr(uint64_t p)
{
    return p >= 0x10000ULL && p < 0x0000800000000000ULL && (p & 7) == 0;
}

/*
 * Application constructor defaults (seen in v1.00):
 *   [off+0x00]=1.0 speed, [off+0x0C]=0.001, [off+0x18]=60.0, [off+0x1C]=6000.0
 */
static int speed_block_at(uint64_t app, uint32_t off)
{
    float spd = *(volatile float *)(uintptr_t)(app + off);
    float tiny = *(volatile float *)(uintptr_t)(app + off + 0x0C);
    float fps = *(volatile float *)(uintptr_t)(app + off + 0x18);
    float cap = *(volatile float *)(uintptr_t)(app + off + 0x1C);

    if (!float_ok(spd) || spd <= 0.0001f || spd > 1000.0f)
        return 0;
    if (!(tiny > 0.0005f && tiny < 0.002f))
        return 0;
    if (!(fps > 59.0f && fps < 61.0f))
        return 0;
    if (!(cap > 5999.0f && cap < 6001.0f))
        return 0;
    return 1;
}

static int find_speed_offset(uint64_t app, uint32_t *out)
{
    for (uint32_t off = 0; off + 0x20 <= PROBE_SIZE; off += 4) {
        if (speed_block_at(app, off)) {
            *out = off;
            return 1;
        }
    }
    return 0;
}

/* Validate a candidate Application pointer (fallback profile path). */
static int app_valid(uint64_t app)
{
    if (!valid_ptr(app))
        return 0;

    float spd = *(volatile float *)(uintptr_t)(app + OFF_GLOBAL_SPEED);
    float ft  = *(volatile float *)(uintptr_t)(app + OFF_FRAME_TIME);
    if (!float_ok(spd) || spd <= 0.0001f || spd > 1000.0f)
        return 0;
    if (!float_ok(ft) || ft < 0.0f || ft > 1.0e6f)
        return 0;
    return 1;
}

/* ---------------------------------------------------------------- config */

static float parse_speed(const char *buf)
{
    float spd = g_speed;
    const char *p = buf;

    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t len = eol ? (size_t)(eol - p) : strlen(p);
        char line[256];
        size_t n = len < sizeof(line) - 1 ? len : sizeof(line) - 1;

        memcpy(line, p, n);
        line[n] = 0;

        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (*s != ';' && *s != '#' && *s != '[') {
            char *eq = strchr(s, '=');
            if (eq) {
                *eq = 0;
                char *k = s;
                char *ke = eq - 1;
                while (ke >= k && (*ke == ' ' || *ke == '\t'))
                    *ke-- = 0;
                if (strcmp(k, "speed") == 0) {
                    float f = strtof(eq + 1, NULL);
                    if (float_ok(f) && f >= MIN_SPEED && f <= MAX_SPEED)
                        spd = f;
                    else
                        tlog("ini: ignoring out-of-range speed value");
                }
            }
        }

        if (!eol)
            break;
        p = eol + 1;
    }
    return spd;
}

static void read_ini(void)
{
    char buf[4096];
    int fd = sceKernelOpen(INI_PATH, 0 /* O_RDONLY */, 0666);
    if (fd < 0)
        return;

    int n = sceKernelRead(fd, buf, sizeof(buf) - 1);
    sceKernelClose(fd);
    if (n <= 0)
        return;
    buf[n] = 0;

    float spd = parse_speed(buf);
    if (spd != g_speed) {
        g_speed = spd;
        tlog("ini: speed = %.3f", spd);
    }
}

/* ------------------------------------------------ in-game pad control */

/*
 * PS4 system toast, drawn over the running game by SceShellUI. Same recipe
 * GoldHEN and other plugins use: type 0, targetId -1, text in message[].
 */
static void notify_user(const char *fmt, ...)
{
    OrbisNotificationRequest req;
    char body[512];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    memset(&req, 0, sizeof(req));
    req.type = NotificationRequest;
    req.targetId = -1;
    req.useIconImageUri = 0;
    snprintf(req.message, sizeof(req.message), "%s", body);
    sceKernelSendNotificationRequest(0, &req, sizeof(req), 0);
}

static int32_t pad_user_id(void)
{
    if (!g_user_inited) {
        OrbisUserServiceInitializeParams params;
        memset(&params, 0, sizeof(params));
        params.priority = ORBIS_KERNEL_PRIO_FIFO_LOWEST;
        sceUserServiceInitialize(&params);
        g_user_inited = 1;
        g_pad_user = 0;
        sceUserServiceGetInitialUser(&g_pad_user);
        tlog("pad: user id 0x%x", g_pad_user);
    }
    return g_pad_user;
}

static void pad_adopt(uint64_t now)
{
    if (g_pad_handle >= 0 || now < g_next_pad_try)
        return;
    g_next_pad_try = now + 2000000ULL; /* retry every 2 s */

    if (!g_pad_inited) {
        scePadInit();
        g_pad_inited = 1;
    }

    /*
     * Passive: attach to the handle the game itself opened. Never call
     * scePadOpen() here - that can collide with the game's own open (the
     * game would get ORBIS_PAD_ERROR_ALREADY_OPENED). Proven approach,
     * same as the DDMK PS4 port on this console.
     */
    int32_t h = scePadGetHandle(pad_user_id(), 0, 0);
    if (h >= 0) {
        g_pad_handle = h;
        tlog("pad: adopted game handle %d", (int)h);
    }
}

/* Persist the hotkey value so the next session starts with it. */
static void save_speed(float v)
{
    char in[4096];
    char out[4608];
    int fd = sceKernelOpen(INI_PATH, 0 /* O_RDONLY */, 0666);
    int n = 0;

    if (fd >= 0) {
        n = sceKernelRead(fd, in, sizeof(in) - 1);
        sceKernelClose(fd);
    }
    if (n < 0)
        n = 0;
    in[n] = 0;

    /* replace the first "speed=" line, keep all other lines/comments */
    size_t pre = 0, post = 0;
    int found = 0;
    char *line = in;

    while (line && *line) {
        char *nl = strchr(line, '\n');
        char *s = line;
        while (*s == ' ' || *s == '\t')
            s++;
        if (strncmp(s, "speed", 5) == 0) {
            char *q = s + 5;
            while (*q == ' ' || *q == '\t')
                q++;
            if (*q == '=') {
                pre = (size_t)(line - in);
                post = nl ? (size_t)(nl + 1 - in) : (size_t)n;
                found = 1;
                break;
            }
        }
        line = nl ? nl + 1 : NULL;
    }

    int m;
    if (found)
        m = snprintf(out, sizeof(out), "%.*sspeed=%.2f\n%s",
                     (int)pre, in, v, in + post);
    else
        m = snprintf(out, sizeof(out), "%sspeed=%.2f\n", in, v);
    if (m <= 0)
        return;
    if ((size_t)m >= sizeof(out))
        m = (int)sizeof(out) - 1;

    fd = sceKernelOpen(INI_PATH, O_WRONLY_ | O_CREAT_ | O_TRUNC_, 0777);
    if (fd >= 0) {
        sceKernelWrite(fd, out, (size_t)m);
        sceKernelClose(fd);
    }
}

static void turbo_step(int dir)
{
    float v = g_speed + (dir > 0 ? 0.05f : -0.05f);

    if (v < 1.00f)
        v = 1.00f;
    if (v > 2.00f)
        v = 2.00f;
    if (v == g_speed)
        return;

    g_speed = v;
    save_speed(v);
    tlog("hotkey: speed=%.2f", v);

    int filled = (int)((v - 1.0f) * 10.0f + 0.5f);
    if (filled < 0)
        filled = 0;
    if (filled > 10)
        filled = 10;

    char bar[11];
    for (int i = 0; i < 10; i++)
        bar[i] = i < filled ? '#' : '-';
    bar[10] = 0;

    notify_user("DMC5 Turbo  %.2fx  [%s]", v, bar);
}

static void pad_tick(uint64_t now)
{
    if (g_pad_handle < 0) {
        pad_adopt(now);
        return;
    }
    if (now < g_next_pad_poll)
        return;
    g_next_pad_poll = now + 16666ULL; /* ~60 Hz */

    OrbisPadData data;
    memset(&data, 0, sizeof(data));
    if (scePadReadState(g_pad_handle, &data) != 0 || !data.connected)
        return;

    uint32_t b = data.buttons;
    uint32_t hot = b & (ORBIS_PAD_BUTTON_LEFT | ORBIS_PAD_BUTTON_RIGHT);
    int dir = 0;

    if (b & ORBIS_PAD_BUTTON_L3) {
        uint32_t pressed = hot & ~g_prev_hot;
        if (pressed & ORBIS_PAD_BUTTON_RIGHT) {
            dir = 1;
            g_next_repeat = now + 400000ULL;
        } else if (pressed & ORBIS_PAD_BUTTON_LEFT) {
            dir = -1;
            g_next_repeat = now + 400000ULL;
        } else if (hot && now >= g_next_repeat) {
            dir = (hot & ORBIS_PAD_BUTTON_RIGHT) ? 1 : -1;
            g_next_repeat = now + 120000ULL;
        }
        g_prev_hot = hot;
    } else {
        g_prev_hot = 0;
    }

    if (dir)
        turbo_step(dir);
}

/* ------------------------------------------------------------------ pin */

static void dump_app_fields(uint64_t app)
{
    uint32_t o = g_off_speed;

    tlog("app=%#lx mode=%s speed_off=%#x speed=%.4f",
         (unsigned long)app,
         g_profile_mode ? "v1.00-profile" : "scan",
         o, *(volatile float *)(uintptr_t)(app + o));
    tlog("block +0=%.3f +4=%.3f +8=%.3f +C=%.5f +18=%.1f +1C=%.1f",
         *(volatile float *)(uintptr_t)(app + o),
         *(volatile float *)(uintptr_t)(app + o + 4),
         *(volatile float *)(uintptr_t)(app + o + 8),
         *(volatile float *)(uintptr_t)(app + o + 0x0C),
         *(volatile float *)(uintptr_t)(app + o + 0x18),
         *(volatile float *)(uintptr_t)(app + o + 0x1C));

    if (g_profile_mode) {
        double tick_scale;
        memcpy(&tick_scale, (void *)(uintptr_t)(app + OFF_TICK_SCALE), 8);
        tlog("tick_scale(+0x278)=%.9f", tick_scale);
    }
}

static void *worker(void *arg)
{
    (void)arg;
    uint64_t next_ini = 0, next_hb = 0;
    uint32_t last_frame = 0;
    int not_ready_logs = 0;

    tlog("worker started");

    while (g_running) {
        if (g_idle) {
            sceKernelSleep(5);
            continue;
        }

        if (!g_base) {
            uint64_t b = find_main_base();
            if (!b) {
                sceKernelSleep(2);
                continue;
            }
            g_base = b;

            scan_getter_slots();
            tlog("scan: %d getter site(s) -> %d distinct singleton slot(s)",
                 g_sites, g_slot_count);

            if (g_slot_count > 0) {
                g_profile_mode = 0;
                tlog("mode: runtime scan (region/version independent)");
            } else if (eboot_matches(b)) {
                g_profile_mode = 1;
                g_slot_va = b + APP_SLOT_VA;
                g_off_speed = OFF_GLOBAL_SPEED;
                tlog("mode: v1.00 profile fallback (signature verified)");
            } else {
                tlog("no compatible getter code found - leaving game untouched");
                g_idle = 1;
                continue;
            }
        }

        if (!g_app) {
            if (g_profile_mode) {
                uint64_t p = *(volatile uint64_t *)(uintptr_t)g_slot_va;
                if (p == 0) {
                    sceKernelSleep(1);
                    continue;
                }
                if (app_valid(p)) {
                    g_app = p;
                    tlog("Application singleton acquired: %#lx (slot %#lx)",
                         (unsigned long)g_app, (unsigned long)g_slot_va);
                    dump_app_fields(g_app);
                } else if (++not_ready_logs <= 3) {
                    tlog("singleton %#lx not ready yet, waiting", (unsigned long)p);
                    sceKernelSleep(1);
                    continue;
                } else {
                    sceKernelSleep(1);
                    continue;
                }
            } else {
                int found = 0;

                for (int i = 0; i < g_slot_count && !found; i++) {
                    uint64_t p = *(volatile uint64_t *)(uintptr_t)g_slots[i];
                    uint32_t off;

                    if (!valid_ptr(p))
                        continue;
                    if (!find_speed_offset(p, &off))
                        continue;

                    /* confirm the slot still holds the same object */
                    sceKernelUsleep(300 * 1000);
                    uint64_t p2 = *(volatile uint64_t *)(uintptr_t)g_slots[i];
                    uint32_t off2;
                    if (p2 != p || !find_speed_offset(p2, &off2) || off2 != off)
                        continue;

                    g_app = p;
                    g_slot_va = g_slots[i];
                    g_off_speed = off;
                    found = 1;
                    tlog("Application discovered: %#lx (slot %#lx, off %#x)",
                         (unsigned long)g_app, (unsigned long)g_slot_va, off);
                    dump_app_fields(g_app);
                }

                if (!found) {
                    if (++not_ready_logs <= 3)
                        tlog("waiting for Application singleton ...");
                    sceKernelSleep(1);
                    continue;
                }
            }
        }

        /* Re-check the slot: the game must own the same object we validated. */
        uint64_t slot = *(volatile uint64_t *)(uintptr_t)g_slot_va;
        if (slot != g_app) {
            tlog("singleton changed (%#lx -> %#lx), re-resolving",
                 (unsigned long)g_app, (unsigned long)slot);
            g_app = 0;
            continue;
        }

        uint64_t now = sceKernelGetProcessTime();
        pad_tick(now);

        if (!g_hint_sent && g_pad_handle >= 0) {
            g_hint_sent = 1;
            notify_user("DMC5 Turbo %.2fx - hold L3 + D-pad Left/Right", g_speed);
        }

        /* THE PIN: same field/meaning as the PC mod's [Application+0x380]. */
        float s = g_speed;
        *(volatile float *)(uintptr_t)(g_app + g_off_speed) = s;
        g_writes++;

        if (now >= next_ini) {
            read_ini();
            next_ini = now + INI_RELOAD_US;
        }
        if (now >= next_hb) {
            float cur = *(volatile float *)(uintptr_t)(g_app + g_off_speed);
            float ft  = *(volatile float *)(uintptr_t)(g_app + g_off_speed + 8);
            uint32_t fc = *(volatile uint32_t *)(uintptr_t)
                              (g_app + g_off_speed + (OFF_FRAME_COUNT - OFF_GLOBAL_SPEED));
            tlog("hb speed=%.3f eng=%.3f writes=%lu frame=%.2fms fc=+%lu",
                 g_speed, cur, (unsigned long)g_writes, ft,
                 (unsigned long)(fc - last_frame));
            last_frame = fc;
            next_hb = now + HEARTBEAT_US;
        }

        sceKernelUsleep((unsigned)PIN_INTERVAL_US);
    }

    tlog("worker exiting");
    g_threadDone = 1;
    return NULL;
}

/* --------------------------------------------------------- plugin entry */

ATTR_REALIGN ATTR_PUBLIC int32_t plugin_load(int32_t argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    g_running = 1;
    g_threadDone = 0;

    tlog("plugin_load v0.08 (SDK %#x)", sys_sdk_version());
    tlog("target: DMC5 PS4 - runtime discovery of Application speed");
    tlog("control: hold L3 + D-pad Left/Right = 1.00x..2.00x (0.05 steps)");

    read_ini();
    tlog("config: speed=%.3f (default %.2f)", g_speed, DEFAULT_SPEED);

    OrbisPthread thread;
    int rc = scePthreadCreate(&thread, NULL, worker, NULL, "turbo_pin");
    if (rc != 0) {
        tlog("FATAL: scePthreadCreate failed: %#x", rc);
        g_running = 0;
        g_threadDone = 1;
        return 0;
    }
    tlog("pin thread created");
    return 0;
}

ATTR_REALIGN ATTR_PUBLIC int32_t plugin_unload(int32_t argc, const char *argv[])
{
    (void)argc;
    (void)argv;

    tlog("plugin_unload requested");
    g_running = 0;

    /* Let the worker leave its loop before this module can be unmapped. */
    for (int i = 0; i < 300 && !g_threadDone; i++)
        sceKernelUsleep(10 * 1000);

    if (!g_threadDone)
        tlog("WARNING: worker still busy at unload");

    if (g_app && g_slot_va) {
        uint64_t slot = *(volatile uint64_t *)(uintptr_t)g_slot_va;
        float cur = slot == g_app
                        ? *(volatile float *)(uintptr_t)(g_app + g_off_speed)
                        : 0.0f;
        if (slot == g_app && cur == g_speed) {
            /* still the value we pinned - safe to hand back to the engine */
            *(volatile float *)(uintptr_t)(g_app + g_off_speed) = 1.0f;
            tlog("restored GlobalSpeed to 1.0");
        }
    }

    tlog("plugin_unload done");
    return 0;
}

ATTR_HIDDEN int32_t module_start(size_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}

ATTR_HIDDEN int32_t module_stop(size_t argc, const void *args)
{
    (void)argc;
    (void)args;
    return 0;
}
