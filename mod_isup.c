/*
 * mod_isup — FreeSWITCH endpoint: ISUP-over-M3UA MGCF.
 *
 * Module load brings up an Osmo thread running osmo_select_main(): it owns the
 * M3UA association (configured from a cs7 VTY file), all ISUP protocol state,
 * the per-circuit timers, and the (synchronous) MGCP bearer transactions.
 * FreeSWITCH session threads never touch ISUP state directly — they post
 * commands through an eventfd-woken queue that the Osmo thread drains, so the
 * state machine has a single owner.
 */
#include <switch.h>
#include <sys/eventfd.h>
#include <unistd.h>
#include <netinet/in.h>

#include <osmocom/core/talloc.h>
#include <osmocom/core/application.h>
#include <osmocom/core/select.h>
#include <osmocom/core/timer.h>
#include <osmocom/core/msgb.h>
#include <osmocom/core/prim.h>
#include <osmocom/vty/vty.h>
#include <osmocom/vty/command.h>
#include <osmocom/vty/logging.h>
#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/mtp_sap.h>
#include <osmocom/sigtran/sigtran_sap.h>
#include <osmocom/sigtran/sccp_sap.h>
#include <osmocom/sigtran/protocol/mtp.h>

#include "isup_sm.h"
#include "isup_cgm.h"
#include "isup_codec.h"
#include "isup_param.h"
#include "isup_map.h"
#include "isup_m3ua.h"
#include "bearer.h"

SWITCH_MODULE_LOAD_FUNCTION(mod_isup_load);
SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_isup_shutdown);
SWITCH_MODULE_DEFINITION(mod_isup, mod_isup_load, mod_isup_shutdown, NULL);

static switch_endpoint_interface_t *isup_endpoint_interface;

/* ------------------------------------------------------------------ */
/* Data model                                                          */
/* ------------------------------------------------------------------ */

struct isup_profile;

struct ckt_timer { struct isup_ckt *ckt; int id; struct osmo_timer_list t; };

typedef struct isup_ckt {
	struct isup_profile  *profile;
	uint16_t              cic;
	uint32_t              dpc;            /* peer PC for replies (from IAM opc) */
	char                  mgw_endpoint[64];
	struct isup_cic       sm;
	struct bearer_ci     *bci;
	char                  mgw_rtp_ip[48];
	uint16_t              mgw_rtp_port;
	char                  fs_rtp_ip[48];   /* our RTP, told to the MGW via MDCX */
	uint16_t              fs_rtp_port;
	switch_core_session_t *session;
	struct ckt_timer      timers[ISUP_T22 + 1];
	struct osmo_timer_list answer_timer;  /* demo auto-answer ring delay */
} isup_ckt_t;

typedef struct isup_profile {
	char                 name[64];
	struct isup_m3ua     m3ua;
	const struct bearer_driver *bearer;
	char                 gateway[64];
	char                 context[64];
	char                 dialplan[64];
	uint32_t             peer_dpc;      /* default DPC for outbound calls */
	isup_ckt_t          *ckt;
	uint16_t             cic_min, cic_max;
	struct isup_cgm      cgm;           /* circuit-group / maintenance manager */
	uint32_t             cgm_dpc;       /* peer PC to reply to for CGM messages */
	struct osmo_timer_list grs_timer;   /* start-up group reset (Q.764 §2.9.1)  */
	int                  grs_tries;     /* start-up GRS (re)transmission count   */
	int                  grs_acked;     /* peer answered our start-up GRS w/ GRA  */
	int                  autoanswer;    /* demo: answer inbound calls in-module   */
} isup_profile_t;

#define ISUP_GRS_MAX_TRIES  20          /* bound the start-up GRS retransmits   */
#define ISUP_GRS_RETRY_S    1           /* T22-style guard before retransmit    */

typedef struct {
	switch_core_session_t *session;
	switch_channel_t      *channel;
	isup_ckt_t            *ckt;
	/* media endpoint */
	switch_codec_t         read_codec, write_codec;
	switch_timer_t         timer;
	switch_frame_t         read_frame;
	uint8_t                read_buf[SWITCH_RECOMMENDED_BUFFER_SIZE];
	switch_rtp_t          *rtp;        /* RTP to the MGW (NULL until media up) */
	switch_port_t          local_port;
	int                    media_up;
} isup_pvt_t;

#define ISUP_PTIME   20
#define ISUP_RATE    8000
#define ISUP_SAMPLES 160          /* 20ms @ 8kHz */
#define ISUP_PT      8            /* PCMA */

/* FS -> Osmo command marshalling */
enum cmd_type { CMD_ORIGINATE, CMD_PROCEEDING, CMD_ANSWER, CMD_HANGUP, CMD_MEDIA };
struct isup_cmd { enum cmd_type type; isup_ckt_t *ckt; uint8_t cause;
		  uint8_t bci[2]; int bci_set; struct isup_msg iam; };

#define ISUP_MAX_PROFILES 16
static struct {
	void                 *tall;
	isup_profile_t       *profiles[ISUP_MAX_PROFILES]; /* ISUP trunks */
	int                   nprofiles;
	struct osmo_ss7_instance *inst;    /* shared M3UA instance (one STP link) */
	struct osmo_ss7_user *ss7_user;    /* shared SI=5 ISUP user               */
	switch_thread_t      *osmo_thread;
	volatile int          running;
	int                   evfd;
	struct osmo_fd        ofd;
	struct isup_cmd       ring[256];
	volatile unsigned     head, tail;
	switch_mutex_t       *qlock;
	switch_memory_pool_t *pool;
	char                  cs7_cfg[256]; /* shared transport cs7 config path    */
	volatile int          ready;       /* 1 = osmo thread set up, -1 = failed */
	int                   sccp_ssn;    /* >0 enables SCCP (SI=3) bound on this SSN */
	struct osmo_sccp_instance *sccp;
	struct osmo_sccp_user     *sccp_user;
	char                  asp_name[64];
} g;

/* Profiles are independent ISUP trunks over the shared M3UA transport, keyed by
 * name (for outbound dial strings) and by originating point code (to demux
 * inbound messages by their destination point code). */
static isup_profile_t *isup_profile_by_name(const char *name)
{
	int i;
	if (!name) return NULL;
	for (i = 0; i < g.nprofiles; i++)
		if (!strcasecmp(g.profiles[i]->name, name)) return g.profiles[i];
	return NULL;
}

static isup_profile_t *isup_profile_by_opc(uint32_t opc)
{
	int i;
	for (i = 0; i < g.nprofiles; i++)
		if (g.profiles[i]->m3ua.opc == opc) return g.profiles[i];
	return NULL;
}

static int isup_transport_setup(void);

static isup_ckt_t *profile_ckt(isup_profile_t *p, uint16_t cic)
{
	if (!p || cic < p->cic_min || cic > p->cic_max || !p->ckt)
		return NULL;
	return &p->ckt[cic - p->cic_min];
}

/* ------------------------------------------------------------------ */
/* Command queue (FS thread -> Osmo thread)                            */
/* ------------------------------------------------------------------ */

static void cmd_post(const struct isup_cmd *c)
{
	uint64_t one = 1;
	switch_mutex_lock(g.qlock);
	g.ring[g.tail % 256] = *c;
	g.tail++;
	switch_mutex_unlock(g.qlock);
	if (write(g.evfd, &one, sizeof(one)) < 0) { /* ignore */ }
}

