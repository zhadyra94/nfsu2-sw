/*
 * xbox_xiso.c - read-only XDVDFS (Xbox disc image) backend
 *
 * C port of tools/xiso/xdvdfs.py. See xbox_xiso.h for the contract.
 *
 * Layout recap: 2048-byte sectors; the volume descriptor sits at sector 32 of
 * the game partition ("MICROSOFT*XBOX*MEDIA", root sector at +0x14, root size
 * at +0x18, FILETIME at +0x1C, magic again at +0x7EC). The partition starts at
 * one of a few known image offsets. A directory is a table of entries
 * {left u16, right u16, sector u32, size u32, attr u8, namelen u8, name},
 * each padded to 4 bytes and never split across a sector; a sector's unused
 * tail is 0xFF. Files are contiguous: offset = base + sector * 2048.
 *
 * The tables are walked linearly per sector, not through the left/right tree
 * links: a bad link would otherwise drop a subtree silently (same choice as
 * xdvdfs.py).
 */

#define _GNU_SOURCE   /* strcasecmp, FNM_CASEFOLD */
#include "xbox_xiso.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)

/* The Win32 build keeps the folder mode only: the whole backend compiles out
 * and these stubs keep the callers linking. */
int  xiso_mount(const char *p) { (void)p; return 0; }
void xiso_unmount(void) {}
int  xiso_autodetect(const char *d) { (void)d; return 0; }
int  xiso_active(void) { return 0; }
const char *xiso_image_path(void) { return NULL; }
const xiso_node *xiso_node_at(int i) { (void)i; return NULL; }
uint64_t xiso_volume_bytes(void) { return 0; }
uint64_t xiso_volume_filetime(void) { return 0; }
int  xiso_find(const char *r, int *p) { (void)r; if (p) *p = 0; return -1; }
void *xiso_read_file(const char *r, size_t *s) { (void)r; (void)s; return NULL; }
int  xiso_attach(HANDLE h, int n) { (void)h; (void)n; return -1; }
void xiso_detach(HANDLE h) { (void)h; }
int  xiso_handle_node(HANDLE h) { (void)h; return -1; }
int64_t xiso_handle_read(HANDLE h, void *b, size_t l, int64_t o, int64_t *a)
{ (void)h; (void)b; (void)l; (void)o; (void)a; return -1; }
int  xiso_handle_getpos(HANDLE h, int64_t *p) { (void)h; (void)p; return -1; }
int  xiso_handle_setpos(HANDLE h, int64_t p) { (void)h; (void)p; return -1; }
int  xiso_dir_next(HANDLE h, const char *p, int r, int *n)
{ (void)h; (void)p; (void)r; (void)n; return -1; }

#else /* POSIX */

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <strings.h>
#include <pthread.h>
#include <sys/stat.h>

#define XISO_MAGIC      "MICROSOFT*XBOX*MEDIA"
#define XISO_MAGIC_LEN  20
#define XISO_ATTR_DIR   0x10
#define XISO_MAX_NODES  65536
#define XISO_MAX_DEPTH  24

/* Known game-partition offsets inside an image, as in xdvdfs.py. */
static const uint64_t s_bases[] = {
    0x00000000ull,      /* bare game partition                               */
    0x0000FD90ull,      /* some rebuilt images                               */
    0x00030600ull,      /* redump style, video partition first               */
    0x0FD90000ull,      /* full "xbox disc" layout                           */
    0x18300000ull,      /* redump full-disc dump (magic at 0x18310000)       */
};

static int       s_fd = -1;
static char      s_path[1024];
static uint64_t  s_base;
static uint64_t  s_img_size;
static uint64_t  s_filetime;
static xiso_node *s_nodes;
static int       s_nnodes, s_cap;

/* ---- image reads ------------------------------------------------------ */

#if defined(__SWITCH__)
/* Never hand guest memory to a Horizon IPC buffer (CLAUDE.md, "Save load/create
 * froze the whole console"): read into a .bss buffer and copy. Same scheme as
 * host_read in kernel_file.c. There is no pread on Horizon's devoptab, so the
 * seek and the read are done under the lock together. */
