/*
 * mod_isup — Q.763 codec unit tests.
 *
 * Build & run:  make -C mod_isup check
 *
 * Covers: exact-byte framing vector, encode/decode round-trips for the
 * core message set, parameter-body helpers, and decoder safety on
 * truncated / malformed input (run under ASan/UBSan).
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

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

static void hexdump(const char *tag, const uint8_t *b, int n)
{
	int i;
	printf("  %s [%d]:", tag, n);
	for (i = 0; i < n; i++)
		printf(" %02x", b[i]);
	printf("\n");
}

/* ---- helpers ---- */

static void add_bytes(struct isup_msg *m, uint8_t code, const uint8_t *v, uint8_t n)
{
	int rc = isup_msg_add(m, code, v, n);
	CHECK(rc == ISUP_OK, "add param 0x%02x rc=%d", code, rc);
}

/* ------------------------------------------------------------------ */
/* 1. Exact-byte IAM framing vector                                    */
/* ------------------------------------------------------------------ */
static void test_iam_exact(void)
{
	struct isup_msg m;
	uint8_t buf[64];
	int n;
	/* hand-computed expected wire bytes (see comment in test) */
	static const uint8_t expect[] = {
		0x01, 0x00,             /* CIC = 1 */
		0x01,                   /* MT = IAM */
		0x00,                   /* NCI */
		0x10, 0x00,             /* FCI */
		0x0a,                   /* CPC = ordinary */
		0x00,                   /* TMR = speech */
		0x02,                   /* ptr -> Called Party Number */
		0x00,                   /* ptr -> optional part (none) */
		0x04,                   /* CdPN length */
		0x83, 0x10, 0x21, 0x03  /* odd/NOA=nat, NPI=ISDN, "123" */
	};
	uint8_t nci = 0x00, fci[2] = { 0x10, 0x00 }, cpc = 0x0a, tmr = 0x00;
	struct isup_number cdpn;
	uint8_t cdpn_buf[32];
	int cdpn_len;

	printf("test_iam_exact\n");

	memset(&cdpn, 0, sizeof(cdpn));
	cdpn.nature = ISUP_NOA_NATIONAL;
	cdpn.npi = ISUP_NPI_ISDN;
	strcpy(cdpn.digits, "123");
	cdpn_len = isup_enc_called(&cdpn, cdpn_buf, sizeof(cdpn_buf));
	CHECK(cdpn_len == 4, "cdpn encode len=%d", cdpn_len);

	isup_msg_init(&m, 1, ISUP_MT_IAM);
	add_bytes(&m, ISUP_P_NATURE_OF_CONN, &nci, 1);
	add_bytes(&m, ISUP_P_FWD_CALL_IND, fci, 2);
	add_bytes(&m, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	add_bytes(&m, ISUP_P_TMR, &tmr, 1);
	add_bytes(&m, ISUP_P_CALLED_NUMBER, cdpn_buf, (uint8_t)cdpn_len);

	n = isup_encode(&m, buf, sizeof(buf));
	CHECK(n == (int)sizeof(expect), "encode len=%d want %d", n, (int)sizeof(expect));
	if (n == (int)sizeof(expect) && memcmp(buf, expect, n) != 0) {
		CHECK(0, "encoded bytes differ");
		hexdump("got ", buf, n);
		hexdump("want", expect, sizeof(expect));
	}
}

/* ------------------------------------------------------------------ */
/* 2. IAM round-trip with optional calling number + generic params     */
/* ------------------------------------------------------------------ */
static void test_iam_roundtrip(void)
{
	struct isup_msg m, m2;
	uint8_t buf[128];
	int n, rc;
	uint8_t nci = 0x01, fci[2] = { 0x60, 0x10 }, cpc = ISUP_CPC_ORDINARY, tmr = ISUP_TMR_SPEECH;
	struct isup_number cgpn, cdpn, dec;
	uint8_t b[32];
	int len;
	uint8_t hop = 15;
	const struct isup_param *p;

	printf("test_iam_roundtrip\n");

	isup_msg_init(&m, 1234, ISUP_MT_IAM);
	add_bytes(&m, ISUP_P_NATURE_OF_CONN, &nci, 1);
	add_bytes(&m, ISUP_P_FWD_CALL_IND, fci, 2);
	add_bytes(&m, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	add_bytes(&m, ISUP_P_TMR, &tmr, 1);

	memset(&cdpn, 0, sizeof(cdpn));
	cdpn.nature = ISUP_NOA_INTERNATIONAL; cdpn.npi = ISUP_NPI_ISDN;
	strcpy(cdpn.digits, "61755500001");
	len = isup_enc_called(&cdpn, b, sizeof(b));
	add_bytes(&m, ISUP_P_CALLED_NUMBER, b, (uint8_t)len);

	/* optional: calling number with CLIR */
	memset(&cgpn, 0, sizeof(cgpn));
	cgpn.nature = ISUP_NOA_NATIONAL; cgpn.npi = ISUP_NPI_ISDN;
	cgpn.apri = ISUP_APRI_RESTRICTED; cgpn.screening = ISUP_SCR_NETWORK_PROVIDED;
	strcpy(cgpn.digits, "0738900000");
	len = isup_enc_calling(&cgpn, b, sizeof(b));
	add_bytes(&m, ISUP_P_CALLING_NUMBER, b, (uint8_t)len);

	/* optional: hop counter */
	add_bytes(&m, ISUP_P_HOP_COUNTER, &hop, 1);

	n = isup_encode(&m, buf, sizeof(buf));
	CHECK(n > 0, "encode rc=%d (%s)", n, isup_strerror(n));

	rc = isup_decode(&m2, buf, n);
	CHECK(rc == ISUP_OK, "decode rc=%d (%s)", rc, isup_strerror(rc));
	CHECK(m2.cic == 1234, "cic=%u", m2.cic);
	CHECK(m2.msg_type == ISUP_MT_IAM, "mt=0x%02x", m2.msg_type);

	/* called number survived */
	p = isup_msg_get(&m2, ISUP_P_CALLED_NUMBER);
	CHECK(p != NULL, "no called number");
	if (p) {
		isup_dec_called(&dec, p->val, p->len);
		CHECK(strcmp(dec.digits, "61755500001") == 0, "cdpn='%s'", dec.digits);
		CHECK(dec.nature == ISUP_NOA_INTERNATIONAL, "cdpn noa=%u", dec.nature);
	}

	/* calling number + CLIR survived */
	p = isup_msg_get(&m2, ISUP_P_CALLING_NUMBER);
	CHECK(p != NULL, "no calling number");
	if (p) {
		isup_dec_calling(&dec, p->val, p->len);
		CHECK(strcmp(dec.digits, "0738900000") == 0, "cgpn='%s'", dec.digits);
		CHECK(dec.apri == ISUP_APRI_RESTRICTED, "cgpn apri=%u", dec.apri);
		CHECK(dec.screening == ISUP_SCR_NETWORK_PROVIDED, "cgpn scr=%u", dec.screening);
	}

	/* hop counter survived */
	p = isup_msg_get(&m2, ISUP_P_HOP_COUNTER);
	CHECK(p != NULL && p->len == 1 && p->val[0] == 15, "hop counter");

	/* re-encode is byte-identical */
	{
		uint8_t buf2[128];
		int n2 = isup_encode(&m2, buf2, sizeof(buf2));
		CHECK(n2 == n && memcmp(buf, buf2, n) == 0, "re-encode mismatch n=%d n2=%d", n, n2);
	}
}

/* ------------------------------------------------------------------ */
/* 3. ACM / 4. REL / 5. RLC / 6. CPG round-trips                       */
/* ------------------------------------------------------------------ */
static void test_simple_messages(void)
{
	struct isup_msg m, m2;
	uint8_t buf[64];
	int n, rc;
	const struct isup_param *p;

	printf("test_simple_messages\n");

	/* ACM with backward call indicators */
	{
		uint8_t bci[2] = { 0x12, 0x14 };
		isup_msg_init(&m, 5, ISUP_MT_ACM);
		add_bytes(&m, ISUP_P_BWD_CALL_IND, bci, 2);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "ACM encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "ACM decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_BWD_CALL_IND);
		CHECK(p && p->len == 2 && p->val[0] == 0x12 && p->val[1] == 0x14, "ACM BCI");
	}

	/* REL with cause = normal call clearing (16) */
	{
		uint8_t cause[2];
		uint8_t cs, loc, cv;
		int cl = isup_enc_cause(0 /*ITU*/, 1 /*public net local*/, 16, cause, sizeof(cause));
		CHECK(cl == 2, "cause enc len=%d", cl);
		isup_msg_init(&m, 5, ISUP_MT_REL);
		add_bytes(&m, ISUP_P_CAUSE, cause, (uint8_t)cl);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "REL encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "REL decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_CAUSE);
		CHECK(p != NULL, "REL has cause");
		if (p) {
			isup_dec_cause(p->val, p->len, &cs, &loc, &cv);
			CHECK(cv == 16, "cause value=%u", cv);
			CHECK(loc == 1, "cause location=%u", loc);
		}
	}

	/* RLC with no parameters: CIC(2)+type(1)+optional-pointer(1)=4, the
	 * optional-part pointer octet is always present and = 0 here. */
	{
		isup_msg_init(&m, 5, ISUP_MT_RLC);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n == 4, "RLC encode len=%d (want 4)", n);
		CHECK(buf[3] == 0x00, "RLC optional pointer=0x%02x", buf[3]);
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "RLC decode %s", isup_strerror(rc));
		CHECK(m2.n_params == 0, "RLC has %d params", m2.n_params);
	}

	/* CPG with event = alerting */
	{
		uint8_t ev = ISUP_EVENT_ALERTING;
		isup_msg_init(&m, 5, ISUP_MT_CPG);
		add_bytes(&m, ISUP_P_EVENT_INFO, &ev, 1);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "CPG encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "CPG decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_EVENT_INFO);
		CHECK(p && p->len == 1 && p->val[0] == ISUP_EVENT_ALERTING, "CPG event");
	}
}