static void op_mode(void *user, const char *mode);

static void cmd_dispatch(struct isup_cmd *c)  /* runs on Osmo thread */
{
	switch (c->type) {
	case CMD_ORIGINATE:  isup_sm_originate(&c->ckt->sm, &c->iam); break;
	case CMD_PROCEEDING:
		if (c->bci_set) isup_sm_set_bci(&c->ckt->sm, c->bci[0], c->bci[1]);
		isup_sm_proceeding(&c->ckt->sm);
		break;
	case CMD_ANSWER:     isup_sm_answer(&c->ckt->sm);             break;
	case CMD_HANGUP:     isup_sm_hangup(&c->ckt->sm, c->cause);   break;
	case CMD_MEDIA:      op_mode(c->ckt, "sendrecv");             break;
	}
}

static int evfd_cb(struct osmo_fd *fd, unsigned int what)
{
	uint64_t v;
	(void)what;
	if (read(fd->fd, &v, sizeof(v)) < 0) { /* ignore */ }
	for (;;) {
		struct isup_cmd c;
		switch_mutex_lock(g.qlock);
		if (g.head == g.tail) { switch_mutex_unlock(g.qlock); break; }
		c = g.ring[g.head % 256];
		g.head++;
		switch_mutex_unlock(g.qlock);
		cmd_dispatch(&c);
	}
	return 0;
}

/* ------------------------------------------------------------------ */
/* State machine ops (all executed on the Osmo thread)                 */
/* ------------------------------------------------------------------ */

static void op_send(void *user, const struct isup_msg *m)
{
	isup_ckt_t *c = user;
	uint8_t buf[ISUP_MAX_ENC_LEN];
	int n = isup_encode(m, buf, sizeof(buf));
	int rc = (n > 0) ? isup_m3ua_send(&c->profile->m3ua, c->dpc,
					  (uint8_t)(c->cic & 0xff), buf, (size_t)n) : -99;
	fprintf(stderr, "op_send mt=0x%02x cic=%u dpc=%u enc=%d send_rc=%d\n",
		m->msg_type, c->cic, c->dpc, n, rc);
}

/* Circuit-group manager transmit. The CGM is per-profile (not per-circuit), so
 * it replies to the DPC stashed when the inbound supervision message arrived,
 * falling back to the configured peer for locally-initiated GRS/CGB. */
static void op_cgm_emit(void *user, const struct isup_msg *m)
{
	isup_profile_t *p = user;
	uint8_t buf[ISUP_MAX_ENC_LEN];
	int n = isup_encode(m, buf, sizeof(buf));
	uint32_t dpc = p->cgm_dpc ? p->cgm_dpc : p->peer_dpc;
	int rc = (n > 0) ? isup_m3ua_send(&p->m3ua, dpc,
					  (uint8_t)(m->cic & 0xff), buf, (size_t)n) : -99;
	fprintf(stderr, "cgm_emit mt=0x%02x cic=%u dpc=%u enc=%d send_rc=%d\n",
		m->msg_type, m->cic, dpc, n, rc);
}
static const struct isup_cgm_ops CGM_OPS = { .emit = op_cgm_emit };

static void on_bearer_ready(void *user, int ok, const char *rtp_ip, uint16_t rtp_port)
{
	isup_ckt_t *c = user;
	fprintf(stderr, "bearer_ready ok=%d rtp=%s:%u\n", ok, rtp_ip ? rtp_ip : "-", rtp_port);
	if (ok && rtp_ip) {
		switch_copy_string(c->mgw_rtp_ip, rtp_ip, sizeof(c->mgw_rtp_ip));
		c->mgw_rtp_port = rtp_port;
		isup_sm_bearer_ready(&c->sm);
	} else {
		isup_sm_hangup(&c->sm, 41);
	}
}

static void op_crcx(void *user, const char *mode)
{
	isup_ckt_t *c = user;
	fprintf(stderr, "op_crcx cic=%u mode=%s gw=%s ep=%s\n",
		c->cic, mode, c->profile->gateway, c->mgw_endpoint);
	if (c->profile->bearer)
		c->profile->bearer->crcx(c->profile->gateway, c->mgw_endpoint, mode,
					 on_bearer_ready, c, &c->bci);
}
static void op_mode(void *user, const char *mode)
{
	isup_ckt_t *c = user;
	/* point the MGW at FreeSWITCH's real RTP (filled by isup_media_start) */
	if (c->profile->bearer && c->bci)
		c->profile->bearer->mdcx(c->bci, mode,
					 c->fs_rtp_ip[0] ? c->fs_rtp_ip : NULL, c->fs_rtp_port);
}
static void op_dlcx(void *user)
{
	isup_ckt_t *c = user;
	if (c->profile->bearer && c->bci) { c->profile->bearer->dlcx(c->bci); c->bci = NULL; }
}

static void op_fs_setup(void *user, const struct isup_msg *iam)
{
	isup_ckt_t *c = user;
	switch_core_session_t *session;
	switch_caller_profile_t *caller;
	isup_pvt_t *pvt;
	const struct isup_param *p;
	struct isup_number called, calling;
	char cidnum[32] = "", dnis[32] = "";

	memset(&called, 0, sizeof(called));
	memset(&calling, 0, sizeof(calling));
	if ((p = isup_msg_get(iam, ISUP_P_CALLED_NUMBER)))  isup_dec_called(&called, p->val, p->len);
	if ((p = isup_msg_get(iam, ISUP_P_CALLING_NUMBER))) isup_dec_calling(&calling, p->val, p->len);
	isup_number_to_e164(&called, dnis, sizeof(dnis));
	isup_number_to_e164(&calling, cidnum, sizeof(cidnum));

	session = switch_core_session_request(isup_endpoint_interface,
					      SWITCH_CALL_DIRECTION_INBOUND, SOF_NO_LIMITS, NULL);
	if (!session) return;
	pvt = switch_core_session_alloc(session, sizeof(*pvt));
	pvt->session = session;
	pvt->channel = switch_core_session_get_channel(session);
	pvt->ckt = c;
	c->session = session;
	switch_core_session_set_private(session, pvt);

	caller = switch_caller_profile_new(switch_core_session_get_pool(session),
					   "isup", c->profile->dialplan, NULL, cidnum,
					   NULL, cidnum, NULL, NULL, "mod_isup",
					   c->profile->context, dnis[0] ? dnis : "0");
	if (isup_calling_is_clir(&calling))
		caller->flags |= SWITCH_CPF_HIDE_NAME | SWITCH_CPF_HIDE_NUMBER;
	switch_channel_set_caller_profile(pvt->channel, caller);
	switch_channel_set_state(pvt->channel, CS_INIT);

	if (c->profile->autoanswer) {
		/* Demo path (no dialplan module available): proper flow — send ACM
		 * (alerting) now, then ANM after a 1.5s ring. CMD_PROCEEDING is
		 * deferred via the queue (we are inside isup_sm_rx); the ring delay
		 * uses an osmo_timer that answers on the Osmo thread. */
		struct isup_cmd p1 = { CMD_PROCEEDING, c, 0, {0}, 0, {0} };
		/* ACM "subscriber free, alerting" */
		isup_sm_set_bci(&c->sm, 0x12, 0x14);
		cmd_post(&p1);
		osmo_timer_schedule(&c->answer_timer, 1, 500000);
	} else {
		switch_core_session_thread_launch(session);
	}
}

