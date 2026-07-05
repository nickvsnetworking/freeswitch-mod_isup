/*
 * mod_isup — Q.763 message codec.
 *
 * Implements the ITU-T Q.763 §1 message structure generically:
 *
 *   [CIC:2][MT:1][ mandatory fixed params ][ pointer area ][ mandatory
 *    variable params ][ optional params (TLV) ][ end-of-optional:1 ]
 *
 * The pointer area holds one octet per mandatory-variable parameter plus,
 * if the message permits optional parameters, one "pointer to optional
 * part". Each pointer's value is the offset in octets from the pointer
 * itself to the length octet of the parameter it points at (0 in the
 * optional pointer means "no optional part present").
 *
 * A per-message descriptor table drives the layout, so adding a message
 * is a one-line table entry, and the framing logic is shared and
 * bounds-checked once.
 */
#include <string.h>
#include "isup_codec.h"

/* ------------------------------------------------------------------ */
/* Message descriptors                                                 */
/* ------------------------------------------------------------------ */

struct fixed_desc {
	uint8_t code; /* parameter code we store it under */
	uint8_t len;  /* fixed length in octets */
};

struct msg_desc {
	uint8_t           msg_type;
	struct fixed_desc fixed[5];
	uint8_t           n_fixed;
	uint8_t           var[2];   /* mandatory variable param codes, in order */
	uint8_t           n_var;
	uint8_t           has_optional;
};

#define F(c, l) { (c), (l) }