/* ------------------------------------------------------------------ */
/* 7. Circuit-group: GRS / GRA / CGB range & status                    */
/* ------------------------------------------------------------------ */
static void test_circuit_group(void)
{
	struct isup_msg m, m2;
	uint8_t buf[64];
	int n, rc;
	const struct isup_param *p;

	printf("test_circuit_group\n");

	/* GRS: range only (reset CICs base..base+5) */
	{
		uint8_t rs[1];
		int rl = isup_enc_range_status(5, NULL, 0, rs, sizeof(rs));
		CHECK(rl == 1, "GRS rs len=%d", rl);
		isup_msg_init(&m, 1, ISUP_MT_GRS);
		add_bytes(&m, ISUP_P_RANGE_AND_STATUS, rs, (uint8_t)rl);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "GRS encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "GRS decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_RANGE_AND_STATUS);
		CHECK(p && p->len == 1 && p->val[0] == 5, "GRS range");
	}

	/* GRA: range + status bitmap */
	{
		uint8_t status[1] = { 0x00 }; /* 6 circuits, all idle/unblocked */
		uint8_t rs[8];
		uint8_t range, st[8];
		int has;
		int rl = isup_enc_range_status(5, status, 1, rs, sizeof(rs));
		CHECK(rl == 2, "GRA rs len=%d (want 2)", rl);
		isup_msg_init(&m, 1, ISUP_MT_GRA);
		add_bytes(&m, ISUP_P_RANGE_AND_STATUS, rs, (uint8_t)rl);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "GRA encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "GRA decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_RANGE_AND_STATUS);
		CHECK(p != NULL, "GRA has range/status");
		if (p) {
			isup_dec_range_status(p->val, p->len, &range, st, sizeof(st), &has);
			CHECK(range == 5 && has == 1, "GRA decode range=%u has=%d", range, has);
		}
	}

	/* CGB: supervision type (fixed) + range and status */
	{
		uint8_t sv = 0x00; /* maintenance oriented */
		uint8_t status[1] = { 0x3f }; /* block first 6 */
		uint8_t rs[8];
		int rl = isup_enc_range_status(5, status, 1, rs, sizeof(rs));
		isup_msg_init(&m, 1, ISUP_MT_CGB);
		add_bytes(&m, ISUP_P_CIRC_GRP_SV_TYPE, &sv, 1);
		add_bytes(&m, ISUP_P_RANGE_AND_STATUS, rs, (uint8_t)rl);
		n = isup_encode(&m, buf, sizeof(buf));
		CHECK(n > 0, "CGB encode %s", isup_strerror(n));
		rc = isup_decode(&m2, buf, n);
		CHECK(rc == ISUP_OK, "CGB decode %s", isup_strerror(rc));
		p = isup_msg_get(&m2, ISUP_P_CIRC_GRP_SV_TYPE);
		CHECK(p && p->len == 1 && p->val[0] == 0x00, "CGB sv type");
		p = isup_msg_get(&m2, ISUP_P_RANGE_AND_STATUS);
		CHECK(p && p->len == 2 && p->val[0] == 5 && p->val[1] == 0x3f, "CGB range/status");
	}
}

