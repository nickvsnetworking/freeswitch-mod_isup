/*
 * mod_isup — Q.764 per-circuit call-control state machine.
 */
#include <string.h>
#include "isup_sm.h"

/* Default Q.764 timer values (ms). Overridable per profile in the real module. */
#define MS_T1    15000
#define MS_T2   180000
#define MS_T5   300000
#define MS_T7    25000
#define MS_T9   120000

/* Q.850 cause values used internally */
#define CAUSE_NORMAL_CLEARING   16
#define CAUSE_NO_ANSWER         19
#define CAUSE_RECOVERY_TIMER   102
#define CAUSE_CONTINUITY_FAIL   41

/* ---- emit helpers ---- */

static void emit(struct isup_cic *c, uint8_t mt)
{
	struct isup_msg m;
	isup_msg_init(&m, c->cic, mt);
	c->ops->send(c->user, &m);
}

static void emit_cause(struct isup_cic *c, uint8_t mt, uint8_t cause)
{
	struct isup_msg m;
	uint8_t buf[2];
	isup_msg_init(&m, c->cic, mt);
	buf[0] = (uint8_t)(0x80 | (0 << 5) | 0x01); /* ITU, location=public local net */
	buf[1] = (uint8_t)(0x80 | (cause & 0x7f));
	isup_msg_add(&m, ISUP_P_CAUSE, buf, 2);
	c->ops->send(c->user, &m);
}

/* Backward Call Indicators: caller-set (isup_sm_set_bci) or a sensible default
 * (charge, subscriber free, ordinary subscriber). */
static void fill_bci(struct isup_cic *c, uint8_t *bci)
{
	if (c->bci_set) { bci[0] = c->bci[0]; bci[1] = c->bci[1]; }
	else            { bci[0] = 0x12;      bci[1] = 0x14; }
}

static void emit_acm(struct isup_cic *c)
{
	struct isup_msg m;
	uint8_t bci[2];
	fill_bci(c, bci);
	isup_msg_init(&m, c->cic, ISUP_MT_ACM);
	isup_msg_add(&m, ISUP_P_BWD_CALL_IND, bci, 2);
	c->ops->send(c->user, &m);
}

static void emit_con(struct isup_cic *c)
{
	struct isup_msg m;
	uint8_t bci[2];
	fill_bci(c, bci);
	isup_msg_init(&m, c->cic, ISUP_MT_CON);
	isup_msg_add(&m, ISUP_P_BWD_CALL_IND, bci, 2);
	c->ops->send(c->user, &m);
}

void isup_sm_set_bci(struct isup_cic *c, uint8_t b0, uint8_t b1)
{
	c->bci[0] = b0; c->bci[1] = b1; c->bci_set = 1;
}

/* Continuity message (Q.764 §2.1.8). Sent by the originating side after an IAM
 * that requested a continuity check, reporting the result on the circuit. */
static void emit_cot(struct isup_cic *c, int success)
{
	struct isup_msg m;
	uint8_t ind = success ? 0x01 : 0x00; /* bit0: 1 = continuity check OK */
	isup_msg_init(&m, c->cic, ISUP_MT_COT);
	isup_msg_add(&m, ISUP_P_CONTINUITY_IND, &ind, 1);
	c->ops->send(c->user, &m);
}

static uint8_t cause_of(const struct isup_msg *m)
{
	const struct isup_param *p = isup_msg_get(m, ISUP_P_CAUSE);
	if (p && p->len >= 2)
		return p->val[1] & 0x7f;
	return CAUSE_NORMAL_CLEARING;
}

/* Continuity-check indicator from IAM's Nature of Connection Indicators. */
static int iam_continuity(const struct isup_msg *iam)
{
	const struct isup_param *p = isup_msg_get(iam, ISUP_P_NATURE_OF_CONN);
	if (p && p->len >= 1)
		return (p->val[0] >> 2) & 0x03; /* 0 none, 1 this circuit, 2 previous */
	return 0;
}

static void go(struct isup_cic *c, enum isup_cic_state s)
{
	c->state = s;
}

/* Tear down: release bearer, stop timers, return to idle. */
static void release_to_idle(struct isup_cic *c)
{
	c->ops->stop_timer(c->user, ISUP_T1);
	c->ops->stop_timer(c->user, ISUP_T2);
	c->ops->stop_timer(c->user, ISUP_T5);
	c->ops->stop_timer(c->user, ISUP_T7);
	c->ops->stop_timer(c->user, ISUP_T9);
	c->ops->bearer_dlcx(c->user);
	c->t1_retries = 0;
	c->suspended = 0;
	go(c, CIC_IDLE);
}

