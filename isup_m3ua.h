/*
 * mod_isup — M3UA transport binding (libosmo-sigtran).
 *
 * Brings up an osmo_ss7 instance as an ASP, registers ISUP as the SI=5
 * MTP-User, and bridges MTP-TRANSFER primitives to/from the transport-
 * agnostic codec + state machine. Link addressing (SCTP multihoming, ports,
 * routing context) is configured via the standard Osmocom VTY config; this
 * binding owns the user registration and the send/receive of ISUP PDUs.
 *
 * This file depends on libosmocore/libosmo-sigtran; the codec and state
 * machine it feeds do not.
 */
#ifndef ISUP_M3UA_H
#define ISUP_M3UA_H

#include <stdint.h>
#include <stddef.h>

struct osmo_ss7_instance;
struct osmo_ss7_user;

/* Called for each received ISUP MTP-TRANSFER.ind. The payload is the raw
 * MTP3-user octets beginning at the CIC (ready for isup_decode). */
typedef void (*isup_m3ua_rx_cb)(void *user, uint32_t opc, uint32_t dpc,
				uint8_t sls, const uint8_t *data, size_t len);

struct isup_m3ua {
	struct osmo_ss7_instance *inst;
	struct osmo_ss7_user     *ss7_user; /* heap-allocated registration   */
	uint32_t                  opc;      /* our originating point code     */
	uint8_t                   ni;       /* network indicator (ITU nat=2)  */
	isup_m3ua_rx_cb           rx_cb;
	void                     *rx_user;
};

/*
 * Initialise: create/find the ss7 instance `id`, set the ITU point-code
 * format, and register the ISUP (SI=5) MTP-User. Returns 0 on success.
 * AS/ASP and link configuration is expected to come from the VTY config that
 * created the same instance id.
 */
int  isup_m3ua_init(struct isup_m3ua *m, void *talloc_ctx, uint32_t id,
		    uint32_t opc, uint8_t ni, isup_m3ua_rx_cb cb, void *cb_user);

/* Send an ISUP PDU (raw octets from the CIC onward) towards `dpc`. */
int  isup_m3ua_send(struct isup_m3ua *m, uint32_t dpc, uint8_t sls,
		    const uint8_t *data, size_t len);

void isup_m3ua_fini(struct isup_m3ua *m);

#endif /* ISUP_M3UA_H */
