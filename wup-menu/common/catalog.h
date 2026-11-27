/* catalog: the apps the App Store offers, from repositories in the format of the Homebrew App Store's libget
 * (github.com/fortheusers/libget): <repo>/repo.json lists the packages, each a zip at <repo>/zips/<name>.zip (with its
 * MD5 in repo.json) holding the files to put on the pad, its icon at <repo>/packages/<name>/icon.png. The project's
 * own repository (CATALOG_URL) is always there; the person adds others (their addresses in /.get/repos.txt, one a
 * line). The list is all of them together, each app with the repository it comes from. What is installed is kept as
 * libget keeps it: /.get/packages/<name>/info.json ({"version": ...}) and manifest.install ("U: <path>" per file).
 * Also the downloading and unzipping that Software Update shares (release.c). */
#pragma once
#include <stdint.h>
#include "wup_https.h"

#ifndef CATALOG_URL
#define CATALOG_URL "https://ravensclawx.github.io/wup-homebrew-apps/v2"   /* (version 1 reads the folder above) */
#endif

typedef struct {
    char name[40], title[48], author[40], version[16], category[16], description[160];
    char md5[33];
    uint32_t filesize_kb;
    char installed[16];          /* the version on the pad, "" if none */
    uint8_t repo;                /* which repository (catalog_repo) */
} catalog_pkg;

/* repositories: 0 is the project's own (CATALOG_URL), then the ones added (/.get/repos.txt) */
#define REPO_MAX 8
int catalog_repos(void);                                  /* how many (read from the file the first time) */
const char *catalog_repo(int i);                          /* its address, without a trailing slash */
const char *catalog_repo_name(int i);                     /* its host, for showing ("ravensclawx.github.io") */
int catalog_repo_add(const char *url);                    /* 0, or < 0: not an https:// address, there already, or full */
void catalog_repo_remove(int i);                          /* (not 0) */
extern int catalog_repo_failed;                           /* after catalog_load: a bit per repository that could not be read */

#define CATALOG_MAX 64
extern catalog_pkg catalog[CATALOG_MAX];
extern int catalog_count;

extern void (*catalog_idle)(void);                       /* called often during downloads and installs: keep the screen moving */
int catalog_load(void);                                   /* 0, or < 0 (catalog_why) */
int catalog_system(const catalog_pkg *p);                 /* the menu, App Store, Software Update (Software Update's) */
const char *catalog_icon(const catalog_pkg *p);           /* its icon, fetched once and kept ("A:/.get/icons/..."), or NULL */
const char *catalog_why(int err);                         /* a short sentence for an error */
int catalog_newer(const char *a, const char *b);          /* is version a newer than b? */
/* the package's zip into the file ZIP_FILE, checked (its MD5) as it arrives. progress: 0..100 */
#define ZIP_FILE "/.get/download.zip"
int catalog_fetch(const catalog_pkg *p, void (*progress)(int));
/* each file in ZIP_FILE, in pieces: begin (its path and size), the data (in order, up to 4 KB at a time), end. 0 = go on */
typedef struct {
    int (*begin)(void *arg, const char *path, uint32_t size);
    int (*data)(void *arg, const uint8_t *d, uint32_t n);
    int (*end)(void *arg);
} catalog_sink;
int catalog_unzip(const catalog_sink *s, void *arg);
int catalog_get_url(const char *url, const char *headers, char **out, uint32_t *n);   /* into memory (free *out) */
int catalog_download(const char *url, uint32_t size_kb, void (*progress)(int));      /* into ZIP_FILE */
int catalog_install(const catalog_pkg *p, void (*progress)(int));   /* download, write the files, remember them */
int catalog_remove(const catalog_pkg *p);                            /* its files (manifest.install) and the record */
enum { CATALOG_ENOWIFI = -100, CATALOG_ENOTIME, CATALOG_ENET, CATALOG_ETLS, CATALOG_EFORMAT, CATALOG_EDAMAGED, CATALOG_ESPACE, CATALOG_EWRITE, CATALOG_ENOMEM };
