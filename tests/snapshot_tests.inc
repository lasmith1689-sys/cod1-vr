// snapshot_tests.inc -- the in-engine snapshot facility (2026-09-06):
// the decision logic against a scripted controller, and the PNG writer
// against an independent decoder (chunk CRCs, stored-deflate inflate,
// adler32) plus a pixel round trip through the downsampler.

static void S_SnapshotDecide()
{
    printf("--- snapshot: decision logic (timer, trigger edge, debounce, cap, disabled) ---\n");
    VRSnapConfig c = { 1, 5.0f, 1, 0, 3, 2, 1.0f };
    VRSnapState st = { -1.0, 0, 0, 0 };
    int why = VRSnap_Decide(&c, &st, 100.0, 0, 0);
    CHECK(why == VRSNAP_WHY_FIRST, "first pair fires at once (why %d)", why);
    CHECK(VRSnap_Decide(&c, &st, 100.5, 0, 0) == 0, "no timer shot inside the interval");
    CHECK(VRSnap_Decide(&c, &st, 104.9, 0, 0) == 0, "not yet at 4.9 s");
    why = VRSnap_Decide(&c, &st, 105.0, 0, 0);
    CHECK(why == VRSNAP_WHY_TIMER, "timer fires at the interval (why %d)", why);
    // Trigger: press edge only, debounced.
    CHECK(VRSnap_Decide(&c, &st, 105.2, 1, 0) == 0, "trigger press 0.2 s after a shot is debounced");
    CHECK(VRSnap_Decide(&c, &st, 106.5, 1, 0) == 0, "held trigger is no new edge");
    CHECK(VRSnap_Decide(&c, &st, 106.6, 0, 0) == 0, "release is nothing");
    why = VRSnap_Decide(&c, &st, 106.7, 1, 0);
    CHECK(why == VRSNAP_WHY_TRIGGER, "fresh press after the debounce fires (why %d)", why);
    CHECK(st.taken == 3, "three taken (got %d)", st.taken);
    CHECK(VRSnap_Decide(&c, &st, 200.0, 0, 0) == 0, "hard cap: the 4th never comes");
    CHECK(VRSnap_Decide(&c, &st, 200.0, 1, 0) == 0, "hard cap holds for the trigger too");
    // Disabled: never, and the edge state still tracks.
    VRSnapConfig off = c; off.enabled = 0;
    VRSnapState s2 = { -1.0, 0, 0, 0 };
    CHECK(VRSnap_Decide(&off, &s2, 1.0, 1, 1) == 0 && s2.taken == 0, "disabled takes nothing");
    // Button source, timer off.
    VRSnapConfig b = { 1, 0.0f, 0, 1, 0, 1, 0.5f };
    VRSnapState s3 = { -1.0, 0, 0, 0 };
    CHECK(VRSnap_Decide(&b, &s3, 0.0, 1, 0) == VRSNAP_WHY_FIRST, "first fires regardless of source");
    CHECK(VRSnap_Decide(&b, &s3, 10.0, 1, 0) == 0, "timer off: a trigger press is ignored when only the button is armed");
    CHECK(VRSnap_Decide(&b, &s3, 10.1, 0, 1) == VRSNAP_WHY_BUTTON, "button edge fires");
    CHECK(VRSnap_Decide(&b, &s3, 30.0, 0, 1) == 0, "held button is not an edge");
    CHECK(VRSnap_Decide(&b, &s3, 30.1, 0, 0) == 0 && VRSnap_Decide(&b, &s3, 30.2, 0, 1) == VRSNAP_WHY_BUTTON,
          "release then press fires again; no cap when max is 0 (taken %d)", s3.taken);
}

