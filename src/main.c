/**
 * Need for Speed: Underground (Xbox, NFSU1) - recompiled game entry point
 *
 * Boot order (same on every host):
 *   crash reporter -> load default.xbe -> xbox_MemoryLayoutInit ->
 *   xbox_kernel_init -> xbox_path_init -> xbox_kernel_bridge_init ->
 *   g_esp = XBOX_STACK_TOP -> recomp_dispatch_init -> xbe_entry_point()
 *
 * Game data is the extracted disc (default.xbe, NFSUNDER/, B3/). Where it is
 * looked for:
 *   Windows/Linux: $NFSU2_GAME_DIR, else ./game
 *   Switch:        sdmc:/switch/nfsu2x/game
 */

#ifdef _WIN32
#  include <windows.h>
#  include <dbghelp.h>
#else
#  include <signal.h>
#  include <unistd.h>
/* POSIX, unconditional: NFSU2_IOS is only defined further down (from
 * __APPLE__ macros), so an #ifdef NFSU2_IOS here skipped the header on real
 * iOS builds while command-line -DNFSU2_IOS=1 checks on Linux saw it. */
#  include <sys/stat.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <errno.h>
#include <signal.h>

#include <xbox/xboxrecomp.h>

#ifdef __SWITCH__
/* libnx lives in switch_nx.c: <switch.h> and the Win32 vocabulary collide. */
void switch_boot(void);
void switch_shutdown(void);
void switch_error_dialog(const char *msg, const char *details);
#elif !defined(_WIN32)

int SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message,
                             void *window);
#endif

extern RECOMP_TLS uint32_t g_eax, g_ecx, g_edx, g_esp;
extern RECOMP_TLS uint32_t g_ebx, g_esi, g_edi;
extern ptrdiff_t g_xbox_mem_offset;
extern volatile uint32_t g_icall_trace[16];
extern volatile uint32_t g_icall_trace_idx;

typedef struct MCPXAPUState MCPXAPUState;
extern MCPXAPUState *mcpx_apu_init_standalone(uint8_t *ram_ptr);
extern MCPXAPUState *g_apu_state;
extern uint64_t mcpx_apu_mmio_read(MCPXAPUState *d, uint64_t addr, unsigned int size);
extern void mcpx_apu_mmio_write(MCPXAPUState *d, uint64_t addr, uint64_t val,
                                unsigned int size);

#define NFSU2_APU_BASE 0xFE800000u
#define NFSU2_APU_END  0xFE880000u

static uint32_t apu_mmio_rd(void *opaque, uint32_t off, unsigned size)
{
    return (uint32_t)mcpx_apu_mmio_read((MCPXAPUState *)opaque, off, size);
}

static void apu_mmio_wr(void *opaque, uint32_t off, uint32_t val, unsigned size)
{
    mcpx_apu_mmio_write((MCPXAPUState *)opaque, off, val, size);
}

extern void xbe_entry_point(void);
extern int recomp_dispatch_init(void);
extern void xbox_path_init(const char *game_dir, const char *save_dir);

/* Disc image backend (xboxrecomp/src/kernel/xbox_xiso.c). */
extern int xiso_autodetect(const char *game_dir);
extern int xiso_find_image(const char *dir, char *out, size_t n);
extern void xiso_unmount(void);
extern void *xiso_read_file(const char *rel, size_t *size);
extern const char *xiso_image_path(void);
extern void xiso_sha256(const void *data, size_t len, uint8_t out[32]);

/* The disc this build expects. These values belong to the gen/ the binary was
 * built from (the XBE the lifter read), so they must change together with it:
 * NFSU1 NTSC-U, default.xbe certificate title ID 0x45410047, certificate
 * version 0x00000001. A different title ID is refused; a different SHA-256
 * (another printing) only warns, because the lifted code may still match. */
#define NFSU2_XBE_TITLE_ID 0x45410047u
#define NFSU2_XBE_VERSION  0x00000001u
static const char NFSU2_XBE_SHA256[] =
    "cdd562ffcc70906b3fecd841db4dc10fd463664b05e03f896a8fb38b9a7d878a";

/* Check the default.xbe read from a disc image. Returns 0 to refuse. */
static int check_disc_identity(const uint8_t *xbe, size_t size)
{
    uint8_t sha[32];
    char hex[65];
    uint32_t base, cert_va, title_id, version = 0;
    size_t cert, i;

    if (size < 0x200 || memcmp(xbe, "XBEH", 4) != 0) {
        fprintf(stderr, "[XISO] FATAL: default.xbe in the image is not an XBE\n");
        return 0;
    }
    memcpy(&base, xbe + 0x104, 4);          /* XBE header: base address   */
    memcpy(&cert_va, xbe + 0x118, 4);       /* XBE header: certificate VA */
    cert = (size_t)(cert_va - base);
    if (cert_va < base || cert + 0xB0 > size) {
        fprintf(stderr, "[XISO] FATAL: default.xbe in the image has no "
                "readable certificate\n");
        return 0;
    }
    memcpy(&title_id, xbe + cert + 0x08, 4);
    memcpy(&version, xbe + cert + 0xAC, 4);
    if (title_id != NFSU2_XBE_TITLE_ID) {
        fprintf(stderr, "[XISO] FATAL: wrong game in %s: title ID 0x%08X, "
                "this build runs only NFSU1 NTSC-U (title ID 0x%08X)\n",
                xiso_image_path(), title_id, NFSU2_XBE_TITLE_ID);
        return 0;
    }
    xiso_sha256(xbe, size, sha);
    for (i = 0; i < 32; i++)
        snprintf(hex + 2 * i, 3, "%02x", sha[i]);
    if (version != NFSU2_XBE_VERSION || strcmp(hex, NFSU2_XBE_SHA256) != 0)
        fprintf(stderr, "[XISO] warning: default.xbe differs from the build's "
                "disc (version 0x%08X, SHA-256 %s); expected version 0x%08X, "
                "%s. Continuing.\n", version, hex, NFSU2_XBE_VERSION,
                NFSU2_XBE_SHA256);
    else
        fprintf(stderr, "[XISO] default.xbe matches the build (SHA-256 %s)\n", hex);
    return 1;
}

#define NFSU2_ENTRY_POINT 0x00164356u


/* iOS detection, once: the iphoneos SDK predefines __IPHONE_OS_VERSION_MIN_REQUIRED__.
 * (TargetConditionals' TARGET_OS_IPHONE only exists after its header.) */
/* clang's Darwin driver predefines __ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__
 * for -target arm64-apple-ios (the plain __IPHONE_OS_VERSION_MIN_REQUIRED__
 * spelling only exists after TargetConditionals.h). Accept both spellings. */
#if defined(__APPLE__) && (defined(__ENVIRONMENT_IPHONE_OS_VERSION_MIN_REQUIRED__) || \
                           defined(__IPHONE_OS_VERSION_MIN_REQUIRED__))
#  define NFSU2_IOS 1
#endif

#ifdef NFSU2_IOS
static void ios_crash_handler(int sig)
{
    static const char msg[] =
        "[CRASH] fatal signal caught - last [VKINIT] marker above shows the stage\n";
    (void)!write(2, msg, sizeof msg - 1);
    _exit(139);
}

