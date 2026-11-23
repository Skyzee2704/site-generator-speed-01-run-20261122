/* entry.c — the freestanding `site` binary's entry point.
 * Usage: site <cases.json> <answers.json>
 * Reads every case from the input file as raw bytes, answers them all
 * with one call into cases_build, and writes the returned bytes out.
 * The input is a top-level JSON array of {id, input} objects; a single
 * {input} object is also accepted (one-case mode, for solve.js). */
#include "zg.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT 0x40
#define O_TRUNC 0x200

static void write_all(int fd, const uint8_t *p, size_t n) {
    while (n > 0) {
        long w = os_write(fd, p, (long)n);
        if (w <= 0) die(1);
        p += w;
        n -= (size_t)w;
    }
}

static void fail(const char *msg) {
    size_t n = 0;
    while (msg[n]) n++;
    write_all(2, (const uint8_t *)msg, n);
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
    pool_open(&pool, 1 << 26);  /* 64 MB working pool */
    pool_open(&out, 1 << 25);   /* 32 MB answers pool */

    /* Read the whole input (a few MB) straight into the working pool:
     * the parser's string spans point at these bytes, so a separate read
     * buffer + copy would touch every input page twice (read faults +
     * copy faults). */
    size_t len = 0;
    for (;;) {
        if (len == pool.cap) fail("site: input too large\n");
        long r = os_read((int)fd, pool.base + len, (long)(pool.cap - len));
        if (r < 0) fail("site: read failed\n");
        if (r == 0) break;
        len += (size_t)r;
    }
    os_close((int)fd);
    pool.len = len;
    const uint8_t *buf = pool.base;

    /* Single-object input (solve.js one-case mode): wrap as one case. */
    size_t i = 0;
    while (i < len && (buf[i] == ' ' || buf[i] == '\t' || buf[i] == '\n' || buf[i] == '\r')) i++;
    if (i < len && buf[i] == '{') {
        /* Parse as one CaseInput and build one answer object. */
        Sp input_s = {0, (uint32_t)len};
        Cur cur = {&pool, input_s.off, input_s.off + input_s.len};
        CaseInput c;
        if (!json_read_case(&cur, &c)) fail("site: bad input\n");
        if (!case_build(&pool, &c, &out)) fail("site: build failed\n");
    } else {
        if (!cases_build(&pool, buf, len, &out)) fail("site: build failed\n");
    }

    long ofd = os_open(out_path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (ofd < 0) fail("site: cannot open output\n");
    write_all((int)ofd, out.base, out.len);
    os_close((int)ofd);
    return 0;
}
