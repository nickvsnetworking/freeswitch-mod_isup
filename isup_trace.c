/*
 * mod_isup — decoded-message tracer.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "isup_trace.h"
#include "isup_codec.h"
#include "isup_param.h"
#include "isup_map.h"

/* bounded appender: advances *off, never writes past cap */
static void app(char *out, size_t cap, size_t *off, const char *fmt, ...)
{
	va_list ap;
	int n;
	if (*off >= cap) {
		/* still account for would-be length so callers can detect trunc */
		va_start(ap, fmt);
		n = vsnprintf(NULL, 0, fmt, ap);
		va_end(ap);
		*off += (n > 0) ? (size_t)n : 0;
		return;
	}
	va_start(ap, fmt);
	n = vsnprintf(out + *off, cap - *off, fmt, ap);
	va_end(ap);
	if (n > 0)
		*off += (size_t)n;
}

static const char *noa_name(uint8_t n)
{
	switch (n) {
	case ISUP_NOA_SUBSCRIBER:    return "subscriber";
	case ISUP_NOA_NATIONAL:      return "national";
	case ISUP_NOA_INTERNATIONAL: return "international";
	case ISUP_NOA_NETWORK_SPECIFIC: return "network-specific";
	default:                     return "unknown";
	}
}

static void trace_number(char *o, size_t c, size_t *off, const struct isup_param *p, int calling)
{
	struct isup_number n;
	if (calling)
		isup_dec_calling(&n, p->val, p->len);
	else
		isup_dec_called(&n, p->val, p->len);
	app(o, c, off, "digits=%s noa=%s npi=%u", n.digits, noa_name(n.nature), n.npi);
	if (calling)
		app(o, c, off, " pres=%s scr=%u",
		    n.apri == ISUP_APRI_RESTRICTED ? "restricted" :
		    n.apri == ISUP_APRI_ALLOWED ? "allowed" : "n/a", n.screening);
}

int isup_trace(const struct isup_msg *m, char *out, size_t cap)
{
	size_t off = 0;
	int i;

	app(out, cap, &off, "%s (0x%02x) CIC=%u\n",
	    isup_msg_type_name(m->msg_type), m->msg_type, m->cic);

	for (i = 0; i < m->n_params; i++) {
		const struct isup_param *p = &m->params[i];
		app(out, cap, &off, "  %s (0x%02x) [%u]: ",
		    isup_param_name(p->code), p->code, p->len);

		switch (p->code) {
		case ISUP_P_CALLED_NUMBER:
			trace_number(out, cap, &off, p, 0);
			break;
		case ISUP_P_CALLING_NUMBER:
		case ISUP_P_REDIRECTING_NUMBER:
		case ISUP_P_CONNECTED_NUMBER:
			trace_number(out, cap, &off, p, 1);
			break;
		case ISUP_P_CAUSE:
			if (p->len >= 2) {
				uint8_t cs, loc, cv;
				isup_dec_cause(p->val, p->len, &cs, &loc, &cv);
				app(out, cap, &off, "value=%u (%s) location=%u",
				    cv, isup_cause_name(cv), loc);
			}
			break;
		case ISUP_P_TMR:
			if (p->len >= 1)
				app(out, cap, &off, "0x%02x -> %s", p->val[0],
				    isup_tmr_to_codec(p->val[0]));
			break;
		case ISUP_P_EVENT_INFO:
			if (p->len >= 1)
				app(out, cap, &off, "event=0x%02x -> SIP %d",
				    p->val[0], isup_event_to_sip(p->val[0] & 0x7f));
			break;
		case ISUP_P_RANGE_AND_STATUS:
			if (p->len >= 1)
				app(out, cap, &off, "range=%u status_octets=%u",
				    p->val[0], (unsigned)(p->len - 1));
			break;
		default: {
			int j;
			for (j = 0; j < p->len; j++)
				app(out, cap, &off, "%02x ", p->val[j]);
			break;
		}
		}
		app(out, cap, &off, "\n");
	}

	return (int)off;
}
