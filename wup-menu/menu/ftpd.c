/* ftpd: a small FTP server (RFC 959, passive mode only) for the pad's littlefs.
 *
 * Commands: USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV LIST NLST RETR STOR DELE MKD RMD RNFR RNTO
 * SIZE MDTM NOOP ABOR QUIT (and the X* aliases). No active mode (PORT): every client today uses passive.
 * Login: anyone (any user name, any password), or user "pad" with the password; only MD5("pad:GamePad:<password>")
 * is kept (the same hash the menu has always stored). No clock on the pad: every file has the same date.
 * /config (the menu's settings) is hidden and refused.
 *
 * Structure: the protocol core works on endpoints through a few transport functions (io_*); the lwIP glue at the
 * end provides them on the pad, tools/ftptest/ftphost.c on the Mac (the same core, tested with real FTP clients).
 * Received data is queued in lwIP's callbacks; all work happens in ftpd_step(), outside lwIP. */
#include <stdint.h>
#include <string.h>
#include "fs.h"
#include "ftpd.h"
typedef uint8_t u8; typedef uint32_t u32;

/* ---- MD5 (RFC 1321): the stored password hash */
typedef struct { u32 s[4], n; u8 b[64]; } md5_t;
static const u32 md5_k[64] = {
    0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501, 0x698098d8, 0x8b44f7af,
    0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821, 0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa,
    0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8, 0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8,
    0x676f02d9, 0x8d2a4c8a, 0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
    0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665, 0xf4292244, 0x432aff97,
    0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1, 0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1,
    0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
static const u8 md5_r[16] = { 7, 12, 17, 22, 5, 9, 14, 20, 4, 11, 16, 23, 6, 10, 15, 21 };
static void md5_block(u32 s[4], const u8 *p) {
    u32 m[16], a = s[0], b = s[1], c = s[2], d = s[3];
    for (int i = 0; i < 16; i++) m[i] = p[4 * i] | p[4 * i + 1] << 8 | p[4 * i + 2] << 16 | (u32)p[4 * i + 3] << 24;
    for (int i = 0; i < 64; i++) {
        u32 f; int g, r = md5_r[(i >> 4) * 4 + (i & 3)];
        if (i < 16) { f = (b & c) | (~b & d); g = i; }
        else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) & 15; }
        else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) & 15; }
        else { f = c ^ (b | ~d); g = (7 * i) & 15; }
        u32 t = d; d = c; c = b; f += a + md5_k[i] + m[g]; b += f << r | f >> (32 - r); a = t;
    }
    s[0] += a; s[1] += b; s[2] += c; s[3] += d;
}
static void md5(const char *a, const char *b, u8 out[16]) {      /* MD5(a b) */
    u32 s[4] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476 }, n = 0; u8 blk[64];
    const char *parts[2] = { a, b };
    for (int k = 0; k < 2; k++) for (const char *p = parts[k]; *p; p++) { blk[n++ & 63] = (u8)*p; if (!(n & 63)) md5_block(s, blk); }
    u32 bits = n * 8; blk[n++ & 63] = 0x80; if (!(n & 63)) md5_block(s, blk);
    while ((n & 63) != 56) { blk[n++ & 63] = 0; if (!(n & 63)) md5_block(s, blk); }
    for (int i = 0; i < 8; i++) { blk[n++ & 63] = i < 4 ? (u8)(bits >> (8 * i)) : 0; if (!(n & 63)) md5_block(s, blk); }
    for (int i = 0; i < 16; i++) out[i] = (u8)(s[i / 4] >> (8 * (i & 3)));
}
void ftpd_hash(const char *password, u8 hash[16]) { md5(FTP_USER ":GamePad:", password, hash); }
static int auth_on; static u8 auth_hash[16];
void ftpd_set_auth(int on, const u8 hash[16]) { auth_on = on; memcpy(auth_hash, hash, 16); }