#define XISO_BOUNCE (256u * 1024u)
static char s_bounce[XISO_BOUNCE] __attribute__((aligned(0x1000)));
static pthread_mutex_t s_img_lock = PTHREAD_MUTEX_INITIALIZER;

static int64_t img_pread(void *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    pthread_mutex_lock(&s_img_lock);
    if (lseek(s_fd, (off_t)off, SEEK_SET) < 0) {
        pthread_mutex_unlock(&s_img_lock);
        return -1;
    }
    while (done < len) {
        size_t want = len - done < XISO_BOUNCE ? len - done : XISO_BOUNCE;
        ssize_t n = read(s_fd, s_bounce, want);
        if (n < 0) {
            if (!done) {
                pthread_mutex_unlock(&s_img_lock);
                return -1;
            }
            break;
        }
        memcpy((char *)buf + done, s_bounce, (size_t)n);
        done += (size_t)n;
        if ((size_t)n < want)
            break;                          /* end of image */
    }
    pthread_mutex_unlock(&s_img_lock);
    return (int64_t)done;
}
#else
/* pread: no shared file position, so guest threads can stream different files
 * at once without a lock. */
static int64_t img_pread(void *buf, size_t len, uint64_t off)
{
    size_t done = 0;

    while (done < len) {
        ssize_t n = pread(s_fd, (char *)buf + done, len - done,
                          (off_t)(off + done));
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (!done)
                return -1;
            break;
        }
        if (n == 0)
            break;
        done += (size_t)n;
    }
    return (int64_t)done;
}
#endif

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* ---- index ------------------------------------------------------------ */

static int node_new(void)
{
    if (s_nnodes >= XISO_MAX_NODES)
        return -1;
    if (s_nnodes == s_cap) {
        int ncap = s_cap ? s_cap * 2 : 256;
        xiso_node *n = realloc(s_nodes, (size_t)ncap * sizeof *n);
        if (!n)
            return -1;
        s_nodes = n;
        s_cap = ncap;
    }
    memset(&s_nodes[s_nnodes], 0, sizeof s_nodes[0]);
    return s_nnodes++;
}

/* Read the table of directory node `dir` and recurse into its subdirectories.
 * The children are appended as one contiguous run before any recursion, so a
 * directory's entries stay together in the array. */
static int index_dir(int dir, int depth)
{
    uint32_t sector = s_nodes[dir].sector, size = s_nodes[dir].size;
    uint8_t *tab;
    int first, count = 0, i;

    if (depth > XISO_MAX_DEPTH || size == 0 || size > (64u << 20))
        return size == 0 ? 0 : -1;
    tab = malloc(size);
    if (!tab)
        return -1;
    if (img_pread(tab, size, s_base + (uint64_t)sector * XISO_SECTOR)
            != (int64_t)size) {
        free(tab);
        fprintf(stderr, "[XISO] directory at sector %u is truncated\n", sector);
        return -1;
    }

    first = s_nnodes;
    for (uint32_t sec = 0; sec < size; sec += XISO_SECTOR) {
        uint32_t lim = size - sec < XISO_SECTOR ? size - sec : XISO_SECTOR;
        const uint8_t *c = tab + sec;
        uint32_t off = 0;

        while (off + 0x0E <= lim) {
            unsigned l = c[off] | (c[off + 1] << 8);
            unsigned r = c[off + 2] | (c[off + 3] << 8);
            unsigned nlen = c[off + 13];
            int n;

            /* 0xFFFF/0xFFFF padding or an empty name ends this sector */
            if ((l == 0xFFFF && r == 0xFFFF) || nlen == 0 ||
                off + 0x0E + nlen > lim)
                break;
            n = node_new();
            if (n < 0) {
                free(tab);
                return -1;
            }
            memcpy(s_nodes[n].name, c + off + 0x0E, nlen);
            s_nodes[n].sector = rd32(c + off + 4);
            s_nodes[n].size   = rd32(c + off + 8);
            s_nodes[n].attr   = c[off + 12];
            s_nodes[n].parent = dir;
            count++;
            off += (0x0E + nlen + 3) & ~3u;
        }
    }
    free(tab);
    s_nodes[dir].first_child = first;
    s_nodes[dir].nchild = count;
    for (i = 0; i < count; i++)
        if ((s_nodes[first + i].attr & XISO_ATTR_DIR) &&
            index_dir(first + i, depth + 1) < 0)
            return -1;
    return 0;
}