static void op_fs_progress(void *user, int answered)
{
	isup_ckt_t *c = user;
	if (c->session) {
		switch_channel_t *ch = switch_core_session_get_channel(c->session);
		if (answered) switch_channel_mark_pre_answered(ch);
		else switch_channel_mark_ring_ready(ch);
	}
}
static void op_fs_answer(void *user)
{
	isup_ckt_t *c = user;
	if (c->session) switch_channel_mark_answered(switch_core_session_get_channel(c->session));
}
static void op_fs_release(void *user, uint8_t cause)
{
	isup_ckt_t *c = user;
	if (c->session)
		switch_channel_hangup(switch_core_session_get_channel(c->session),
				      (switch_call_cause_t)cause);
}

static void timer_cb(void *data)
{
	struct ckt_timer *ct = data;
	isup_sm_timer(&ct->ckt->sm, ct->id);
}
/* demo auto-answer: fires after the ring delay, on the Osmo thread */
static void demo_answer_cb(void *data)
{
	isup_ckt_t *c = data;
	isup_sm_answer(&c->sm);
}
/* True if any circuit on the profile is carrying a call (not idle). */
static int profile_call_active(isup_profile_t *p)
{
	int i, n = p->cic_max - p->cic_min + 1;
	for (i = 0; i < n; i++)
		if (p->ckt[i].sm.state != CIC_IDLE)
			return 1;
	return 0;
}

/* Q.764 §2.9.1 start-up group reset, with T22-style retransmission. We (re)send
 * a GRS over our circuit span until the peer acknowledges with GRA (which the
 * circuit-group manager records by clearing reset_pending). Retransmitting is
 * what makes this robust against the boot race: at start-up the reverse MTP
 * route to the peer may not be installed yet and the first GRS is dropped by
 * the STP ("no route"); a later attempt gets through once the route is up.
 * Runs on the Osmo thread. Never resets while a call is up, and stops once the
 * group is confirmed idle or the retry budget is exhausted. */
static void startup_grs_cb(void *data)
{
	isup_profile_t *p = data;
	if (p->cic_max < p->cic_min)
		return;

	/* Stop once the peer has answered our GRS with a GRA, if a call has come up
	 * (never reset a live circuit), or after the retry budget. We key on a
	 * received GRA rather than reset_pending: receiving the *peer's* GRS also
	 * clears reset_pending, which would otherwise look like our own reset was
	 * acknowledged and stop us retransmitting before our GRS ever got through
	 * (the early attempts fail with "no route" until the reverse link is up). */
	if (p->grs_acked || profile_call_active(p) ||
	    p->grs_tries >= ISUP_GRS_MAX_TRIES)
		return;

	isup_cgm_reset_all(&p->cgm);   /* (re)send GRS over the span */
	p->grs_tries++;
	osmo_timer_schedule(&p->grs_timer, ISUP_GRS_RETRY_S, 0);
}

/* The start-up group reset has resolved once the peer has acknowledged it with
 * a GRA, or we have exhausted the retransmit budget (gave up — the peer may not
 * implement GRS). Until then the circuits' post-restart state is not agreed with
 * the peer, so calls must not be placed on them (Q.764 §2.9.1). */
static int isup_reset_settled(const isup_profile_t *p)
{
	return p->grs_acked || p->grs_tries >= ISUP_GRS_MAX_TRIES;
}
static void op_tstart(void *user, int id, int ms)
{
	isup_ckt_t *c = user;
	if (id >= 0 && id <= ISUP_T22)
		osmo_timer_schedule(&c->timers[id].t, ms / 1000, (ms % 1000) * 1000);
}
static void op_tstop(void *user, int id)
{
	isup_ckt_t *c = user;
	if (id >= 0 && id <= ISUP_T22)
		osmo_timer_del(&c->timers[id].t);
}

static const struct isup_sm_ops ISUP_OPS = {
	.send = op_send, .bearer_crcx = op_crcx, .bearer_mode = op_mode,
	.bearer_dlcx = op_dlcx, .fs_setup = op_fs_setup, .fs_progress = op_fs_progress,
	.fs_answer = op_fs_answer, .fs_release = op_fs_release,
	.start_timer = op_tstart, .stop_timer = op_tstop,
};

/* ------------------------------------------------------------------ */
/* M3UA receive (Osmo thread) -> decode -> circuit FSM                 */
/* ------------------------------------------------------------------ */

static int isup_prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	struct osmo_mtp_prim *omp = (struct osmo_mtp_prim *)oph;
	(void)ctx;

	if (oph->sap == MTP_SAP_USER &&
	    OSMO_PRIM_HDR(oph) == OSMO_PRIM(OSMO_MTP_PRIM_TRANSFER, PRIM_OP_INDICATION)) {
		struct isup_msg m;
		const uint8_t *d = msgb_l2(oph->msg);
		unsigned int n = msgb_l2len(oph->msg);
		if (d && n && isup_decode(&m, d, n) == ISUP_OK) {
			/* Demux to the profile whose OPC is this message's destination
			 * point code — that is the trunk the message is addressed to. */
			isup_profile_t *p = isup_profile_by_opc(omp->u.transfer.dpc);
			isup_ckt_t *c;
			uint32_t src = omp->u.transfer.opc; /* reply to sender */
			if (!p) goto drop;
			c = profile_ckt(p, m.cic);
			if (c) c->dpc = src;
			p->cgm_dpc = src;

			switch (m.msg_type) {
			case ISUP_MT_GRS: {
				/* Group reset (Q.764 §2.9): release every active call in the
				 * range, then the circuit-group manager clears blocking state
				 * and answers with a single GRA. */
				const struct isup_param *rs = isup_msg_get(&m, ISUP_P_RANGE_AND_STATUS);
				int range = (rs && rs->len >= 1) ? rs->val[0] : 0, i;
				for (i = 0; i <= range; i++) {
					isup_ckt_t *cc = profile_ckt(p, (uint16_t)(m.cic + i));
					if (cc) { cc->dpc = src; isup_sm_reset(&cc->sm); }
				}
				isup_cgm_rx(&p->cgm, &m);
				break;
			}
			case ISUP_MT_RSC:
				/* Single-circuit reset: the call SM releases the call and
				 * returns RLC; the CGM clears the maintenance blocking state. */
				if (c) isup_sm_rx(&c->sm, &m);
				isup_cgm_rx(&p->cgm, &m);
				break;
			default:
				/* A GRA acknowledges our start-up group reset — record it so
				 * the retransmit loop stops (distinct from the peer's own GRS,
				 * which the manager also treats as clearing reset_pending). */
				if (m.msg_type == ISUP_MT_GRA)
					p->grs_acked = 1;
				/* Circuit maintenance / group supervision (BLO/UBL/CGB/CGU/
				 * CQM/...) is consumed by the circuit-group manager; anything
				 * it does not own is a call message for the per-circuit SM. */
				if (!isup_cgm_rx(&p->cgm, &m)) {
					if (c) isup_sm_rx(&c->sm, &m);
				}
				break;
			}
		}
	}
drop:
	if (oph->msg) msgb_free(oph->msg);
	return 0;
}

/* SCCP receive (SI=3). N-UNITDATA / N-CONNECT indications for a future TCAP
 * layer would be dispatched here; for now they are accepted and freed. */
static int isup_sccp_prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	(void)ctx;
	if (oph && oph->msg)
		msgb_free(oph->msg);
	return 0;
}

/* ------------------------------------------------------------------ */
/* Osmo thread                                                         */
/* ------------------------------------------------------------------ */

