/*
 * mod_isup — decoded-message tracer.
 *
 * Renders a parsed ISUP message into a human-readable, Wireshark-style
 * multi-line decode for logging/debugging. Pure (no FS/Osmo), bounded
 * output, safe on any message produced by isup_decode().
 */
#ifndef ISUP_TRACE_H
#define ISUP_TRACE_H

#include "isup_proto.h"

/*
 * Format `m` into `out` (capacity `cap`). Always NUL-terminates (if cap>0).
 * Returns the number of characters that would have been written (like
 * snprintf), so a truncation is detectable.
 */
int isup_trace(const struct isup_msg *m, char *out, size_t cap);

#endif /* ISUP_TRACE_H */