static int count_files(void)
{
    int n = 0;
    for (int i = 1; i < s_nnodes; i++)
        if (!(s_nodes[i].attr & XISO_ATTR_DIR))
            n++;
    return n;
}

void xiso_unmount(void)
{
    if (s_fd >= 0)
        close(s_fd);
    s_fd = -1;
    free(s_nodes);
    s_nodes = NULL;
    s_nnodes = s_cap = 0;
    s_path[0] = '\0';
}

int xiso_mount(const char *path)
{
    uint8_t desc[XISO_SECTOR];
    struct stat st;
    int b, found = 0;

    if (s_fd >= 0)
        xiso_unmount();
    s_fd = open(path, O_RDONLY);
    if (s_fd < 0) {
        fprintf(stderr, "[XISO] cannot open %s: %s\n", path, strerror(errno));
        return 0;
    }
    s_img_size = (fstat(s_fd, &st) == 0) ? (uint64_t)st.st_size : 0;
    snprintf(s_path, sizeof s_path, "%s", path);

    for (b = 0; b < (int)(sizeof s_bases / sizeof s_bases[0]); b++) {
        s_base = s_bases[b];
        if (img_pread(desc, sizeof desc, s_base + 32ull * XISO_SECTOR)
                == (int64_t)sizeof desc &&
            memcmp(desc, XISO_MAGIC, XISO_MAGIC_LEN) == 0) {
            found = 1;
            break;
        }
    }
    if (!found) {
        fprintf(stderr, "[XISO] %s is not an XDVDFS image (no %s at sector 32 "
                "of any known partition base)\n", path, XISO_MAGIC);
        xiso_unmount();
        return 0;
    }
    if (memcmp(desc + 0x7EC, XISO_MAGIC, XISO_MAGIC_LEN) != 0)
        fprintf(stderr, "[XISO] warning: no closing magic in the volume "
                "descriptor of %s\n", path);

    s_filetime = (uint64_t)rd32(desc + 0x1C) | ((uint64_t)rd32(desc + 0x20) << 32);
    {
        int root = node_new();
        if (root != XISO_ROOT) {
            xiso_unmount();
            return 0;
        }
        snprintf(s_nodes[root].name, sizeof s_nodes[root].name, "%s", "");
        s_nodes[root].sector = rd32(desc + 0x14);
        s_nodes[root].size   = rd32(desc + 0x18);
        s_nodes[root].attr   = XISO_ATTR_DIR;
        s_nodes[root].parent = -1;
    }
    if (index_dir(XISO_ROOT, 0) < 0 || s_nodes[XISO_ROOT].nchild == 0) {
        fprintf(stderr, "[XISO] %s: cannot read the directory tree\n", path);
        xiso_unmount();
        return 0;
    }
    fprintf(stderr, "[XISO] mounted %s: base 0x%llX, %d files\n", path,
            (unsigned long long)s_base, count_files());
    return 1;
}

int xiso_active(void) { return s_fd >= 0; }
const char *xiso_image_path(void) { return s_path; }

const xiso_node *xiso_node_at(int idx)
{
    return (idx >= 0 && idx < s_nnodes) ? &s_nodes[idx] : NULL;
}

uint64_t xiso_volume_bytes(void)
{
    return s_img_size > s_base ? s_img_size - s_base : 0;
}

uint64_t xiso_volume_filetime(void) { return s_filetime; }

/* ---- lookup ----------------------------------------------------------- */