/* SDL constants (values from SDL_video.h) — main.c avoids including SDL.h */
#define SDL_INIT_VIDEO          0x00000020u
#define SDL_WINDOWPOS_CENTERED  0x2FFF0000u
#define SDL_WINDOW_VULKAN       0x10000000u
#define SDL_WINDOW_ALLOW_HIGHDPI 0x00002000u
#define SDL_WINDOW_RESIZABLE    0x00000020u
int SDL_InitSubSystem(uint32_t flags);
void *SDL_CreateWindow(const char *title, int x, int y, int w, int h, uint32_t flags);
const char *SDL_GetError(void);
int SDL_WaitEvent(void *event);
void SDL_RaiseWindow(void *window);
/* Settings: Documents/nfsu2_env.txt, KEY=VALUE (Switch nfsu2x_env.txt format). */
static struct { char key[64]; char val[64]; unsigned char dirty; } s_cfg[64];   /* 64: cfg_load
 * reads up to 64 lines; a 17th line used to overflow the 16-entry array.
 * key[64]: MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS is 36 characters and a
 * 32-byte key exported a truncated name MoltenVK never reads. */
static int s_cfg_n;
static const char *cfg_get(const char *k)
{
    for (int i = 0; i < s_cfg_n; i++)
        if (!strcmp(s_cfg[i].key, k))
            return s_cfg[i].val;
    return NULL;
}
static char *cfg_trim(char *t)
{
    while (*t == ' ' || *t == '\t')
        t++;
    char *e = t + strlen(t);
    while (e > t && (e[-1] == ' ' || e[-1] == '\t'))
        *--e = 0;
    return t;
}

static char s_cfg_path[1100];   /* where nfsu2_cfg_set writes back */

static void cfg_load(const char *path)
{
    FILE *f;
    char line[128];
    /* Recorded before fopen: a missing file is created by the first save. */
    snprintf(s_cfg_path, sizeof s_cfg_path, "%s", path);
    f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "nfsu2: no settings file at %s (%s)\n",
                path, strerror(errno));
        return;
    }
    while (s_cfg_n < 64 && fgets(line, sizeof line, f)) {
        char *eq = strchr(line, '=');
        if (!eq || eq == line)
            continue;
        *eq = 0;
        char *nl = eq + 1 + strcspn(eq + 1, "\r\n");
        *nl = 0;
        /* UTF-8 BOM on the first line (Files app edits) shifts the key. */
        if (s_cfg_n == 0 && (unsigned char)line[0] == 0xEF &&
            (unsigned char)line[1] == 0xBB && (unsigned char)line[2] == 0xBF)
            memmove(line, line + 3, strlen(line + 2));
        char *k = cfg_trim(line), *v = cfg_trim(eq + 1);
        snprintf(s_cfg[s_cfg_n].key, sizeof s_cfg[0].key, "%s", k);
        snprintf(s_cfg[s_cfg_n].val, sizeof s_cfg[0].val, "%s", v);
        /* Same contract as the Switch's nfsu2x_env.txt: every line is also
         * exported to the environment so renderer/game switches
         * (RECOMP_PRESENT_INTERVAL, RECOMP_GL_SCALE, ...) read it. */
        setenv(s_cfg[s_cfg_n].key, s_cfg[s_cfg_n].val, 0);
        s_cfg_n++;
    }
    fclose(f);
    fprintf(stderr, "nfsu2: settings loaded (%d keys) from %s\n", s_cfg_n, path);
    for (int i = 0; i < s_cfg_n; i++)
        fprintf(stderr, "nfsu2:   %s=%s\n", s_cfg[i].key, s_cfg[i].val);
}
const char *nfsu2_cfg_path(void)
{
    return s_cfg_path[0] ? s_cfg_path : NULL;
}

/* The file's value (as changed by nfsu2_cfg_set), else the environment (a
 * key set outside the file), else NULL. The pointer may be into s_cfg: copy it before calling nfsu2_cfg_set. */
const char *nfsu2_cfg_get(const char *key)
{
    const char *v = cfg_get(key);
    return v ? v : getenv(key);
}

/* Does the line [p, eol) hold `key` (trimmed, as cfg_load reads it)? */
static int cfg_line_has_key(const char *p, const char *eol, const char *key,
                            size_t klen)
{
    const char *eq = (const char *)memchr(p, '=', (size_t)(eol - p));
    const char *e;
    if (!eq || eq == p)
        return 0;
    while (p < eq && (*p == ' ' || *p == '\t'))
        p++;
    e = eq;
    while (e > p && (e[-1] == ' ' || e[-1] == '\t'))
        e--;
    return (size_t)(e - p) == klen && memcmp(p, key, klen) == 0;
}

/* Rewrite the settings file with key=val. Every other line is copied byte
 * for byte (comments, unknown keys, line endings). The new file goes to
 * <path>.tmp first and rename() swaps it in, so a kill mid-write cannot
 * leave a half file. Returns 1 on success. */
static int cfg_save(const char *key, const char *val)
{
    char *buf = NULL;
    size_t len = 0, cap = 0, pos = 0, klen = strlen(key);
    char tmp[sizeof s_cfg_path + 8];
    int done = 0, ok = 1;
    FILE *f = fopen(s_cfg_path, "rb");

    if (f) {
        for (;;) {
            size_t n;
            if (len + 1 >= cap) {
                size_t nc = cap ? cap * 2 : 1024;
                char *nb = (char *)realloc(buf, nc);
                if (!nb) {
                    free(buf);
                    fclose(f);
                    return 0;
                }
                buf = nb;
                cap = nc;
            }
            n = fread(buf + len, 1, cap - len - 1, f);
            if (n == 0)
                break;
            len += n;
            if (len > (1u << 20)) {   /* not a settings file */
                free(buf);
                fclose(f);
                return 0;
            }
        }
        fclose(f);
    }

    snprintf(tmp, sizeof tmp, "%s.tmp", s_cfg_path);
    f = fopen(tmp, "wb");
    if (!f) {
        fprintf(stderr, "nfsu2: cannot write %s (%s)\n", tmp, strerror(errno));
        free(buf);
        return 0;
    }
    while (pos < len) {
        size_t e = pos, next, eol, bom = 0;
        while (e < len && buf[e] != '\n')
            e++;
        next = e < len ? e + 1 : e;
        eol = e;
        if (eol > pos && buf[eol - 1] == '\r')
            eol--;
        /* UTF-8 BOM on the first line: skip it for the match, keep it. */
        if (pos == 0 && eol >= 3 && (unsigned char)buf[0] == 0xEF &&
            (unsigned char)buf[1] == 0xBB && (unsigned char)buf[2] == 0xBF)
            bom = 3;
        if (!done && cfg_line_has_key(buf + pos + bom, buf + eol, key, klen)) {
            if (bom)
                fwrite(buf, 1, bom, f);
            fprintf(f, "%s=%s", key, val);
            fwrite(buf + eol, 1, next - eol, f);   /* original line ending */
            done = 1;
        } else {
            fwrite(buf + pos, 1, next - pos, f);
        }
        pos = next;
    }
    if (!done) {
        if (len && buf[len - 1] != '\n')
            fputc('\n', f);
        fprintf(f, "%s=%s\n", key, val);
    }
    free(buf);
    if (fflush(f) != 0 || ferror(f))
        ok = 0;
    else
        fsync(fileno(f));
    if (fclose(f) != 0)
        ok = 0;
    if (ok && rename(tmp, s_cfg_path) != 0) {
        fprintf(stderr, "nfsu2: rename %s failed (%s)\n", tmp, strerror(errno));
        ok = 0;
    }
    if (!ok)
        remove(tmp);
    return ok;
}