/* ---- transport, provided by the glue */
typedef struct ep ep_t;
static ep_t *io_listen(int port);                 /* a listening endpoint, or 0 */
static ep_t *io_accept(ep_t *l);                  /* a new connection on it, or 0 */
static int io_recv(ep_t *e, void *buf, int max);  /* bytes; 0: nothing yet; < 0: closed by the peer (or broken) */
static int io_room(ep_t *e);                      /* bytes io_send takes now */
static int io_send(ep_t *e, const void *d, int n);
static void io_flush(ep_t *e);
static void io_close(ep_t *e);                    /* (sends what is queued first) */
static u32 io_local_ip(ep_t *e);                  /* as on the wire: the first number in the lowest byte */
static u32 io_ms(void);

/* ---- sessions */
#define NSESS 3
#define PATHN 192
enum { OP_NONE, OP_LIST, OP_NLST, OP_RETR, OP_STOR };
typedef struct {
    ep_t *ctl, *pasv, *data;
    int used, user_pad, logged, quit;
    char cwd[PATHN], rnfr[PATHN];
    char line[512]; int line_n;
    int op; u32 op_t, last;
    lfs_file_t f; u8 fbuf[256]; struct lfs_file_config fcfg; int fopen;
    lfs_dir_t dir; int dopen; char list_one[64]; long list_size; int list_dir;   /* LIST of one file / its folder */
    u8 buf[1460]; int buf_n, buf_off;
} sess_t;
static sess_t ss[NSESS];
static ep_t *listener;
static u32 apps_dirty;
int ftpd_apps_changed(void) { int r = (int)apps_dirty; apps_dirty = 0; return r; }
int ftpd_busy(void);

static void reply(sess_t *s, const char *a) {
    if (!s->ctl) return;
    io_send(s->ctl, a, (int)strlen(a)); io_send(s->ctl, "\r\n", 2); io_flush(s->ctl);
}
static char *cat(char *o, const char *s) { while (*s) *o++ = *s++; *o = 0; return o; }
static char *catn(char *o, unsigned long v) { char t[12]; int n = 0; do t[n++] = (char)('0' + v % 10); while (v /= 10); while (n) *o++ = t[--n]; *o = 0; return o; }

/* "/a/b" from the current folder and an argument (absolute or relative, with . and ..); 0 = too long */
static int resolve(const char *cwd, const char *arg, char *out) {
    char t[PATHN * 2]; int n = 0;
    if (arg[0] != '/') { for (const char *p = cwd; *p && n < PATHN; p++) t[n++] = *p; if (n < PATHN) t[n++] = '/'; }
    for (const char *p = arg; *p && n < (int)sizeof t - 1; p++) t[n++] = *p;
    t[n] = 0;
    int o = 0; out[0] = 0;
    for (char *p = t; *p; ) {
        while (*p == '/') p++;
        char *q = p; while (*q && *q != '/') q++;
        int k = (int)(q - p);
        if (k == 1 && p[0] == '.') {}
        else if (k == 2 && p[0] == '.' && p[1] == '.') { while (o > 0 && out[--o] != '/') {} out[o] = 0; }
        else if (k) { if (o + 1 + k >= PATHN) return 0; out[o++] = '/'; memcpy(out + o, p, (size_t)k); o += k; out[o] = 0; }
        p = q;
    }
    if (!o) { out[0] = '/'; out[1] = 0; }
    return 1;
}
static int hidden(const char *p) { return !strncmp(p, "/config", 7) && (!p[7] || p[7] == '/'); }
static int stat_(const char *p, struct lfs_info *i) {
    if (hidden(p)) return -1;
    if (!strcmp(p, "/")) { i->type = LFS_TYPE_DIR; i->size = 0; return 0; }
    return lfs_stat(&fs_lfs, p, i);
}
static void note_change(const char *p) { if (!strncmp(p, "/apps", 5)) apps_dirty = 1; }

