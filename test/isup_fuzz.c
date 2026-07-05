/*
 * mod_isup — libFuzzer entry point covering the whole hostile-input surface.
 *
 * One arbitrary byte string is pushed through:
 *   isup_decode -> re-encode -> typed parameter decoders ->
 *   circuit-group manager -> call state machine -> segmentation/reassembly
 *
 * None of these may crash, over-read, over-write, or hang on any input.
 *
 * Build & run:  make -C mod_isup fuzz          (clang -fsanitize=fuzzer,address)
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "../isup_codec.h"
#include "../isup_param.h"
#include "../isup_cgm.h"
#include "../isup_sm.h"
#include "../isup_segment.h"
#include "../isup_trace.h"

/* no-op ops so the managers can run without external dependencies */
static void cgm_emit(void *u, const struct isup_msg *m) { (void)u; (void)m; }
static const struct isup_cgm_ops FCGM = { .emit = cgm_emit };

static void sm_send(void *u, const struct isup_msg *m) { (void)u; (void)m; }
static void sm_crcx(void *u, const char *s) { (void)u; (void)s; }
static void sm_mode(void *u, const char *s) { (void)u; (void)s; }
static void sm_dlcx(void *u) { (void)u; }
static void sm_setup(void *u, const struct isup_msg *m) { (void)u; (void)m; }
static void sm_prog(void *u, int a) { (void)u; (void)a; }
static void sm_ans(void *u) { (void)u; }
static void sm_rel(void *u, uint8_t c) { (void)u; (void)c; }
static void sm_ts(void *u, int i, int ms) { (void)u; (void)i; (void)ms; }
static void sm_te(void *u, int i) { (void)u; (void)i; }
static const struct isup_sm_ops FSM = {
	.send = sm_send, .bearer_crcx = sm_crcx, .bearer_mode = sm_mode,
	.bearer_dlcx = sm_dlcx, .fs_setup = sm_setup, .fs_progress = sm_prog,
	.fs_answer = sm_ans, .fs_release = sm_rel, .start_timer = sm_ts, .stop_timer = sm_te,
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
	struct isup_msg m;
	const struct isup_param *p;
	uint8_t buf[ISUP_MAX_ENC_LEN];

	if (isup_decode(&m, data, size) != ISUP_OK)
		return 0;

	/* round-trip: a successfully decoded message must re-encode safely */
	isup_encode(&m, buf, sizeof(buf));

	/* tracer must survive any decoded message, into a deliberately tiny buf */
	{
		char tb[64];
		isup_trace(&m, tb, sizeof(tb));
	}

	/* typed parameter-body decoders on whatever turned up */
	{
		struct isup_number num;
		uint8_t cs, loc, cv, range, st[64];
		int has;
		if ((p = isup_msg_get(&m, ISUP_P_CALLED_NUMBER)))
			isup_dec_called(&num, p->val, p->len);
		if ((p = isup_msg_get(&m, ISUP_P_CALLING_NUMBER)))
			isup_dec_calling(&num, p->val, p->len);
		if ((p = isup_msg_get(&m, ISUP_P_CAUSE)))
			isup_dec_cause(p->val, p->len, &cs, &loc, &cv);
		if ((p = isup_msg_get(&m, ISUP_P_RANGE_AND_STATUS)))
			isup_dec_range_status(p->val, p->len, &range, st, sizeof(st), &has);
	}

	/* circuit-group manager (the layer with the most index arithmetic) */
	{
		struct isup_cgm g;
		isup_cgm_init(&g, 0, 64, &FCGM, NULL);
		isup_cgm_rx(&g, &m);
	}

	/* call state machine — fresh circuit, single stimulus */
	{
		struct isup_cic c;
		isup_sm_init(&c, m.cic, &FSM, NULL, 1);
		isup_sm_rx(&c, &m);
	}

	/* segmentation + reassembly */
	{
		struct isup_msg base, sgm;
		int need;
		if (isup_segment(&m, 30, &base, &sgm, &need) == ISUP_OK && need) {
			struct isup_msg r = base;
			isup_reassemble(&r, &sgm);
		}
	}

	return 0;
}
