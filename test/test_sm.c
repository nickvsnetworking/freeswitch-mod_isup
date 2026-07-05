/*
 * mod_isup — Q.764 state-machine unit tests.
 *
 * Drives the FSM against a mock ops vtable that records every outward
 * action into a single space-separated trace string, then asserts the
 * exact action sequence and resulting state for each call scenario.
 */
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "../isup_sm.h"
#include "../isup_codec.h"

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

/* ---- mock ops ---- */

struct mock { char log[2048]; };

static void mlog(struct mock *M, const char *fmt, ...)
{
	char tmp[96];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (M->log[0])
		strncat(M->log, " ", sizeof(M->log) - strlen(M->log) - 1);
	strncat(M->log, tmp, sizeof(M->log) - strlen(M->log) - 1);
}

static void op_send(void *u, const struct isup_msg *m)        { mlog(u, "S:%s", isup_msg_type_name(m->msg_type)); }
static void op_crcx(void *u, const char *mode)                { mlog(u, "crcx:%s", mode); }
static void op_mode(void *u, const char *mode)                { mlog(u, "mode:%s", mode); }
static void op_dlcx(void *u)                                  { mlog(u, "dlcx"); }
static void op_setup(void *u, const struct isup_msg *iam)     { (void)iam; mlog(u, "setup"); }
static void op_progress(void *u, int answered)                { mlog(u, "prog:%d", answered); }
static void op_answer(void *u)                                { mlog(u, "answer"); }
static void op_release(void *u, uint8_t cause)                { mlog(u, "rel:%d", cause); }
static const char *tname(int id)
{
	switch (id) {
	case ISUP_T1:  return "T1";
	case ISUP_T2:  return "T2";
	case ISUP_T5:  return "T5";
	case ISUP_T7:  return "T7";
	case ISUP_T8:  return "T8";
	case ISUP_T9:  return "T9";
	case ISUP_T12: return "T12";
	case ISUP_T16: return "T16";
	case ISUP_T22: return "T22";
	default:       return "T?";
	}
}

static void op_tstart(void *u, int id, int ms)                { (void)ms; mlog(u, "t+:%s", tname(id)); }
static void op_tstop(void *u, int id)                         { mlog(u, "t-:%s", tname(id)); }

static const struct isup_sm_ops MOPS = {
	.send = op_send, .bearer_crcx = op_crcx, .bearer_mode = op_mode,
	.bearer_dlcx = op_dlcx, .fs_setup = op_setup, .fs_progress = op_progress,
	.fs_answer = op_answer, .fs_release = op_release,
	.start_timer = op_tstart, .stop_timer = op_tstop,
};

/* ---- message builders ---- */