/* Main thread only (the panel). Updates the live table, then the file.
 * Returns 1 when the file was written too. No setenv here: the renderer
 * calls getenv() on its own thread every flip, and setenv can reallocate
 * environ under it. Boot-time keys are exported once by cfg_load; the panel
 * reads values back through nfsu2_cfg_get (s_cfg first); live items take
 * effect through their own setters. */
int nfsu2_cfg_set(const char *key, const char *val)
{
    int idx = -1, i;

    if (!key || !val || !key[0] ||
        strlen(key) >= sizeof s_cfg[0].key || strlen(val) >= sizeof s_cfg[0].val ||
        strpbrk(key, "=\r\n \t") || strpbrk(val, "=\r\n")) {
        fprintf(stderr, "nfsu2: setting rejected (bad key or value)\n");
        return 0;
    }
    for (i = 0; i < s_cfg_n; i++)
        if (!strcmp(s_cfg[i].key, key)) {
            idx = i;
            break;
        }
    if (idx < 0) {
        if (s_cfg_n >= (int)(sizeof s_cfg / sizeof s_cfg[0])) {
            fprintf(stderr, "nfsu2: setting %s dropped (table full)\n", key);
            return 0;
        }
        idx = s_cfg_n++;
        snprintf(s_cfg[idx].key, sizeof s_cfg[0].key, "%s", key);
    }
    snprintf(s_cfg[idx].val, sizeof s_cfg[0].val, "%s", val);
    /* Changed since boot: nfsu2_cfg_export_changed() exports it, even when
     * the file write below fails (the value still applies to this boot). */
    s_cfg[idx].dirty = 1;

    if (!s_cfg_path[0]) {
        fprintf(stderr, "nfsu2: setting %s=%s not saved (no settings path)\n", key, val);
        return 0;
    }
    if (!cfg_save(key, val)) {
        fprintf(stderr, "nfsu2: setting %s=%s not saved (write failed)\n", key, val);
        return 0;
    }
    fprintf(stderr, "nfsu2: setting %s=%s saved\n", key, val);
    return 1;
}

/* Export the keys nfsu2_cfg_set changed since boot (or since the last call)
 * with overwrite, then clear their flags. Same contract as cfg_load's bridge
 * (empty values too: SDL_AUDIO_CATEGORY "on" is ""), but overwriting, so a
 * launch-screen choice beats the value cfg_load exported. setenv may
 * reallocate environ: main() calls this on the main thread before the first
 * thread exists, and nowhere else. */
void nfsu2_cfg_export_changed(void)
{
    int i, n = 0;

    for (i = 0; i < s_cfg_n; i++) {
        if (!s_cfg[i].dirty)
            continue;
        s_cfg[i].dirty = 0;
        if (setenv(s_cfg[i].key, s_cfg[i].val, 1) == 0)
            n++;
    }
    if (n)
        fprintf(stderr, "nfsu2: %d launch-screen setting(s) exported\n", n);
}

float nfsu2_cfg_overlay_alpha(void)
{
    const char *v = cfg_get("NFSU2_OVERLAY_ALPHA");
    float a = v ? (float)atof(v) : 0.55f;
    return a < 0.2f ? 0.2f : a > 0.8f ? 0.8f : a;
}
int nfsu2_cfg_show_fps(void)
{
    const char *v = cfg_get("NFSU2_SHOW_FPS");
    return v ? (*v == '1' || *v == 'y') : 1;
}
void SDL_SetHint(const char *name, const char *value);
int SDL_PollEvent(void *event);
/* SDL_syswm.h subset: SDL_GetWindowWMInfo(SDL_Window*, SDL_SysWMinfo*).
 * Layout per SDL 2.30.x: version = three single BYTES, then an int
 * subsystem at offset 4, then the union at offset 8. The UIKit variant
 * writes the UIWindow at info.uikit.window (and framebuffer fields after
 * it from 2.0.4 - hence the 64-byte dummy to make room). */
struct sdl_syswm_uikit { void *window; uint32_t framebuffer, colorbuffer,
                          resolveframebuffer; };
struct sdl_syswm_info {
    uint8_t version_major, version_minor, version_patch;
    int subsystem;
    union {
        struct sdl_syswm_uikit uikit;
        uint8_t dummy[64];
    } info;
};
int SDL_GetWindowWMInfo(void *window, struct sdl_syswm_info *info);
/* SDL_EventFilter is int (*)(void *userdata, SDL_Event *event); the event
 * pointer is opaque here and its type is the first Uint32. */
void SDL_AddEventWatch(int (*filter)(void *userdata, void *event), void *userdata);
/* SDL_hints.h / SDL_video.h / SDL_syswm.h values */
#define SDL_HINT_ORIENTATIONS "SDL_IOS_ORIENTATIONS"
#define SDL_WINDOWEVENT 0x200
#define SDL_QUIT 0x100
#define SDL_SYSWM_UIKIT 5   /* SDL_syswm.h: UNKNOWN0 WINDOWS1 X11-2 DIRECTFB3 COCOA4 UIKIT5 */
#define SDL_APP_WILLENTERBACKGROUND 0x103
#define SDL_APP_DIDENTERBACKGROUND 0x104
#define SDL_APP_DIDENTERFOREGROUND 0x106
/* SDL_WindowEvent: type, timestamp, windowID, event, padding, data1, data2 */
struct sdl_winev { uint32_t type, ts; uint32_t win; uint8_t ev; int pad; int d1, d2; };
#endif

#if defined(__SWITCH__)
#  define NFSU2_DEFAULT_GAME_DIR "sdmc:/switch/nfsu2x/game"
#  define NFSU2_DEFAULT_SAVE_DIR "sdmc:/switch/nfsu2x/save"
#elif defined(NFSU2_IOS)
/* iOS sandbox: the app bundle is read-only; the game data goes in the app's
 * Documents folder (Files app → On My iPhone → NFSU2, or pushed by
 * SideStore/Filza). NFSU2_GAME_DIR still wins if set. */
#  define NFSU2_DEFAULT_GAME_DIR NSTemporaryDirectory_path("Documents")
#  define NFSU2_DEFAULT_SAVE_DIR NSTemporaryDirectory_path("Documents/Saves")
#else
#  define NFSU2_DEFAULT_GAME_DIR "game"
#  define NFSU2_DEFAULT_SAVE_DIR NULL
#endif

#if defined(NFSU2_IOS)
/* Forward declaration: the default-dir macros expand to this call. */
static const char *NSTemporaryDirectory_path(const char *sub);
/* stderr has no console on iOS: mirror it to Documents/nfsu2_log.txt so the
 * first boots are diagnosable from the Files app. */
static void ios_log_to_documents(void)
{
    static char log_path[1100];
    static int done;
    if (done)
        return;
    done = 1;
    snprintf(log_path, sizeof log_path, "%s/nfsu2_log.txt",
             NSTemporaryDirectory_path("Documents"));
    if (!freopen(log_path, "w", stderr))
        return;
    setvbuf(stderr, NULL, _IOLBF, 0);
}
/* Documents = the app sandbox's Documents folder. Computed once, kept
 * for the process lifetime (xbox_path_init copies the string). */
