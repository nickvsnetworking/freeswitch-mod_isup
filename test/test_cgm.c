/*
 * mod_isup — circuit group / maintenance management tests.
 *
 * Each end records the messages it emits; messages are delivered to the peer
 * via the real codec (encode then decode) so range-and-status framing is
 * exercised on the wire, not just in memory.
 */
#include <stdio.h>
#include <string.h>

#include "../isup_cgm.h"
#include "../isup_codec.h"
#include "../isup_param.h"

static int g_tests, g_fail;
#define CHECK(cond, ...) do {                                   \
	g_tests++;                                               \
	if (!(cond)) { g_fail++;                                 \
		printf("  FAIL %s:%d: ", __FILE__, __LINE__);   \
		printf(__VA_ARGS__); printf("\n"); }            \
} while (0)

struct rec { struct isup_msg m[16]; int n; };
static void rec_emit(void *u, const struct isup_msg *msg)
{
	struct rec *r = u;
	if (r->n < 16)
		r->m[r->n++] = *msg;
}
static const struct isup_cgm_ops ROPS = { .emit = rec_emit };

/* deliver a message to a cgm through the codec (encode -> decode) */
static int deliver(struct isup_cgm *dst, const struct isup_msg *m)
{
	uint8_t buf[ISUP_MAX_MSG_LEN];
	struct isup_msg d;
	int n = isup_encode(m, buf, sizeof(buf));
	if (n <= 0)
		return -1;
	if (isup_decode(&d, buf, n) != ISUP_OK)
		return -1;
	return isup_cgm_rx(dst, &d);
}

static void test_single_block(void)
{
	struct isup_cgm A, B;
	struct rec ra = {0}, rb = {0};

	printf("test_single_block\n");
	isup_cgm_init(&A, 1, 32, &ROPS, &ra);
	isup_cgm_init(&B, 1, 32, &ROPS, &rb);

	/* A blocks CIC 5 */
	isup_cgm_block(&A, 5);
	CHECK(ra.n == 1 && ra.m[0].msg_type == ISUP_MT_BLO && ra.m[0].cic == 5, "A emits BLO/5");
	CHECK(A.c[4].loc_blocked && A.c[4].loc_pending, "A cic5 loc blocked+pending");

	/* B receives BLO, returns BLA */
	deliver(&B, &ra.m[0]);
	CHECK(B.c[4].rem_blocked, "B cic5 rem blocked");
	CHECK(rb.n == 1 && rb.m[0].msg_type == ISUP_MT_BLA && rb.m[0].cic == 5, "B emits BLA/5");
	CHECK(isup_cgm_is_blocked(&B, 5), "B reports cic5 blocked");

	/* A receives BLA, clears pending */
	deliver(&A, &rb.m[0]);
	CHECK(A.c[4].loc_blocked && !A.c[4].loc_pending, "A cic5 blocked, ack'd");

	/* now unblock */
	ra.n = rb.n = 0;
	isup_cgm_unblock(&A, 5);
	deliver(&B, &ra.m[0]);
	CHECK(!B.c[4].rem_blocked, "B cic5 unblocked");
	CHECK(rb.m[0].msg_type == ISUP_MT_UBA, "B emits UBA");
	deliver(&A, &rb.m[0]);
	CHECK(!A.c[4].loc_blocked && !A.c[4].loc_pending, "A cic5 fully unblocked");
}

static void test_group_block(void)
{
	struct isup_cgm A, B;
	struct rec ra = {0}, rb = {0};
	uint8_t mask[1] = { 0x05 }; /* circuits 0 and 2 in the range */

	printf("test_group_block\n");
	isup_cgm_init(&A, 1, 32, &ROPS, &ra);
	isup_cgm_init(&B, 1, 32, &ROPS, &rb);

	/* A group-blocks starting at CIC 1, range 5, mask {1, 3} */
	isup_cgm_group_block(&A, 1, 5, mask);
	CHECK(ra.m[0].msg_type == ISUP_MT_CGB, "A emits CGB");
	CHECK(A.c[0].loc_blocked && A.c[2].loc_blocked && !A.c[1].loc_blocked, "A masked block");

	deliver(&B, &ra.m[0]);
	CHECK(B.c[0].rem_blocked && B.c[2].rem_blocked && !B.c[1].rem_blocked, "B masked block");
	CHECK(rb.m[0].msg_type == ISUP_MT_CGBA, "B emits CGBA");

	deliver(&A, &rb.m[0]);
	CHECK(!A.c[0].loc_pending && !A.c[2].loc_pending, "A group block ack'd");
}

static void test_group_reset(void)
{
	struct isup_cgm A, B;
	struct rec ra = {0}, rb = {0};

	printf("test_group_reset\n");
	isup_cgm_init(&A, 1, 8, &ROPS, &ra);
	isup_cgm_init(&B, 1, 8, &ROPS, &rb);

	/* pre-block some circuits on B so reset has visible effect */
	B.c[2].rem_blocked = 1;
	B.c[5].loc_blocked = 1;

	isup_cgm_reset_all(&A);
	CHECK(ra.m[0].msg_type == ISUP_MT_GRS, "A emits GRS");
	CHECK(A.c[0].reset_pending && A.c[7].reset_pending, "A reset pending set");

	deliver(&B, &ra.m[0]);
	CHECK(!B.c[2].rem_blocked && !B.c[5].loc_blocked, "B circuits cleared by GRS");
	CHECK(rb.m[0].msg_type == ISUP_MT_GRA, "B emits GRA");

	deliver(&A, &rb.m[0]);
	CHECK(!A.c[0].reset_pending && !A.c[7].reset_pending, "A reset complete");
}

static void test_query(void)
{
	struct isup_cgm B;
	struct rec rb = {0};
	struct isup_msg cqm;
	uint8_t rs[1];
	const struct isup_param *csi;

	printf("test_query\n");
	isup_cgm_init(&B, 1, 8, &ROPS, &rb);
	B.c[1].rem_blocked = 1;   /* CIC 2 blocked */

	/* query CICs 1..4 (range 3) */
	isup_enc_range_status(3, NULL, 0, rs, sizeof(rs));
	isup_msg_init(&cqm, 1, ISUP_MT_CQM);
	isup_msg_add(&cqm, ISUP_P_RANGE_AND_STATUS, rs, 1);

	deliver(&B, &cqm);
	CHECK(rb.n == 1 && rb.m[0].msg_type == ISUP_MT_CQR, "B emits CQR");
	csi = isup_msg_get(&rb.m[0], ISUP_P_CIRC_STATE_IND);
	CHECK(csi && csi->len == 4, "CQR has 4 circuit-state octets");
	if (csi) {
		CHECK(csi->val[0] == 0x00, "CIC1 unblocked");
		CHECK(csi->val[1] == 0x03, "CIC2 blocked");
		CHECK(csi->val[2] == 0x00, "CIC3 unblocked");
	}
}

int main(void)
{
	printf("== mod_isup circuit-group management tests ==\n");
	test_single_block();
	test_group_block();
	test_group_reset();
	test_query();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
