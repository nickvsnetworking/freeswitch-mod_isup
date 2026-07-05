/*
 * mod_isup — circuit group / maintenance management (Q.764 §2.8–2.10).
 */
#include <string.h>
#include "isup_cgm.h"
#include "isup_param.h"

void isup_cgm_init(struct isup_cgm *g, uint16_t base_cic, int count,
		   const struct isup_cgm_ops *ops, void *user)
{
	memset(g, 0, sizeof(*g));
	g->base_cic = base_cic;
	g->count = count > CGM_MAX_CICS ? CGM_MAX_CICS : count;
	g->ops = ops;
	g->user = user;
}

int isup_cgm_idx(const struct isup_cgm *g, uint16_t cic)
{
	int idx = (int)cic - (int)g->base_cic;
	if (idx < 0 || idx >= g->count)
		return -1;
	return idx;
}

int isup_cgm_is_blocked(const struct isup_cgm *g, uint16_t cic)
{
	int idx = isup_cgm_idx(g, cic);
	if (idx < 0)
		return 0;
	return g->c[idx].rem_blocked || g->c[idx].loc_blocked;
}

/* ---- status-bitmap helpers (LSB-first within each octet) ---- */

static int mask_bit(const uint8_t *mask, int i)
{
	return (mask[i / 8] >> (i % 8)) & 1;
}

/* Number of status octets required to carry (range+1) bits. */
static size_t status_octets(uint8_t range)
{
	return ((size_t)range + 1 + 7) / 8;
}

/* True if a Range-and-status parameter is long enough to carry a status
 * bitmap for `range`. Guards every mask_bit() read below against a peer that
 * sends a large range with a short (or absent) status field. */
static int rs_has_status(const struct isup_param *rsp, uint8_t range)
{
	return rsp && (size_t)rsp->len >= 1 + status_octets(range);
}

static void set_bit(uint8_t *mask, int i)
{
	mask[i / 8] |= (uint8_t)(1u << (i % 8));
}

/* ---- emit helpers ---- */

static void emit_simple(struct isup_cgm *g, uint8_t mt, uint16_t cic)
{
	struct isup_msg m;
	isup_msg_init(&m, cic, mt);
	g->ops->emit(g->user, &m);
}

static void emit_range_status(struct isup_cgm *g, uint8_t mt, uint16_t cic,
			      uint8_t range, const uint8_t *status, int with_status,
			      const uint8_t *sv_type)
{
	struct isup_msg m;
	uint8_t rs[1 + (256 / 8) + 1];
	int rl;
	isup_msg_init(&m, cic, mt);
	if (sv_type)
		isup_msg_add(&m, ISUP_P_CIRC_GRP_SV_TYPE, sv_type, 1);
	rl = isup_enc_range_status(range, status, with_status, rs, sizeof(rs));
	if (rl > 0)
		isup_msg_add(&m, ISUP_P_RANGE_AND_STATUS, rs, (uint8_t)rl);
	g->ops->emit(g->user, &m);
}

/* ---- receive ---- */

