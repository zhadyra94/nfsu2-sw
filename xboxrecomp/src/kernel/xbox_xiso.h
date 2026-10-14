/*
 * xbox_xiso.h - read-only XDVDFS (Xbox disc image) backend
 *
 * Serves the title's disc (D:\ and \Device\CdRom0\) from an .iso instead of
 * an extracted folder. The kernel file layer asks xiso_active() first and
 * falls back to the host folder when no image is mounted, so folder mode is
 * untouched. The format logic is a C port of tools/xiso/xdvdfs.py.
 *
 * Only the disc is served from here. Saves, caches and partition paths stay
 * on the host filesystem (kernel_path.c).
 */
#ifndef XBOX_XISO_H
#define XBOX_XISO_H

#include <stddef.h>
#include <stdint.h>
#include "kernel.h"

#define XISO_SECTOR 2048u

/* One file or directory of the image. Children of a directory sit in
 * nodes [first_child, first_child + nchild). */
typedef struct {
    char     name[256];
    uint32_t sector;        /* XDVDFS sector, relative to the partition base */
    uint32_t size;          /* bytes (directory: table size)                  */
    uint8_t  attr;          /* XDVDFS attribute byte (0x10 = directory)       */
    int      parent;
    int      first_child;
    int      nchild;
} xiso_node;

#define XISO_ROOT 0

/* Mount the image at `path`. Returns 1 on success, 0 on failure (logged). */
int  xiso_mount(const char *path);
void xiso_unmount(void);

/* Decide whether to use an image, and mount it:
 *   NFSU2_ISO=0            image mode off
 *   NFSU2_ISO=<path>       mount that image (falls back to the folder on error)
 *   otherwise              no <game_dir>/default.xbe and exactly one *.iso in
 *                          game_dir (several: the first in sorted order wins)
 * Returns 1 when an image is mounted. */
int  xiso_autodetect(const char *game_dir);

/* The same decision, without mounting or opening anything and without log
 * output. Returns 1 and the image path in out (NFSU2_ISO=<path> as given,
 * else <dir>/<first *.iso in sorted order>); 0 and an empty out when image
 * mode is off (NFSU2_ISO=0), when <dir>/default.xbe exists, or when there is
 * no *.iso. xiso_autodetect is this plus xiso_mount. */
int  xiso_find_image(const char *dir, char *out, size_t n);

int         xiso_active(void);
const char *xiso_image_path(void);
const xiso_node *xiso_node_at(int idx);
uint64_t    xiso_volume_bytes(void);       /* size of the game partition   */
uint64_t    xiso_volume_filetime(void);    /* descriptor timestamp         */

/* Path (relative to the disc root, '\' or '/' separated, any case) to node.
 * Returns the node index, or -1. *parent_ok is set to 1 when every directory
 * above the last component exists (-> "name not found" rather than "path
 * not found"). */
int  xiso_find(const char *rel, int *parent_ok);

/* Whole file into a malloc'd buffer (default.xbe). NULL on failure. */
void *xiso_read_file(const char *rel, size_t *size);

/* Per-handle state. The handle is the host HANDLE returned by
 * w32_open_handle; no host fd is attached to it. */
int  xiso_attach(HANDLE h, int node);        /* 0 ok, -1 table full          */
void xiso_detach(HANDLE h);                  /* NtClose; ignores other handles */
int  xiso_handle_node(HANDLE h);             /* node index, -1 = not an image handle */

/* Read at byte_offset (>= 0) or at the handle position (< 0). Returns the
 * number of bytes read (0 at end of file), -1 on I/O error. *at gets the
 * file offset the read started at. Thread-safe: no shared file position. */
int64_t xiso_handle_read(HANDLE h, void *buf, size_t len, int64_t byte_offset,
                         int64_t *at);
int  xiso_handle_getpos(HANDLE h, int64_t *pos);
int  xiso_handle_setpos(HANDLE h, int64_t pos);

/* One directory entry per call (pattern: fnmatch, case-insensitive; used at
 * the first call and on restart). Returns 1 and the child node, 0 when the
 * scan is finished (the next call starts over), -1 for a non-directory. */
int  xiso_dir_next(HANDLE h, const char *pattern, int restart, int *node);

void xiso_sha256(const void *data, size_t len, uint8_t out[32]);

#endif
