/*
 * mod_isup — ISUP simple segmentation (SGM), Q.764 §2.1.12.
 *
 * When an ISUP message with all its optional parameters would exceed the
 * available MTP3-user payload, ITU "simple segmentation" splits it into the
 * base message (carrying the mandatory part plus as many optional parameters
 * as fit, with the simple-segmentation indicator set) and exactly one
 * following Segmentation (SGM) message carrying the remaining optional
 * parameters.
 */
#ifndef ISUP_SEGMENT_H
#define ISUP_SEGMENT_H

#include "isup_proto.h"
#include "isup_codec.h"

/* Simple segmentation indicator — Optional Forward/Backward Call Indicators,
 * bit C (Q.763 §3.38 / §3.37). */
#define ISUP_SEG_IND_BIT 0x04

/*
 * Split `full` so the base message encodes within `max` octets.
 *
 *   need_sgm == 0 : the message fits; *base is a copy of *full, *sgm unused.
 *   need_sgm == 1 : *base holds the mandatory part + the optional parameters
 *                   that fit (with the segmentation indicator set), and *sgm
 *                   is an SGM message holding the overflow optional params.
 *
 * Returns ISUP_OK, or a negative isup_rc (e.g. ISUP_ERR_NOSPACE if the
 * mandatory part alone exceeds `max`).
 */
int isup_segment(const struct isup_msg *full, size_t max,
		 struct isup_msg *base, struct isup_msg *sgm, int *need_sgm);

/*
 * Merge a received SGM into the previously received base message: all SGM
 * parameters are added to `base` and the segmentation indicator is cleared.
 */
int isup_reassemble(struct isup_msg *base, const struct isup_msg *sgm);

#endif /* ISUP_SEGMENT_H */
