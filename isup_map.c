/*
 * mod_isup — ISUP <-> SIP / FreeSWITCH interworking helpers.
 */
#include <string.h>
#include <strings.h>
#include <stdio.h>
#include "isup_map.h"

const char *isup_cause_name(uint8_t cause_value)
{
	switch (cause_value) {
	case 1:   return "unallocated number";
	case 16:  return "normal clearing";
	case 17:  return "user busy";
	case 18:  return "no user responding";
	case 19:  return "no answer from user";
	case 21:  return "call rejected";
	case 27:  return "destination out of order";
	case 28:  return "invalid number format";
	case 31:  return "normal unspecified";
	case 34:  return "no circuit available";
	case 38:  return "network out of order";
	case 41:  return "temporary failure";
	case 42:  return "switching equipment congestion";
	case 44:  return "requested circuit unavailable";
	case 102: return "recovery on timer expiry";
	case 111: return "protocol error";
	case 127: return "interworking unspecified";
	default:  return "cause";
	}
}

const char *isup_tmr_to_codec(uint8_t tmr)
{
	switch (tmr) {
	case ISUP_TMR_SPEECH:           return "PCMA";
	case ISUP_TMR_3K1_AUDIO:        return "PCMA";
	case ISUP_TMR_64K_UNRESTRICTED: return "CLEARMODE";
	case ISUP_TMR_64K_PREFERRED:    return "PCMA";
	default:                        return "PCMA";
	}
}

uint8_t isup_codec_to_tmr(const char *codec)
{
	if (codec && strcasecmp(codec, "CLEARMODE") == 0)
		return ISUP_TMR_64K_UNRESTRICTED;
	return ISUP_TMR_SPEECH;
}

int isup_event_to_sip(uint8_t event)
{
	switch (event) {
	case ISUP_EVENT_ALERTING:    return 180;
	case ISUP_EVENT_PROGRESS:    return 183;
	case ISUP_EVENT_INBAND_INFO: return 183;
	default:                     return 0;
	}
}

uint8_t isup_sip_to_event(int status)
{
	switch (status) {
	case 180: return ISUP_EVENT_ALERTING;
	case 183: return ISUP_EVENT_PROGRESS;
	default:  return 0;
	}
}

const char *isup_number_to_e164(const struct isup_number *n, char *out, size_t cap)
{
	if (!cap)
		return "unknown";
	if (n->nature == ISUP_NOA_INTERNATIONAL) {
		snprintf(out, cap, "+%s", n->digits);
		return "international";
	}
	snprintf(out, cap, "%s", n->digits);
	switch (n->nature) {
	case ISUP_NOA_SUBSCRIBER:       return "subscriber";
	case ISUP_NOA_NATIONAL:         return "national";
	case ISUP_NOA_NETWORK_SPECIFIC: return "network_specific";
	default:                        return "unknown";
	}
}

int isup_calling_is_clir(const struct isup_number *n)
{
	return n->apri == ISUP_APRI_RESTRICTED;
}