static void *SWITCH_THREAD_FUNC osmo_thread_run(switch_thread_t *thread, void *obj)
{
	(void)thread; (void)obj;
	/* libosmocore keeps per-thread talloc + select state; initialise it on
	 * THIS thread before any osmo_fd/timer/ss7 work, then keep all osmo state
	 * here (the thread that runs osmo_select_main). */
	osmo_ctx_init("isup");
	osmo_select_init();
	if (isup_transport_setup() < 0) {
		g.ready = -1;
		return NULL;
	}
	g.ready = 1;
	while (g.running)
		osmo_select_main(0);
	return NULL;
}

/* ------------------------------------------------------------------ */
/* FreeSWITCH endpoint io + state handlers                             */
/* ------------------------------------------------------------------ */

/* PCMA read/write codec + 20ms timer (media layer); idempotent */
static void isup_codecs_up(isup_pvt_t *pvt)
{
	switch_memory_pool_t *pool = switch_core_session_get_pool(pvt->session);
	if (pvt->media_up) return;
	if (switch_core_codec_init(&pvt->read_codec, "PCMA", NULL, NULL, ISUP_RATE, ISUP_PTIME, 1,
				   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool) != SWITCH_STATUS_SUCCESS)
		return;
	if (switch_core_codec_init(&pvt->write_codec, "PCMA", NULL, NULL, ISUP_RATE, ISUP_PTIME, 1,
				   SWITCH_CODEC_FLAG_ENCODE | SWITCH_CODEC_FLAG_DECODE, NULL, pool) != SWITCH_STATUS_SUCCESS)
		return;
	switch_core_session_set_read_codec(pvt->session, &pvt->read_codec);
	switch_core_session_set_write_codec(pvt->session, &pvt->write_codec);
	switch_core_timer_init(&pvt->timer, "soft", ISUP_PTIME, ISUP_SAMPLES, pool);
	pvt->read_frame.codec = &pvt->read_codec;
	pvt->read_frame.data = pvt->read_buf;
	pvt->read_frame.buflen = sizeof(pvt->read_buf);
	/* tell the core this channel carries audio, so it calls read/write_frame */
	switch_channel_set_flag(pvt->channel, CF_AUDIO);
	switch_channel_set_flag(pvt->channel, CF_ACCEPT_CNG);
	pvt->media_up = 1;
}

/* Real RTP to the MGW once its RTP address is known (from CRCX); idempotent.
 * Records our local RTP on the circuit so op_mode()'s MDCX points the MGW back
 * at us. With a deterministic MGW endpoint per CIC the two legs bridge. */
static void isup_rtp_up(isup_pvt_t *pvt)
{
	isup_ckt_t *c = pvt->ckt;
	switch_rtp_flag_t flags[SWITCH_RTP_FLAG_INVALID] = { 0 };
	const char *err = NULL, *lip;
	switch_memory_pool_t *pool = switch_core_session_get_pool(pvt->session);

	if (pvt->rtp || !c || !c->mgw_rtp_port) return;
	isup_codecs_up(pvt);
	lip = switch_core_get_variable("local_ip_v4");
	if (zstr(lip)) lip = "127.0.0.1";
	pvt->local_port = switch_rtp_request_port(lip);
	/* switch_rtp_new's "ms_per_packet" arg is actually MICROSECONDS per packet */
	pvt->rtp = switch_rtp_new(lip, pvt->local_port, c->mgw_rtp_ip, c->mgw_rtp_port, ISUP_PT,
				  ISUP_SAMPLES, ISUP_PTIME * 1000, flags, "soft", &err, pool, 0, 0);
	if (!pvt->rtp) { fprintf(stderr, "isup rtp_new failed: %s\n", err ? err : "?"); return; }
	switch_copy_string(c->fs_rtp_ip, lip, sizeof(c->fs_rtp_ip));
	c->fs_rtp_port = pvt->local_port;
	fprintf(stderr, "isup media cic=%u FS %s:%u <-> MGW %s:%u\n",
		c->cic, lip, pvt->local_port, c->mgw_rtp_ip, c->mgw_rtp_port);
	/* Push our just-learned RTP address to the MGW via MDCX (on the Osmo
	 * thread). For the originating leg the SM's earlier MDCX raced ahead of
	 * RTP setup, so the MGW still has r=NULL for our connection and cannot
	 * bridge the far end's audio to us until we re-MDCX here. */
	{ struct isup_cmd m = { CMD_MEDIA, c, 0, {0}, 0, {0} }; cmd_post(&m); }
}

static switch_status_t channel_on_init(switch_core_session_t *session)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	switch_channel_set_state(switch_core_session_get_channel(session), CS_ROUTING);
	if (pvt) { isup_codecs_up(pvt); isup_rtp_up(pvt); }
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_hangup(switch_core_session_t *session)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	if (pvt) {
		if (pvt->rtp) { switch_rtp_destroy(&pvt->rtp); pvt->rtp = NULL; }
		if (pvt->ckt) {
			struct isup_cmd c = { CMD_HANGUP, pvt->ckt,
				(uint8_t)switch_channel_get_cause(switch_core_session_get_channel(session)),
				{0}, 0, {0} };
			pvt->ckt->session = NULL;
			cmd_post(&c);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_on_destroy(switch_core_session_t *session)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	if (pvt && pvt->media_up) {
		if (switch_core_codec_ready(&pvt->read_codec))  switch_core_codec_destroy(&pvt->read_codec);
		if (switch_core_codec_ready(&pvt->write_codec)) switch_core_codec_destroy(&pvt->write_codec);
		if (pvt->timer.interval) switch_core_timer_destroy(&pvt->timer);
	}
	return SWITCH_STATUS_SUCCESS;
}
static switch_status_t channel_kill_channel(switch_core_session_t *session, int sig) { (void)session; (void)sig; return SWITCH_STATUS_SUCCESS; }

static switch_status_t channel_read_frame(switch_core_session_t *session, switch_frame_t **frame,
					  switch_io_flag_t flags, int stream_id)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	(void)stream_id;
	if (!pvt) return SWITCH_STATUS_FALSE;
	isup_rtp_up(pvt);
	if (pvt->rtp && switch_rtp_ready(pvt->rtp)) {
		if (switch_rtp_zerocopy_read_frame(pvt->rtp, &pvt->read_frame, flags) != SWITCH_STATUS_SUCCESS)
			return SWITCH_STATUS_FALSE;
		/* the frame struct is shared with the pre-media CNG path; a real RTP
		 * payload (non-zero, not the PT-13 comfort-noise type) is live audio */
		if (pvt->read_frame.datalen && pvt->read_frame.payload != 13)
			switch_clear_flag(&pvt->read_frame, SFF_CNG);
		pvt->read_frame.codec = &pvt->read_codec;
		*frame = &pvt->read_frame;
		return SWITCH_STATUS_SUCCESS;
	}
	/* media not up yet: pace + comfort noise */
	if (pvt->media_up) {
		switch_core_timer_next(&pvt->timer);
		pvt->read_frame.datalen = pvt->read_codec.implementation->decoded_bytes_per_packet;
	} else {
		switch_yield(20000);
		pvt->read_frame.datalen = 0;
	}
	pvt->read_frame.flags = SFF_CNG;
	*frame = &pvt->read_frame;
	return SWITCH_STATUS_SUCCESS;
}
static switch_status_t channel_write_frame(switch_core_session_t *session, switch_frame_t *frame,
					   switch_io_flag_t flags, int stream_id)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	(void)flags; (void)stream_id;
	if (pvt && pvt->rtp && switch_rtp_ready(pvt->rtp))
		switch_rtp_write_frame(pvt->rtp, frame);
	return SWITCH_STATUS_SUCCESS;
}