static char nfsu2_ios_dir[1024];
static char nfsu2_ios_save[1024];
static char nfsu2_ios_gamedir[1024];
static const char *NSTemporaryDirectory_path(const char *sub)
{
    /* CFFIXED_USER_HOME is the sandbox home iOS sets for every app; Documents
     * sits directly in it. TMPDIR is <sandbox>/tmp, so from there Documents is
     * one level up. */
    const char *home = getenv("CFFIXED_USER_HOME");
    if (home && home[0])
        snprintf(nfsu2_ios_dir, sizeof nfsu2_ios_dir, "%s/Documents", home);
    else {
        const char *tmp = getenv("TMPDIR");
        if (!tmp || !tmp[0])
            tmp = "/tmp";
        size_t n = strlen(tmp);
        while (n > 1 && tmp[n - 1] == '/')
            n--;
        snprintf(nfsu2_ios_dir, sizeof nfsu2_ios_dir, "%.*s/Documents",
                 (int)(strcmp(tmp + n - 4, "/tmp") == 0 ? n - 4 : n), tmp);
    }
    snprintf(nfsu2_ios_save, sizeof nfsu2_ios_save, "%s/Saves", nfsu2_ios_dir);
    (void)sub;
    return nfsu2_ios_dir;
}
static const char *ios_save_dir(void)
{
    if (!nfsu2_ios_save[0])
        NSTemporaryDirectory_path("Documents");
    return nfsu2_ios_save;
}

/* Folders people copy the game into, probed in this order after the plain
 * game dir. Shared with game_main so the launch screen and the boot agree. */
static const char *const nfsu2_ios_alts[] = { "NFSU1-data", "game", "nfsu1", "NFSU1" };

/* Build label for the launch screen. */
const char *nfsu2_build_label(void)
{
    return "u1-4 " __DATE__;
}

/* Is the launch screen asked for? NFSU2_LAUNCHER, default OFF (v29): the game
 * starts at once and the gear button opens the settings. 1/y/t on, anything
 * else (0/n/f, an odd value, unset) off. Missing game data shows it anyway:
 * see the call site in main(). */
static int nfsu2_launcher_enabled(void)
{
    const char *v = nfsu2_cfg_get("NFSU2_LAUNCHER");

    return v && (*v == '1' || *v == 'y' || *v == 'Y' || *v == 't' || *v == 'T');
}

/* What game_main will find, decided WITHOUT mounting or reading anything, so
 * the launch screen can run on the main thread before the window exists.
 * Mirrors game_main's resolution in the same order (game_dir at lines
 * 1155-1185, xiso_autodetect(game_dir) at line 1196; keep them in step):
 *   1. NFSU2_GAME_DIR, else Documents                    (game_main: game_dir)
 *   2. <dir>/default.xbe; if absent, the first of nfsu2_ios_alts whose
 *      default.xbe exists becomes the dir; if none does the dir stays as is
 *   3. xiso_find_image(dir): NFSU2_ISO=0 off, NFSU2_ISO=<path> (beats a
 *      folder, as xiso_autodetect always did), no default.xbe -> the first
 *      *.iso in sorted order. Found and a regular file -> image.
 *   4. else default.xbe present -> folder
 *   5. else none
 * Returns 1 folder, 2 image, 0 none; desc gets "folder: <path>",
 * "image: <name> (<size>)" or "none". An image that exists but fails to
 * mount at boot cannot be seen from here (nothing is opened). Local buffers
 * only: nfsu2_ios_gamedir stays for game_main. */
int nfsu2_game_data_status(char *desc, size_t n)
{
    char dir[1100], probe[1200], img[1100];
    const char *gd = getenv("NFSU2_GAME_DIR");
    int found = 0;
    size_t i;
    FILE *f;
    struct stat st;

    if (!desc || n == 0)
        return 0;
    desc[0] = 0;
    if (!gd || !gd[0])
        gd = NFSU2_DEFAULT_GAME_DIR;
    snprintf(dir, sizeof dir, "%s", gd);

    snprintf(probe, sizeof probe, "%s/default.xbe", dir);
    f = fopen(probe, "rb");
    if (f) { fclose(f); found = 1; }
    for (i = 0; !found && i < sizeof nfsu2_ios_alts / sizeof nfsu2_ios_alts[0]; i++) {
        char alt[1100];

        snprintf(alt, sizeof alt, "%s/%s", dir, nfsu2_ios_alts[i]);
        snprintf(probe, sizeof probe, "%s/default.xbe", alt);
        f = fopen(probe, "rb");
        if (f) {
            fclose(f);
            snprintf(dir, sizeof dir, "%s", alt);
            found = 1;
        }
    }

    if (xiso_find_image(dir, img, sizeof img) &&
        stat(img, &st) == 0 && S_ISREG(st.st_mode)) {
        const char *base = strrchr(img, '/');
        double gb = (double)st.st_size / 1e9;

        snprintf(desc, n, "image: %s (%.2f GB)", base ? base + 1 : img, gb);
        return 2;
    }
    if (found) {
        snprintf(desc, n, "folder: %s", dir);
        return 1;
    }
    snprintf(desc, n, "none");
    return 0;
}
#endif

/* ------------------------------------------------------------------ */
/* Crash reporting                                                     */
/* ------------------------------------------------------------------ */

/* What the guest was doing: its registers and the return addresses on its
 * stack. This is the useful half of any crash report -- the host PC names a
 * generated sub_XXXXXXXX at best, the guest stack names the whole chain. */
void print_guest_state(void)
{
    fprintf(stderr, "  guest: eax=%08X ecx=%08X edx=%08X ebx=%08X\n",
            g_eax, g_ecx, g_edx, g_ebx);
    fprintf(stderr, "         esi=%08X edi=%08X esp=%08X\n",
            g_esi, g_edi, g_esp);

    if (g_xbox_mem_offset && g_esp >= XBOX_STACK_BASE
            && g_esp < XBOX_STACK_BASE + XBOX_STACK_SIZE) {
        const uint32_t *sp =
            (const uint32_t *)((uintptr_t)g_xbox_mem_offset + g_esp);
        int shown = 0, i;
        fprintf(stderr, "  guest stack (return addresses, innermost first):\n");
        for (i = 0; i < 512 && shown < 24; i++) {
            uint32_t v = sp[i];
            if (g_esp + (uint32_t)i * 4u >= XBOX_STACK_BASE + XBOX_STACK_SIZE)
                break;
            if (v > g_xbox_code_lo && v < g_xbox_code_hi) {
                fprintf(stderr, "    [esp+%-4d] 0x%08X\n", i * 4, v);
                shown++;
            }
        }
    }
    {
        unsigned k;
        fprintf(stderr, "  recent ICALL targets:");
        for (k = 0; k < 16; k++)
            fprintf(stderr, " %08X", g_icall_trace[(g_icall_trace_idx + k) & 15]);
        fprintf(stderr, "\n");
    }
    fflush(stderr);
}

#ifdef _WIN32
static LONG CALLBACK veh_handler(PEXCEPTION_POINTERS ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        uintptr_t fault = ep->ExceptionRecord->ExceptionInformation[1];
        char buf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *)buf;
        DWORD64 disp = 0;

        /* The NV2A aperture belongs to the runtime's own handlers. */
        if (fault >= 0xFD000000 && fault < 0xFE000000)
            return EXCEPTION_CONTINUE_SEARCH;

        fprintf(stderr, "[CRASH] access violation at RIP=0x%llX, %s 0x%llX "
                        "(guest VA 0x%08X)\n",
                (unsigned long long)ep->ContextRecord->Rip,
                ep->ExceptionRecord->ExceptionInformation[0] ? "write" : "read",
                (unsigned long long)fault,
                (uint32_t)(fault - (uintptr_t)g_xbox_mem_offset));
        memset(buf, 0, sizeof(buf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        if (SymFromAddr(GetCurrentProcess(), ep->ContextRecord->Rip, &disp, sym))
            fprintf(stderr, "  in %s+0x%llX\n", sym->Name,
                    (unsigned long long)disp);
        print_guest_state();
    }
    return EXCEPTION_CONTINUE_SEARCH;
}