static void make_iam(struct isup_msg *m, uint16_t cic, uint8_t nci)
{
	uint8_t fci[2] = { 0x60, 0x10 }, cpc = ISUP_CPC_ORDINARY, tmr = ISUP_TMR_SPEECH;
	uint8_t cdpn[4] = { 0x83, 0x10, 0x21, 0x03 }; /* "123" national/ISDN */
	isup_msg_init(m, cic, ISUP_MT_IAM);
	isup_msg_add(m, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(m, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(m, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(m, ISUP_P_TMR, &tmr, 1);
	isup_msg_add(m, ISUP_P_CALLED_NUMBER, cdpn, 4);
}

static void make_rel(struct isup_msg *m, uint16_t cic, uint8_t cause)
{
	uint8_t c[2];
	c[0] = (uint8_t)(0x80 | 0x01);
	c[1] = (uint8_t)(0x80 | (cause & 0x7f));
	isup_msg_init(m, cic, ISUP_MT_REL);
	isup_msg_add(m, ISUP_P_CAUSE, c, 2);
}

static void make_cot(struct isup_msg *m, uint16_t cic, int success)
{
	uint8_t ci = success ? 0x01 : 0x00;
	isup_msg_init(m, cic, ISUP_MT_COT);
	isup_msg_add(m, ISUP_P_CONTINUITY_IND, &ci, 1);
}

static void make_simple(struct isup_msg *m, uint16_t cic, uint8_t mt)
{
	isup_msg_init(m, cic, mt);
}

static void make_susres(struct isup_msg *m, uint16_t cic, uint8_t mt, int network)
{
	uint8_t ind = network ? 0x01 : 0x00;
	isup_msg_init(m, cic, mt);
	isup_msg_add(m, ISUP_P_SUSP_RESUME_IND, &ind, 1);
}

/* bring an outbound call all the way to the ACTIVE state */
static void drive_to_active(struct isup_cic *c, struct mock *M, uint16_t cic)
{
	struct isup_msg m;
	isup_sm_init(c, cic, &MOPS, M, 1);
	make_iam(&m, cic, 0x00);
	isup_sm_originate(c, &m);
	isup_sm_bearer_ready(c);
	make_simple(&m, cic, ISUP_MT_ACM); isup_sm_rx(c, &m);
	make_simple(&m, cic, ISUP_MT_ANM); isup_sm_rx(c, &m);
	M->log[0] = '\0'; /* discard setup trace */
}

static void check_log(struct mock *M, const char *want, const char *scenario)
{
	CHECK(strcmp(M->log, want) == 0, "%s\n    got : %s\n    want: %s",
	      scenario, M->log, want);
}

/* ------------------------------------------------------------------ */

static void test_outbound_basic(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_outbound_basic\n");
	isup_sm_init(&c, 5, &MOPS, &M, 1);

	make_iam(&m, 5, 0x00);
	isup_sm_originate(&c, &m);
	CHECK(c.state == CIC_OUT_BEARER, "state=%s", isup_state_name(c.state));

	isup_sm_bearer_ready(&c);
	CHECK(c.state == CIC_IAM_SENT, "state=%s", isup_state_name(c.state));

	make_simple(&m, 5, ISUP_MT_ACM);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_OUT_RINGING, "state=%s", isup_state_name(c.state));

	make_simple(&m, 5, ISUP_MT_ANM);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_ACTIVE, "state=%s", isup_state_name(c.state));

	isup_sm_hangup(&c, 16);
	CHECK(c.state == CIC_REL_SENT, "state=%s", isup_state_name(c.state));

	make_simple(&m, 5, ISUP_MT_RLC);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IDLE, "state=%s", isup_state_name(c.state));

	check_log(&M,
		"crcx:sendrecv S:IAM t+:T7 t-:T7 prog:0 t+:T9 t-:T9 mode:sendrecv answer "
		"S:REL t+:T1 t+:T5 t-:T1 t-:T2 t-:T5 t-:T7 t-:T9 dlcx",
		"outbound basic");
}

static void test_inbound_basic(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_inbound_basic\n");
	isup_sm_init(&c, 7, &MOPS, &M, 1);

	make_iam(&m, 7, 0x00);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IN_BEARER, "state=%s", isup_state_name(c.state));

	isup_sm_bearer_ready(&c);
	CHECK(c.state == CIC_IN_PROCEEDING, "state=%s", isup_state_name(c.state));

	isup_sm_proceeding(&c);
	CHECK(c.state == CIC_IN_RINGING, "state=%s", isup_state_name(c.state));

	isup_sm_answer(&c);
	CHECK(c.state == CIC_ACTIVE, "state=%s", isup_state_name(c.state));

	make_rel(&m, 7, 16);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IDLE, "state=%s", isup_state_name(c.state));

	check_log(&M,
		"crcx:recvonly setup S:ACM S:ANM mode:sendrecv rel:16 S:RLC "
		"t-:T1 t-:T2 t-:T5 t-:T7 t-:T9 dlcx",
		"inbound basic");
}

static void test_inbound_continuity(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_inbound_continuity\n");
	isup_sm_init(&c, 9, &MOPS, &M, 1);

	make_iam(&m, 9, 0x04);   /* continuity-check on this circuit */
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IN_WAIT_COT, "state=%s", isup_state_name(c.state));

	isup_sm_bearer_ready(&c); /* loopback established, no transition */
	CHECK(c.state == CIC_IN_WAIT_COT, "state=%s", isup_state_name(c.state));

	make_cot(&m, 9, 1);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IN_PROCEEDING, "state=%s", isup_state_name(c.state));

	check_log(&M, "crcx:loopback mode:recvonly setup", "inbound continuity");
}

static void test_t7_no_acm(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_t7_no_acm\n");
	isup_sm_init(&c, 5, &MOPS, &M, 1);
	make_iam(&m, 5, 0x00);
	isup_sm_originate(&c, &m);
	isup_sm_bearer_ready(&c);

	isup_sm_timer(&c, ISUP_T7);
	CHECK(c.state == CIC_REL_SENT, "state=%s", isup_state_name(c.state));
	check_log(&M, "crcx:sendrecv S:IAM t+:T7 rel:102 S:REL t+:T1 t+:T5", "T7 expiry");
}