static switch_status_t channel_receive_message(switch_core_session_t *session, switch_core_session_message_t *msg)
{
	isup_pvt_t *pvt = switch_core_session_get_private(session);
	if (!pvt || !pvt->ckt) return SWITCH_STATUS_SUCCESS;
	switch (msg->message_id) {
	case SWITCH_MESSAGE_INDICATE_RINGING:
	case SWITCH_MESSAGE_INDICATE_PROGRESS: {
		struct isup_cmd c = { CMD_PROCEEDING, pvt->ckt, 0, {0}, 0, {0} };
		/* ACM Backward Call Indicators settable via channel var isup_bci
		 * (4 hex digits), e.g. export isup_bci=1614 */
		const char *bci = switch_channel_get_variable(
			switch_core_session_get_channel(session), "isup_bci");
		if (bci) { long v = strtol(bci, NULL, 16);
			   c.bci[0] = (v >> 8) & 0xff; c.bci[1] = v & 0xff; c.bci_set = 1; }
		cmd_post(&c);
		break;
	}
	case SWITCH_MESSAGE_INDICATE_ANSWER: {
		struct isup_cmd c = { CMD_ANSWER, pvt->ckt, 0, {0} };
		cmd_post(&c);
		break;
	}
	default: break;
	}
	return SWITCH_STATUS_SUCCESS;
}

/* ---- originate channel-variable parsing (the {var=val} on the dial string) ---- */
static const char *ov(switch_event_t *e, const char *n) { return e ? switch_event_get_header(e, n) : NULL; }
static long ov_l(switch_event_t *e, const char *n, long def, int base)
{ const char *v = ov(e, n); return v ? strtol(v, NULL, base) : def; }

static uint8_t cpc_parse(const char *v, uint8_t def)
{
	if (!v) return def;
	if (!strcasecmp(v, "ordinary")) return ISUP_CPC_ORDINARY;
	if (!strcasecmp(v, "operator")) return ISUP_CPC_OPERATOR_EN;
	if (!strcasecmp(v, "payphone")) return ISUP_CPC_PAYPHONE;
	if (!strcasecmp(v, "test"))     return ISUP_CPC_TEST_CALL;
	if (!strcasecmp(v, "priority")) return ISUP_CPC_PRIORITY;
	if (!strcasecmp(v, "data"))     return ISUP_CPC_DATA_CALL;
	return (uint8_t)strtol(v, NULL, 0);
}
static uint8_t tmr_parse(const char *v, uint8_t def)
{
	if (!v) return def;
	if (!strcasecmp(v, "speech"))       return ISUP_TMR_SPEECH;
	if (!strcasecmp(v, "3k1") || !strcasecmp(v, "3.1khz")) return ISUP_TMR_3K1_AUDIO;
	if (!strcasecmp(v, "64k") || !strcasecmp(v, "unrestricted")) return ISUP_TMR_64K_UNRESTRICTED;
	return (uint8_t)strtol(v, NULL, 0);
}
static uint8_t noa_parse(const char *v, uint8_t def)
{
	if (!v) return def;
	if (!strcasecmp(v, "subscriber"))    return ISUP_NOA_SUBSCRIBER;
	if (!strcasecmp(v, "national"))      return ISUP_NOA_NATIONAL;
	if (!strcasecmp(v, "international")) return ISUP_NOA_INTERNATIONAL;
	return (uint8_t)strtol(v, NULL, 0);
}