static int child_find(int dir, const char *name, size_t len)
{
    const xiso_node *d = &s_nodes[dir];

    for (int i = 0; i < d->nchild; i++) {
        const char *n = s_nodes[d->first_child + i].name;
        /* XDVDFS ignores case; titles mix it freely (see fix_case in
         * kernel_path.c for the folder equivalent). */
        if (strlen(n) == len && strncasecmp(n, name, len) == 0)
            return d->first_child + i;
    }
    return -1;
}

int xiso_find(const char *rel, int *parent_ok)
{
    int cur = XISO_ROOT;
    const char *p = rel ? rel : "";

    if (parent_ok)
        *parent_ok = 1;
    if (s_fd < 0) {
        if (parent_ok)
            *parent_ok = 0;
        return -1;
    }
    for (;;) {
        const char *e;
        size_t len;
        int next;

        while (*p == '\\' || *p == '/')
            p++;
        if (!*p)
            return cur;
        e = p;
        while (*e && *e != '\\' && *e != '/')
            e++;
        len = (size_t)(e - p);
        if (len == 1 && p[0] == '.') {          /* "." stays put */
            p = e;
            continue;
        }
        if (!(s_nodes[cur].attr & XISO_ATTR_DIR)) {
            if (parent_ok)
                *parent_ok = 0;                 /* a file used as a directory */
            return -1;
        }
        next = child_find(cur, p, len);
        if (next < 0) {
            /* a missing last component is "name not found"; a missing middle
             * one is "path not found" */
            const char *t = e;
            while (*t == '\\' || *t == '/')
                t++;
            if (*t && parent_ok)
                *parent_ok = 0;
            return -1;
        }
        cur = next;
        p = e;
    }
}

void *xiso_read_file(const char *rel, size_t *size)
{
    int idx = xiso_find(rel, NULL);
    uint8_t *buf;

    if (idx < 0 || (s_nodes[idx].attr & XISO_ATTR_DIR) || !s_nodes[idx].size)
        return NULL;
    buf = malloc(s_nodes[idx].size);
    if (!buf)
        return NULL;
    if (img_pread(buf, s_nodes[idx].size,
                  s_base + (uint64_t)s_nodes[idx].sector * XISO_SECTOR)
            != (int64_t)s_nodes[idx].size) {
        free(buf);
        return NULL;
    }
    if (size)
        *size = s_nodes[idx].size;
    return buf;
}

/* ---- handles ---------------------------------------------------------- */

/* A guest rarely holds more than a few dozen disc handles at once. */
#define XISO_MAX_HANDLES 1024

typedef struct {
    HANDLE  h;
    int     node;
    int64_t pos;
    int     scan_idx;           /* directories: next child to look at   */
    int     scanning;
    char    pattern[64];
} xiso_handle;

static xiso_handle     s_handles[XISO_MAX_HANDLES];
static pthread_mutex_t s_hlock = PTHREAD_MUTEX_INITIALIZER;

static xiso_handle *hfind(HANDLE h)
{
    if (!h)
        return NULL;
    for (int i = 0; i < XISO_MAX_HANDLES; i++)
        if (s_handles[i].h == h)
            return &s_handles[i];
    return NULL;
}

int xiso_attach(HANDLE h, int node)
{
    int rc = -1;

    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);          /* a reused HANDLE value replaces */
        for (int i = 0; !e && i < XISO_MAX_HANDLES; i++)
            if (!s_handles[i].h)
                e = &s_handles[i];
        if (e) {
            memset(e, 0, sizeof *e);
            e->h = h;
            e->node = node;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&s_hlock);
    if (rc)
        fprintf(stderr, "[XISO] handle table full\n");
    return rc;
}

void xiso_detach(HANDLE h)
{
    if (s_fd < 0)               /* folder mode: every NtClose lands here */
        return;
    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e)
            memset(e, 0, sizeof *e);
    }
    pthread_mutex_unlock(&s_hlock);
}

int xiso_handle_node(HANDLE h)
{
    int n = -1;

    if (s_fd < 0)
        return -1;
    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e)
            n = e->node;
    }
    pthread_mutex_unlock(&s_hlock);
    return n;
}