static void test_glare(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_glare\n");

	/* non-controlling exchange yields to the incoming IAM */
	isup_sm_init(&c, 5, &MOPS, &M, 0);
	make_iam(&m, 5, 0x00);
	isup_sm_originate(&c, &m);
	isup_sm_bearer_ready(&c);
	make_iam(&m, 5, 0x00);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IN_BEARER, "yield state=%s", isup_state_name(c.state));
	check_log(&M, "crcx:sendrecv S:IAM t+:T7 t-:T7 rel:16 dlcx crcx:recvonly",
		  "glare yield");

	/* controlling exchange ignores the incoming IAM */
	memset(&M, 0, sizeof(M));
	isup_sm_init(&c, 5, &MOPS, &M, 1);
	make_iam(&m, 5, 0x00);
	isup_sm_originate(&c, &m);
	isup_sm_bearer_ready(&c);
	make_iam(&m, 5, 0x00);
	isup_sm_rx(&c, &m);
	CHECK(c.state == CIC_IAM_SENT, "win state=%s", isup_state_name(c.state));
	check_log(&M, "crcx:sendrecv S:IAM t+:T7", "glare win");
}

static void test_fast_answer(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_fast_answer\n");
	isup_sm_init(&c, 3, &MOPS, &M, 1);
	make_iam(&m, 3, 0x00);
	isup_sm_rx(&c, &m);
	isup_sm_bearer_ready(&c);
	isup_sm_answer(&c); /* answer before alerting -> CON */
	CHECK(c.state == CIC_ACTIVE, "state=%s", isup_state_name(c.state));
	check_log(&M, "crcx:recvonly setup S:CON mode:sendrecv", "fast answer");
}

static void test_suspend_resume(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_suspend_resume\n");
	drive_to_active(&c, &M, 5);

	/* network-initiated SUS: mark suspended, signal FS, arm T2 */
	make_susres(&m, 5, ISUP_MT_SUS, 1);
	isup_sm_rx(&c, &m);
	CHECK(c.suspended == 1, "suspended");
	CHECK(c.state == CIC_ACTIVE, "still active while suspended");
	check_log(&M, "prog:1 t+:T2", "network suspend");

	/* RES clears it */
	M.log[0] = '\0';
	make_susres(&m, 5, ISUP_MT_RES, 1);
	isup_sm_rx(&c, &m);
	CHECK(c.suspended == 0, "resumed");
	check_log(&M, "t-:T2", "resume");

	/* suspend again, then T2 expires -> release */
	make_susres(&m, 5, ISUP_MT_SUS, 1);
	isup_sm_rx(&c, &m);
	M.log[0] = '\0';
	isup_sm_timer(&c, ISUP_T2);
	CHECK(c.state == CIC_REL_SENT, "released on T2 (%s)", isup_state_name(c.state));
	check_log(&M, "rel:16 S:REL t+:T1 t+:T5", "T2 expiry release");
}

static void test_netmgmt(void)
{
	struct mock M = { {0} };
	struct isup_cic c;
	struct isup_msg m;

	printf("test_netmgmt\n");
	isup_sm_init(&c, 5, &MOPS, &M, 1);

	/* User Part Test -> User Part Available */
	make_simple(&m, 5, ISUP_MT_UPT);
	isup_sm_rx(&c, &m);
	check_log(&M, "S:UPA", "UPT->UPA");

	/* Information Request -> Information */
	M.log[0] = '\0';
	make_simple(&m, 5, ISUP_MT_INR);
	isup_sm_rx(&c, &m);
	check_log(&M, "S:INF", "INR->INF");

	/* charge/overload/confusion/forward-transfer/SAM/LPA accepted silently */
	M.log[0] = '\0';
	make_simple(&m, 5, ISUP_MT_CRG); isup_sm_rx(&c, &m);
	make_simple(&m, 5, ISUP_MT_SAM); isup_sm_rx(&c, &m);
	make_simple(&m, 5, ISUP_MT_LPA); isup_sm_rx(&c, &m);
	make_simple(&m, 5, ISUP_MT_CFN); isup_sm_rx(&c, &m);
	CHECK(M.log[0] == '\0', "accepted silently, log='%s'", M.log);
	CHECK(c.state == CIC_IDLE, "state unchanged (%s)", isup_state_name(c.state));
}

int main(void)
{
	printf("== mod_isup Q.764 state-machine tests ==\n");
	test_outbound_basic();
	test_inbound_basic();
	test_inbound_continuity();
	test_t7_no_acm();
	test_glare();
	test_fast_answer();
	test_suspend_resume();
	test_netmgmt();
	printf("== %d checks, %d failures ==\n", g_tests, g_fail);
	return g_fail ? 1 : 0;
}