static void install_crash_reporter(void)
{
    SymSetOptions(SYMOPT_DEFERRED_LOADS | SYMOPT_UNDNAME);
    SymInitialize(GetCurrentProcess(), NULL, TRUE);
    AddVectoredExceptionHandler(1, veh_handler);
}
#elif defined(__SWITCH__)
/* The CPU exception handler is in switch_nx.c; it calls print_guest_state. */
static void install_crash_reporter(void) {}
#else
#include <execinfo.h>
#ifdef __linux__
#include <sys/syscall.h>
#include <unistd.h>
#endif

static void crash_signal(int sig, siginfo_t *si, void *uc)
{
    (void)uc;
    fprintf(stderr, "[CRASH] signal %d, fault addr %p (guest VA 0x%08X)\n",
            sig, si->si_addr,
            (uint32_t)((uintptr_t)si->si_addr - (uintptr_t)g_xbox_mem_offset));
    print_guest_state();
    {
        /* The host frames name the lifted function (sub_XXXXXXXX) that
         * faulted -- the guest stack alone does not on a worker thread. */
        void *frames[32];
        int n = backtrace(frames, 32);
        fprintf(stderr, "  host backtrace:\n");
        fflush(stderr);
        backtrace_symbols_fd(frames, n, 2);
    }
    signal(sig, SIG_DFL);
    raise(sig);
}

#ifdef __linux__
/* SIGUSR1 to one thread (tgkill): print that thread's guest state and host
 * backtrace, then go on. For hangs: a stack snapshot of every guest thread. */
static void snapshot_signal(int sig, siginfo_t *si, void *uc)
{
    void *frames[32];
    int n;
    (void)sig; (void)si; (void)uc;
    fprintf(stderr, "[SNAP] thread %ld\n", (long)syscall(SYS_gettid));
    print_guest_state();
    n = backtrace(frames, 32);
    fprintf(stderr, "  host backtrace:\n");
    fflush(stderr);
    backtrace_symbols_fd(frames, n, 2);
}
#endif

static void install_crash_reporter(void)
{
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = crash_signal;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK;
    {
        static char altstack[64 * 1024];
        stack_t ss;
        ss.ss_sp = altstack;
        ss.ss_size = sizeof(altstack);
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGBUS, &sa, NULL);
    sigaction(SIGILL, &sa, NULL);
    sigaction(SIGFPE, &sa, NULL);
#ifdef __linux__
    sa.sa_sigaction = snapshot_signal;
    sigaction(SIGUSR1, &sa, NULL);
#endif
}
#endif

/* ------------------------------------------------------------------ */

static void *load_file(const char *path, size_t *out_size)
{
    FILE *f = fopen(path, "rb");
    long size;
    void *data;

    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    size = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (size <= 0) {
        fclose(f);
        return NULL;
    }
    data = malloc((size_t)size);
    if (data && fread(data, 1, (size_t)size, f) != (size_t)size) {
        free(data);
        data = NULL;
    }
    fclose(f);
    if (data)
        *out_size = (size_t)size;
    return data;
}

static void fatal(const char *msg)
{
    fprintf(stderr, "[FATAL] %s\n", msg);
#if defined(_WIN32)
    MessageBoxA(NULL, msg, "NFSU2 recomp", MB_ICONERROR);
#elif defined(__SWITCH__)
    switch_error_dialog(msg, NULL);
#else
    SDL_ShowSimpleMessageBox(0x10, "NFSU2 recomp", msg, NULL);
#endif
}

static int game_main(void);
void xbox_guest_pin(int interrupt);   /* win32_compat.c: the Xbox's one CPU */
void xbox_gil_enter(void);             /* kernel_bridge.c: the guest lock */
void xbox_gil_leave(void);
void xbox_gil_mark_main(void);

#ifdef __SWITCH__
#include <pthread.h>
/* The first guest thread runs inline on whichever host thread boots the
 * title, and Horizon's main thread stack is small for recompiled code; boot
 * on a thread with a stack sized like a desktop main thread instead. */
void xbox_nx_spread_thread(void);   /* win32_compat.c: off core 0, all cores allowed */
void xbox_nx_track_thread(void *entry);   /* win32_compat.c: Switch [perf] report */

static void *game_thread(void *arg)
{
    xbox_nx_spread_thread();
    xbox_nx_track_thread(NULL);          /* "game:" in the [perf] report */
    *(int *)arg = game_main();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc = 1;

    (void)argc;
    (void)argv;
    switch_boot();
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u * 1024 * 1024);
    if (pthread_create(&th, &attr, game_thread, &rc) == 0)
        pthread_join(th, NULL);
    else
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
    switch_shutdown();
    return rc;
}
#elif defined(NFSU2_IOS)
#include <pthread.h>
#if defined(__APPLE__)
#include <pthread/qos.h>
#define NFSU2_HAS_QOS 1
#endif
/* iOS: SDL renames main to SDL_main and calls it on the UIKit main thread.
 * game_main blocks there forever, so the run loop never turns: no screen
 * commit, no touch delivery, watchdog kills backgrounding. Mirror the
 * Switch layout instead: the game runs on a big pthread and the main thread
 * services SDL events. */
static void *ios_game_thread(void *arg)
{
#if defined(NFSU2_HAS_QOS)
    const char *q = getenv("RECOMP_QOS");
    if (!q || strcmp(q, "0") != 0) {     /* default on, RECOMP_QOS=0 off */
        pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
        fprintf(stderr, "[QOS] game thread -> user interactive\n");
    }
#endif
    *(int *)arg = game_main();
    return NULL;
}

/* Detached: reports the renderer state for 60s without blocking the main
 * run loop. If the screen stays black with no crash, these lines name the
 * guilty stage from the log alone. */
static void *ios_diag_thread(void *arg)
{
    extern int nv2a_vk_diag(void);
    int i;
    (void)arg;
    for (i = 0; i < 12; i++) {
        sleep(5);
        fprintf(stderr, "[DIAG] t=%ds renderer=0x%x (1 tried 2 ok 4 headless 8 dyn-vi)\n",
                (i + 1) * 5, nv2a_vk_diag());
    }
    return NULL;
}

/* Background handshake. SDL2 calls event watchers synchronously inside
 * UIKit's callback (SDL_OnApplicationWillResignActive / DidEnterBackground)
 * while it holds its watcher lock. iOS kills an app that commits GPU work
 * after it is backgrounded, and the poll loop below only sees the window
 * events after that callback has returned. So the watcher parks the
 * executor and drains the GPU here, but the wait must stay bounded
 * (300 ms): a watchdog also kills an app that blocks in the callback.
 * NFSU2_BG_HANDSHAKE=0: only set the flag (old behaviour; the executor then
 * parks without a drain). Check the type first; no other SDL calls here. */
static int s_bg_handshake = 1;