int64_t xiso_handle_read(HANDLE h, void *buf, size_t len, int64_t byte_offset,
                         int64_t *at)
{
    int node = -1;
    int64_t pos = 0, n;
    uint64_t fsize, start;

    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e) {
            node = e->node;
            pos = byte_offset >= 0 ? byte_offset : e->pos;
        }
    }
    pthread_mutex_unlock(&s_hlock);
    if (node < 0 || (s_nodes[node].attr & XISO_ATTR_DIR))
        return -1;
    if (at)
        *at = pos;

    fsize = s_nodes[node].size;
    if ((uint64_t)pos >= fsize)
        n = 0;
    else {
        if (len > fsize - (uint64_t)pos)
            len = (size_t)(fsize - (uint64_t)pos);
        start = s_base + (uint64_t)s_nodes[node].sector * XISO_SECTOR +
                (uint64_t)pos;
        n = len ? img_pread(buf, len, start) : 0;
        if (n < 0)
            return -1;
    }
    /* Like read()/pread(): the handle position follows a sequential read;
     * an explicit-offset read leaves it where the Win32/NT layer would put
     * it, at the end of the data just read. */
    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e)
            e->pos = pos + n;
    }
    pthread_mutex_unlock(&s_hlock);
    return n;
}

int xiso_handle_getpos(HANDLE h, int64_t *pos)
{
    int rc = -1;

    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e) {
            *pos = e->pos;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&s_hlock);
    return rc;
}

int xiso_handle_setpos(HANDLE h, int64_t pos)
{
    int rc = -1;

    if (pos < 0)
        return -1;
    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e) {
            e->pos = pos;
            rc = 0;
        }
    }
    pthread_mutex_unlock(&s_hlock);
    return rc;
}

int xiso_dir_next(HANDLE h, const char *pattern, int restart, int *node)
{
    int rc = -1;

    pthread_mutex_lock(&s_hlock);
    {
        xiso_handle *e = hfind(h);
        if (e && (s_nodes[e->node].attr & XISO_ATTR_DIR)) {
            const xiso_node *d = &s_nodes[e->node];

            if (restart || !e->scanning) {
                e->scanning = 1;
                e->scan_idx = 0;
                snprintf(e->pattern, sizeof e->pattern, "%s",
                         pattern && pattern[0] ? pattern : "*");
            }
            rc = 0;
            while (e->scan_idx < d->nchild) {
                int c = d->first_child + e->scan_idx++;
                if (fnmatch(e->pattern, s_nodes[c].name, FNM_CASEFOLD) == 0) {
                    *node = c;
                    rc = 1;
                    break;
                }
            }
            if (rc == 0)
                e->scanning = 0;        /* finished: the next call starts over */
        }
    }
    pthread_mutex_unlock(&s_hlock);
    return rc;
}

/* ---- mount policy ----------------------------------------------------- */

static int has_iso_suffix(const char *n)
{
    size_t l = strlen(n);

    return n[0] != '.' && l > 4 && strcasecmp(n + l - 4, ".iso") == 0;
}

static int name_cmp(const void *a, const void *b)
{
    int c = strcasecmp(*(char *const *)a, *(char *const *)b);
    return c ? c : strcmp(*(char *const *)a, *(char *const *)b);
}