static const struct msg_desc msg_table[] = {
	{ ISUP_MT_IAM,
	  { F(ISUP_P_NATURE_OF_CONN, 1), F(ISUP_P_FWD_CALL_IND, 2),
	    F(ISUP_P_CALLING_CATEGORY, 1), F(ISUP_P_TMR, 1) }, 4,
	  { ISUP_P_CALLED_NUMBER }, 1, 1 },

	{ ISUP_MT_SAM,
	  { {0,0} }, 0,
	  { ISUP_P_SUBSEQUENT_NUMBER }, 1, 1 },

	{ ISUP_MT_INR,
	  { F(ISUP_P_INR_IND, 2) }, 1, { 0 }, 0, 1 },
	{ ISUP_MT_INF,
	  { F(ISUP_P_INF_IND, 2) }, 1, { 0 }, 0, 1 },

	{ ISUP_MT_COT,
	  { F(ISUP_P_CONTINUITY_IND, 1) }, 1, { 0 }, 0, 0 },

	{ ISUP_MT_ACM,
	  { F(ISUP_P_BWD_CALL_IND, 2) }, 1, { 0 }, 0, 1 },
	{ ISUP_MT_CON,
	  { F(ISUP_P_BWD_CALL_IND, 2) }, 1, { 0 }, 0, 1 },

	{ ISUP_MT_ANM, { {0,0} }, 0, { 0 }, 0, 1 },
	{ ISUP_MT_FOT, { {0,0} }, 0, { 0 }, 0, 1 },

	{ ISUP_MT_CPG,
	  { F(ISUP_P_EVENT_INFO, 1) }, 1, { 0 }, 0, 1 },

	{ ISUP_MT_REL,
	  { {0,0} }, 0, { ISUP_P_CAUSE }, 1, 1 },
	{ ISUP_MT_RLC, { {0,0} }, 0, { 0 }, 0, 1 },

	{ ISUP_MT_SUS,
	  { F(ISUP_P_SUSP_RESUME_IND, 1) }, 1, { 0 }, 0, 1 },
	{ ISUP_MT_RES,
	  { F(ISUP_P_SUSP_RESUME_IND, 1) }, 1, { 0 }, 0, 1 },

	/* Circuit maintenance — CIC only, no parameters */
	{ ISUP_MT_CCR, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_RSC, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_BLO, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_UBL, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_BLA, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_UBA, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_LPA, { {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_UCIC,{ {0,0} }, 0, { 0 }, 0, 0 },
	{ ISUP_MT_UPT, { {0,0} }, 0, { 0 }, 0, 1 },
	{ ISUP_MT_UPA, { {0,0} }, 0, { 0 }, 0, 1 },

	/* Circuit group — range and status mandatory variable */
	{ ISUP_MT_GRS, { {0,0} }, 0, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_GRA, { {0,0} }, 0, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_CQM, { {0,0} }, 0, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_CQR, { {0,0} }, 0,
	  { ISUP_P_RANGE_AND_STATUS, ISUP_P_CIRC_STATE_IND }, 2, 0 },
	{ ISUP_MT_CGB,
	  { F(ISUP_P_CIRC_GRP_SV_TYPE, 1) }, 1, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_CGU,
	  { F(ISUP_P_CIRC_GRP_SV_TYPE, 1) }, 1, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_CGBA,
	  { F(ISUP_P_CIRC_GRP_SV_TYPE, 1) }, 1, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },
	{ ISUP_MT_CGUA,
	  { F(ISUP_P_CIRC_GRP_SV_TYPE, 1) }, 1, { ISUP_P_RANGE_AND_STATUS }, 1, 0 },

	{ ISUP_MT_CFN,  { {0,0} }, 0, { ISUP_P_CAUSE }, 1, 1 },
	{ ISUP_MT_USR,  { {0,0} }, 0, { 0 }, 0, 1 },
	{ ISUP_MT_CRG,  { {0,0} }, 0, { 0 }, 0, 1 },
	{ ISUP_MT_SGM,  { {0,0} }, 0, { 0 }, 0, 1 }, /* all optional params */
};

static const struct msg_desc *find_desc(uint8_t mt)
{
	size_t i;
	for (i = 0; i < sizeof(msg_table) / sizeof(msg_table[0]); i++)
		if (msg_table[i].msg_type == mt)
			return &msg_table[i];
	return NULL;
}

/* ------------------------------------------------------------------ */
/* Message table helpers                                               */
/* ------------------------------------------------------------------ */

void isup_msg_init(struct isup_msg *m, uint16_t cic, uint8_t msg_type)
{
	memset(m, 0, sizeof(*m));
	m->cic = cic;
	m->msg_type = msg_type;
}

/* The value buffer must accommodate any value a single length octet can
 * express, so isup_msg_add() can never overflow it regardless of input. */
_Static_assert(ISUP_MAX_PARAM_LEN >= 255, "param value buffer must hold 255 octets");

int isup_msg_add(struct isup_msg *m, uint8_t code, const uint8_t *val, uint8_t len)
{
	struct isup_param *p;
	int i;

	/* replace if already present */
	for (i = 0; i < m->n_params; i++) {
		if (m->params[i].code == code) {
			p = &m->params[i];
			goto store;
		}
	}
	if (m->n_params >= ISUP_MAX_PARAMS)
		return ISUP_ERR_TOOMANY;
	p = &m->params[m->n_params++];

store:
	p->code = code;
	p->len = len;
	if (len && val)
		memcpy(p->val, val, len);
	return ISUP_OK;
}

const struct isup_param *isup_msg_get(const struct isup_msg *m, uint8_t code)
{
	int i;
	for (i = 0; i < m->n_params; i++)
		if (m->params[i].code == code)
			return &m->params[i];
	return NULL;
}

int isup_is_mandatory_param(uint8_t mt, uint8_t code)
{
	const struct msg_desc *d = find_desc(mt);
	int i;
	if (!d)
		return 0;
	for (i = 0; i < d->n_fixed; i++)
		if (d->fixed[i].code == code)
			return 1;
	for (i = 0; i < d->n_var; i++)
		if (d->var[i] == code)
			return 1;
	return 0;
}

/* ------------------------------------------------------------------ */
/* Encode                                                              */
/* ------------------------------------------------------------------ */

int isup_encode(const struct isup_msg *m, uint8_t *buf, size_t buflen)
{
	const struct msg_desc *d = find_desc(m->msg_type);
	uint8_t used[ISUP_MAX_PARAMS] = { 0 };
	size_t pos = 0;
	int ptr_base, opt_ptr_pos = -1;
	int i, j;

	if (!d)
		return ISUP_ERR_UNKNOWN_MT;

	/* CIC (12 bits, little-endian low octet first) + message type */
	if (buflen < 3)
		return ISUP_ERR_NOSPACE;
	buf[pos++] = (uint8_t)(m->cic & 0xff);
	buf[pos++] = (uint8_t)((m->cic >> 8) & 0x0f);
	buf[pos++] = m->msg_type;

	/* mandatory fixed part */
	for (i = 0; i < d->n_fixed; i++) {
		const struct isup_param *p = isup_msg_get(m, d->fixed[i].code);
		if (!p)
			return ISUP_ERR_MISSING;
		if (p->len != d->fixed[i].len)
			return ISUP_ERR_BADLEN;
		if (pos + p->len > buflen)
			return ISUP_ERR_NOSPACE;
		memcpy(buf + pos, p->val, p->len);
		pos += p->len;
		for (j = 0; j < m->n_params; j++)
			if (&m->params[j] == p)
				used[j] = 1;
	}

	/* pointer area: one per variable param, plus optional pointer */
	ptr_base = (int)pos;
	{
		int n_ptr = d->n_var + (d->has_optional ? 1 : 0);
		if (pos + (size_t)n_ptr > buflen)
			return ISUP_ERR_NOSPACE;
		memset(buf + pos, 0, n_ptr);
		pos += n_ptr;
		if (d->has_optional)
			opt_ptr_pos = ptr_base + d->n_var;
	}

	/* mandatory variable part */
	for (i = 0; i < d->n_var; i++) {
		const struct isup_param *p = isup_msg_get(m, d->var[i]);
		int ptr_pos = ptr_base + i;
		if (!p)
			return ISUP_ERR_MISSING;
		if (pos + 1 + p->len > buflen)
			return ISUP_ERR_NOSPACE;
		buf[ptr_pos] = (uint8_t)(pos - ptr_pos);
		buf[pos++] = p->len;
		memcpy(buf + pos, p->val, p->len);
		pos += p->len;
		for (j = 0; j < m->n_params; j++)
			if (&m->params[j] == p)
				used[j] = 1;
	}

	/* optional part */
	if (d->has_optional) {
		int any = 0;
		for (j = 0; j < m->n_params; j++)
			if (!used[j]) { any = 1; break; }

		if (!any) {
			buf[opt_ptr_pos] = 0; /* no optional part */
		} else {
			buf[opt_ptr_pos] = (uint8_t)(pos - opt_ptr_pos);
			for (j = 0; j < m->n_params; j++) {
				const struct isup_param *p = &m->params[j];
				if (used[j])
					continue;
				if (pos + 2 + p->len > buflen)
					return ISUP_ERR_NOSPACE;
				buf[pos++] = p->code;
				buf[pos++] = p->len;
				memcpy(buf + pos, p->val, p->len);
				pos += p->len;
			}
			if (pos + 1 > buflen)
				return ISUP_ERR_NOSPACE;
			buf[pos++] = ISUP_P_END_OF_OPT;
		}
	}

	return (int)pos;
}

/* ------------------------------------------------------------------ */
/* Decode                                                              */
/* ------------------------------------------------------------------ */

int isup_decode(struct isup_msg *m, const uint8_t *buf, size_t buflen)
{
	const struct msg_desc *d;
	size_t pos = 0;
	int ptr_base, i, n_ptr;

	if (buflen < 3)
		return ISUP_ERR_TRUNCATED;

	memset(m, 0, sizeof(*m));
	m->cic = (uint16_t)(buf[0] | ((buf[1] & 0x0f) << 8));
	m->msg_type = buf[2];
	pos = 3;

	d = find_desc(m->msg_type);
	if (!d)
		return ISUP_ERR_UNKNOWN_MT;

	/* mandatory fixed part */
	for (i = 0; i < d->n_fixed; i++) {
		uint8_t len = d->fixed[i].len;
		if (pos + len > buflen)
			return ISUP_ERR_TRUNCATED;
		if (isup_msg_add(m, d->fixed[i].code, buf + pos, len) != ISUP_OK)
			return ISUP_ERR_TOOMANY;
		pos += len;
	}

	/* pointer area */
	ptr_base = (int)pos;
	n_ptr = d->n_var + (d->has_optional ? 1 : 0);
	if (pos + (size_t)n_ptr > buflen)
		return ISUP_ERR_TRUNCATED;
	pos += n_ptr;

	/* mandatory variable part — located via pointers */
	for (i = 0; i < d->n_var; i++) {
		int ptr_pos = ptr_base + i;
		uint8_t ptr = buf[ptr_pos];
		size_t loc, len;
		if (ptr == 0)
			return ISUP_ERR_BADPTR; /* mandatory var must be present */
		loc = (size_t)ptr_pos + ptr;
		if (loc >= buflen)
			return ISUP_ERR_BADPTR;
		len = buf[loc];
		if (loc + 1 + len > buflen)
			return ISUP_ERR_TRUNCATED;
		if (len > ISUP_MAX_PARAM_LEN)
			return ISUP_ERR_BADLEN;
		if (isup_msg_add(m, d->var[i], buf + loc + 1, (uint8_t)len) != ISUP_OK)
			return ISUP_ERR_TOOMANY;
	}

	/* optional part */
	if (d->has_optional) {
		uint8_t optr = buf[ptr_base + d->n_var];
		if (optr != 0) {
			size_t loc = (size_t)(ptr_base + d->n_var) + optr;
			while (loc < buflen) {
				uint8_t code = buf[loc];
				uint8_t len;
				if (code == ISUP_P_END_OF_OPT)
					break;
				if (loc + 2 > buflen)
					return ISUP_ERR_TRUNCATED;
				len = buf[loc + 1];
				if (loc + 2 + len > buflen)
					return ISUP_ERR_TRUNCATED;
				if (isup_msg_add(m, code, buf + loc + 2, len) != ISUP_OK)
					return ISUP_ERR_TOOMANY;
				loc += 2 + len;
			}
		}
	}

	return ISUP_OK;
}

/* ------------------------------------------------------------------ */
/* Names (tracing)                                                     */
/* ------------------------------------------------------------------ */

const char *isup_msg_type_name(uint8_t mt)
{
	switch (mt) {
	case ISUP_MT_IAM:  return "IAM";
	case ISUP_MT_SAM:  return "SAM";
	case ISUP_MT_INR:  return "INR";
	case ISUP_MT_INF:  return "INF";
	case ISUP_MT_COT:  return "COT";
	case ISUP_MT_ACM:  return "ACM";
	case ISUP_MT_CON:  return "CON";
	case ISUP_MT_FOT:  return "FOT";
	case ISUP_MT_ANM:  return "ANM";
	case ISUP_MT_REL:  return "REL";
	case ISUP_MT_SUS:  return "SUS";
	case ISUP_MT_RES:  return "RES";
	case ISUP_MT_RLC:  return "RLC";
	case ISUP_MT_CCR:  return "CCR";
	case ISUP_MT_RSC:  return "RSC";
	case ISUP_MT_BLO:  return "BLO";
	case ISUP_MT_UBL:  return "UBL";
	case ISUP_MT_BLA:  return "BLA";
	case ISUP_MT_UBA:  return "UBA";
	case ISUP_MT_GRS:  return "GRS";
	case ISUP_MT_CGB:  return "CGB";
	case ISUP_MT_CGU:  return "CGU";
	case ISUP_MT_CGBA: return "CGBA";
	case ISUP_MT_CGUA: return "CGUA";
	case ISUP_MT_LPA:  return "LPA";
	case ISUP_MT_PAM:  return "PAM";
	case ISUP_MT_GRA:  return "GRA";
	case ISUP_MT_CQM:  return "CQM";
	case ISUP_MT_CQR:  return "CQR";
	case ISUP_MT_CPG:  return "CPG";
	case ISUP_MT_USR:  return "USR";
	case ISUP_MT_UCIC: return "UCIC";
	case ISUP_MT_CFN:  return "CFN";
	case ISUP_MT_OLM:  return "OLM";
	case ISUP_MT_CRG:  return "CRG";
	case ISUP_MT_NRM:  return "NRM";
	case ISUP_MT_FAC:  return "FAC";
	case ISUP_MT_UPT:  return "UPT";
	case ISUP_MT_UPA:  return "UPA";
	case ISUP_MT_IDR:  return "IDR";
	case ISUP_MT_IRS:  return "IRS";
	case ISUP_MT_SGM:  return "SGM";
	default:           return "UNKNOWN";
	}
}

const char *isup_param_name(uint8_t code)
{
	switch (code) {
	case ISUP_P_TMR:                return "TransmissionMediumRequirement";
	case ISUP_P_ACCESS_TRANSPORT:   return "AccessTransport";
	case ISUP_P_CALLED_NUMBER:      return "CalledPartyNumber";
	case ISUP_P_SUBSEQUENT_NUMBER:  return "SubsequentNumber";
	case ISUP_P_NATURE_OF_CONN:     return "NatureOfConnectionIndicators";
	case ISUP_P_FWD_CALL_IND:       return "ForwardCallIndicators";
	case ISUP_P_OPT_FWD_CALL_IND:   return "OptionalForwardCallIndicators";
	case ISUP_P_CALLING_CATEGORY:   return "CallingPartysCategory";
	case ISUP_P_CALLING_NUMBER:     return "CallingPartyNumber";
	case ISUP_P_REDIRECTING_NUMBER: return "RedirectingNumber";
	case ISUP_P_REDIRECTION_NUMBER: return "RedirectionNumber";
	case ISUP_P_CONTINUITY_IND:     return "ContinuityIndicators";
	case ISUP_P_BWD_CALL_IND:       return "BackwardCallIndicators";
	case ISUP_P_CAUSE:              return "CauseIndicators";
	case ISUP_P_REDIRECTION_INFO:   return "RedirectionInformation";
	case ISUP_P_CIRC_GRP_SV_TYPE:   return "CircuitGroupSupervisionType";
	case ISUP_P_RANGE_AND_STATUS:   return "RangeAndStatus";
	case ISUP_P_USER_TO_USER_INFO:  return "UserToUserInformation";
	case ISUP_P_CONNECTED_NUMBER:   return "ConnectedNumber";
	case ISUP_P_SUSP_RESUME_IND:    return "SuspendResumeIndicators";
	case ISUP_P_EVENT_INFO:         return "EventInformation";
	case ISUP_P_ORIG_CALLED_NUMBER: return "OriginalCalledNumber";
	case ISUP_P_OPT_BWD_CALL_IND:   return "OptionalBackwardCallIndicators";
	case ISUP_P_GENERIC_NOTIF:      return "GenericNotificationIndicator";
	case ISUP_P_HOP_COUNTER:        return "HopCounter";
	case ISUP_P_GENERIC_NUMBER:     return "GenericNumber";
	case ISUP_P_GENERIC_DIGITS:     return "GenericDigits";
	default:                        return "Param";
	}
}

const char *isup_strerror(int rc)
{
	switch (rc) {
	case ISUP_OK:             return "ok";
	case ISUP_ERR_TRUNCATED:  return "truncated";
	case ISUP_ERR_UNKNOWN_MT: return "unknown message type";
	case ISUP_ERR_MISSING:    return "missing mandatory parameter";
	case ISUP_ERR_BADLEN:     return "bad parameter length";
	case ISUP_ERR_TOOMANY:    return "too many parameters";
	case ISUP_ERR_NOSPACE:    return "output buffer too small";
	case ISUP_ERR_BADPTR:     return "bad parameter pointer";
	default:                  return "unknown error";
	}
}