int isup_cgm_rx(struct isup_cgm *g, const struct isup_msg *m)
{
	uint16_t cic = m->cic;
	int idx = isup_cgm_idx(g, cic);
	const struct isup_param *rsp = isup_msg_get(m, ISUP_P_RANGE_AND_STATUS);
	const struct isup_param *sv = isup_msg_get(m, ISUP_P_CIRC_GRP_SV_TYPE);

	switch (m->msg_type) {
	case ISUP_MT_BLO:
		if (idx >= 0) g->c[idx].rem_blocked = 1;
		emit_simple(g, ISUP_MT_BLA, cic);
		return 1;
	case ISUP_MT_UBL:
		if (idx >= 0) g->c[idx].rem_blocked = 0;
		emit_simple(g, ISUP_MT_UBA, cic);
		return 1;
	case ISUP_MT_BLA:
		if (idx >= 0) g->c[idx].loc_pending = 0;
		return 1;
	case ISUP_MT_UBA:
		if (idx >= 0) { g->c[idx].loc_blocked = 0; g->c[idx].loc_pending = 0; }
		return 1;

	case ISUP_MT_RSC:
		if (idx >= 0) {
			g->c[idx].rem_blocked = g->c[idx].loc_blocked = 0;
			g->c[idx].reset_pending = 0;
		}
		/* call release is handled by the call SM; here we clear blocking */
		return 1;

	case ISUP_MT_GRS:
		if (rsp && rsp->len >= 1) {
			uint8_t range = rsp->val[0];
			uint8_t status[256 / 8];
			int i;
			memset(status, 0, sizeof(status));
			for (i = 0; i <= range; i++) {
				int j = isup_cgm_idx(g, (uint16_t)(cic + i));
				if (j >= 0) {
					g->c[j].rem_blocked = g->c[j].loc_blocked = 0;
					g->c[j].reset_pending = 0;
				}
			}
			/* GRA reports all-unblocked after reset */
			emit_range_status(g, ISUP_MT_GRA, cic, range, status, 1, NULL);
		}
		return 1;

	case ISUP_MT_GRA:
		if (rsp && rsp->len >= 1) {
			uint8_t range = rsp->val[0];
			int i;
			for (i = 0; i <= range; i++) {
				int j = isup_cgm_idx(g, (uint16_t)(cic + i));
				if (j >= 0)
					g->c[j].reset_pending = 0;
			}
		}
		return 1;

	case ISUP_MT_CGB:
	case ISUP_MT_CGU:
		if (rsp && rsp->len >= 1 && rs_has_status(rsp, rsp->val[0])) {
			uint8_t range = rsp->val[0];
			const uint8_t *mask = rsp->val + 1;
			int block = (m->msg_type == ISUP_MT_CGB);
			int i;
			for (i = 0; i <= range; i++) {
				int j;
				if (!mask_bit(mask, i))
					continue;
				j = isup_cgm_idx(g, (uint16_t)(cic + i));
				if (j >= 0)
					g->c[j].rem_blocked = (uint8_t)block;
			}
			/* acknowledge, echoing range + status */
			emit_range_status(g,
				m->msg_type == ISUP_MT_CGB ? ISUP_MT_CGBA : ISUP_MT_CGUA,
				cic, range, mask, 1, sv ? sv->val : NULL);
		}
		return 1;

	case ISUP_MT_CGBA:
	case ISUP_MT_CGUA:
		if (rsp && rsp->len >= 1 && rs_has_status(rsp, rsp->val[0])) {
			uint8_t range = rsp->val[0];
			const uint8_t *mask = rsp->val + 1;
			int i;
			for (i = 0; i <= range; i++) {
				int j;
				if (!mask_bit(mask, i))
					continue;
				j = isup_cgm_idx(g, (uint16_t)(cic + i));
				if (j >= 0) {
					g->c[j].loc_pending = 0;
					if (m->msg_type == ISUP_MT_CGUA)
						g->c[j].loc_blocked = 0;
				}
			}
		}
		return 1;

	case ISUP_MT_CQM:
		if (rsp && rsp->len >= 1) {
			uint8_t range = rsp->val[0];
			uint8_t state[ISUP_MAX_PARAM_LEN];
			struct isup_msg out;
			uint8_t rs[2];
			int i, rl;
			/* circuit state indicator carries one octet per circuit;
			 * cap at the parameter's maximum length. */
			int nstate = (int)range + 1;
			if (nstate > ISUP_MAX_PARAM_LEN)
				nstate = ISUP_MAX_PARAM_LEN;
			for (i = 0; i < nstate; i++) {
				int j = isup_cgm_idx(g, (uint16_t)(cic + i));
				/* circuit state indicator: bits1-0 maintenance block */
				state[i] = (j >= 0 && isup_cgm_is_blocked(g, (uint16_t)(cic + i)))
					   ? 0x03 : 0x00;
			}
			isup_msg_init(&out, cic, ISUP_MT_CQR);
			rl = isup_enc_range_status(range, NULL, 0, rs, sizeof(rs));
			isup_msg_add(&out, ISUP_P_RANGE_AND_STATUS, rs, (uint8_t)rl);
			isup_msg_add(&out, ISUP_P_CIRC_STATE_IND, state, (uint8_t)nstate);
			g->ops->emit(g->user, &out);
		}
		return 1;

	default:
		return 0; /* not a circuit-management message */
	}
}

/* ---- local actions ---- */

void isup_cgm_block(struct isup_cgm *g, uint16_t cic)
{
	int idx = isup_cgm_idx(g, cic);
	if (idx < 0)
		return;
	g->c[idx].loc_blocked = 1;
	g->c[idx].loc_pending = 1;
	emit_simple(g, ISUP_MT_BLO, cic);
}

void isup_cgm_unblock(struct isup_cgm *g, uint16_t cic)
{
	int idx = isup_cgm_idx(g, cic);
	if (idx < 0)
		return;
	g->c[idx].loc_pending = 1;
	emit_simple(g, ISUP_MT_UBL, cic);
}

void isup_cgm_reset_all(struct isup_cgm *g)
{
	uint8_t range;
	int i;
	if (g->count <= 0)
		return;
	range = (uint8_t)(g->count - 1);
	for (i = 0; i < g->count; i++)
		g->c[i].reset_pending = 1;
	emit_range_status(g, ISUP_MT_GRS, g->base_cic, range, NULL, 0, NULL);
}

void isup_cgm_group_block(struct isup_cgm *g, uint16_t base, uint8_t range,
			  const uint8_t *mask)
{
	uint8_t sv = 0x00; /* maintenance oriented */
	int i;
	for (i = 0; i <= range; i++) {
		int j;
		if (!mask_bit(mask, i))
			continue;
		j = isup_cgm_idx(g, (uint16_t)(base + i));
		if (j >= 0) {
			g->c[j].loc_blocked = 1;
			g->c[j].loc_pending = 1;
		}
	}
	emit_range_status(g, ISUP_MT_CGB, base, range, mask, 1, &sv);
	(void)set_bit; /* helper retained for symmetry/local mask building */
}