// Independent PNG reader for the test: verifies the container and returns
// the pixels. Returns 0 on any structural problem.
static unsigned SnapCrc(const unsigned char* p, size_t n, unsigned crc)
{
    crc ^= 0xFFFFFFFFu;
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k) crc = (crc & 1) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
    }
    return crc ^ 0xFFFFFFFFu;
}
static unsigned SnapBe32(const unsigned char* p) { return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static int SnapReadPng(const char* path, unsigned char** outRgb, int* w, int* h, char* err, size_t errN)
{
    *outRgb = nullptr; *w = *h = 0; err[0] = 0;
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) { sprintf_s(err, errN, "open"); return 0; }
    fseek(f, 0, SEEK_END); const long len = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char* buf = (unsigned char*)malloc((size_t)len);
    const size_t got = fread(buf, 1, (size_t)len, f); fclose(f);
    if ((long)got != len || len < 8) { free(buf); sprintf_s(err, errN, "read"); return 0; }
    static const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    if (memcmp(buf, sig, 8)) { free(buf); sprintf_s(err, errN, "signature"); return 0; }
    size_t p = 8; unsigned char* z = nullptr; size_t zn = 0; int sawEnd = 0;
    while (p + 12 <= (size_t)len) {
        const unsigned n = SnapBe32(buf + p);
        const unsigned char* type = buf + p + 4;
        if (p + 12 + n > (size_t)len) { sprintf_s(err, errN, "chunk overrun"); break; }
        const unsigned crc = SnapCrc(buf + p + 4, n + 4, 0);
        if (crc != SnapBe32(buf + p + 8 + n)) { sprintf_s(err, errN, "crc %c%c%c%c", type[0], type[1], type[2], type[3]); break; }
        if (!memcmp(type, "IHDR", 4)) {
            if (n != 13) { sprintf_s(err, errN, "ihdr len"); break; }
            *w = (int)SnapBe32(buf + p + 8); *h = (int)SnapBe32(buf + p + 12);
            const unsigned char* q = buf + p + 16;
            if (q[0] != 8 || q[1] != 2 || q[2] || q[3] || q[4]) { sprintf_s(err, errN, "ihdr fields"); break; }
        } else if (!memcmp(type, "IDAT", 4)) {
            z = (unsigned char*)realloc(z, zn + n); memcpy(z + zn, buf + p + 8, n); zn += n;
        } else if (!memcmp(type, "IEND", 4)) { sawEnd = 1; }
        p += 12 + n;
    }
    free(buf);
    if (err[0] || !sawEnd || !z || *w < 1 || *h < 1) { if (!err[0]) sprintf_s(err, errN, "structure"); free(z); return 0; }
    // zlib: header, stored blocks, adler32.
    if (zn < 6 || (z[0] & 0x0F) != 8 || ((z[0] << 8) | z[1]) % 31) { sprintf_s(err, errN, "zlib header"); free(z); return 0; }
    const size_t rowBytes = (size_t)*w * 3 + 1, rawN = rowBytes * (size_t)*h;
    unsigned char* raw = (unsigned char*)malloc(rawN); size_t rp = 0; size_t q = 2; int last = 0;
    while (!last) {
        if (q + 5 > zn) { sprintf_s(err, errN, "block header"); break; }
        const unsigned char hdr = z[q]; last = hdr & 1;
        if ((hdr >> 1) & 3) { sprintf_s(err, errN, "not a stored block"); break; }
        const unsigned n = z[q + 1] | (z[q + 2] << 8), nn = z[q + 3] | (z[q + 4] << 8);
        if ((n ^ nn) != 0xFFFF) { sprintf_s(err, errN, "nlen"); break; }
        if (q + 5 + n > zn || rp + n > rawN) { sprintf_s(err, errN, "block overrun"); break; }
        memcpy(raw + rp, z + q + 5, n); rp += n; q += 5 + n;
    }
    if (!err[0]) {
        if (rp != rawN) sprintf_s(err, errN, "raw length %zu != %zu", rp, rawN);
        else {
            unsigned a = 1, b = 0;
            for (size_t i = 0; i < rawN; ++i) { a = (a + raw[i]) % 65521u; b = (b + a) % 65521u; }
            if (q + 4 > zn || SnapBe32(z + q) != ((b << 16) | a)) sprintf_s(err, errN, "adler32");
        }
    }
    free(z);
    if (err[0]) { free(raw); return 0; }
    unsigned char* rgb = (unsigned char*)malloc((size_t)*w * *h * 3);
    for (int y = 0; y < *h; ++y) {
        if (raw[y * rowBytes] != 0) { sprintf_s(err, errN, "filter row %d", y); free(raw); free(rgb); return 0; }
        memcpy(rgb + (size_t)y * *w * 3, raw + y * rowBytes + 1, (size_t)*w * 3);
    }
    free(raw);
    *outRgb = rgb;
    return 1;
}

