/*
 * mod_isup — bearer (media gateway) control abstraction.
 *
 * One internal API with pluggable drivers so the MGCF can control an
 * osmo-mgw via MGCP today and a MEGACO/H.248 MGW later without the call
 * logic changing. The CRCX is asynchronous: the driver invokes ready_cb on
 * the Osmo thread when the MGW has answered, handing back its RTP address
 * (which FreeSWITCH then points its own RTP at).
 */
#ifndef ISUP_BEARER_H
#define ISUP_BEARER_H

#include <stdint.h>

struct bearer_ci; /* opaque per-call connection identifier */

/* ok=1 on success; rtp_ip/rtp_port carry the MGW's RTP endpoint. */
typedef void (*bearer_ready_cb)(void *user, int ok,
				const char *rtp_ip, uint16_t rtp_port);

struct bearer_driver {
	const char *name;

	/* Create a connection on `endpoint` in `mode` ("recvonly"/"sendrecv"/
	 * "loopback"). Calls cb asynchronously with the MGW RTP info. */
	int (*crcx)(const char *gateway, const char *endpoint, const char *mode,
		    bearer_ready_cb cb, void *user, struct bearer_ci **out);

	/* Modify: change mode and/or set the far-end (FreeSWITCH) RTP. */
	int (*mdcx)(struct bearer_ci *ci, const char *mode,
		    const char *remote_ip, uint16_t remote_port);

	/* Delete the connection. */
	int (*dlcx)(struct bearer_ci *ci);
};

/* The active driver (selected per-MGW in config: mgcp now, megaco later). */
const struct bearer_driver *bearer_mgcp_driver(void);
const struct bearer_driver *bearer_megaco_driver(void); /* later phase */

#endif /* ISUP_BEARER_H */
