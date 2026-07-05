/*
 * mod_isup — ISUP <-> SIP / FreeSWITCH interworking helpers.
 *
 * Pure mapping functions (no FS/Osmo dependency) so they can be unit-tested:
 *   - Q.850 cause <-> FreeSWITCH hangup cause (FS hangup causes ARE Q.850)
 *   - Transmission Medium Requirement <-> codec
 *   - ISUP CPG event <-> SIP provisional status
 *   - number nature/presentation/screening <-> E.164 string + SIP privacy
 */
#ifndef ISUP_MAP_H
#define ISUP_MAP_H

#include "isup_proto.h"
#include "isup_param.h"

/* Q.850 cause -> short name (for tracing / CDR). */
const char *isup_cause_name(uint8_t cause_value);

/* Transmission Medium Requirement -> FreeSWITCH/SDP codec token. */
const char *isup_tmr_to_codec(uint8_t tmr);

/* FS/SDP codec token -> TMR (for outbound IAM). Defaults to speech. */
uint8_t isup_codec_to_tmr(const char *codec);

/* CPG event -> SIP provisional status (180/183), 0 if none. */
int isup_event_to_sip(uint8_t event);

/* SIP status (18x) -> CPG event, 0 if none. */
uint8_t isup_sip_to_event(int status);

/*
 * Render an ISUP number as an E.164-style string into out.
 *   international -> "+<digits>"
 *   national/subscriber/other -> "<digits>"
 * Returns the FreeSWITCH TON string ("international"/"national"/...).
 */
const char *isup_number_to_e164(const struct isup_number *n, char *out, size_t cap);

/* Calling-party presentation -> SIP privacy: 1 if presentation restricted. */
int isup_calling_is_clir(const struct isup_number *n);

#endif /* ISUP_MAP_H */
