/*
 * mod_isup — segmentation (SGM) unit tests.
 */
#include <stdio.h>
#include <string.h>

#include "../isup_segment.h"
#include "../isup_codec.h"
#include "../isup_param.h"

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

/* count occurrences of a param code across base+sgm */
static int count_in(const struct isup_msg *a, const struct isup_msg *b, uint8_t code)
{
	int n = 0, i;
	for (i = 0; i < a->n_params; i++) if (a->params[i].code == code) n++;
	for (i = 0; i < b->n_params; i++) if (b->params[i].code == code) n++;
	return n;
}

static void add_blob(struct isup_msg *m, uint8_t code, uint8_t fill, uint8_t len)
{
	uint8_t buf[64];
	memset(buf, fill, len);
	isup_msg_add(m, code, buf, len);
}

static void test_fits_no_split(void)
{
	struct isup_msg full, base, sgm;
	int need = -1, rc;
	uint8_t nci = 0, fci[2] = {0,0}, cpc = 0x0a, tmr = 0, cd[4] = {0x83,0x10,0x21,0x03};

	printf("test_fits_no_split\n");
	isup_msg_init(&full, 1, ISUP_MT_IAM);
	isup_msg_add(&full, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(&full, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(&full, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(&full, ISUP_P_TMR, &tmr, 1);
	isup_msg_add(&full, ISUP_P_CALLED_NUMBER, cd, 4);

	rc = isup_segment(&full, 272, &base, &sgm, &need);
	CHECK(rc == ISUP_OK, "rc=%s", isup_strerror(rc));
	CHECK(need == 0, "need_sgm=%d (want 0)", need);
}

static void test_split_and_reassemble(void)
{
	struct isup_msg full, base, sgm, reasm;
	int need = -1, rc, blen, slen;
	uint8_t nci = 0, fci[2] = {0x60,0x10}, cpc = 0x0a, tmr = 0, cd[4] = {0x83,0x10,0x21,0x03};
	const uint8_t opt_codes[] = {
		ISUP_P_CALLING_NUMBER, ISUP_P_GENERIC_NUMBER,
		ISUP_P_REDIRECTING_NUMBER, ISUP_P_USER_TO_USER_INFO,
		ISUP_P_ORIG_CALLED_NUMBER,
	};
	int i;

	printf("test_split_and_reassemble\n");

	isup_msg_init(&full, 4095, ISUP_MT_IAM);
	isup_msg_add(&full, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(&full, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(&full, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(&full, ISUP_P_TMR, &tmr, 1);
	isup_msg_add(&full, ISUP_P_CALLED_NUMBER, cd, 4);
	/* large optional set that won't fit in 50 octets */
	add_blob(&full, ISUP_P_CALLING_NUMBER, 0xa1, 12);
	add_blob(&full, ISUP_P_GENERIC_NUMBER, 0xb2, 20);
	add_blob(&full, ISUP_P_REDIRECTING_NUMBER, 0xc3, 12);
	add_blob(&full, ISUP_P_USER_TO_USER_INFO, 0xd4, 30);
	add_blob(&full, ISUP_P_ORIG_CALLED_NUMBER, 0xe5, 12);

	rc = isup_segment(&full, 50, &base, &sgm, &need);
	CHECK(rc == ISUP_OK, "segment rc=%s", isup_strerror(rc));
	CHECK(need == 1, "need_sgm=%d (want 1)", need);

	/* base must actually fit, sgm too */
	{
		uint8_t buf[ISUP_MAX_MSG_LEN];
		blen = isup_encode(&base, buf, sizeof(buf));
		slen = isup_encode(&sgm, buf, sizeof(buf));
	}
	CHECK(blen > 0 && blen <= 50, "base len=%d (want <=50)", blen);
	CHECK(slen > 0, "sgm len=%d", slen);

	/* segmentation indicator set in base */
	{
		const struct isup_param *p = isup_msg_get(&base, ISUP_P_OPT_FWD_CALL_IND);
		CHECK(p && (p->val[0] & ISUP_SEG_IND_BIT), "seg indicator set");
	}

	/* every original optional parameter appears exactly once across base+sgm */
	for (i = 0; i < (int)sizeof(opt_codes); i++)
		CHECK(count_in(&base, &sgm, opt_codes[i]) == 1,
		      "param 0x%02x count=%d (want 1)", opt_codes[i],
		      count_in(&base, &sgm, opt_codes[i]));

	/* mandatory parts stay in base */
	CHECK(isup_msg_get(&base, ISUP_P_CALLED_NUMBER) != NULL, "CdPN in base");
	CHECK(isup_msg_get(&base, ISUP_P_NATURE_OF_CONN) != NULL, "NCI in base");

	/* reassemble: base + sgm => original parameter set, seg bit cleared */
	reasm = base;
	rc = isup_reassemble(&reasm, &sgm);
	CHECK(rc == ISUP_OK, "reassemble rc=%s", isup_strerror(rc));
	for (i = 0; i < full.n_params; i++) {
		const struct isup_param *orig = &full.params[i];
		const struct isup_param *got = isup_msg_get(&reasm, orig->code);
		CHECK(got != NULL, "reasm missing 0x%02x", orig->code);
		if (got)
			CHECK(got->len == orig->len &&
			      memcmp(got->val, orig->val, orig->len) == 0,
			      "reasm 0x%02x value mismatch", orig->code);
	}
	{
		const struct isup_param *p = isup_msg_get(&reasm, ISUP_P_OPT_FWD_CALL_IND);
		CHECK(!p || !(p->val[0] & ISUP_SEG_IND_BIT), "seg indicator cleared");
	}
}

int main(void)
{
	printf("== mod_isup segmentation tests ==\n");
	test_fits_no_split();
	test_split_and_reassemble();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