static switch_call_cause_t channel_outgoing_channel(switch_core_session_t *session,
		switch_event_t *var_event, switch_caller_profile_t *outbound_profile,
		switch_core_session_t **new_session, switch_memory_pool_t **pool,
		switch_originate_flag_t flags, switch_call_cause_t *cancel_cause)
{
	isup_profile_t *profile = NULL;
	isup_ckt_t *c = NULL;
	isup_pvt_t *pvt;
	switch_core_session_t *nsession;
	switch_channel_t *channel;
	struct isup_cmd cmd; int cic, nlen;
	struct isup_number called; uint8_t nbuf[32];
	uint8_t nci = 0, fci[2] = { 0x60, 0x10 }, cpc = ISUP_CPC_ORDINARY, tmr = ISUP_TMR_SPEECH;
	const char *number;
	(void)var_event; (void)cancel_cause;

	if (!outbound_profile || zstr(outbound_profile->destination_number))
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;

	/* Dial string is  isup/<profile>/<number>  — FreeSWITCH hands us
	 * "<profile>/<number>" as the destination; split off the trunk name. */
	{
		const char *dest = outbound_profile->destination_number;
		const char *slash = strchr(dest, '/');
		if (slash) {
			char pname[64];
			size_t plen = (size_t)(slash - dest);
			if (plen >= sizeof(pname)) plen = sizeof(pname) - 1;
			memcpy(pname, dest, plen);
			pname[plen] = 0;
			profile = isup_profile_by_name(pname);
			number = slash + 1;
		} else {
			profile = (g.nprofiles == 1) ? g.profiles[0] : NULL;
			number = dest;
		}
	}
	if (!profile) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_isup: originate to unknown ISUP profile in '%s'\n",
			outbound_profile->destination_number);
		return SWITCH_CAUSE_INVALID_NUMBER_FORMAT;
	}

	/* Q.764 §2.9.1: hold off call origination until the start-up circuit-group
	 * reset has resolved with the peer; the circuits are not yet known-idle at
	 * both ends. An early call gets a temporary failure (caller may retry). */
	if (!isup_reset_settled(profile)) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
			"mod_isup: rejecting originate — start-up group reset not yet complete\n");
		return SWITCH_CAUSE_NORMAL_TEMPORARY_FAILURE;
	}

	for (cic = profile->cic_min; cic <= profile->cic_max; cic++) {
		isup_ckt_t *cand = profile_ckt(profile, (uint16_t)cic);
		if (cand && cand->sm.state == CIC_IDLE && !cand->session) { c = cand; break; }
	}
	if (!c) return SWITCH_CAUSE_NORMAL_CIRCUIT_CONGESTION;

	nsession = switch_core_session_request(isup_endpoint_interface,
					       SWITCH_CALL_DIRECTION_OUTBOUND,
					       flags | SOF_NO_LIMITS, pool);
	if (!nsession) return SWITCH_CAUSE_DESTINATION_OUT_OF_ORDER;
	pvt = switch_core_session_alloc(nsession, sizeof(*pvt));
	pvt->session = nsession;
	pvt->channel = switch_core_session_get_channel(nsession);
	pvt->ckt = c;
	c->session = nsession;
	c->dpc = profile->peer_dpc; /* route outbound IAM to the peer exchange */
	switch_core_session_set_private(nsession, pvt);
	channel = switch_core_session_get_channel(nsession);
	switch_channel_set_caller_profile(channel, switch_caller_profile_clone(nsession, outbound_profile));

	/* Mandatory IAM parameters — settable via originate channel variables,
	 * e.g.  originate {isup_tmr=3k1,isup_cpc=payphone,isup_fci=6011,
	 *                  isup_calling_number=0738900000}isup/lab/1002 */
	nci = (uint8_t)ov_l(var_event, "isup_nci", 0x00, 16);
	{ long f = ov_l(var_event, "isup_fci", 0x6010, 16); fci[0] = (f >> 8) & 0xff; fci[1] = f & 0xff; }
	cpc = cpc_parse(ov(var_event, "isup_cpc"), ISUP_CPC_ORDINARY);
	tmr = tmr_parse(ov(var_event, "isup_tmr"), ISUP_TMR_SPEECH);

	memset(&called, 0, sizeof(called));
	called.nature = noa_parse(ov(var_event, "isup_called_noa"), ISUP_NOA_NATIONAL);
	called.npi    = (uint8_t)ov_l(var_event, "isup_called_npi", ISUP_NPI_ISDN, 0);
	switch_copy_string(called.digits, number, sizeof(called.digits));
	nlen = isup_enc_called(&called, nbuf, sizeof(nbuf));

	memset(&cmd, 0, sizeof(cmd));
	cmd.type = CMD_ORIGINATE; cmd.ckt = c;
	isup_msg_init(&cmd.iam, c->cic, ISUP_MT_IAM);
	isup_msg_add(&cmd.iam, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(&cmd.iam, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(&cmd.iam, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(&cmd.iam, ISUP_P_TMR, &tmr, 1);
	if (nlen > 0) isup_msg_add(&cmd.iam, ISUP_P_CALLED_NUMBER, nbuf, (uint8_t)nlen);

	/* Optional: calling party number (from var or caller-id), with CLIR */
	{
		const char *cgnum = ov(var_event, "isup_calling_number");
		if (!cgnum && !zstr(outbound_profile->caller_id_number))
			cgnum = outbound_profile->caller_id_number;
		if (cgnum && *cgnum) {
			struct isup_number cg; uint8_t cb[32]; int cl;
			const char *pres = ov(var_event, "isup_calling_pres");
			memset(&cg, 0, sizeof(cg));
			cg.nature = noa_parse(ov(var_event, "isup_calling_noa"), ISUP_NOA_NATIONAL);
			cg.npi = ISUP_NPI_ISDN;
			cg.apri = ((pres && !strcasecmp(pres, "restricted")) ||
				   (outbound_profile->flags & SWITCH_CPF_HIDE_NUMBER))
				  ? ISUP_APRI_RESTRICTED : ISUP_APRI_ALLOWED;
			cg.screening = (uint8_t)ov_l(var_event, "isup_calling_screening",
						     ISUP_SCR_NETWORK_PROVIDED, 0);
			switch_copy_string(cg.digits, cgnum, sizeof(cg.digits));
			cl = isup_enc_calling(&cg, cb, sizeof(cb));
			if (cl > 0) isup_msg_add(&cmd.iam, ISUP_P_CALLING_NUMBER, cb, (uint8_t)cl);
		}
	}
	/* Optional: hop counter */
	{ const char *hc = ov(var_event, "isup_hop_counter");
	  if (hc) { uint8_t h = (uint8_t)atoi(hc); isup_msg_add(&cmd.iam, ISUP_P_HOP_COUNTER, &h, 1); } }

	switch_channel_set_state(channel, CS_INIT);
	cmd_post(&cmd);
	*new_session = nsession;
	return SWITCH_CAUSE_SUCCESS;
}

static switch_state_handler_table_t isup_state_handlers = {
	.on_init = channel_on_init, .on_hangup = channel_on_hangup, .on_destroy = channel_on_destroy,
};
static switch_io_routines_t isup_io_routines = {
	.outgoing_channel = channel_outgoing_channel, .read_frame = channel_read_frame,
	.write_frame = channel_write_frame, .kill_channel = channel_kill_channel,
	.receive_message = channel_receive_message,
};

/* ------------------------------------------------------------------ */
/* OAM: fs_cli / API  —  "isup status|m3ua|mgw|cic|sccp"               */
/* ------------------------------------------------------------------ */
#define ISUP_API_SYNTAX "status | m3ua | mgw | cic | sccp"

static struct osmo_ss7_asp *isup_find_asp(void)
{
	if (!g.inst || !g.asp_name[0]) return NULL;
	/* By name, so the ASP's SCTP local (source) port is free to be pinned in
	 * the cs7 config (needed to run two exchanges off one source IP). */
	return osmo_ss7_asp_find_by_name(g.inst, g.asp_name);
}

SWITCH_STANDARD_API(isup_api_function)
{
	struct osmo_ss7_asp *asp = isup_find_asp();
	int asp_up = asp && osmo_ss7_asp_active(asp);
	const char *sub = zstr(cmd) ? "status" : cmd;
	int pi, i;

	(void)session;
	if (g.nprofiles == 0) { stream->write_function(stream, "-ERR mod_isup not ready\n"); return SWITCH_STATUS_SUCCESS; }

	if (!strcasecmp(sub, "status")) {
		stream->write_function(stream, "M3UA ASP %-8s : %s   (%d profile(s))\n",
				       g.asp_name, asp_up ? "ACTIVE" : "down", g.nprofiles);
		stream->write_function(stream, "SCCP (SI=3)      : %s", g.sccp_ssn ? "enabled" : "disabled");
		if (g.sccp_ssn) stream->write_function(stream, " (SSN=%d)", g.sccp_ssn);
		stream->write_function(stream, "\n");
		for (pi = 0; pi < g.nprofiles; pi++) {
			isup_profile_t *p = g.profiles[pi];
			int n = p->cic_max - p->cic_min + 1, busy = 0;
			for (i = 0; i < n; i++) if (p->ckt[i].sm.state != CIC_IDLE) busy++;
			stream->write_function(stream,
				"profile '%s': OPC=%u  peer-DPC=%u  NI=%u  MGW=%s  CIC %u-%u (%d in use)\n",
				p->name, p->m3ua.opc, p->peer_dpc, p->m3ua.ni, p->gateway,
				p->cic_min, p->cic_max, busy);
		}
	} else if (!strcasecmp(sub, "m3ua")) {
		stream->write_function(stream, "M3UA ASP %s: %s\n", g.asp_name, asp_up ? "ACTIVE" : "down");
		stream->write_function(stream, "  cs7 config : %s\n", g.cs7_cfg);
		for (pi = 0; pi < g.nprofiles; pi++) {
			isup_profile_t *p = g.profiles[pi];
			stream->write_function(stream, "  profile '%s': OPC=%u  peer-DPC=%u  NI=%u\n",
					       p->name, p->m3ua.opc, p->peer_dpc, p->m3ua.ni);
		}
	} else if (!strcasecmp(sub, "mgw")) {
		for (pi = 0; pi < g.nprofiles; pi++) {
			isup_profile_t *p = g.profiles[pi];
			int n = p->cic_max - p->cic_min + 1;
			stream->write_function(stream, "profile '%s'  MGW %s  driver=%s\n",
					       p->name, p->gateway, p->bearer ? p->bearer->name : "-");
			for (i = 0; i < n; i++) {
				isup_ckt_t *c = &p->ckt[i];
				if (c->mgw_rtp_port || c->sm.state != CIC_IDLE)
					stream->write_function(stream, "  CIC %-3u  ep=%-16s  MGW-RTP=%s:%u  FS-RTP=%s:%u\n",
							       c->cic, c->mgw_endpoint, c->mgw_rtp_ip, c->mgw_rtp_port,
							       c->fs_rtp_ip[0] ? c->fs_rtp_ip : "-", c->fs_rtp_port);
			}
		}
	} else if (!strcasecmp(sub, "cic")) {
		for (pi = 0; pi < g.nprofiles; pi++) {
			isup_profile_t *p = g.profiles[pi];
			int n = p->cic_max - p->cic_min + 1;
			stream->write_function(stream, "profile '%s':\n", p->name);
			stream->write_function(stream, "  %-5s %-15s %-7s %s\n", "CIC", "state", "session", "MGW-RTP");
			for (i = 0; i < n; i++) {
				isup_ckt_t *c = &p->ckt[i];
				stream->write_function(stream, "  %-5u %-15s %-7s %s:%u\n",
						       c->cic, isup_state_name(c->sm.state),
						       c->session ? "yes" : "-", c->mgw_rtp_ip, c->mgw_rtp_port);
			}
		}
	} else if (!strcasecmp(sub, "sccp")) {
		stream->write_function(stream, "SCCP (SI=3): %s\n", g.sccp_ssn ? "enabled" : "disabled");
		if (g.sccp_ssn) stream->write_function(stream, "  bound SSN  : %d\n  user       : %p\n",
						       g.sccp_ssn, (void *)g.sccp_user);
	} else {
		stream->write_function(stream, "-USAGE: isup %s\n", ISUP_API_SYNTAX);
	}
	return SWITCH_STATUS_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Module load / unload                                                */
/* ------------------------------------------------------------------ */

/* No module-specific log categories; osmo's own defaults still register. */
static const struct log_info g_log_info = { .cat = NULL, .num_cat = 0 };
static struct vty_app_info g_vty_info = { .name = "mod_isup", .version = "0" };

/* Bring up one profile's circuits, timers, circuit-group manager and start-up
 * group reset over the already-established shared transport. */
static void isup_profile_bring_up(isup_profile_t *p)
{
	int i, t;

	p->m3ua.inst = g.inst;
	p->m3ua.ss7_user = g.ss7_user;

	for (i = 0; i < (p->cic_max - p->cic_min + 1); i++) {
		isup_ckt_t *c = &p->ckt[i];
		c->profile = p;
		c->cic = (uint16_t)(p->cic_min + i);
		c->dpc = p->m3ua.opc;
		snprintf(c->mgw_endpoint, sizeof(c->mgw_endpoint), "rtpbridge/%u@mgw", c->cic);
		isup_sm_init(&c->sm, c->cic, &ISUP_OPS, c, (c->cic % 2) == 1);
		for (t = 0; t <= ISUP_T22; t++) {
			c->timers[t].ckt = c; c->timers[t].id = t;
			osmo_timer_setup(&c->timers[t].t, timer_cb, &c->timers[t]);
		}
		osmo_timer_setup(&c->answer_timer, demo_answer_cb, c);
	}

	/* circuit-group / maintenance manager spanning this profile's CICs, then a
	 * start-up group reset once the ASP is ACTIVE (Q.764 §2.9.1). */
	isup_cgm_init(&p->cgm, p->cic_min, p->cic_max - p->cic_min + 1, &CGM_OPS, p);
	osmo_timer_setup(&p->grs_timer, startup_grs_cb, p);
	osmo_timer_schedule(&p->grs_timer, ISUP_GRS_RETRY_S, 0); /* once ASP is up */
}

/* Establish the shared M3UA transport (one instance / ASP / SI=5 user for the
 * whole module), then bring up every configured profile over it. */
static int isup_transport_setup(void)
{
	struct osmo_ss7_instance *inst;
	struct osmo_ss7_user *user;
	int pi;

	g.tall = talloc_named_const(NULL, 0, "mod_isup");
	osmo_init_logging2(g.tall, &g_log_info);
	osmo_ss7_init();
	vty_init(&g_vty_info);
	logging_vty_add_cmds();
	osmo_ss7_vty_init_sg(g.tall); /* superset of _asp; adds route-table/listen */
	if (vty_read_config_file(g.cs7_cfg, NULL) < 0) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR,
				  "mod_isup: failed to read cs7 config %s\n", g.cs7_cfg);
		return -1;
	}
	inst = osmo_ss7_instance_find(0);
	if (!inst) return -1;
	g.inst = inst;

	/* Kick the configured (client) ASP into connecting to the STP. */
	if (g.asp_name[0]) {
		struct osmo_ss7_asp *asp = osmo_ss7_asp_find_by_name(inst, g.asp_name);
		if (asp) osmo_ss7_asp_restart(asp);
	}

	/* One shared SI=5 ISUP user for the transport; the prim_cb demuxes inbound
	 * messages to the owning profile by destination point code. */
	user = talloc_zero(g.tall, struct osmo_ss7_user);
	if (!user) return -1;
	user->inst = inst;
	user->name = "isup";
	user->prim_cb = isup_prim_cb;
	user->priv = &g;
	osmo_ss7_user_register(inst, MTP_SI_ISUP, user);
	g.ss7_user = user;

	/* Optional SCCP (SI=3) on the same association — reserved for a future
	 * TCAP/MAP/INAP layer. Enabled only when an SSN is configured. */
	if (g.sccp_ssn > 0) {
		g.sccp = osmo_ss7_ensure_sccp(inst);
		if (g.sccp) {
			g.sccp_user = osmo_sccp_user_bind(g.sccp, "isup-sccp",
							  isup_sccp_prim_cb, (uint16_t)g.sccp_ssn);
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "mod_isup: SCCP enabled (SI=3, SSN=%d)\n", g.sccp_ssn);
		}
	}

	for (pi = 0; pi < g.nprofiles; pi++)
		isup_profile_bring_up(g.profiles[pi]);

	/* eventfd for FS -> Osmo marshalling, registered in the select loop */
	g.evfd = eventfd(0, EFD_NONBLOCK);
	osmo_fd_setup(&g.ofd, g.evfd, OSMO_FD_READ, evfd_cb, NULL, 0);
	osmo_fd_register(&g.ofd);
	return 0;
}

