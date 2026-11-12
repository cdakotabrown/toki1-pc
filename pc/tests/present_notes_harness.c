/* SPDX-License-Identifier: GPL-3.0-or-later */
/* present_notes_harness.c -- the presenter's notes queue (pc/hal/
 * present_layout.c present_notes_*, pc/include/pc_present.h "Notes")
 * without SDL.  pc/tools/test_ui_state_machines.py builds it with
 * present_layout.c (and with source mutations, its negative controls) and
 * runs it; each case prints "ok NAME" or "FAIL NAME: why", the exit status
 * is the number of failures.  docs/pc-port/STATE-MACHINES.md A8. */
#include "pc_present.h"
#include <stdio.h>
#include <string.h>

static int g_fail;

static void check(const char *name, int ok, const char *why) {
    if (ok) printf("ok %s\n", name);
    else { printf("FAIL %s: %s\n", name, why); g_fail++; }
}

static int is(const char *got, const char *want) {
    if (!got || !want) return got == want;
    return !strcmp(got, want);
}

/* Two notes at once (the launch's "cannot be resumed" and "English
 * unavailable"): both are shown, in order, each for its own time. */
static void two_in_order(void) {
    PresentNotes q;
    int ok = 1;
    memset(&q, 0, sizeof q);
    present_notes_push(&q, "first", 1000);
    present_notes_push(&q, "second", 1000);
    ok &= is(present_notes_show(&q, 0, 1), "first");
    ok &= is(present_notes_show(&q, 600, 1), "first");
    ok &= is(present_notes_show(&q, 999, 1), "first");
    ok &= is(present_notes_show(&q, 1000, 1), "second");
    ok &= is(present_notes_show(&q, 1999, 1), "second");
    ok &= is(present_notes_show(&q, 2000, 1), NULL);
    check("two_in_order", ok, "the second note replaced the first, or a note ended early");
}

/* A note shown under the menu (or the pause label, a toast...) waits: its
 * time runs only while it is on screen. */
static void covered_waits(void) {
    PresentNotes q;
    int ok = 1;
    memset(&q, 0, sizeof q);
    present_notes_push(&q, "note", 1000);
    ok &= is(present_notes_show(&q, 0, 1), "note");
    ok &= is(present_notes_show(&q, 500, 1), "note");      /* 500 ms seen */
    ok &= is(present_notes_show(&q, 600, 0), NULL);        /* the menu opens */
    ok &= is(present_notes_show(&q, 9000, 0), NULL);       /* ... for 8 s */
    ok &= is(present_notes_show(&q, 9100, 1), "note");     /* closed: 500 ms left */
    ok &= is(present_notes_show(&q, 9500, 1), "note");
    ok &= is(present_notes_show(&q, 9700, 1), NULL);
    check("covered_waits", ok, "the note's time ran while it was covered");
}

/* A note queued while covered is shown once the slot is free. */
static void queued_while_covered(void) {
    PresentNotes q;
    int ok = 1;
    memset(&q, 0, sizeof q);
    ok &= is(present_notes_show(&q, 0, 0), NULL);
    present_notes_push(&q, "late", 300);
    ok &= is(present_notes_show(&q, 5000, 0), NULL);
    ok &= is(present_notes_show(&q, 6000, 1), "late");
    ok &= is(present_notes_show(&q, 6299, 1), "late");
    ok &= is(present_notes_show(&q, 6300, 1), NULL);
    check("queued_while_covered", ok, "a note queued under the menu was lost");
}

/* The same text twice is one note; past PRESENT_NOTES the newest is dropped. */
static void duplicates_and_capacity(void) {
    PresentNotes q;
    char t[16];
    int i, ok = 1;
    memset(&q, 0, sizeof q);
    ok &= present_notes_push(&q, "same", 100) == 0;
    ok &= present_notes_push(&q, "same", 100) == 1;
    ok &= q.n == 1;
    for (i = 1; i < PRESENT_NOTES; i++) {
        snprintf(t, sizeof t, "n%d", i);
        ok &= present_notes_push(&q, t, 100) == 0;
    }
    ok &= present_notes_push(&q, "overflow", 100) == -1 && q.dropped == 1 && q.n == PRESENT_NOTES;
    ok &= is(present_notes_show(&q, 0, 1), "same");
    check("duplicates_and_capacity", ok, "duplicate or capacity rule broken");
}

int main(void) {
    two_in_order();
    covered_waits();
    queued_while_covered();
    duplicates_and_capacity();
    return g_fail;
}
