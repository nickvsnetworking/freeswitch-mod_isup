/*
 * mod_isup — Q.763 parameter body encode/decode helpers.
 *
 * These build/parse the *contents* of individual parameters (the value
 * octets stored in struct isup_param.val), separate from the message
 * framing in isup_codec.c.
 */
#ifndef ISUP_PARAM_H
#define ISUP_PARAM_H

#include "isup_proto.h"

/* Called / Calling party number (and the other number parameters). */
struct isup_number {
	uint8_t nature;       /* enum isup_noa */
	uint8_t npi;          /* enum isup_npi */
	uint8_t inn;          /* internal network number ind (called) */
	uint8_t ni;           /* number incomplete ind (calling) */
	uint8_t apri;         /* presentation restricted (calling) */
	uint8_t screening;    /* screening indicator (calling) */
	char    digits[32];   /* NUL-terminated decimal/hex digit string */
};

/* Encode a called-party-style number (octet2: INN|NPI). Returns length. */
int isup_enc_called(const struct isup_number *n, uint8_t *out, size_t cap);
int isup_dec_called(struct isup_number *n, const uint8_t *in, size_t len);

/* Encode a calling-party-style number (octet2: NI|NPI|APRI|screening). */
int isup_enc_calling(const struct isup_number *n, uint8_t *out, size_t cap);
int isup_dec_calling(struct isup_number *n, const uint8_t *in, size_t len);

/* Cause indicators (Q.850). location: 0=user .. 0x0a=BI, etc. */
int isup_enc_cause(uint8_t coding_std, uint8_t location, uint8_t cause_value,
		   uint8_t *out, size_t cap);
int isup_dec_cause(const uint8_t *in, size_t len,
		   uint8_t *coding_std, uint8_t *location, uint8_t *cause_value);

/*
 * Range and status. range = (last CIC in group) - (first CIC) ; the status
 * bitmap has (range+1) bits, one per circuit, LSB-first within each octet.
 * If status is NULL (GRS/CQM), only the range octet is emitted.
 */
int isup_enc_range_status(uint8_t range, const uint8_t *status, int with_status,
			  uint8_t *out, size_t cap);
int isup_dec_range_status(const uint8_t *in, size_t len,
			  uint8_t *range, uint8_t *status, size_t status_cap,
			  int *has_status);

#endif /* ISUP_PARAM_H */
