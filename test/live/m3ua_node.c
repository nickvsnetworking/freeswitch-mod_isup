/* Live M3UA loop test: connect as an ASP to osmo-stp over real SCTP, register
 * ISUP (SI=5), send an IAM to our own point code; the STP routes it back via
 * the dynamically-registered routing key, and our user callback receives it.
 * Proves the full SCTP + M3UA + MTP-User path with the real osmo stack. */
#include <stdio.h>
#include <string.h>
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
#include <osmocom/sigtran/protocol/mtp.h>
#include "../../isup_codec.h"

#define MY_PC 2          /* 0.0.2 in 3-8-3 */
static int g_rx = 0, g_sent = 0;
static struct osmo_ss7_user *g_user;
static struct osmo_timer_list t_send, t_done;

static int prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	struct osmo_mtp_prim *omp = (struct osmo_mtp_prim *)oph;
	if (oph->sap == MTP_SAP_USER &&
	    OSMO_PRIM_HDR(oph) == OSMO_PRIM(OSMO_MTP_PRIM_TRANSFER, PRIM_OP_INDICATION)) {
		struct isup_msg m;
		const uint8_t *d = msgb_l2(oph->msg);
		unsigned int n = msgb_l2len(oph->msg);
		printf("RX MTP-TRANSFER: opc=%u dpc=%u sls=%u len=%u\n",
		       omp->u.transfer.opc, omp->u.transfer.dpc, omp->u.transfer.sls, n);
		if (isup_decode(&m, d, n) == ISUP_OK) {
			const struct isup_param *p = isup_msg_get(&m, ISUP_P_CALLED_NUMBER);
			printf("   decoded %s CIC=%u, called param len=%d\n",
			       isup_msg_type_name(m.msg_type), m.cic, p ? p->len : -1);
			g_rx = 1;
		}
	}
	if (oph->msg) msgb_free(oph->msg);
	return 0;
}

static void send_iam(void *data)
{
	struct osmo_ss7_instance *inst = data;
	struct isup_msg iam;
	uint8_t nci=0, fci[2]={0x60,0x10}, cpc=0x0a, tmr=0, cd[4]={0x83,0x10,0x21,0x03};
	uint8_t buf[64]; int len;
	struct msgb *msg; struct osmo_mtp_prim *prim;

	(void)inst;
	isup_msg_init(&iam, 5, ISUP_MT_IAM);
	isup_msg_add(&iam, ISUP_P_NATURE_OF_CONN, &nci, 1);
	isup_msg_add(&iam, ISUP_P_FWD_CALL_IND, fci, 2);
	isup_msg_add(&iam, ISUP_P_CALLING_CATEGORY, &cpc, 1);
	isup_msg_add(&iam, ISUP_P_TMR, &tmr, 1);
	isup_msg_add(&iam, ISUP_P_CALLED_NUMBER, cd, 4);
	len = isup_encode(&iam, buf, sizeof(buf));

	msg = msgb_alloc(sizeof(*prim) + len + 64, "IAM");
	prim = (struct osmo_mtp_prim *) msgb_put(msg, sizeof(*prim));
	memset(prim, 0, sizeof(*prim));
	osmo_prim_init(&prim->oph, MTP_SAP_USER, OSMO_MTP_PRIM_TRANSFER, PRIM_OP_REQUEST, msg);
	prim->u.transfer.opc = MY_PC;
	prim->u.transfer.dpc = MY_PC;        /* route back to ourselves */
	prim->u.transfer.sls = 0;
	prim->u.transfer.sio = MTP_SIO(MTP_SI_ISUP, 2);
	msg->l2h = msgb_put(msg, len);
	memcpy(msg->l2h, buf, len);
	printf("TX IAM to dpc=%u (%d bytes)\n", MY_PC, len);
	osmo_ss7_user_mtp_sap_prim_down(g_user, prim);
	g_sent = 1;
}

static void done(void *data)
{
	(void)data;
	printf("RESULT: %s\n", (g_sent && g_rx) ? "PASS — live M3UA loop OK" : "FAIL");
	exit((g_sent && g_rx) ? 0 : 1);
}

static const struct log_info_cat cats[] = {};
static const struct log_info linfo = { .cat = cats, .num_cat = 0 };
static struct vty_app_info vinfo = { .name = "isup-node", .version = "0" };

int main(void)
{
	void *ctx = talloc_named_const(NULL, 0, "isup-node");
	struct osmo_ss7_instance *inst;

	osmo_init_logging2(ctx, &linfo);
	osmo_ss7_init();
	vty_init(&vinfo);
	logging_vty_add_cmds();
	osmo_ss7_vty_init_asp(ctx);
	if (vty_read_config_file("test/live/node.cfg", NULL) < 0) {
		fprintf(stderr, "config parse failed\n"); return 2;
	}
	inst = osmo_ss7_instance_find(0);
	if (!inst) { fprintf(stderr, "no cs7 instance 0\n"); return 2; }

	g_user = osmo_ss7_user_create(inst, "isup");
	osmo_ss7_user_set_prim_cb(g_user, prim_cb);
	osmo_ss7_user_register(g_user, MTP_SI_ISUP);

	osmo_timer_setup(&t_send, send_iam, inst);
	osmo_timer_setup(&t_done, done, NULL);
	osmo_timer_schedule(&t_send, 2, 0);   /* after ASP comes ACTIVE */
	osmo_timer_schedule(&t_done, 5, 0);

	printf("node up, waiting for M3UA association...\n");
	while (1) osmo_select_main(0);
	return 0;
}