static void end_op(sess_t *s) {
    if (s->fopen) { lfs_file_close(&fs_lfs, &s->f); s->fopen = 0; }
    if (s->dopen) { lfs_dir_close(&fs_lfs, &s->dir); s->dopen = 0; }
    if (s->data) { io_close(s->data); s->data = 0; }
    s->op = OP_NONE; s->buf_n = s->buf_off = 0;
}
static void sess_end(sess_t *s) {
    end_op(s);
    if (s->pasv) { io_close(s->pasv); s->pasv = 0; }
    if (s->ctl) { io_close(s->ctl); s->ctl = 0; }
    s->used = 0;
}
static void start_op(sess_t *s, int op) {
    if (!s->pasv && !s->data) { end_op(s); reply(s, "425 Use PASV or EPSV first."); return; }
    s->op = op; s->op_t = io_ms(); s->buf_n = s->buf_off = 0;
    reply(s, "150 Opening data connection.");
}
static void list_line(sess_t *s, const char *name, int dir, unsigned long size) {
    char *o = (char *)s->buf;
    if (s->op == OP_NLST) o = cat(o, name);
    else { o = cat(o, dir ? "drwxr-xr-x 1 pad pad " : "-rw-r--r-- 1 pad pad "); o = catn(o, size); o = cat(o, " Jan  1  2026 "); o = cat(o, name); }
    o = cat(o, "\r\n");
    s->buf_n = (int)(o - (char *)s->buf); s->buf_off = 0;
}