/* ------------------------------------------------------------------ */
/* Lifecycle                                                           */
/* ------------------------------------------------------------------ */

void isup_sm_init(struct isup_cic *c, uint16_t cic,
		  const struct isup_sm_ops *ops, void *user, int controlling)
{
	memset(c, 0, sizeof(*c));
	c->cic = cic;
	c->ops = ops;
	c->user = user;
	c->controlling = controlling;
	c->state = CIC_IDLE;
}

/* ------------------------------------------------------------------ */
/* FreeSWITCH-side stimuli                                             */
/* ------------------------------------------------------------------ */

void isup_sm_originate(struct isup_cic *c, const struct isup_msg *iam)
{
	if (c->state != CIC_IDLE)
		return;
	c->pending_iam = *iam;
	c->pending_iam.cic = c->cic;
	c->pending_iam.msg_type = ISUP_MT_IAM;
	/* honour a continuity-check request carried in the IAM's NCI: after the
	 * IAM we will run the check on our bearer and report it with a COT. */
	c->continuity = iam_continuity(iam);
	/* set state before crcx: a synchronous bearer driver may invoke the
	 * ready callback re-entrantly before this returns. */
	go(c, CIC_OUT_BEARER);
	c->ops->bearer_crcx(c->user, "sendrecv");
}

void isup_sm_proceeding(struct isup_cic *c)
{
	if (c->state == CIC_IN_PROCEEDING) {
		emit_acm(c);
		go(c, CIC_IN_RINGING);
	}
}

void isup_sm_answer(struct isup_cic *c)
{
	switch (c->state) {
	case CIC_IN_PROCEEDING:
		emit_con(c);                       /* answer before ACM */
		c->ops->bearer_mode(c->user, "sendrecv");
		go(c, CIC_ACTIVE);
		break;
	case CIC_IN_RINGING:
		emit(c, ISUP_MT_ANM);
		c->ops->bearer_mode(c->user, "sendrecv");
		go(c, CIC_ACTIVE);
		break;
	default:
		break;
	}
}

void isup_sm_hangup(struct isup_cic *c, uint8_t cause)
{
	switch (c->state) {
	case CIC_IDLE:
	case CIC_REL_SENT:
	case CIC_RELEASING:
		break;
	default:
		c->last_cause = cause;
		emit_cause(c, ISUP_MT_REL, cause);
		c->ops->start_timer(c->user, ISUP_T1, MS_T1);
		c->ops->start_timer(c->user, ISUP_T5, MS_T5);
		c->t1_retries = 0;
		go(c, CIC_REL_SENT);
		break;
	}
}

/* Forced reset of the circuit (Q.764 §2.9, group reset GRS). Releases any call
 * in progress towards FreeSWITCH and returns the circuit to idle, WITHOUT
 * emitting a per-circuit RLC — for a group reset the single GRA acknowledges
 * the whole range. (A single-circuit RSC is acknowledged with RLC by the rx
 * path instead.) */
void isup_sm_reset(struct isup_cic *c)
{
	if (c->state != CIC_IDLE && c->state != CIC_RELEASING)
		c->ops->fs_release(c->user, CAUSE_NORMAL_CLEARING);
	release_to_idle(c);
}

/* ------------------------------------------------------------------ */
/* Bearer-side stimulus                                                */
/* ------------------------------------------------------------------ */

