/*
 * mod_isup — Q.763 parameter body encode/decode helpers.
 */
#include <string.h>
#include "isup_param.h"

/* ---- BCD address-signal helpers ---- */

static int digit_val(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c == '*') return 0x0b;
	if (c == '#') return 0x0c;
	if (c >= 'a' && c <= 'f') return c - 'a' + 0x0a; /* hex (generic digits) */
	if (c >= 'A' && c <= 'F') return c - 'A' + 0x0a;
	return -1;
}

static char val_digit(int v)
{
	static const char map[] = "0123456789*#abcd";
	return map[v & 0x0f];
}

/* Pack a digit string into BCD, two per octet, low nibble first.
 * Returns number of octets written, or -1 on bad input / overflow.
 * Sets *odd to 1 if the digit count is odd. */
static int pack_digits(const char *digits, uint8_t *out, size_t cap, int *odd)
{
	size_t n = strlen(digits), i, o = 0;
	for (i = 0; i < n; i++) {
		int v = digit_val(digits[i]);
		if (v < 0)
			return -1;
		if ((i & 1) == 0) {
			if (o >= cap)
				return -1;
			out[o] = (uint8_t)v;       /* low nibble */
		} else {
			out[o] |= (uint8_t)(v << 4); /* high nibble */
			o++;
		}
	}
	if (n & 1)
		o++; /* last octet's high nibble stays 0 (filler) */
	*odd = (int)(n & 1);
	return (int)o;
}

/* Unpack BCD address signals. n_octets octets, odd => drop trailing filler. */
static void unpack_digits(const uint8_t *in, size_t n_octets, int odd, char *out, size_t cap)
{
	size_t i, o = 0;
	for (i = 0; i < n_octets && o + 2 < cap; i++) {
		out[o++] = val_digit(in[i] & 0x0f);
		if (!(odd && i == n_octets - 1))
			out[o++] = val_digit((in[i] >> 4) & 0x0f);
	}
	out[o] = '\0';
}

/* ---- Called party number ---- */

int isup_enc_called(const struct isup_number *n, uint8_t *out, size_t cap)
{
	int odd, ndig;
	if (cap < 2)
		return -1;
	ndig = pack_digits(n->digits, out + 2, cap - 2, &odd);
	if (ndig < 0)
		return -1;
	out[0] = (uint8_t)((odd << 7) | (n->nature & 0x7f));
	out[1] = (uint8_t)(((n->inn & 1) << 7) | ((n->npi & 7) << 4));
	return 2 + ndig;
}

int isup_dec_called(struct isup_number *n, const uint8_t *in, size_t len)
{
	int odd;
	if (len < 2)
		return -1;
	memset(n, 0, sizeof(*n));
	odd = (in[0] >> 7) & 1;
	n->nature = in[0] & 0x7f;
	n->inn = (in[1] >> 7) & 1;
	n->npi = (in[1] >> 4) & 7;
	unpack_digits(in + 2, len - 2, odd, n->digits, sizeof(n->digits));
	return 0;
}

/* ---- Calling party number ---- */

int isup_enc_calling(const struct isup_number *n, uint8_t *out, size_t cap)
{
	int odd, ndig;
	if (cap < 2)
		return -1;
	ndig = pack_digits(n->digits, out + 2, cap - 2, &odd);
	if (ndig < 0)
		return -1;
	out[0] = (uint8_t)((odd << 7) | (n->nature & 0x7f));
	out[1] = (uint8_t)(((n->ni & 1) << 7) | ((n->npi & 7) << 4) |
			   ((n->apri & 3) << 2) | (n->screening & 3));
	return 2 + ndig;
}

int isup_dec_calling(struct isup_number *n, const uint8_t *in, size_t len)
{
	int odd;
	if (len < 2)
		return -1;
	memset(n, 0, sizeof(*n));
	odd = (in[0] >> 7) & 1;
	n->nature = in[0] & 0x7f;
	n->ni = (in[1] >> 7) & 1;
	n->npi = (in[1] >> 4) & 7;
	n->apri = (in[1] >> 2) & 3;
	n->screening = in[1] & 3;
	unpack_digits(in + 2, len - 2, odd, n->digits, sizeof(n->digits));
	return 0;
}

/* ---- Cause indicators (Q.850) ---- */

int isup_enc_cause(uint8_t coding_std, uint8_t location, uint8_t cause_value,
		   uint8_t *out, size_t cap)
{
	if (cap < 2)
		return -1;
	/* octet 1: ext=1, coding standard (2), spare (1), location (4) */
	out[0] = (uint8_t)(0x80 | ((coding_std & 3) << 5) | (location & 0x0f));
	/* octet 2: ext=1, cause value (7) */
	out[1] = (uint8_t)(0x80 | (cause_value & 0x7f));
	return 2;
}

int isup_dec_cause(const uint8_t *in, size_t len,
		   uint8_t *coding_std, uint8_t *location, uint8_t *cause_value)
{
	if (len < 2)
		return -1;
	if (coding_std)  *coding_std  = (in[0] >> 5) & 3;
	if (location)    *location    = in[0] & 0x0f;
	if (cause_value) *cause_value = in[1] & 0x7f;
	return 0;
}

/* ---- Range and status ---- */

int isup_enc_range_status(uint8_t range, const uint8_t *status, int with_status,
			  uint8_t *out, size_t cap)
{
	if (cap < 1)
		return -1;
	out[0] = range;
	if (!with_status)
		return 1;
	{
		/* (range + 1) status bits → ceil((range+1)/8) octets */
		size_t nb = ((size_t)range + 1 + 7) / 8;
		if (1 + nb > cap)
			return -1;
		if (status)
			memcpy(out + 1, status, nb);
		else
			memset(out + 1, 0, nb);
		return (int)(1 + nb);
	}
}

int isup_dec_range_status(const uint8_t *in, size_t len,
			  uint8_t *range, uint8_t *status, size_t status_cap,
			  int *has_status)
{
	if (len < 1)
		return -1;
	if (range)
		*range = in[0];
	if (len > 1) {
		size_t nb = len - 1;
		if (has_status)
			*has_status = 1;
		if (status && nb <= status_cap)
			memcpy(status, in + 1, nb);
	} else if (has_status) {
		*has_status = 0;
	}
	return 0;
}