static void command(sess_t *s, char *cmd, char *arg) {
    for (char *p = cmd; *p; p++) if (*p >= 'a' && *p <= 'z') *p = (char)(*p - 32);
    if (!strcmp(cmd, "XPWD")) cmd = "PWD"; else if (!strcmp(cmd, "XCWD")) cmd = "CWD"; else if (!strcmp(cmd, "XCUP")) cmd = "CDUP";
    else if (!strcmp(cmd, "XMKD")) cmd = "MKD"; else if (!strcmp(cmd, "XRMD")) cmd = "RMD";
    char P[PATHN], line[PATHN + 64]; struct lfs_info info;
    if (!strcmp(cmd, "USER")) {
        s->logged = 0; s->user_pad = !strcmp(arg, FTP_USER);
        if (!auth_on) { s->logged = 1; reply(s, "230 Signed in."); } else reply(s, "331 Password required.");
        return;
    }
    if (!strcmp(cmd, "PASS")) {
        u8 h[16]; ftpd_hash(arg, h);
        if (!auth_on || (s->user_pad && !memcmp(h, auth_hash, 16))) { s->logged = 1; reply(s, "230 Signed in."); }
        else reply(s, "530 Incorrect user name or password.");
        return;
    }
    if (!strcmp(cmd, "QUIT")) { reply(s, "221 Goodbye."); s->quit = 1; return; }
    if (!strcmp(cmd, "SYST")) { reply(s, "215 UNIX Type: L8"); return; }
    if (!strcmp(cmd, "FEAT")) { reply(s, "211-Features:\r\n UTF8\r\n SIZE\r\n MDTM\r\n EPSV\r\n PASV\r\n211 End"); return; }
    if (!strcmp(cmd, "OPTS") || !strcmp(cmd, "NOOP") || !strcmp(cmd, "MODE") || !strcmp(cmd, "STRU")) { reply(s, "200 OK."); return; }
    if (!s->logged) { reply(s, "530 Sign in first."); return; }
    if (!strcmp(cmd, "TYPE")) { reply(s, "200 OK."); return; }
    if (!strcmp(cmd, "PWD")) { cat(cat(cat(line, "257 \""), s->cwd), "\""); reply(s, line); return; }
    if (!strcmp(cmd, "CWD") || !strcmp(cmd, "CDUP")) {
        if (!resolve(s->cwd, !strcmp(cmd, "CDUP") ? ".." : arg, P) || stat_(P, &info) || info.type != LFS_TYPE_DIR) { reply(s, "550 No such folder."); return; }
        strcpy(s->cwd, P); reply(s, "250 OK."); return;
    }
    if (!strcmp(cmd, "PASV") || !strcmp(cmd, "EPSV")) {
        int port = FTP_PASV_PORT + (int)(s - ss);
        if (s->data) { io_close(s->data); s->data = 0; }
        if (!s->pasv && !(s->pasv = io_listen(port))) { reply(s, "425 Cannot open a data port."); return; }
        if (cmd[0] == 'E') { char *o = cat(line, "229 Entering Extended Passive Mode (|||"); o = catn(o, (unsigned long)port); cat(o, "|)"); }
        else {
            u32 ip = io_local_ip(s->ctl); char *o = cat(line, "227 Entering Passive Mode (");
            for (int i = 0; i < 4; i++) { o = catn(o, ip >> (8 * i) & 255); o = cat(o, ","); }
            o = catn(o, (unsigned long)port >> 8); o = cat(o, ","); o = catn(o, (unsigned long)port & 255); cat(o, ")");
        }
        reply(s, line); return;
    }
    if (!strcmp(cmd, "PORT") || !strcmp(cmd, "EPRT")) { reply(s, "502 Use passive mode."); return; }
    if (!strcmp(cmd, "LIST") || !strcmp(cmd, "NLST") || !strcmp(cmd, "MLSD")) {
        if (!strcmp(cmd, "MLSD")) { reply(s, "502 Use LIST."); return; }
        while (*arg == '-') { while (*arg && *arg != ' ') arg++; while (*arg == ' ') arg++; }   /* (options like -la) */
        if (!resolve(s->cwd, *arg ? arg : ".", P) || stat_(P, &info)) { reply(s, "550 Not found."); return; }
        s->list_one[0] = 0;
        if (info.type == LFS_TYPE_DIR) {
            if (lfs_dir_open(&fs_lfs, &s->dir, P)) { reply(s, "550 Not found."); return; }
            s->dopen = 1; s->list_dir = !strcmp(P, "/");
        } else { int k = 0; for (; info.name[k] && k < (int)sizeof s->list_one - 1; k++) s->list_one[k] = info.name[k]; s->list_one[k] = 0; s->list_size = (long)info.size; }
        start_op(s, cmd[0] == 'N' ? OP_NLST : OP_LIST); return;
    }
    if (!strcmp(cmd, "RETR") || !strcmp(cmd, "STOR")) {
        int st = cmd[0] == 'S';
        if (!resolve(s->cwd, arg, P) || !*arg || hidden(P) || !strcmp(P, "/")) { reply(s, "550 Not allowed."); return; }
        if (!st && (stat_(P, &info) || info.type != LFS_TYPE_REG)) { reply(s, "550 No such file."); return; }
        memset(&s->fcfg, 0, sizeof s->fcfg); s->fcfg.buffer = s->fbuf;
        if (lfs_file_opencfg(&fs_lfs, &s->f, P, st ? LFS_O_WRONLY | LFS_O_CREAT | LFS_O_TRUNC : LFS_O_RDONLY, &s->fcfg)) { reply(s, st ? "553 Cannot create the file." : "550 Cannot open the file."); return; }
        s->fopen = 1;
        if (st) note_change(P);
        start_op(s, st ? OP_STOR : OP_RETR); return;
    }
    if (!strcmp(cmd, "SIZE")) {
        if (!resolve(s->cwd, arg, P) || stat_(P, &info) || info.type != LFS_TYPE_REG) { reply(s, "550 No such file."); return; }
        catn(cat(line, "213 "), info.size); reply(s, line); return;
    }
    if (!strcmp(cmd, "MDTM")) {
        if (!resolve(s->cwd, arg, P) || stat_(P, &info)) { reply(s, "550 No such file."); return; }
        reply(s, "213 20260101000000"); return;
    }
    if (!strcmp(cmd, "DELE") || !strcmp(cmd, "RMD")) {
        if (!resolve(s->cwd, arg, P) || !strcmp(P, "/") || stat_(P, &info) || (info.type == LFS_TYPE_DIR) != (cmd[0] == 'R')) { reply(s, "550 Not found."); return; }
        if (lfs_remove(&fs_lfs, P)) { reply(s, cmd[0] == 'R' ? "550 The folder is not empty." : "550 Cannot delete."); return; }
        note_change(P); reply(s, "250 Deleted."); return;
    }
    if (!strcmp(cmd, "MKD")) {
        if (!resolve(s->cwd, arg, P) || hidden(P) || lfs_mkdir(&fs_lfs, P)) { reply(s, "550 Cannot create the folder."); return; }
        note_change(P); cat(cat(cat(line, "257 \""), P), "\" created."); reply(s, line); return;
    }
    if (!strcmp(cmd, "RNFR")) {
        if (!resolve(s->cwd, arg, P) || !strcmp(P, "/") || stat_(P, &info)) { reply(s, "550 Not found."); return; }
        strcpy(s->rnfr, P); reply(s, "350 Ready."); return;
    }
    if (!strcmp(cmd, "RNTO")) {
        if (!s->rnfr[0]) { reply(s, "503 RNFR first."); return; }
        if (!resolve(s->cwd, arg, P) || hidden(P) || lfs_rename(&fs_lfs, s->rnfr, P)) { s->rnfr[0] = 0; reply(s, "550 Cannot rename."); return; }
        note_change(s->rnfr); note_change(P); s->rnfr[0] = 0; reply(s, "250 Renamed."); return;
    }
    if (!strcmp(cmd, "ABOR")) { reply(s, "226 OK."); return; }
    if (!strcmp(cmd, "ALLO")) { reply(s, "202 OK."); return; }
    reply(s, "502 Not supported.");
}