/* Load settings from autoload_configs/isup.conf.xml. The XML file is the
 * canonical configuration; same-named ISUP_* environment variables override it
 * (convenient for containerised deployments). */
static void isup_parse_profile_param(isup_profile_t *p, const char *var, const char *val)
{
	if      (!strcasecmp(var, "opc"))               p->m3ua.opc = (uint32_t)atoi(val);
	else if (!strcasecmp(var, "peer-dpc"))          p->peer_dpc = (uint32_t)atoi(val);
	else if (!strcasecmp(var, "network-indicator")) p->m3ua.ni  = (uint8_t)atoi(val);
	else if (!strcasecmp(var, "mgw"))               switch_copy_string(p->gateway, val, sizeof(p->gateway));
	else if (!strcasecmp(var, "cic-min"))           p->cic_min  = (uint16_t)atoi(val);
	else if (!strcasecmp(var, "cic-max"))           p->cic_max  = (uint16_t)atoi(val);
	else if (!strcasecmp(var, "context"))           switch_copy_string(p->context, val, sizeof(p->context));
	else if (!strcasecmp(var, "dialplan"))          switch_copy_string(p->dialplan, val, sizeof(p->dialplan));
	else if (!strcasecmp(var, "auto-answer"))       p->autoanswer = switch_true(val);
}

