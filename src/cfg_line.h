// cfg_line.h -- one pure helper: set an engine cvar line inside a config
// text the proxy owns (main\autoexec.cfg). Used to mirror codvr.cfg's
// vr_crosshair into cg_drawCrosshair, which only the engine can apply.
#pragma once
#include <string.h>
#include <stdio.h>
#include <ctype.h>

// Rewrites `text` (capacity `cap`, NUL-terminated) so that exactly one line
// reads `seta <cvar> "<value>"`. Every existing line whose first token (after
// an optional `set`/`seta`) is the cvar is replaced; the first one keeps its
// place, later duplicates are dropped; if none exists the line is appended.
// Returns 1 when the text changed, 0 when it already said so, -1 when the
// result would not fit.
static inline int CodvrSetCvarLine(char* text, size_t cap, const char* cvar, const char* value)
{
    char want[256];
    snprintf(want, sizeof(want), "seta %s \"%s\"", cvar, value);
    const size_t n = strlen(text);
    if (n + strlen(want) + 4 >= cap) return -1;
    static char out[16384];
    if (cap > sizeof(out)) return -1;
    size_t op = 0;
    int found = 0, changed = 0;
    const char* p = text;
    while (*p) {
        const char* e = strchr(p, '\n');
        const size_t len = e ? (size_t)(e - p) : strlen(p);
        // the first token, skipping a set/seta prefix
        const char* q = p;
        while (q < p + len && (*q == ' ' || *q == '\t')) ++q;
        if ((size_t)(p + len - q) >= 5 && !_strnicmp(q, "seta ", 5)) q += 5;
        else if ((size_t)(p + len - q) >= 4 && !_strnicmp(q, "set ", 4)) q += 4;
        while (q < p + len && (*q == ' ' || *q == '\t')) ++q;
        const size_t cl = strlen(cvar);
        const int isIt = (size_t)(p + len - q) >= cl && !_strnicmp(q, cvar, cl)
                      && (q + cl == p + len || q[cl] == ' ' || q[cl] == '\t' || q[cl] == '\r');
        if (isIt) {
            if (!found) {
                found = 1;
                // keep the line exactly if it already says so (ignoring CR)
                size_t body = len; while (body && p[body - 1] == '\r') --body;
                if (body == strlen(want) && !strncmp(p, want, body)) {
                    memcpy(out + op, p, len); op += len;
                } else {
                    memcpy(out + op, want, strlen(want)); op += strlen(want);
                    if (len && p[len - 1] == '\r') out[op++] = '\r';
                    changed = 1;
                }
            } else {
                changed = 1;          // duplicate dropped
                p += len + (e ? 1 : 0);
                continue;
            }
        } else {
            memcpy(out + op, p, len); op += len;
        }
        if (e) out[op++] = '\n';
        p += len + (e ? 1 : 0);
        if (op + 512 >= sizeof(out)) return -1;
    }
    if (!found) {
        if (op && out[op - 1] != '\n') out[op++] = '\n';
        memcpy(out + op, want, strlen(want)); op += strlen(want);
        out[op++] = '\n';
        changed = 1;
    }
    out[op] = 0;
    if (op + 1 > cap) return -1;
    if (!changed) return 0;
    memcpy(text, out, op + 1);
    return 1;
}