/* ------------------------------------------------------------------ */
/* 8. Decoder safety on truncated / malformed input                    */
/* ------------------------------------------------------------------ */
static void test_decoder_safety(void)
{
	struct isup_msg m, src;
	uint8_t buf[128];
	int n, i, rc;

	printf("test_decoder_safety\n");

	/* build a valid IAM, then decode every truncation of it */
	{
		uint8_t nci = 0, fci[2] = { 0, 0 }, cpc = 0x0a, tmr = 0;
		struct isup_number cd; uint8_t b[16]; int len;
		memset(&cd, 0, sizeof(cd));
		cd.nature = ISUP_NOA_NATIONAL; cd.npi = ISUP_NPI_ISDN; strcpy(cd.digits, "5551234");
		len = isup_enc_called(&cd, b, sizeof(b));
		isup_msg_init(&src, 7, ISUP_MT_IAM);
		add_bytes(&src, ISUP_P_NATURE_OF_CONN, &nci, 1);
		add_bytes(&src, ISUP_P_FWD_CALL_IND, fci, 2);
		add_bytes(&src, ISUP_P_CALLING_CATEGORY, &cpc, 1);
		add_bytes(&src, ISUP_P_TMR, &tmr, 1);
		add_bytes(&src, ISUP_P_CALLED_NUMBER, b, (uint8_t)len);
		n = isup_encode(&src, buf, sizeof(buf));
		CHECK(n > 0, "safety setup encode %s", isup_strerror(n));
	}
	for (i = 0; i <= n; i++) {
		rc = isup_decode(&m, buf, i);   /* must not crash (ASan) */
		(void)rc;
	}
	CHECK(1, "survived truncation sweep");

	/* unknown message type is rejected, not crashed */
	{
		uint8_t junk[3] = { 0x01, 0x00, 0xff };
		rc = isup_decode(&m, junk, sizeof(junk));
		CHECK(rc == ISUP_ERR_UNKNOWN_MT, "unknown mt rc=%d", rc);
	}

	/* deterministic pseudo-random fuzz of the decoder */
	{
		unsigned seed = 0x1234abcdu;
		int it;
		for (it = 0; it < 20000; it++) {
			uint8_t fb[64];
			int flen, j;
			seed = seed * 1103515245u + 12345u;
			flen = (int)(seed % sizeof(fb));
			for (j = 0; j < flen; j++) {
				seed = seed * 1103515245u + 12345u;
				fb[j] = (uint8_t)(seed >> 16);
			}
			rc = isup_decode(&m, fb, flen); /* never crash/OOB */
			(void)rc;
		}
		CHECK(1, "survived 20000 random decodes");
	}
}