void isup_sm_bearer_ready(struct isup_cic *c)
{
	switch (c->state) {
	case CIC_OUT_BEARER:
		c->ops->send(c->user, &c->pending_iam);
		c->ops->start_timer(c->user, ISUP_T7, MS_T7);
		/* If this IAM requested a continuity check on this circuit, the bearer
		 * is now up; run the (modelled) check and report success with a COT so
		 * the far end can remove its loopback and proceed. */
		if (c->continuity == 1)
			emit_cot(c, 1);
		go(c, CIC_IAM_SENT);
		break;
	case CIC_IN_BEARER:
		c->ops->fs_setup(c->user, &c->pending_iam);
		go(c, CIC_IN_PROCEEDING);
		break;
	case CIC_IN_WAIT_COT:
		/* bearer looped back; await COT from peer */
		break;
	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Transport-side stimulus (received ISUP message)                     */
/* ------------------------------------------------------------------ */

void isup_sm_rx(struct isup_cic *c, const struct isup_msg *m)
{
	/* Messages valid in (almost) any state first. */
	switch (m->msg_type) {
	case ISUP_MT_REL:
		c->last_cause = cause_of(m);
		if (c->state != CIC_IDLE && c->state != CIC_RELEASING)
			c->ops->fs_release(c->user, c->last_cause);
		emit(c, ISUP_MT_RLC);
		release_to_idle(c);
		return;
	case ISUP_MT_RSC:
		if (c->state != CIC_IDLE)
			c->ops->fs_release(c->user, CAUSE_NORMAL_CLEARING);
		emit(c, ISUP_MT_RLC);
		release_to_idle(c);
		return;
	case ISUP_MT_BLO:
		emit(c, ISUP_MT_BLA);
		return;
	case ISUP_MT_UBL:
		emit(c, ISUP_MT_UBA);
		return;
	case ISUP_MT_CCR:
		/* peer asks us to loop for continuity */
		c->ops->bearer_mode(c->user, "loopback");
		return;
	case ISUP_MT_UPT:
		/* User Part Test -> User Part Available */
		emit(c, ISUP_MT_UPA);
		return;
	case ISUP_MT_INR: {
		/* Information Request -> Information (Q.764 2.1.10). Reply with a
		 * minimal Information Indicators parameter. */
		struct isup_msg inf;
		uint8_t ind[2] = { 0x00, 0x00 };
		isup_msg_init(&inf, c->cic, ISUP_MT_INF);
		isup_msg_add(&inf, ISUP_P_INF_IND, ind, 2);
		c->ops->send(c->user, &inf);
		return;
	}
	case ISUP_MT_CRG:   /* charge information         */
	case ISUP_MT_OLM:   /* overload                   */
	case ISUP_MT_NRM:   /* network resource mgmt      */
	case ISUP_MT_CFN:   /* confusion                  */
	case ISUP_MT_FOT:   /* forward transfer           */
	case ISUP_MT_INF:   /* information                */
	case ISUP_MT_LPA:   /* continuity loop-back ack   */
	case ISUP_MT_SAM:   /* subsequent address (overlap; digits handled at FS) */
		/* accepted; no circuit-state change required */
		return;
	default:
		break;
	}

	switch (c->state) {
	case CIC_IDLE:
		if (m->msg_type == ISUP_MT_IAM) {
			c->pending_iam = *m;
			c->continuity = iam_continuity(m);
			/* set state before crcx (synchronous bearer may call back
			 * re-entrantly). */
			if (c->continuity == 1) {
				go(c, CIC_IN_WAIT_COT);
				c->ops->bearer_crcx(c->user, "loopback");
			} else {
				go(c, CIC_IN_BEARER);
				c->ops->bearer_crcx(c->user, "recvonly");
			}
		}
		break;

	case CIC_IN_WAIT_COT:
		if (m->msg_type == ISUP_MT_COT) {
			const struct isup_param *p = isup_msg_get(m, ISUP_P_CONTINUITY_IND);
			int ok = !(p && p->len >= 1) || (p->val[0] & 0x01);
			if (ok) {
				c->ops->bearer_mode(c->user, "recvonly");
				c->ops->fs_setup(c->user, &c->pending_iam);
				go(c, CIC_IN_PROCEEDING);
			} else {
				emit_cause(c, ISUP_MT_REL, CAUSE_CONTINUITY_FAIL);
				c->ops->start_timer(c->user, ISUP_T1, MS_T1);
				go(c, CIC_REL_SENT);
			}
		}
		break;

	case CIC_OUT_BEARER:
	case CIC_IAM_SENT:
		if (m->msg_type == ISUP_MT_IAM) {     /* glare / dual seizure */
			if (!c->controlling) {
				/* yield: drop our attempt, accept the incoming call */
				c->ops->stop_timer(c->user, ISUP_T7);
				c->ops->fs_release(c->user, CAUSE_NORMAL_CLEARING);
				c->ops->bearer_dlcx(c->user);
				go(c, CIC_IDLE);
				isup_sm_rx(c, m);            /* reprocess as inbound */
			}
			/* controlling side ignores the incoming IAM */
			return;
		}
		if (c->state != CIC_IAM_SENT)
			break;
		if (m->msg_type == ISUP_MT_ACM) {
			c->ops->stop_timer(c->user, ISUP_T7);
			c->ops->fs_progress(c->user, 0);
			c->ops->start_timer(c->user, ISUP_T9, MS_T9);
			go(c, CIC_OUT_RINGING);
		} else if (m->msg_type == ISUP_MT_CON || m->msg_type == ISUP_MT_ANM) {
			c->ops->stop_timer(c->user, ISUP_T7);
			c->ops->bearer_mode(c->user, "sendrecv");
			c->ops->fs_answer(c->user);
			go(c, CIC_ACTIVE);
		}
		break;

	case CIC_OUT_RINGING:
		if (m->msg_type == ISUP_MT_ANM || m->msg_type == ISUP_MT_CON) {
			c->ops->stop_timer(c->user, ISUP_T9);
			c->ops->bearer_mode(c->user, "sendrecv");
			c->ops->fs_answer(c->user);
			go(c, CIC_ACTIVE);
		} else if (m->msg_type == ISUP_MT_CPG) {
			c->ops->fs_progress(c->user, 0);
		}
		break;

	case CIC_ACTIVE:
		if (m->msg_type == ISUP_MT_SUS) {
			const struct isup_param *p = isup_msg_get(m, ISUP_P_SUSP_RESUME_IND);
			int network = p && p->len >= 1 && (p->val[0] & 0x01);
			c->suspended = 1;
			c->ops->fs_progress(c->user, 1); /* signal hold-ish to FS */
			if (network)
				c->ops->start_timer(c->user, ISUP_T2, MS_T2);
		} else if (m->msg_type == ISUP_MT_RES) {
			if (c->suspended) {
				c->suspended = 0;
				c->ops->stop_timer(c->user, ISUP_T2);
			}
		}
		break;

	case CIC_REL_SENT:
		if (m->msg_type == ISUP_MT_RLC)
			release_to_idle(c);
		break;

	default:
		break;
	}
}

/* ------------------------------------------------------------------ */
/* Timer expiry                                                        */
/* ------------------------------------------------------------------ */

void isup_sm_timer(struct isup_cic *c, int timer_id)
{
	switch (timer_id) {
	case ISUP_T7: /* no ACM after IAM */
		if (c->state == CIC_IAM_SENT) {
			c->ops->fs_release(c->user, CAUSE_RECOVERY_TIMER);
			isup_sm_hangup(c, CAUSE_RECOVERY_TIMER);
		}
		break;
	case ISUP_T9: /* no ANM after ACM */
		if (c->state == CIC_OUT_RINGING) {
			c->ops->fs_release(c->user, CAUSE_NO_ANSWER);
			isup_sm_hangup(c, CAUSE_NO_ANSWER);
		}
		break;
	case ISUP_T2: /* network suspend not resumed in time */
		if (c->state == CIC_ACTIVE && c->suspended) {
			c->ops->fs_release(c->user, CAUSE_NORMAL_CLEARING);
			isup_sm_hangup(c, CAUSE_NORMAL_CLEARING);
		}
		break;
	case ISUP_T1: /* no RLC after REL — resend REL while T5 runs */
		if (c->state == CIC_REL_SENT) {
			c->t1_retries++;
			emit_cause(c, ISUP_MT_REL, c->last_cause);
			c->ops->start_timer(c->user, ISUP_T1, MS_T1);
		}
		break;
	case ISUP_T5: /* long guard — escalate to RSC */
		if (c->state == CIC_REL_SENT) {
			c->ops->stop_timer(c->user, ISUP_T1);
			emit(c, ISUP_MT_RSC);
			go(c, CIC_RESET);
		}
		break;
	default:
		break;
	}
}

const char *isup_state_name(enum isup_cic_state s)
{
	switch (s) {
	case CIC_IDLE:          return "IDLE";
	case CIC_OUT_BEARER:    return "OUT_BEARER";
	case CIC_IAM_SENT:      return "IAM_SENT";
	case CIC_OUT_RINGING:   return "OUT_RINGING";
	case CIC_IN_BEARER:     return "IN_BEARER";
	case CIC_IN_WAIT_COT:   return "IN_WAIT_COT";
	case CIC_IN_PROCEEDING: return "IN_PROCEEDING";
	case CIC_IN_RINGING:    return "IN_RINGING";
	case CIC_ACTIVE:        return "ACTIVE";
	case CIC_REL_SENT:      return "REL_SENT";
	case CIC_RELEASING:     return "RELEASING";
	case CIC_RESET:         return "RESET";
	case CIC_BLOCKED:       return "BLOCKED";
	default:                return "?";
	}
}