int xiso_autodetect(const char *game_dir)
{
    const char *env = getenv("NFSU2_ISO");
    char probe[1100];
    DIR *d;
    struct dirent *de;
    char **names = NULL;
    int n = 0, cap = 0, mounted = 0;

    if (env && env[0]) {
        if (strcmp(env, "0") == 0) {
            fprintf(stderr, "[XISO] disc image mode disabled (NFSU2_ISO=0)\n");
            return 0;
        }
        if (xiso_mount(env))
            return 1;
        fprintf(stderr, "[XISO] NFSU2_ISO=%s unusable, using the game folder\n",
                env);
        return 0;
    }
    if (!game_dir || !game_dir[0])
        return 0;
    snprintf(probe, sizeof probe, "%s/default.xbe", game_dir);
    if (access(probe, R_OK) == 0)
        return 0;                               /* extracted disc wins */

    d = opendir(game_dir);
    if (!d)
        return 0;
    while ((de = readdir(d)) != NULL) {
        struct stat st;

        if (!has_iso_suffix(de->d_name))
            continue;
        snprintf(probe, sizeof probe, "%s/%s", game_dir, de->d_name);
        if (stat(probe, &st) != 0 || !S_ISREG(st.st_mode))
            continue;
        if (n == cap) {
            char **t = realloc(names, (size_t)(cap = cap ? cap * 2 : 8) * sizeof *t);
            if (!t)
                break;
            names = t;
        }
        names[n] = strdup(de->d_name);
        if (names[n])
            n++;
    }
    closedir(d);

    if (n > 0) {
        qsort(names, (size_t)n, sizeof *names, name_cmp);
        snprintf(probe, sizeof probe, "%s/%s", game_dir, names[0]);
        for (int i = 1; i < n; i++)
            fprintf(stderr, "[XISO] ignoring extra image %s/%s\n", game_dir,
                    names[i]);
        mounted = xiso_mount(probe);
        if (!mounted)
            fprintf(stderr, "[XISO] %s unusable, using the game folder\n", probe);
    }
    for (int i = 0; i < n; i++)
        free(names[i]);
    free(names);
    return mounted;
}

#endif /* _WIN32 */

/* ---- SHA-256 (identity check of default.xbe) -------------------------- */

static const uint32_t K256[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1,
    0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786,
    0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
    0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a,
    0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

#define ROR32(x, n) (((x) >> (n)) | ((x) << (32 - (n))))

static void sha256_block(uint32_t h[8], const uint8_t *p)
{
    uint32_t w[64], a, b, c, d, e, f, g, hh;

    for (int i = 0; i < 16; i++)
        w[i] = ((uint32_t)p[4 * i] << 24) | ((uint32_t)p[4 * i + 1] << 16) |
               ((uint32_t)p[4 * i + 2] << 8) | p[4 * i + 3];
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = ROR32(w[i - 15], 7) ^ ROR32(w[i - 15], 18) ^ (w[i - 15] >> 3);
        uint32_t s1 = ROR32(w[i - 2], 17) ^ ROR32(w[i - 2], 19) ^ (w[i - 2] >> 10);
        w[i] = w[i - 16] + s0 + w[i - 7] + s1;
    }
    a = h[0]; b = h[1]; c = h[2]; d = h[3];
    e = h[4]; f = h[5]; g = h[6]; hh = h[7];
    for (int i = 0; i < 64; i++) {
        uint32_t S1 = ROR32(e, 6) ^ ROR32(e, 11) ^ ROR32(e, 25);
        uint32_t ch = (e & f) ^ (~e & g);
        uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
        uint32_t S0 = ROR32(a, 2) ^ ROR32(a, 13) ^ ROR32(a, 22);
        uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t t2 = S0 + mj;
        hh = g; g = f; f = e; e = d + t1;
        d = c; c = b; b = a; a = t1 + t2;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
}

void xiso_sha256(const void *data, size_t len, uint8_t out[32])
{
    uint32_t h[8] = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 };
    const uint8_t *p = data;
    uint8_t tail[128];
    size_t full = len / 64, rem = len % 64, tl;
    uint64_t bits = (uint64_t)len * 8;

    for (size_t i = 0; i < full; i++)
        sha256_block(h, p + 64 * i);
    memset(tail, 0, sizeof tail);
    memcpy(tail, p + 64 * full, rem);
    tail[rem] = 0x80;
    tl = rem < 56 ? 64 : 128;
    for (int i = 0; i < 8; i++)
        tail[tl - 1 - i] = (uint8_t)(bits >> (8 * i));
    sha256_block(h, tail);
    if (tl == 128)
        sha256_block(h, tail + 64);
    for (int i = 0; i < 8; i++) {
        out[4 * i]     = (uint8_t)(h[i] >> 24);
        out[4 * i + 1] = (uint8_t)(h[i] >> 16);
        out[4 * i + 2] = (uint8_t)(h[i] >> 8);
        out[4 * i + 3] = (uint8_t)h[i];
    }
}
