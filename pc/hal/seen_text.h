/* SPDX-License-Identifier: GPL-3.0-or-later */
/* seen_text.h -- the dialogue pages the player has read, in any session
 * (pc/hal/seen_text.c), for the enhanced profile's "skip read dialogue".
 *
 * A page is identified by a 64-bit hash of its TEXT: the Shift-JIS codes
 * the backlog capture recovered for every glyph outside the speaker's name
 * (a glyph without a code: its bitmap), in string order, with a mark
 * between strings.  Position, colour, voice and screen are left out, so
 * the same line seen elsewhere is the same page; the speaker's name is
 * left out so that a renamed player does not make every line new (a line
 * that SPELLS the player's name inside the text still differs).
 *
 * A page counts as read once the capture took it (pc/hal/backlog_capture.c:
 * its arrow wait started, or it was confirmed away during its voice).
 * The set lives in <saves>/seen_text.dat ($TOKI_SEEN_FILE wins): the
 * 8-byte magic "TKSEEN01", then each page's hash, u64 little-endian,
 * appended as it is read.
 */
#ifndef PC_HAL_SEEN_TEXT_H
#define PC_HAL_SEEN_TEXT_H

#include "types.h"
#include "pc_backlog.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    unsigned long long *slot;   /* open addressing, 0 = empty */
    unsigned long cap, n;
} SeenSet;

/* pure pieces (pc/tests/test_seen_text.c) */
unsigned long long seen_text_page_hash(const BacklogPage *p);
int  seen_set_has(const SeenSet *s, unsigned long long h);
int  seen_set_add(SeenSet *s, unsigned long long h);   /* 1 new, 0 known, -1 no memory */
void seen_set_free(SeenSet *s);
/* read a seen file into S (a missing file is an empty set); -1 not a seen file */
int  seen_set_load(SeenSet *s, const char *path);
/* append H to the file at PATH (written with its magic when new); 0 ok */
int  seen_file_append(const char *path, unsigned long long h);

/* the session (the backlog capture and the input assist) */
int  seen_text_recording(void);     /* pages are recorded (enhanced, interactive or forced) */
int  seen_text_skip_enabled(void);  /* [enhanced] skip_seen (or TOKI_SKIP_SEEN=1) */
int  seen_text_has(unsigned long long h);
void seen_text_add(unsigned long long h);
unsigned long seen_text_count(void);

#ifdef __cplusplus
}
#endif
#endif