/* ------------------------------------------------------------------ */
/* 9. Odd/even digit boundary in number coding                          */
/* ------------------------------------------------------------------ */
static void test_number_parity(void)
{
	struct isup_number n, d;
	uint8_t b[32];
	int len;

	printf("test_number_parity\n");

	/* even count */
	memset(&n, 0, sizeof(n));
	n.nature = ISUP_NOA_NATIONAL; n.npi = ISUP_NPI_ISDN; strcpy(n.digits, "1234");
	len = isup_enc_called(&n, b, sizeof(b));
	CHECK(len == 4, "even len=%d", len);
	CHECK((b[0] >> 7) == 0, "even parity bit");
	isup_dec_called(&d, b, len);
	CHECK(strcmp(d.digits, "1234") == 0, "even rt '%s'", d.digits);

	/* odd count */
	strcpy(n.digits, "12345");
	len = isup_enc_called(&n, b, sizeof(b));
	CHECK(len == 5, "odd len=%d", len);
	CHECK((b[0] >> 7) == 1, "odd parity bit");
	isup_dec_called(&d, b, len);
	CHECK(strcmp(d.digits, "12345") == 0, "odd rt '%s'", d.digits);
}

int main(void)
{
	printf("== mod_isup Q.763 codec tests ==\n");
	test_iam_exact();
	test_iam_roundtrip();
	test_simple_messages();
	test_circuit_group();
	test_decoder_safety();
	test_number_parity();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