static void S_SnapshotPng()
{
    printf("--- snapshot: PNG writer against an independent decoder; downsampler ---\n");
    // A 131 x 77 gradient with a hard edge: odd sizes, > 1 stored block when
    // scaled up, every byte value in play.
    const int w = 131, h = 77;
    unsigned char* img = (unsigned char*)malloc((size_t)w * h * 3);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            unsigned char* p = img + ((size_t)y * w + x) * 3;
            p[0] = (unsigned char)(x * 255 / (w - 1));
            p[1] = (unsigned char)(y * 255 / (h - 1));
            p[2] = (unsigned char)((x + y) & 1 ? 255 : 0);
        }
    char dir[MAX_PATH]; GetTempPathA(MAX_PATH, dir);
    char path[MAX_PATH]; sprintf_s(path, "%scodvr_snap_test.png", dir);
    const int bytes = VRSnap_WritePng(path, img, w, h);
    CHECK(bytes > 0, "write returns a byte count (%d)", bytes);
    unsigned char* back = nullptr; int rw = 0, rh = 0; char err[128];
    const int ok = SnapReadPng(path, &back, &rw, &rh, err, sizeof(err));
    CHECK(ok, "independent decoder accepts the file (%s)", ok ? "ok" : err);
    if (ok) {
        CHECK(rw == w && rh == h, "dimensions round trip (%dx%d)", rw, rh);
        CHECK(memcmp(back, img, (size_t)w * h * 3) == 0, "pixels round trip byte-exact");
        free(back);
    }
    // A big image crosses the 65535-byte stored-block limit many times.
    const int bw = 640, bh = 360;
    unsigned char* big = (unsigned char*)malloc((size_t)bw * bh * 3);
    for (size_t i = 0; i < (size_t)bw * bh * 3; ++i) big[i] = (unsigned char)((i * 7919u) >> 3);
    sprintf_s(path, "%scodvr_snap_test_big.png", dir);
    CHECK(VRSnap_WritePng(path, big, bw, bh) > 0, "big write ok");
    const int okb = SnapReadPng(path, &back, &rw, &rh, err, sizeof(err));
    CHECK(okb && rw == bw && rh == bh && memcmp(back, big, (size_t)bw * bh * 3) == 0,
          "multi-block stored deflate round trips (%s)", okb ? "ok" : err);
    if (okb) free(back);
    // Mutant: corrupt one byte of the file -> the CRC check must reject it.
    {
        FILE* f = nullptr;
        if (fopen_s(&f, path, "r+b") == 0 && f) { fseek(f, 100, SEEK_SET); fputc(0x5A, f); fclose(f); }
        const int okm = SnapReadPng(path, &back, &rw, &rh, err, sizeof(err));
        CHECK(!okm, "mutant: a flipped byte is rejected by the decoder (%s)", okm ? "ACCEPTED" : err);
        if (okm) free(back);
    }
    free(big);
    // Downsampler: 1/2 of a 4x2 block image averages each 2x2 cell.
    unsigned char src[4 * 2 * 3];
    for (int i = 0; i < 4 * 2; ++i) { src[i * 3] = (unsigned char)(i * 10); src[i * 3 + 1] = 200; src[i * 3 + 2] = (unsigned char)(i & 1 ? 255 : 0); }
    unsigned char dst[4 * 2 * 3]; int ow = 0, oh = 0;
    VRSnap_Downsample(src, 4, 2, 2, dst, &ow, &oh);
    // cell 0 = pixels 0,1,4,5 -> R (0+10+40+50)/4 = 25; B (0+255+0+255)/4 = 128 (rounded)
    CHECK(ow == 2 && oh == 1, "downsample size 2x1 (got %dx%d)", ow, oh);
    CHECK(dst[0] == 25 && dst[1] == 200 && dst[2] == 128, "cell 0 box average (%d %d %d)", dst[0], dst[1], dst[2]);
    CHECK(dst[3] == 45 && dst[4] == 200 && dst[5] == 128, "cell 1 box average (%d %d %d)", dst[3], dst[4], dst[5]);
    VRSnap_Downsample(src, 4, 2, 1, dst, &ow, &oh);
    CHECK(ow == 4 && oh == 2, "divisor 1 keeps the size");
    free(img);
}