/* one transfer step: as much as the connection takes now. Returns the final reply when the transfer is over. */
static const char *transfer(sess_t *s) {
    if (s->op == OP_STOR) {
        for (int k = 0; k < 8; k++) {
            int n = io_recv(s->data, s->buf, sizeof s->buf);
            if (n < 0) { int r = lfs_file_close(&fs_lfs, &s->f); s->fopen = 0; apps_dirty = 1; return r ? "552 The GamePad is full." : "226 Done."; }   /* (an upload is whole now) */
            if (!n) return 0;
            if (lfs_file_write(&fs_lfs, &s->f, s->buf, (lfs_size_t)n) != n) return "552 The GamePad is full.";
        }
        return 0;
    }
    for (;;) {
        if (s->buf_off < s->buf_n) {
            int room = io_room(s->data), n = s->buf_n - s->buf_off;
            if (room <= 0) { io_flush(s->data); return 0; }   /* (0: not over yet) */
            if (n > room) n = room;
            n = io_send(s->data, s->buf + s->buf_off, n);
            if (n <= 0) { io_flush(s->data); return 0; }
            s->buf_off += n; continue;
        }
        s->buf_n = s->buf_off = 0;
        if (s->op == OP_RETR) {
            int n = lfs_file_read(&fs_lfs, &s->f, s->buf, sizeof s->buf);
            if (n < 0) return "451 Read error.";
            if (!n) return "226 Done.";
            s->buf_n = n;
        } else if (s->list_one[0]) { list_line(s, s->list_one, 0, (unsigned long)s->list_size); s->list_one[0] = 0; }
        else if (s->dopen) {
            struct lfs_info i; int r = lfs_dir_read(&fs_lfs, &s->dir, &i);
            if (r <= 0) return "226 Done.";
            if (!strcmp(i.name, ".") || !strcmp(i.name, "..") || (s->list_dir && !strcmp(i.name, "config"))) continue;
            list_line(s, i.name, i.type == LFS_TYPE_DIR, (unsigned long)i.size);
        } else return "226 Done.";
    }
}

