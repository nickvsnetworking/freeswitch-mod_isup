/*
 * mod_isup — hardening / regression tests.
 *
 * Locks in the boundary conditions and the specific memory-safety bugs found
 * during the bulletproofing pass, so they cannot silently regress.
 */
#include <stdio.h>
#include <string.h>

#include "../isup_codec.h"
#include "../isup_param.h"
#include "../isup_cgm.h"

static int g_tests, g_fail;
#define CHECK(cond, ...) do {                                   \
	g_tests++;                                               \
	if (!(cond)) { g_fail++;                                 \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
		printf(__VA_ARGS__); printf("\n"); }            \
} while (0)

/* A 255-octet optional parameter (max a length octet can express) must
 * round-trip intact. */
static void test_max_param_len(void)
{
	struct isup_msg m, d;
	uint8_t big[255], buf[512];
	const struct isup_param *p;
	int n, i;

	printf("test_max_param_len\n");
	for (i = 0; i < 255; i++) big[i] = (uint8_t)(i ^ 0x5a);
	isup_msg_init(&m, 100, ISUP_MT_RLC);
	CHECK(isup_msg_add(&m, ISUP_P_ACCESS_TRANSPORT, big, 255) == ISUP_OK, "add 255");
	n = isup_encode(&m, buf, sizeof(buf));
	CHECK(n > 0, "encode 255-param %s", isup_strerror(n));
	CHECK(isup_decode(&d, buf, n) == ISUP_OK, "decode 255-param");
	p = isup_msg_get(&d, ISUP_P_ACCESS_TRANSPORT);
	CHECK(p && p->len == 255 && memcmp(p->val, big, 255) == 0, "255-param intact");
}

/* Maximum CIC value (12 bits = 4095) survives the 2-octet encoding. */
static void test_max_cic(void)
{
	struct isup_msg m, d;
	uint8_t buf[16];
	int n;
	printf("test_max_cic\n");
	isup_msg_init(&m, 4095, ISUP_MT_RLC);
	n = isup_encode(&m, buf, sizeof(buf));
	CHECK(n > 0 && isup_decode(&d, buf, n) == ISUP_OK, "rlc enc/dec");
	CHECK(d.cic == 4095, "cic=%u (want 4095)", d.cic);
	/* the upper 4 bits of octet 2 must be spare/zero */
	CHECK((buf[1] & 0xf0) == 0, "cic high nibble spare");
}

/* The parameter table is bounded: the 33rd parameter is rejected, not
 * overflowed. */
static void test_param_table_full(void)
{
	struct isup_msg m;
	uint8_t v = 0;
	int i, rc = ISUP_OK;
	printf("test_param_table_full\n");
	isup_msg_init(&m, 1, ISUP_MT_IAM);
	for (i = 0; i < ISUP_MAX_PARAMS; i++)
		rc = isup_msg_add(&m, (uint8_t)(0x40 + i), &v, 1);
	CHECK(rc == ISUP_OK, "filled %d params", ISUP_MAX_PARAMS);
	rc = isup_msg_add(&m, 0x9a, &v, 1); /* one too many, new code */
	CHECK(rc == ISUP_ERR_TOOMANY, "33rd rejected rc=%d", rc);
	CHECK(m.n_params == ISUP_MAX_PARAMS, "n_params=%d", m.n_params);
}

/* REGRESSION: CGB advertising a huge range but carrying a short status mask
 * must not read past the parameter (was a 31-byte over-read). */
static void test_cgm_short_mask(void)
{
	struct isup_cgm g;
	struct isup_msg cgb;
	uint8_t sv = 0;
	uint8_t rs[2] = { 0xff, 0xff }; /* range=255, only ONE status octet */
	int i, any_blocked = 0;
	/* emit deliberately NULL: a correct guard must reject the malformed
	 * message *before* attempting to emit, so this NULL is never called. */
	struct isup_cgm_ops ops = { .emit = NULL };

	printf("test_cgm_short_mask\n");
	isup_cgm_init(&g, 1, 64, &ops, NULL);
	isup_msg_init(&cgb, 1, ISUP_MT_CGB);
	isup_msg_add(&cgb, ISUP_P_CIRC_GRP_SV_TYPE, &sv, 1);
	isup_msg_add(&cgb, ISUP_P_RANGE_AND_STATUS, rs, 2);

	CHECK(isup_cgm_rx(&g, &cgb) == 1, "malformed CGB recognised");
	for (i = 0; i < 64; i++)
		if (g.c[i].rem_blocked) any_blocked = 1;
	CHECK(!any_blocked, "no circuits blocked from short mask");
}

/* isup_dec_range_status must not write past a small status buffer even when
 * the parameter is long. */
static void test_range_status_overflow_guard(void)
{
	uint8_t in[255];
	uint8_t st[8];
	uint8_t range = 0;
	int has = -1, rc, i;
	printf("test_range_status_overflow_guard\n");
	for (i = 0; i < 255; i++) in[i] = 0xff;
	in[0] = 200; /* big range */
	rc = isup_dec_range_status(in, sizeof(in), &range, st, sizeof(st), &has);
	CHECK(rc == 0, "decode returns ok");
	CHECK(range == 200, "range decoded");
	CHECK(has == 1, "status present flag");
	/* st was too small (8 < 254) so the copy must have been skipped, not
	 * overflowed — ASan verifies the absence of OOB write. */
}

/* A decoder pointer that aims past the end of the buffer is handled, not
 * dereferenced. */
static void test_optptr_out_of_range(void)
{
	struct isup_msg d;
	/* RLC: cic=1, mt=0x10, optional pointer = 200 (way past end) */
	uint8_t buf[] = { 0x01, 0x00, 0x10, 200 };
	int rc;
	printf("test_optptr_out_of_range\n");
	rc = isup_decode(&d, buf, sizeof(buf));
	CHECK(rc == ISUP_OK, "decode ok (no optional found) rc=%s", isup_strerror(rc));
	CHECK(d.n_params == 0, "no params from bogus pointer");
}

int main(void)
{
	printf("== mod_isup hardening / regression tests ==\n");
	test_max_param_len();
	test_max_cic();
	test_param_table_full();
	test_cgm_short_mask();
	test_range_status_overflow_guard();
	test_optptr_out_of_range();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
