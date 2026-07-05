/*
 * mod_isup — Q.763 message codec API.
 *
 * The codec converts between the wire format (the MTP3-user octets handed
 * up by M3UA, beginning at the CIC) and the parsed struct isup_msg.
 */
#ifndef ISUP_CODEC_H
#define ISUP_CODEC_H

#include "isup_proto.h"

/* ---- message table helpers ---- */

/* Reset a message and set CIC + type. */
void isup_msg_init(struct isup_msg *m, uint16_t cic, uint8_t msg_type);

/* Add/replace a parameter. Returns ISUP_OK or ISUP_ERR_TOOMANY/BADLEN. */
int  isup_msg_add(struct isup_msg *m, uint8_t code, const uint8_t *val, uint8_t len);

/* Find a parameter by code; NULL if absent. */
const struct isup_param *isup_msg_get(const struct isup_msg *m, uint8_t code);

/* ---- wire codec ---- */

/*
 * Encode m into buf (capacity buflen). On success returns the number of
 * octets written; on failure returns a negative enum isup_rc.
 */
int  isup_encode(const struct isup_msg *m, uint8_t *buf, size_t buflen);

/*
 * Decode buflen octets at buf into m. Returns ISUP_OK or a negative
 * enum isup_rc. Never reads past buf[buflen-1]; safe on hostile input.
 */
int  isup_decode(struct isup_msg *m, const uint8_t *buf, size_t buflen);

/* True if `code` is a mandatory (fixed or variable) parameter of message
 * type `mt`. Used by the segmentation layer to know which parameters may be
 * moved into an SGM message (only optional ones). */
int  isup_is_mandatory_param(uint8_t mt, uint8_t code);

/* Human-readable name for a message type / parameter code (for tracing). */
const char *isup_msg_type_name(uint8_t mt);
const char *isup_param_name(uint8_t code);
const char *isup_strerror(int rc);

#endif /* ISUP_CODEC_H */