static void sess_step(sess_t *s) {
    u32 now = io_ms();
    /* the control connection: whole lines; while a transfer runs, only ABOR (the rest waits) */
    for (;;) {
        char *nl = 0;
        for (int i = 0; i < s->line_n; i++) if (s->line[i] == '\n') { nl = s->line + i; break; }
        if (!nl) {
            if (s->line_n == (int)sizeof s->line - 1) s->line_n = 0;          /* (an endless line: dropped) */
            int n = io_recv(s->ctl, s->line + s->line_n, (int)sizeof s->line - 1 - s->line_n);
            if (n < 0) { sess_end(s); return; }
            if (!n) break;
            s->line_n += n; s->last = now; continue;
        }
        int len = (int)(nl - s->line) + 1;
        char l[512]; memcpy(l, s->line, (size_t)len); l[len] = 0;
        char *c = l; while (*c && (u8)*c >= 0x80) c++;                       /* (Telnet IP/Synch before ABOR) */
        int abor = (c[0] | 32) == 'a' && (c[1] | 32) == 'b' && (c[2] | 32) == 'o' && (c[3] | 32) == 'r';
        if (s->op != OP_NONE && !abor) break;
        memmove(s->line, s->line + len, (size_t)(s->line_n - len)); s->line_n -= len;
        char *e = l + strlen(l); while (e > l && (e[-1] == '\r' || e[-1] == '\n')) *--e = 0;
        if (abor && s->op != OP_NONE) { end_op(s); reply(s, "426 Transfer aborted."); reply(s, "226 OK."); continue; }
        char *a = c; while (*a && *a != ' ') a++;
        if (*a) *a++ = 0;
        command(s, c, a);
        if (s->quit) { sess_end(s); return; }
    }
    if (s->op != OP_NONE) {
        if (!s->data && s->pasv) s->data = io_accept(s->pasv);
        if (!s->data) { if (now - s->op_t > 10000) { end_op(s); reply(s, "425 No data connection."); } return; }
        const char *r = transfer(s);
        if (r) { end_op(s); reply(s, r); }
        s->last = now;
    } else if (now - s->last > 600000) { reply(s, "421 Timeout."); sess_end(s); }
}

int ftpd_busy(void) { for (int i = 0; i < NSESS; i++) if (ss[i].used && ss[i].op != OP_NONE) return 1; return 0; }
static int ftpd_open_(void) {
    if (listener) return 0;
    listener = io_listen(FTP_PORT);
    return listener ? 0 : -1;
}
static void ftpd_close_(void) {
    for (int i = 0; i < NSESS; i++) if (ss[i].used) sess_end(&ss[i]);
    if (listener) { io_close(listener); listener = 0; }
}
static void ftpd_step_(void) {
    if (!listener) return;
    for (ep_t *e; (e = io_accept(listener)); ) {
        int i = 0; while (i < NSESS && ss[i].used) i++;
        if (i == NSESS) { io_send(e, "421 Too many connections.\r\n", 27); io_close(e); continue; }
        sess_t *s = &ss[i];
        memset(s, 0, sizeof *s);
        s->used = 1; s->ctl = e; s->cwd[0] = '/'; s->last = io_ms();
        reply(s, "220 GamePad");
    }
    for (int i = 0; i < NSESS; i++) if (ss[i].used) sess_step(&ss[i]);
}

