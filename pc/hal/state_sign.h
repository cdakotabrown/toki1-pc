/* state_sign.h -- the per-install signature of save states
 * (pc/hal/state_sign.c). */
#ifndef STATE_SIGN_H
#define STATE_SIGN_H

#include <stddef.h>
#include <stdint.h>

#define STATE_KEY_LEN 32
#define STATE_MAC_LEN 32
#define STATE_SIG_MAGIC "TKSIGv01"
#define STATE_TRAILER_LEN (8 + STATE_MAC_LEN)

/* this install's key (made on first use); 0, or -1 when it cannot be had */
int state_key(uint8_t key[STATE_KEY_LEN]);
void state_hmac(const uint8_t key[STATE_KEY_LEN], const void *data, size_t n,
                uint8_t out[STATE_MAC_LEN]);
/* the trailer to append after `data`; 0, or -1 without a key */
int state_sign_trailer(const void *data, size_t n, uint8_t trailer[STATE_TRAILER_LEN]);
/* 0 when `file` ends in a valid trailer (*body = the signed length);
 * -1 unsigned or signed by another key, -2 no key on this install */
int state_verify(const unsigned char *file, unsigned long n, unsigned long *body);

#endif