static int ios_bg_watch(void *userdata, void *event)
{
    extern int nv2a_vk_background_handshake(int timeout_ms);
    extern void nv2a_vk_set_background(int bg);
    uint32_t type = *(uint32_t *)event;
    (void)userdata;
    if (type != SDL_APP_WILLENTERBACKGROUND && type != SDL_APP_DIDENTERBACKGROUND)
        return 1;
    if (s_bg_handshake)
        nv2a_vk_background_handshake(300);
    else
        nv2a_vk_set_background(1);
    return 1;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc = 1;
    int vblank_lock_on = 1;

    (void)argc;
    (void)argv;
    /* Redirect the log FIRST so main-thread init output is captured. */
    ios_log_to_documents();
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "nfsu1: build u1-4 (u1-3 + EA Trax crash fixed, Auto Pad/Drive reads NFSU1, GPU wait sleeps)\n");
    fprintf(stderr, "nfsu2: HOME=%s TMPDIR=%s\n",
            getenv("CFFIXED_USER_HOME") ? getenv("CFFIXED_USER_HOME") : "(unset)",
            getenv("TMPDIR") ? getenv("TMPDIR") : "(unset)");
    {
        /* SA_ONSTACK: a guest-thread stack overflow must still reach the
         * handler (plain signal() would die on the overflowed stack). */
        struct sigaction sa;
        static char crash_stack[SIGSTKSZ * 4];
        stack_t ss;
        memset(&sa, 0, sizeof sa);
        sa.sa_handler = ios_crash_handler;
        sa.sa_flags = SA_ONSTACK;
        sigaction(SIGSEGV, &sa, NULL);
        sigaction(SIGBUS, &sa, NULL);
        sigaction(SIGILL, &sa, NULL);
        sigaction(SIGABRT, &sa, NULL);
        ss.ss_sp = crash_stack;
        ss.ss_size = sizeof crash_stack;
        ss.ss_flags = 0;
        sigaltstack(&ss, NULL);
    }
    /* Env and cwd BEFORE SDL_CreateWindow: SDL's iOS Vulkan loader calls
     * vkEnumerateInstanceExtensionProperties inside SDL_CreateWindow, and
     * MoltenVK reads its MVK_CONFIG_* environment once, on first use. A
     * setenv after that is ignored. */
    {
        /* Relative cache paths (progcache.bin, vkspirv.bin,
         * vkpipecache.bin) resolve against the cwd, which is read-only
         * on iOS. chdir into Documents so they persist across launches;
         * NFSU2_GAME_DIR overrides still win inside game_main. */
        const char *gd = getenv("NFSU2_GAME_DIR");
        const char *docs = NSTemporaryDirectory_path("Documents");
        if (chdir(gd && gd[0] ? gd : docs) == 0) {
            /* Settings live right there: nfsu2_env.txt. */
            char cfgp[1100];
            snprintf(cfgp, sizeof cfgp, "%s/nfsu2_env.txt", gd && gd[0] ? gd : docs);
            cfg_load(cfgp);
            fprintf(stderr, "nfsu2: cwd %s (caches persist)\n",
                    gd && gd[0] ? gd : docs);
        }
    }
    {
        /* Launch screen: AFTER cfg_load, BEFORE the defaults below, the
         * window and every thread. Off by default (v29); shown when
         * NFSU2_LAUNCHER=1, and always when there is no game data (it is the
         * only guidance for that). It runs on this thread only, writes its
         * choices with nfsu2_cfg_set (table + file, no setenv) and returns
         * on Play; the changed keys are then exported once with overwrite
         * (safe: no other thread exists yet). Any failure inside the
         * launcher is logged there and we go straight on to the game. The
         * skip path creates nothing the window needs (SDL builds its own
         * UIWindow), so it is the same flow as v13-v23. */
        extern int ios_launcher_run(void);
        char dd[1200];
        int no_data = nfsu2_game_data_status(dd, sizeof dd) == 0;
        if (no_data || nfsu2_launcher_enabled()) {
            fprintf(stderr, no_data ? "nfsu2: launch screen shown: no game data\n"
                                    : "nfsu2: launch screen shown (NFSU2_LAUNCHER=1)\n");
            fprintf(stderr, "nfsu2: launch screen %s\n",
                    ios_launcher_run() ? "done (Play)" : "not shown");
            nfsu2_cfg_export_changed();
        } else {
            fprintf(stderr, "nfsu2: launch screen off (NFSU2_LAUNCHER=1 shows it)\n");
        }
    }
    {
        /* Default render scale 2 on iOS (A17 has the fill rate; 480p
         * upscaled looks soft). cfg_load's bridge and the launch screen's
         * export ran first, so an explicit key always wins. */
        if (!getenv("RECOMP_GL_SCALE"))
            setenv("RECOMP_GL_SCALE", "2", 0);
        /* Async queue submits: the default (1) encodes inside
         * vkQueueSubmit on the calling thread, which then waits for the
         * CAMetalLayer drawable. Overwrite 0: a key in nfsu2_env.txt
         * wins; =1 restores synchronous submits for A/B. */
        setenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS", "0", 0);
        fprintf(stderr, "nfsu2: MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS=%s\n",
                getenv("MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS"));
        /* MoltenVK log level 1 = errors only. Level 2 (default) prints
         * "[mvk-warn] Metal does not support disabling primitive restart"
         * for every pipeline (123 lines in one short session). Same
         * rule: read once, so it must precede SDL_CreateWindow. */
        setenv("MVK_CONFIG_LOG_LEVEL", "1", 0);
        /* iOS defaults, set here on the main thread before any other
         * thread reads the environment; a key in nfsu2_env.txt wins.
         * Frame lag: the game waits on the previous frame's fence, so game
         * code and the executor overlap (Eden race 9-18 -> 21-24 fps; the
         * log says "[D3D] frame fence: previous frame"). Quiet: every log
         * line is an unbuffered write to Documents; drop the kernel and
         * executor chatter, as on the Switch. */
        setenv("RECOMP_FRAME_LAG", "1", 0);
        setenv("RECOMP_QUIET", "1", 0);
        /* 60 fps races (v27): present every vblank. The panel row has
         * def_on = 1 to match; a "0" in nfsu2_env.txt still wins. */
        setenv("RECOMP_PRESENT_INTERVAL", "1", 0);
        /* NFSU1: 4:3 and no movie crop until both are checked for this
         * title (game_main sets the same defaults; setting them here keeps
         * every setenv before the other threads start). */
        setenv("RECOMP_WIDESCREEN", "0", 0);
        setenv("NFSU2_MOVIE_CROP", "0", 0);
    }
    {
        extern void nv2a_vk_set_window(void *win);
        SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
        /* Defer all edge gestures: a steering thumb sliding off the bottom
         * edge must not leave the game. */
        SDL_SetHint("SDL_IOS_HIDE_HOME_INDICATOR", "2");
        SDL_InitSubSystem(SDL_INIT_VIDEO);
        /* Drawable size (v29): without ALLOW_HIGHDPI SDL sets the layer's
         * contentsScale to 1.0 and MoltenVK's surface extent is the size in
         * points. Decided here, on the main thread, before the window (and
         * before any thread starts: it reads NFSU2_POINT_RES). */
        extern int ios_point_res_wanted(char *desc, size_t n);
        char prdesc[96];
        int point_res = ios_point_res_wanted(prdesc, sizeof prdesc);
        fprintf(stderr, "nfsu2: drawable at %s (%s)\n",
                point_res ? "point resolution" : "native pixels", prdesc);
        fprintf(stderr, "nfsu2: creating SDL window on main thread\n");
        void *win = SDL_CreateWindow("NFSU1", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                                     1280, 720,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_RESIZABLE |
                                     (point_res ? 0 : SDL_WINDOW_ALLOW_HIGHDPI));
        fprintf(stderr, "nfsu2: window=%p (%s)\n", win, win ? "ok" : SDL_GetError());
        nv2a_vk_set_window(win);
        if (!win)
            fprintf(stderr, "nfsu2: FATAL no window; refusing off-main fallback\n");
        else {
            extern int nv2a_vk_init_on_main(void);
            fprintf(stderr, "nfsu2: eager renderer init on main thread\n");
            int ok = nv2a_vk_init_on_main();
            fprintf(stderr, "nfsu2: eager init %s\n", ok ? "ok" : "FAILED (headless)");
            /* Overlay AFTER renderer init: SDL reassigns rootViewController's
             * view when the Metal view is created, which would bury the
             * overlay under the render view if installed earlier. */
            struct sdl_syswm_info smi;
            memset(&smi, 0, sizeof smi);
            smi.version_major = 2; smi.version_minor = 30; smi.version_patch = 9;
            /* NFSU2_OVERLAY=0 (nfsu2_env.txt, loaded above): skip the whole
             * UIKit overlay -- drawings, gear/settings panel, haptics, tilt
             * and the keyboard observer (ios_keyboard_install runs from it).
             * Touch input itself still works. A way in if the overlay ever
             * breaks launch. */
            const char *ov = getenv("NFSU2_OVERLAY");
            if (ov && ov[0] == '0') {
                fprintf(stderr, "nfsu2: touch overlay off (NFSU2_OVERLAY=0)\n");
            } else if (SDL_GetWindowWMInfo(win, &smi) && smi.subsystem == SDL_SYSWM_UIKIT) {
                extern void ios_touch_overlay_install(void *uiwindow);
                ios_touch_overlay_install(smi.info.uikit.window);
                fprintf(stderr, "nfsu2: touch overlay installed\n");
            } else {
                fprintf(stderr, "nfsu2: touch overlay unavailable (syswm=%u)\n", smi.subsystem);
            }
        }
    }
    {
        /* After the renderer's eager init, before the game thread starts. */
        const char *hs = getenv("NFSU2_BG_HANDSHAKE");
        s_bg_handshake = !(hs && hs[0] == '0');
        SDL_AddEventWatch(ios_bg_watch, NULL);
        fprintf(stderr, "nfsu2: background handshake %s\n",
                s_bg_handshake ? "on" : "off (flag only)");
    }
    /* Read before the game thread exists: game_main setenv()s more keys. */
    {
        const char *vl = getenv("NFSU2_VBLANK_LOCK");
        vblank_lock_on = !(vl && vl[0] == '0');
        if (!vblank_lock_on)
            fprintf(stderr, "nfsu2: vblank display lock off (NFSU2_VBLANK_LOCK=0)\n");
    }
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u * 1024 * 1024);
    if (pthread_create(&th, &attr, ios_game_thread, &rc) == 0) {
        pthread_t dth;
        pthread_create(&dth, NULL, ios_diag_thread, NULL);
        pthread_detach(dth);
        if (vblank_lock_on) {
            /* v27: display vsync timestamps for the guest vblank (own
             * thread; the kernel timer thread consumes them). The samples
             * wait in a ring until the timer thread runs. */
            extern void ios_vblank_link_start(void);
            ios_vblank_link_start();
        }
        /* SDL_Event is opaque here (main.c avoids SDL.h): 64 bytes is its
         * sizeof on every SDL2 target. */
        char ev[64] __attribute__((aligned(16)));
        for (;;) {
            int got;
            /* Blocks; also keeps the SDL/UIKit state serviced. */
            while (!(got = SDL_PollEvent(ev))) {
                struct timespec ts = { 0, 16 * 1000 * 1000 };
                nanosleep(&ts, NULL);
            }
            if (!got)
                break;
            {
                struct sdl_winev *we = (struct sdl_winev *)ev;
                extern void nv2a_vk_set_background(int bg);
                extern void nv2a_vk_mark_swapchain_dirty(void);
                if (we->type == SDL_QUIT) {
                    fprintf(stderr, "nfsu2: SDL_QUIT\n");
                    break;
                }
                if (we->type == SDL_WINDOWEVENT) {
                    /* SDL2 SDL_WINDOWEVENT_* : 1=SHOWN 2=HIDDEN 3=EXPOSED
                     * 5=RESIZED 6=SIZE_CHANGED 7=MINIMIZED 9=RESTORED
                     * 12=FOCUS_GAINED 13=FOCUS_LOST. Set the background flag
                     * on HIDDEN/MINIMIZED/FOCUS_LOST; clear it ONLY on the
                     * explicit foreground events (RESTORED/FOCUS_GAINED) so
                     * incidental events never un-pause a hidden app. */
                    fprintf(stderr, "nfsu2: windowevent %d\n", we->ev);
                    if (we->ev == 2 || we->ev == 7 || we->ev == 13)
                        nv2a_vk_set_background(1);
                    else if (we->ev == 9 || we->ev == 12)
                        nv2a_vk_set_background(0);
                    if (we->ev == 6) {
                        /* Rotation: the swapchain extent is now stale; the
                         * next flip recreates it before acquiring. */
                        nv2a_vk_mark_swapchain_dirty();
                    }
                }
            }
        }
        pthread_join(th, NULL);
    } else {
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
    }
    return rc;
}
#else
/* POSIX (Linux host proof): the game on a big thread, main joins. */
#include <pthread.h>
static void *linux_game_thread(void *arg)
{
    (void)arg;
    game_main();
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_attr_t attr;
    pthread_t th;
    int rc = 1;

    (void)argc;
    (void)argv;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16u << 20);
    if (pthread_create(&th, &attr, linux_game_thread, NULL) == 0) {
        pthread_join(th, NULL);
        rc = 0;
    } else {
        fprintf(stderr, "[FATAL] cannot start the game thread\n");
    }
    return rc;
}
#endif

