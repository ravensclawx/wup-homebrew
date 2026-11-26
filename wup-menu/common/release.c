/* release: see release.h. The JSON is read with jsmn; downloading and unzipping are the catalogue's (catalog.c). */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "wup.h"
#include "fs.h"
#include "wup_system.h"
#include "catalog.h"
#include "release.h"
#define JSMN_STATIC
#include "jsmn/jsmn.h"

static int is(const char *js, const jsmntok_t *t, const char *s) {
    return t->type == JSMN_STRING && (int)strlen(s) == t->end - t->start && !strncmp(js + t->start, s, (size_t)(t->end - t->start));
}
static void copy(char *out, size_t n, const char *js, const jsmntok_t *t) {
    size_t k = (size_t)(t->end - t->start); if (k >= n) k = n - 1;
    memcpy(out, js + t->start, k); out[k] = 0;
}
static int skip(const jsmntok_t *t, int i) {          /* the token after i and everything inside it */
    int n = 1;
    for (int k = 0; k < n; k++) { if (t[i + k].type == JSMN_OBJECT) n += 2 * t[i + k].size; else if (t[i + k].type == JSMN_ARRAY) n += t[i + k].size; }
    return i + n;
}

int release_check(release_t *rel) {
    memset(rel, 0, sizeof *rel);
    char *js; uint32_t len;
    int r = catalog_get_url(RELEASES_URL, "Accept: application/vnd.github+json\r\n", &js, &len);
    if (r) return r;
    jsmn_parser jp; jsmn_init(&jp);
    int n = jsmn_parse(&jp, js, len, NULL, 0);
    jsmntok_t *t = n > 0 ? malloc((size_t)n * sizeof *t) : 0;
    if (!t) { free(js); return n > 0 ? CATALOG_ENOMEM : CATALOG_EFORMAT; }
    jsmn_init(&jp); n = jsmn_parse(&jp, js, len, t, (unsigned)n);
    if (n > 0 && t[0].type == JSMN_OBJECT) {
        for (int i = 1; i < n; i = skip(t, i + 1)) {
            if (is(js, &t[i], "tag_name")) {
                char tag[32]; copy(tag, sizeof tag, js, &t[i + 1]);
                snprintf(rel->version, sizeof rel->version, "%s", tag[0] == 'v' || tag[0] == 'V' ? tag + 1 : tag);
            } else if (is(js, &t[i], "assets") && t[i + 1].type == JSMN_ARRAY) {
                int a = i + 2;
                for (int e = 0; e < t[i + 1].size; e++) {
                    int end = skip(t, a), mine = 0; char url[sizeof rel->url] = ""; uint32_t size = 0;
                    for (int j = a + 1; j < end; j = skip(t, j + 1)) {
                        if (is(js, &t[j], "name")) mine = is(js, &t[j + 1], RELEASE_ASSET);
                        else if (is(js, &t[j], "browser_download_url")) copy(url, sizeof url, js, &t[j + 1]);
                        else if (is(js, &t[j], "size")) size = (uint32_t)atol(js + t[j + 1].start);
                    }
                    if (mine && url[0]) { snprintf(rel->url, sizeof rel->url, "%s", url); rel->size_kb = (size + 1023) / 1024; }
                    a = end;
                }
            }
        }
    }
    free(t); free(js);
    return rel->version[0] ? 0 : CATALOG_EFORMAT;          /* (a release without our asset: nothing to install) */
}

/* the zip's two files into their slots: each one's first 4 KB is its header sector, the rest its image (written as it
   comes); the header is written last, once the image is whole and its CRC agrees (wup_system.h) */
typedef struct { int which, first, done; } slot_t;
static int s_begin(void *arg, const char *path, uint32_t size) {
    slot_t *x = arg;
    x->which = !strcmp(path, "kernel.bin") ? 1 : !strcmp(path, "homebrew_menu.bin") ? 2 : 0;
    x->first = 1;
    return x->which && size < 4096 + 64 ? CATALOG_EDAMAGED : 0;
}
static int s_data(void *arg, const uint8_t *d, uint32_t n) {
    slot_t *x = arg;
    if (!x->which) return 0;
    if (x->first) {                                   /* (pieces are 4 KB: the first is the header sector) */
        x->first = 0;
        if (n != 4096) return CATALOG_EDAMAGED;
        return (x->which == 1 ? wup_kernel_slot_begin(d) : wup_menu_slot_begin(d)) ? CATALOG_EDAMAGED : 0;
    }
    return wup_menu_slot_write(d, n) ? CATALOG_EWRITE : 0;
}
static int s_end(void *arg) {
    slot_t *x = arg;
    if (!x->which) return 0;
    if (wup_menu_slot_end()) return CATALOG_EWRITE;
    x->done |= x->which;
    return 0;
}
int release_install(const release_t *rel, void (*progress)(int)) {
    int r = catalog_download(rel->url, rel->size_kb, progress);
    if (r) return r;
    static const catalog_sink sink = { s_begin, s_data, s_end };
    slot_t x = { 0 };
    r = catalog_unzip(&sink, &x);
    if (!r && x.done != 3) r = CATALOG_EDAMAGED;      /* (both, or the release is not one of ours) */
    fs_remove(ZIP_FILE);
    return r;
}
