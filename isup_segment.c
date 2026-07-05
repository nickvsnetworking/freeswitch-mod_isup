/*
 * mod_isup — ISUP simple segmentation (SGM), Q.764 §2.1.12.
 */
#include <string.h>
#include "isup_segment.h"

static int enc_len(const struct isup_msg *m)
{
	uint8_t tmp[ISUP_MAX_ENC_LEN];
	return isup_encode(m, tmp, sizeof(tmp));
}

/* Set the simple-segmentation indicator in the base message's Optional
 * Forward Call Indicators (creating that parameter if absent). */
static void set_seg_indicator(struct isup_msg *m)
{
	const struct isup_param *p = isup_msg_get(m, ISUP_P_OPT_FWD_CALL_IND);
	uint8_t v = ISUP_SEG_IND_BIT;
	if (p && p->len >= 1)
		v = p->val[0] | ISUP_SEG_IND_BIT;
	isup_msg_add(m, ISUP_P_OPT_FWD_CALL_IND, &v, 1);
}

static void clear_seg_indicator(struct isup_msg *m)
{
	const struct isup_param *p = isup_msg_get(m, ISUP_P_OPT_FWD_CALL_IND);
	if (p && p->len >= 1) {
		uint8_t v = p->val[0] & (uint8_t)~ISUP_SEG_IND_BIT;
		isup_msg_add(m, ISUP_P_OPT_FWD_CALL_IND, &v, 1);
	}
}

/* Index of the last parameter that may be moved into an SGM: it must be
 * optional for this message type and must not be the segmentation indicator
 * itself (which stays in the base). Returns -1 if none. */
static int last_movable(const struct isup_msg *m)
{
	int i;
	for (i = m->n_params - 1; i >= 0; i--) {
		uint8_t code = m->params[i].code;
		if (code == ISUP_P_OPT_FWD_CALL_IND)
			continue;
		if (isup_is_mandatory_param(m->msg_type, code))
			continue;
		return i;
	}
	return -1;
}

static void remove_param(struct isup_msg *m, int idx)
{
	int i;
	for (i = idx; i < m->n_params - 1; i++)
		m->params[i] = m->params[i + 1];
	m->n_params--;
}

int isup_segment(const struct isup_msg *full, size_t max,
		 struct isup_msg *base, struct isup_msg *sgm, int *need_sgm)
{
	int len;

	*base = *full;
	isup_msg_init(sgm, full->cic, ISUP_MT_SGM);
	*need_sgm = 0;

	len = enc_len(base);
	if (len < 0)
		return len;
	if ((size_t)len <= max)
		return ISUP_OK; /* fits as-is */

	*need_sgm = 1;
	set_seg_indicator(base);

	for (;;) {
		int idx;
		len = enc_len(base);
		if (len < 0)
			return len;
		if ((size_t)len <= max)
			break;
		idx = last_movable(base);
		if (idx < 0)
			return ISUP_ERR_NOSPACE; /* mandatory part alone too big */
		if (isup_msg_add(sgm, base->params[idx].code,
				 base->params[idx].val,
				 base->params[idx].len) != ISUP_OK)
			return ISUP_ERR_TOOMANY;
		remove_param(base, idx);
	}
	return ISUP_OK;
}

int isup_reassemble(struct isup_msg *base, const struct isup_msg *sgm)
{
	int i;
	for (i = 0; i < sgm->n_params; i++) {
		if (isup_msg_add(base, sgm->params[i].code,
				 sgm->params[i].val,
				 sgm->params[i].len) != ISUP_OK)
			return ISUP_ERR_TOOMANY;
	}
	clear_seg_indicator(base);
	return ISUP_OK;
}