static int game_main(void)
{
    xbox_guest_pin(0);     /* the boot thread becomes the title's first thread */
    char xbe_path[512];
    const char *game_dir;
    void *xbe_data;
    size_t xbe_size = 0;
#ifdef NFSU2_IOS
    ios_log_to_documents();
    setvbuf(stderr, NULL, _IONBF, 0);
    fprintf(stderr, "nfsu1: build u1-4 (u1-3 + EA Trax crash fixed, Auto Pad/Drive reads NFSU1, GPU wait sleeps)\n");
    fprintf(stderr, "nfsu2: HOME=%s CFFIXED_USER_HOME=%s TMPDIR=%s\n",
            getenv("HOME") ? getenv("HOME") : "(unset)",
            getenv("CFFIXED_USER_HOME") ? getenv("CFFIXED_USER_HOME") : "(unset)",
            getenv("TMPDIR") ? getenv("TMPDIR") : "(unset)");
#endif

    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    printf("=== Need for Speed: Underground (Xbox) - static recompilation ===\n");
    install_crash_reporter();
    if (getenv("NFSU2_EXIT_TRACE"))
        atexit(print_guest_state);

    /* Runtime defaults this title needs; an explicit setting still wins.
     *   RECOMP_VBLANK      the linked D3D waits on its vblank ISR every frame
     *   RECOMP_AC97_READY  DirectSound needs the codec present to create; "plain"
     *                      is the variant that needs no fault handling */
#ifdef _WIN32
    if (!getenv("RECOMP_VBLANK"))     _putenv("RECOMP_VBLANK=1");
    if (!getenv("RECOMP_AC97_READY")) _putenv("RECOMP_AC97_READY=plain");
    if (!getenv("RECOMP_USB"))        _putenv("RECOMP_USB=1");
#else
    setenv("RECOMP_VBLANK", "1", 0);
    setenv("RECOMP_AC97_READY", "plain", 0);
    setenv("RECOMP_USB", "1", 0);       /* the pad is on the MCPX's OHCI */
    setenv("RECOMP_PB_EXEC", "1", 0);   /* the title draws through NV2A */
    /* NFSU1 first boot: no movie crop; no widescreen EEPROM bit (a known
     * CreateDevice stall path). */
    setenv("NFSU2_MOVIE_CROP", "0", 0);
    setenv("RECOMP_WIDESCREEN", "0", 0);
#endif
#if defined(__SWITCH__)
    /* Every periodic log line flushes to the SD card; keep the log to what
     * matters on a console. RECOMP_QUIET=0 in nfsu2x_env.txt brings it back. */
    setenv("RECOMP_QUIET", "1", 0);
#endif

    game_dir = getenv("NFSU2_GAME_DIR");
    if (!game_dir || !game_dir[0])
        game_dir = NFSU2_DEFAULT_GAME_DIR;
#ifdef NFSU2_IOS
    /* Files pushed from a PC often land one level deeper than expected.
     * Before failing, probe the layouts people actually produce. */
    {
        char probe[1100];
        FILE *f;
        size_t i;
        int found = 0;

        snprintf(probe, sizeof probe, "%s/default.xbe", game_dir);
        f = fopen(probe, "rb");
        if (f) { fclose(f); found = 1; }
        for (i = 0; !found && i < sizeof nfsu2_ios_alts / sizeof nfsu2_ios_alts[0]; i++) {
            snprintf(nfsu2_ios_gamedir, sizeof nfsu2_ios_gamedir,
                     "%s/%s", game_dir, nfsu2_ios_alts[i]);
            snprintf(probe, sizeof probe, "%s/default.xbe", nfsu2_ios_gamedir);
            f = fopen(probe, "rb");
            if (f) {
                fclose(f);
                game_dir = nfsu2_ios_gamedir;
                found = 1;
            }
        }
        if (!found)
            fprintf(stderr, "nfsu2: no default.xbe under %s (probed subdirs)\n",
                    game_dir);
    }
#endif
    snprintf(xbe_path, sizeof(xbe_path), "%s/default.xbe", game_dir);
#ifdef NFSU2_IOS
    fprintf(stderr, "nfsu2: game dir = %s\n", game_dir);
    fprintf(stderr, "nfsu2: loading %s\n", xbe_path);
#endif

    /* A disc image (.iso) instead of the extracted folder. After the iOS
     * layout probe so a folder that holds default.xbe still wins. If the image
     * has no usable default.xbe the folder path below is tried. */
    xbe_data = NULL;
    if (xiso_autodetect(game_dir)) {
        xbe_data = xiso_read_file("default.xbe", &xbe_size);
        if (!xbe_data) {
            fprintf(stderr, "[XISO] no default.xbe in %s, using the game folder\n",
                    xiso_image_path());
            xiso_unmount();
        } else if (!check_disc_identity((const uint8_t *)xbe_data, xbe_size)) {
            fatal("This disc image is not Need for Speed: Underground "
                  "(NTSC-U Xbox). See the log for the title ID found.");
            free(xbe_data);
            xiso_unmount();
            return 1;
        } else {
            snprintf(xbe_path, sizeof(xbe_path), "%s:/default.xbe",
                     xiso_image_path());
        }
    }
    if (!xbe_data)
        xbe_data = load_file(xbe_path, &xbe_size);
    if (!xbe_data) {
        fprintf(stderr, "cannot read %s\n", xbe_path);
        fatal("Failed to load default.xbe. Put the extracted disc or a single .iso in the game "
              "directory (or set NFSU2_GAME_DIR / NFSU2_ISO).");
        return 1;
    }
    printf("XBE %s: %zu bytes\n", xbe_path, xbe_size);

    if (!xbox_MemoryLayoutInit(xbe_data, xbe_size)) {
        fatal("Failed to initialise the Xbox memory layout "
              "(the guest address range may be unavailable).");
        free(xbe_data);
        return 1;
    }
    g_xbox_mem_offset = xbox_GetMemoryOffset();
    printf("Xbox memory mapped at host offset 0x%llX\n",
           (unsigned long long)g_xbox_mem_offset);

    /* NFSU2 raised a sim-step cap in .data here (0x3A4C64); that address is
     * outside NFSU1's image, so NFSU1 has no such patch. */

    /* The emulated APU. DirectSound's accesses reach it through the MMIO
     * accessors the DSOUND section is lifted with (regen.sh passes
     * --mmio-sections DSOUND), so the APU registers stay plain memory and no
     * fault handling is needed -- the same on every host. NFSU2_APU=0 runs
     * without it (DirectSound then creates, but nothing ever plays).
     * Under RECOMP_AC97_READY=1 the runtime traps the APU instead (Windows). */
    if (getenv("RECOMP_AC97_READY")) {
        const char *apu = getenv("NFSU2_APU");
        int plain = strcmp(getenv("RECOMP_AC97_READY"), "plain") == 0;

        if (!plain || !apu || strcmp(apu, "0") != 0) {
            g_apu_state = mcpx_apu_init_standalone((uint8_t *)xbox_GetMemoryBase());
            fprintf(stderr, "[BOOT] emulated APU %s\n",
                    g_apu_state ? "up" : "FAILED to initialise");
            if (g_apu_state && plain)
                xbox_MmioRegister(NFSU2_APU_BASE, NFSU2_APU_END,
                                  apu_mmio_rd, apu_mmio_wr, g_apu_state);
        }
    }

    /* The USB host controllers the title's XPP library drives to find its
     * gamepad (RECOMP_USB). On hosts without fault handling their registers
     * are reached through the MMIO accessors the XPP section is lifted with. */
    {
        extern void xbox_OhciInit(void);
        xbox_OhciInit();
    }

    /* The NIC the title's XNET library drives (src/nic.c). NFSU1's XBE has no
     * XNET section, so this should be harmless but unused. */
    {
        extern void nfsu2_nic_init(void);
        nfsu2_nic_init();
    }

#ifndef _WIN32
    /* The GPU renderer (NFSU2_GL=0 falls back to the executor's CPU one). */
    {
        const char *gl = getenv("NFSU2_GL");
        if (!gl || strcmp(gl, "0") != 0) {
#if defined(NFSU2_VULKAN)
            extern void nv2a_vk_install(void);
            nv2a_vk_install();
#else
            extern void nv2a_gl_install(void);
            nv2a_gl_install();
#endif
        }
    }
#endif

    xbox_kernel_init();
    /* NFSU2's string-table patch (text_patch.c) does not apply to NFSU1. */
#ifdef NFSU2_IOS
    xbox_path_init(game_dir, ios_save_dir());
#else
    xbox_path_init(game_dir, NFSU2_DEFAULT_SAVE_DIR);
#endif
    xbox_kernel_bridge_init();

    g_esp = XBOX_STACK_TOP;

    if (!recomp_dispatch_init())
        fprintf(stderr, "[BOOT] flat dispatch unavailable; "
                        "indirect calls will use the binary search\n");

    xbox_WatchdogStart();

    printf("Starting guest at 0x%08X (esp=0x%08X)\n", NFSU2_ENTRY_POINT, g_esp);
    xbox_gil_mark_main();      /* the frame-rate thread (RECOMP_GIL_EAGER) */
    xbox_gil_enter();          /* guest code from here on (kernel_bridge.c) */
    xbe_entry_point();
    xbox_gil_leave();

    printf("Guest returned. Cleaning up.\n");
    xbox_kernel_shutdown();
    xbox_MemoryLayoutShutdown();
    free(xbe_data);
    return 0;
}
