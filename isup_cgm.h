/*
 * mod_isup — circuit group / maintenance management (Q.764 §2.8–2.10).
 *
 * Handles the per-circuit blocking and reset state for a span of circuits:
 *   BLO/UBL/BLA/UBA            single-circuit blocking
 *   CGB/CGU/CGBA/CGUA          circuit group blocking (range + status mask)
 *   GRS/GRA                    circuit group reset
 *   RSC                        single-circuit reset
 *   CQM/CQR                    circuit group query
 *
 * Like the call state machine, this layer is transport-agnostic: it consumes
 * decoded messages and emits decoded responses through an ops callback, so it
 * is unit-testable with no FS/Osmo dependency.
 */
#ifndef ISUP_CGM_H
#define ISUP_CGM_H

#include "isup_proto.h"
#include "isup_codec.h"

#define CGM_MAX_CICS 256

struct cgm_circuit {
	uint8_t rem_blocked;     /* blocked by peer (BLO/CGB received)     */
	uint8_t loc_blocked;     /* blocked by us  (BLO/CGB sent)          */
	uint8_t loc_pending;     /* awaiting BLA/CGBA/UBA/CGUA from peer    */
	uint8_t reset_pending;   /* awaiting GRA/RLC after GRS/RSC we sent  */
};

struct isup_cgm_ops {
	void (*emit)(void *user, const struct isup_msg *m);
};

struct isup_cgm {
	uint16_t                   base_cic;
	int                        count;
	struct cgm_circuit         c[CGM_MAX_CICS];
	const struct isup_cgm_ops *ops;
	void                      *user;
};

void isup_cgm_init(struct isup_cgm *g, uint16_t base_cic, int count,
		   const struct isup_cgm_ops *ops, void *user);

/* Feed a received message. Returns 1 if it was a circuit-management message
 * (and was handled, with any response emitted), 0 if it is not ours. */
int  isup_cgm_rx(struct isup_cgm *g, const struct isup_msg *m);

/* Local actions (emit the corresponding message towards the peer). */
void isup_cgm_block(struct isup_cgm *g, uint16_t cic);
void isup_cgm_unblock(struct isup_cgm *g, uint16_t cic);
void isup_cgm_reset_all(struct isup_cgm *g);            /* GRS over the span */
void isup_cgm_group_block(struct isup_cgm *g, uint16_t base, uint8_t range,
			  const uint8_t *mask);          /* CGB */

/* Queries */
int  isup_cgm_is_blocked(const struct isup_cgm *g, uint16_t cic); /* either side */
int  isup_cgm_idx(const struct isup_cgm *g, uint16_t cic);        /* -1 if out of span */

#endif /* ISUP_CGM_H */