#ifndef FTP_HOST
/* ---- lwIP glue (the pad) */
#include "lwip/tcp.h"
struct ep { struct tcp_pcb *pcb; struct pbuf *pend; int used, fin, dead, lst; struct ep *acc[2]; };
static struct ep eps[1 + NSESS * 3];
static ep_t *ep_new(void) {
    for (unsigned i = 0; i < sizeof eps / sizeof *eps; i++) if (!eps[i].used) { memset(&eps[i], 0, sizeof eps[i]); eps[i].used = 1; return &eps[i]; }
    return 0;
}
static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    ep_t *e = arg; (void)err;
    if (!e) { if (p) pbuf_free(p); else tcp_close(pcb); return ERR_OK; }
    if (!p) { e->fin = 1; return ERR_OK; }
    if (e->pend) pbuf_cat(e->pend, p); else e->pend = p;
    return ERR_OK;
}
static void on_err(void *arg, err_t err) { ep_t *e = arg; (void)err; if (e) { e->pcb = 0; e->dead = 1; } }   /* (lwIP freed the pcb) */
static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    ep_t *l = arg, *e; (void)err;
    int k = !l ? 2 : !l->acc[0] ? 0 : !l->acc[1] ? 1 : 2;
    if (k == 2 || !(e = ep_new())) { tcp_abort(pcb); return ERR_ABRT; }
    e->pcb = pcb;
    tcp_arg(pcb, e); tcp_recv(pcb, on_recv); tcp_err(pcb, on_err);
    tcp_nagle_disable(pcb);
    l->acc[k] = e;
    return ERR_OK;
}
static ep_t *io_listen(int port) {
    ep_t *e = ep_new(); if (!e) return 0;
    struct tcp_pcb *p = tcp_new();
    if (p) ip_set_option(p, SOF_REUSEADDR);              /* (a passive port again right after its last transfer: that one is in TIME_WAIT) */
    if (!p || tcp_bind(p, IP4_ADDR_ANY, (u16_t)port) != ERR_OK) { if (p) tcp_close(p); e->used = 0; return 0; }
    struct tcp_pcb *l = tcp_listen(p);
    if (!l) { tcp_close(p); e->used = 0; return 0; }
    e->pcb = l; e->lst = 1;
    tcp_arg(l, e); tcp_accept(l, on_accept);
    return e;
}
static ep_t *io_accept(ep_t *l) { ep_t *e = l->acc[0]; l->acc[0] = l->acc[1]; l->acc[1] = 0; return e; }
static int io_recv(ep_t *e, void *buf, int max) {
    int n = 0;
    while (e->pend && n < max) {
        struct pbuf *q = e->pend;
        u16_t k = (u16_t)(max - n < (int)q->tot_len ? max - n : (int)q->tot_len);
        pbuf_copy_partial(q, (u8 *)buf + n, k, 0);
        n += k;
        e->pend = k == q->tot_len ? (pbuf_free(q), (struct pbuf *)0) : pbuf_free_header(q, k);
        if (e->pcb) tcp_recved(e->pcb, k);
    }
    return n ? n : (e->fin || e->dead) ? -1 : 0;
}
static int io_room(ep_t *e) {
    if (!e->pcb || tcp_sndqueuelen(e->pcb) >= TCP_SND_QUEUELEN - 2) return 0;
    return (int)tcp_sndbuf(e->pcb);
}
static int io_send(ep_t *e, const void *d, int n) {
    if (!e->pcb || n <= 0) return 0;
    return tcp_write(e->pcb, d, (u16_t)n, TCP_WRITE_FLAG_COPY) == ERR_OK ? n : 0;
}
static void io_flush(ep_t *e) { if (e->pcb) tcp_output(e->pcb); }
static void io_close(ep_t *e) {
    if (e->pcb) {
        if (e->lst) {
            for (int i = 0; i < 2; i++) if (e->acc[i]) io_close(e->acc[i]);
            tcp_arg(e->pcb, 0); tcp_accept(e->pcb, 0); tcp_close(e->pcb);
        } else {
            tcp_arg(e->pcb, 0); tcp_recv(e->pcb, 0); tcp_err(e->pcb, 0);
            if (tcp_close(e->pcb) != ERR_OK) tcp_abort(e->pcb);
        }
    }
    if (e->pend) pbuf_free(e->pend);
    memset(e, 0, sizeof *e);
}
static u32 io_local_ip(ep_t *e) { return e->pcb ? ip4_addr_get_u32(ip_2_ip4(&e->pcb->local_ip)) : 0; }
#include "lwip/sys.h"
static u32 io_ms(void) { return sys_now(); }
#else
#include "ftphost.h"                              /* ---- host glue: tools/ftptest/ftphost.c */
#endif

#ifndef FTP_HOST
/* lwIP runs in its own thread (the menu: net = yes): its callbacks above run there, holding its core lock; these run
   on the menu's thread under the same lock, so the two never overlap */
#include "lwip/tcpip.h"
int ftpd_open(void) { LOCK_TCPIP_CORE(); int r = ftpd_open_(); UNLOCK_TCPIP_CORE(); return r; }
void ftpd_close(void) { LOCK_TCPIP_CORE(); ftpd_close_(); UNLOCK_TCPIP_CORE(); }
void ftpd_step(void) { LOCK_TCPIP_CORE(); ftpd_step_(); UNLOCK_TCPIP_CORE(); }
#else
int ftpd_open(void) { return ftpd_open_(); }
void ftpd_close(void) { ftpd_close_(); }
void ftpd_step(void) { ftpd_step_(); }
#endif
