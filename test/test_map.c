/*
 * mod_isup — ISUP<->SIP/FS mapping unit tests.
 */
#include <stdio.h>
#include <string.h>

#include "../isup_map.h"

static int g_tests, g_fail;

#define CHECK(cond, ...) do {                                   \
	g_tests++;                                               \
	if (!(cond)) {                                           \
		g_fail++;                                        \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
		printf(__VA_ARGS__);                            \
		printf("\n");                                   \
	}                                                       \
} while (0)

static void test_tmr_codec(void)
{
	printf("test_tmr_codec\n");
	CHECK(strcmp(isup_tmr_to_codec(ISUP_TMR_SPEECH), "PCMA") == 0, "speech->PCMA");
	CHECK(strcmp(isup_tmr_to_codec(ISUP_TMR_64K_UNRESTRICTED), "CLEARMODE") == 0, "64k->CLEARMODE");
	CHECK(isup_codec_to_tmr("CLEARMODE") == ISUP_TMR_64K_UNRESTRICTED, "CLEARMODE->64k");
	CHECK(isup_codec_to_tmr("PCMU") == ISUP_TMR_SPEECH, "default speech");
}

static void test_events(void)
{
	printf("test_events\n");
	CHECK(isup_event_to_sip(ISUP_EVENT_ALERTING) == 180, "alerting->180");
	CHECK(isup_event_to_sip(ISUP_EVENT_PROGRESS) == 183, "progress->183");
	CHECK(isup_sip_to_event(180) == ISUP_EVENT_ALERTING, "180->alerting");
	CHECK(isup_sip_to_event(183) == ISUP_EVENT_PROGRESS, "183->progress");
	CHECK(isup_event_to_sip(0x7f) == 0, "unknown event");
}

static void test_numbers(void)
{
	struct isup_number n;
	char buf[40];
	const char *ton;

	printf("test_numbers\n");

	memset(&n, 0, sizeof(n));
	n.nature = ISUP_NOA_INTERNATIONAL;
	strcpy(n.digits, "61755500001");
	ton = isup_number_to_e164(&n, buf, sizeof(buf));
	CHECK(strcmp(buf, "+61755500001") == 0, "intl '%s'", buf);
	CHECK(strcmp(ton, "international") == 0, "intl ton '%s'", ton);

	n.nature = ISUP_NOA_NATIONAL;
	strcpy(n.digits, "0738900000");
	ton = isup_number_to_e164(&n, buf, sizeof(buf));
	CHECK(strcmp(buf, "0738900000") == 0, "natl '%s'", buf);
	CHECK(strcmp(ton, "national") == 0, "natl ton '%s'", ton);

	n.apri = ISUP_APRI_RESTRICTED;
	CHECK(isup_calling_is_clir(&n) == 1, "CLIR set");
	n.apri = ISUP_APRI_ALLOWED;
	CHECK(isup_calling_is_clir(&n) == 0, "CLIR clear");
}

static void test_cause_names(void)
{
	printf("test_cause_names\n");
	CHECK(strcmp(isup_cause_name(16), "normal clearing") == 0, "cause 16");
	CHECK(strcmp(isup_cause_name(17), "user busy") == 0, "cause 17");
	CHECK(strcmp(isup_cause_name(102), "recovery on timer expiry") == 0, "cause 102");
}

int main(void)
{
	printf("== mod_isup mapping tests ==\n");
	test_tmr_codec();
	test_events();
	test_numbers();
	test_cause_names();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
