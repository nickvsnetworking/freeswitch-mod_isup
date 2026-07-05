/*
 * mod_isup — M3UA transport binding (libosmo-sigtran).
 *
 * Verified to compile against libosmo-sigtran 2.x (osmo_ss7 user SAP). The
 * SI=5 MTP-User registration is the mechanism the design relies on so that
 * ISUP rides M3UA directly, sharing the association with SCCP (SI=3).
 */
#include <string.h>
#include <errno.h>

#include <osmocom/core/msgb.h>
#include <osmocom/core/prim.h>

#include <osmocom/sigtran/osmo_ss7.h>
#include <osmocom/sigtran/mtp_sap.h>
#include <osmocom/sigtran/sigtran_sap.h>
#include <osmocom/sigtran/protocol/mtp.h>

#include "isup_m3ua.h"

/* MTP-TRANSFER.ind from the stack -> upper layer */
static int isup_m3ua_prim_cb(struct osmo_prim_hdr *oph, void *ctx)
{
	struct isup_m3ua *m = ctx;
	struct osmo_mtp_prim *omp = (struct osmo_mtp_prim *) oph;

	if (oph->sap != MTP_SAP_USER)
		goto done;

	switch (OSMO_PRIM_HDR(oph)) {
	case OSMO_PRIM(OSMO_MTP_PRIM_TRANSFER, PRIM_OP_INDICATION):
		if (m && m->rx_cb && oph->msg) {
			const uint8_t *data = msgb_l2(oph->msg);
			unsigned int len = msgb_l2len(oph->msg);
			if (data && len)
				m->rx_cb(m->rx_user,
					 omp->u.transfer.opc,
					 omp->u.transfer.dpc,
					 omp->u.transfer.sls,
					 data, len);
		}
		break;
	default:
		/* PAUSE/RESUME/STATUS indications: link state changes that the
		 * profile layer reacts to (block circuits, trigger GRS). */
		break;
	}

done:
	if (oph->msg)
		msgb_free(oph->msg);
	return 0;
}

int isup_m3ua_init(struct isup_m3ua *m, void *talloc_ctx, uint32_t id,
		   uint32_t opc, uint8_t ni, isup_m3ua_rx_cb cb, void *cb_user)
{
	(void)talloc_ctx;
	memset(m, 0, sizeof(*m));
	m->opc = opc;
	m->ni = ni;
	m->rx_cb = cb;
	m->rx_user = cb_user;

	m->inst = osmo_ss7_instance_find_or_create(talloc_ctx, id);
	if (!m->inst)
		return -ENOMEM;

	/* ITU 14-bit point codes: 3-8-3 sub-field structure. */
	osmo_ss7_instance_set_pc_fmt(m->inst, 3, 8, 3);

	m->ss7_user = osmo_ss7_user_create(m->inst, "isup");
	if (!m->ss7_user)
		return -ENOMEM;
	osmo_ss7_user_set_prim_cb(m->ss7_user, isup_m3ua_prim_cb);
	osmo_ss7_user_set_priv(m->ss7_user, m);

	if (osmo_ss7_user_register(m->ss7_user, MTP_SI_ISUP) < 0)
		return -EEXIST;

	return 0;
}

int isup_m3ua_send(struct isup_m3ua *m, uint32_t dpc, uint8_t sls,
		   const uint8_t *data, size_t len)
{
	struct msgb *msg;
	struct osmo_mtp_prim *prim;

	if (!m->ss7_user || len == 0)
		return -EINVAL;

	msg = msgb_alloc(sizeof(*prim) + len + 64, "ISUP-TX");
	if (!msg)
		return -ENOMEM;

	/* The prim header sits at the front of the buffer; the MTP-user payload
	 * follows and is referenced via l2h, where the stack reads it. */
	prim = (struct osmo_mtp_prim *) msgb_put(msg, sizeof(*prim));
	memset(prim, 0, sizeof(*prim));
	osmo_prim_init(&prim->oph, MTP_SAP_USER,
		       OSMO_MTP_PRIM_TRANSFER, PRIM_OP_REQUEST, msg);
	prim->u.transfer.opc = m->opc;
	prim->u.transfer.dpc = dpc;
	prim->u.transfer.sls = sls;
	prim->u.transfer.sio = MTP_SIO(MTP_SI_ISUP, m->ni);

	msg->l2h = msgb_put(msg, len);
	memcpy(msg->l2h, data, len);

	return osmo_ss7_user_mtp_sap_prim_down(m->ss7_user, prim);
}

void isup_m3ua_fini(struct isup_m3ua *m)
{
	if (m->ss7_user) {
		osmo_ss7_user_unregister(m->ss7_user, MTP_SI_ISUP);
		osmo_ss7_user_destroy(m->ss7_user);
		m->ss7_user = NULL;
	}
}