static isup_profile_t *isup_new_profile(const char *name)
{
	isup_profile_t *p = switch_core_alloc(g.pool, sizeof(*p));
	switch_copy_string(p->name, name, sizeof(p->name));
	p->m3ua.opc = 1;
	p->peer_dpc = 2;
	p->m3ua.ni  = 2;               /* national */
	p->cic_min  = 1;
	p->cic_max  = 4;
	p->autoanswer = 0;
	switch_copy_string(p->gateway,  "127.0.0.1:2427", sizeof(p->gateway));
	switch_copy_string(p->context,  "default", sizeof(p->context));
	switch_copy_string(p->dialplan, "XML", sizeof(p->dialplan));
	return p;
}

/* Load the shared transport <settings> and the ISUP <profiles> from
 * isup.conf.xml. ISUP_* environment variables override the shared transport
 * settings; when exactly one profile is defined, the identity env vars
 * (ISUP_OPC / ISUP_PEER_DPC / ISUP_MGW / ISUP_AUTOANSWER) override it too, for
 * containerised single-exchange deployments. */
static void isup_load_configs(void)
{
	switch_xml_t xml, cfg, settings, profiles, xprof, param;
	const char *cs7 = NULL, *asp = NULL, *env;
	int i;

	g.sccp_ssn = 0;
	g.nprofiles = 0;

	if ((xml = switch_xml_open_cfg("isup.conf", &cfg, NULL))) {
		if ((settings = switch_xml_child(cfg, "settings"))) {
			for (param = switch_xml_child(settings, "param"); param; param = param->next) {
				const char *var = switch_xml_attr_soft(param, "name");
				const char *val = switch_xml_attr_soft(param, "value");
				if      (!strcasecmp(var, "asp-name"))   asp = val;
				else if (!strcasecmp(var, "cs7-config")) cs7 = val;
				else if (!strcasecmp(var, "sccp-ssn"))   g.sccp_ssn = atoi(val);
			}
		}
		if ((profiles = switch_xml_child(cfg, "profiles"))) {
			for (xprof = switch_xml_child(profiles, "profile"); xprof; xprof = xprof->next) {
				const char *pname = switch_xml_attr_soft(xprof, "name");
				isup_profile_t *p;
				if (zstr(pname) || g.nprofiles >= ISUP_MAX_PROFILES) continue;
				p = isup_new_profile(pname);
				for (param = switch_xml_child(xprof, "param"); param; param = param->next)
					isup_parse_profile_param(p, switch_xml_attr_soft(param, "name"),
								 switch_xml_attr_soft(param, "value"));
				g.profiles[g.nprofiles++] = p;
			}
		}
		switch_xml_free(xml);
	} else {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_WARNING,
				  "mod_isup: isup.conf.xml not found; using a default profile + environment\n");
	}

	/* Shared transport: environment overrides */
	if ((env = getenv("ISUP_CS7_CFG")))  cs7 = env;
	if ((env = getenv("ISUP_ASP_NAME"))) asp = env;
	if ((env = getenv("ISUP_SCCP_SSN"))) g.sccp_ssn = atoi(env);
	switch_copy_string(g.asp_name, asp ? asp : "asp", sizeof(g.asp_name));
	switch_copy_string(g.cs7_cfg,  cs7 ? cs7 : "/usr/local/freeswitch/conf/isup-cs7.cfg", sizeof(g.cs7_cfg));

	/* No <profiles> configured: synthesise one (env/defaults, container use). */
	if (g.nprofiles == 0)
		g.profiles[g.nprofiles++] = isup_new_profile(getenv("ISUP_PROFILE") ? getenv("ISUP_PROFILE") : "lab");

	/* Single-profile identity env overrides (containerised single exchange). */
	if (g.nprofiles == 1) {
		isup_profile_t *p = g.profiles[0];
		if ((env = getenv("ISUP_OPC")))      p->m3ua.opc = (uint32_t)atoi(env);
		if ((env = getenv("ISUP_PEER_DPC"))) p->peer_dpc = (uint32_t)atoi(env);
		if ((env = getenv("ISUP_MGW")))      switch_copy_string(p->gateway, env, sizeof(p->gateway));
		if (getenv("ISUP_AUTOANSWER"))       p->autoanswer = 1;
	}

	/* Finalise each profile: bearer driver + circuit array. */
	for (i = 0; i < g.nprofiles; i++) {
		isup_profile_t *p = g.profiles[i];
		if (p->cic_max < p->cic_min) p->cic_max = p->cic_min;
		p->bearer = bearer_mgcp_driver();
		p->ckt = switch_core_alloc(g.pool, sizeof(isup_ckt_t) * (p->cic_max - p->cic_min + 1));
	}
}

SWITCH_MODULE_LOAD_FUNCTION(mod_isup_load)
{
	switch_endpoint_interface_t *ep;

	memset(&g, 0, sizeof(g));
	g.pool = pool;
	switch_mutex_init(&g.qlock, SWITCH_MUTEX_NESTED, pool);

	{
		switch_api_interface_t *api_interface;
		*module_interface = switch_loadable_module_create_module_interface(pool, modname);
		ep = switch_loadable_module_create_interface(*module_interface, SWITCH_ENDPOINT_INTERFACE);
		ep->interface_name = "isup";
		ep->io_routines = &isup_io_routines;
		ep->state_handler = &isup_state_handlers;
		isup_endpoint_interface = ep;
		SWITCH_ADD_API(api_interface, "isup", "ISUP OAM/status", isup_api_function, ISUP_API_SYNTAX);
		switch_console_set_complete("add isup status");
		switch_console_set_complete("add isup m3ua");
		switch_console_set_complete("add isup mgw");
		switch_console_set_complete("add isup cic");
		switch_console_set_complete("add isup sccp");
	}

	/* Configuration from autoload_configs/isup.conf.xml (shared transport +
	 * one or more ISUP trunk profiles). */
	isup_load_configs();

	/* Launch the Osmo thread; it performs all osmo setup then runs the loop.
	 * Wait (briefly) until it signals ready so outbound calls find state. */
	g.running = 1;
	{
		switch_threadattr_t *ta;
		switch_threadattr_create(&ta, pool);
		switch_thread_create(&g.osmo_thread, ta, osmo_thread_run, NULL, pool);
	}
	{
		int spins = 0;
		while (!g.ready && spins++ < 300)
			switch_yield(10000); /* up to ~3s */
	}
	if (g.ready != 1) {
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_ERROR, "mod_isup: osmo setup failed\n");
		return SWITCH_STATUS_FALSE;
	}

	{
		int i;
		switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
				  "mod_isup loaded: %d profile(s) over ASP '%s'\n",
				  g.nprofiles, g.asp_name);
		for (i = 0; i < g.nprofiles; i++) {
			isup_profile_t *p = g.profiles[i];
			switch_log_printf(SWITCH_CHANNEL_LOG, SWITCH_LOG_NOTICE,
					  "  profile '%s': OPC=%u peer-DPC=%u CIC %u-%u MGW %s\n",
					  p->name, p->m3ua.opc, p->peer_dpc,
					  p->cic_min, p->cic_max, p->gateway);
		}
	}
	return SWITCH_STATUS_SUCCESS;
}

SWITCH_MODULE_SHUTDOWN_FUNCTION(mod_isup_shutdown)
{
	switch_status_t st;
	g.running = 0;
	if (g.evfd >= 0) { uint64_t one = 1; if (write(g.evfd, &one, sizeof(one)) < 0) {} }
	if (g.osmo_thread) switch_thread_join(&st, g.osmo_thread);
	return SWITCH_STATUS_SUCCESS;
}
