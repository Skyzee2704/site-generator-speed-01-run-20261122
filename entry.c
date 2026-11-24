/* entry.c — the freestanding `site` binary's entry point.
 * Usage: site <cases.json> <answers.json>
 * Maps the input file read-only (zero-copy, MAP_POPULATE) with a read
 * fallback for non-regular files, and streams answers directly into a
 * 512 KB output buffer flushed to the output fd. */
#include "zg.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200

static void write_err(const char *msg) {
    size_t n = 0;
    while (msg[n]) n++;
    while (n > 0) {
        long w = os_write(2, msg, (long)n);
        if (w <= 0) break;
        msg += w;
        n -= (size_t)w;
    }
}

static void fail(const char *msg) {
    write_err(msg);
    die(1);
}

int entry_main(long *sp) {
    cpu_detect();
    long argc = sp[0];
    if (argc < 3) fail("usage: site <cases.json> <answers.json>\n");
    const char *in_path = (const char *)sp[2];
    const char *out_path = (const char *)sp[3];

    long fd = os_open(in_path, O_RDONLY, 0);
    if (fd < 0) fail("site: cannot open input\n");

    Pool pool, out;
    pool_open(&pool, 1 << 26);  /* 64 MB working pool (virtual; only touched pages fault) */

    const uint8_t *buf = 0;
    size_t len = 0;
    size_t fsz = 0;
    if (os_fstat_reg_size((int)fd, &fsz) == 0 && fsz > 0) {
        void *mp = os_mmap_ro((int)fd, fsz);
        if (mp) {
            buf = (const uint8_t *)mp;
            len = fsz;
        }
    }
    if (!buf) {
        for (;;) {
            if (len == pool.cap) fail("site: input too large\n");
            long r = os_read((int)fd, pool.base + len, (long)(pool.cap - len));
            if (r < 0) fail("site: read failed\n");
            if (r == 0) break;
            len += (size_t)r;
        }
        pool.len = len;
        buf = pool.base;
    }
    os_close((int)fd);

    long ofd = os_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) fail("site: cannot open output\n");
    pool_open_stream(&out, 256 * 1024, (int)ofd);

    /* Single-object input (solve.js one-case mode): wrap as one case. */
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
    if (i < len && buf[i] == '{') {
        Cur cur = {buf, &pool, 0, len};
        CaseInput c;
        if (!json_read_case(&cur, &c)) fail("site: bad input\n");
        if (!case_build(&pool, &c, &out)) fail("site: build failed\n");
    } else {
        if (!cases_build(&pool, buf, len, &out)) fail("site: build failed\n");
    }

    out_flush(&out);
    os_close((int)ofd);
    return 0;
}
