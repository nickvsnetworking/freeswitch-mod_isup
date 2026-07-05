/*
 * mod_isup — decoded-message tracer tests.
 */
#include <stdio.h>
#include <string.h>

#include "../isup_trace.h"
#include "../isup_codec.h"
#include "../isup_param.h"

static int g_tests, g_fail;
#define CHECK(cond, ...) do {                                   \
	g_tests++;                                               \
	if (!(cond)) { g_fail++;                                 \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
		printf(__VA_ARGS__); printf("\n"); }            \
} while (0)

static int has(const char *hay, const char *needle)
{
	return strstr(hay, needle) != NULL;
}

static void test_iam_trace(void)
{
	struct isup_msg m;
	char out[1024];
	uint8_t nci = 0, fci[2] = { 0x60, 0x10 }, cpc = ISUP_CPC_ORDINARY, tmr = ISUP_TMR_SPEECH;
	struct isup_number cd, cg;
	uint8_t b[32];
	int len;

	printf("test_iam_trace\n");
	isup_msg_init(&m, 77, ISUP_MT_IAM);
	isup_msg_add(&m, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(&m, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(&m, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(&m, ISUP_P_TMR, &tmr, 1);

	memset(&cd, 0, sizeof(cd));
	cd.nature = ISUP_NOA_INTERNATIONAL; cd.npi = ISUP_NPI_ISDN;
	strcpy(cd.digits, "61755500001");
	len = isup_enc_called(&cd, b, sizeof(b));
	isup_msg_add(&m, ISUP_P_CALLED_NUMBER, b, (uint8_t)len);

	memset(&cg, 0, sizeof(cg));
	cg.nature = ISUP_NOA_NATIONAL; cg.npi = ISUP_NPI_ISDN;
	cg.apri = ISUP_APRI_RESTRICTED;
	strcpy(cg.digits, "0738900000");
	len = isup_enc_calling(&cg, b, sizeof(b));
	isup_msg_add(&m, ISUP_P_CALLING_NUMBER, b, (uint8_t)len);

	isup_trace(&m, out, sizeof(out));

	CHECK(has(out, "IAM (0x01) CIC=77"), "header line");
	CHECK(has(out, "CalledPartyNumber"), "called param name");
	CHECK(has(out, "digits=61755500001"), "called digits");
	CHECK(has(out, "international"), "called noa text");
	CHECK(has(out, "CallingPartyNumber"), "calling param name");
	CHECK(has(out, "digits=0738900000"), "calling digits");
	CHECK(has(out, "restricted"), "CLIR shown");
	CHECK(has(out, "TransmissionMediumRequirement"), "tmr name");
	CHECK(has(out, "PCMA"), "tmr codec mapping");
}

static void test_rel_trace(void)
{
	struct isup_msg m;
	char out[512];
	uint8_t cause[2];
	int cl = isup_enc_cause(0, 1, 17, cause, sizeof(cause)); /* user busy */

	printf("test_rel_trace\n");
	isup_msg_init(&m, 9, ISUP_MT_REL);
	isup_msg_add(&m, ISUP_P_CAUSE, cause, (uint8_t)cl);
	isup_trace(&m, out, sizeof(out));

	CHECK(has(out, "REL (0x0c) CIC=9"), "rel header");
	CHECK(has(out, "value=17 (user busy)"), "cause decoded");
}

static void test_trace_truncation(void)
{
	struct isup_msg m;
	char small[16];
	int need;
	uint8_t cause[2];
	int cl = isup_enc_cause(0, 1, 16, cause, sizeof(cause));

	printf("test_trace_truncation\n");
	isup_msg_init(&m, 9, ISUP_MT_REL);
	isup_msg_add(&m, ISUP_P_CAUSE, cause, (uint8_t)cl);

	/* tiny buffer must not be overflowed (ASan) and must report the full
	 * would-be length so truncation is detectable. */
	need = isup_trace(&m, small, sizeof(small));
	CHECK(need > (int)sizeof(small), "reports truncation need=%d", need);
	CHECK(small[sizeof(small) - 1] == '\0' || strlen(small) < sizeof(small),
	      "NUL terminated");
}

int main(void)
{
	printf("== mod_isup tracer tests ==\n");
	test_iam_trace();
	test_rel_trace();
	test_trace_truncation();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
