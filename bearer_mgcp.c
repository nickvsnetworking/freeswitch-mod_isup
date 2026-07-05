/*
 * mod_isup — bearer driver: MGCP (RFC 3435) over UDP to osmo-mgw.
 *
 * Synchronous transactions (CRCX/MDCX/DLCX) issued from the Osmo thread.
 * Proven live against osmo-mgw 1.15.0 (see test/live/mgcp_probe.py for the
 * same exchange driven externally).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include "bearer.h"

struct bearer_ci {
	int                sock;
	struct sockaddr_in dst;
	char               endpoint[128];  /* concrete endpoint from CRCX (Z:) */
	char               conn[64];       /* connection id (I:)                */
	char               callid[32];     /* stable MGCP CallId (C:) per call  */
	char               rtp_ip[48];     /* MGW RTP address (c=)              */
	uint16_t           rtp_port;       /* MGW RTP port (m=audio)            */
	uint32_t           txid;
};

/* gateway string is "ip:port" (default 127.0.0.1:2427) */
static void parse_gw(const char *gw, struct sockaddr_in *sin)
{
	char ip[48] = "127.0.0.1";
	int port = 2427;
	if (gw && *gw) {
		const char *colon = strchr(gw, ':');
		if (colon) {
			size_t n = (size_t)(colon - gw);
			if (n < sizeof(ip)) { memcpy(ip, gw, n); ip[n] = '\0'; }
			port = atoi(colon + 1);
		} else {
			snprintf(ip, sizeof(ip), "%s", gw);
		}
	}
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons((uint16_t)port);
	inet_pton(AF_INET, ip, &sin->sin_addr);
}

/* send one MGCP request, wait (<=1s) for the response. Returns status code or -1. */
static int mgcp_txn(struct bearer_ci *ci, const char *req, char *resp, size_t rcap)
{
	struct timeval tv = { 1, 0 };
	ssize_t n;
	setsockopt(ci->sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	if (sendto(ci->sock, req, strlen(req), 0,
		   (struct sockaddr *)&ci->dst, sizeof(ci->dst)) < 0)
		return -1;
	n = recv(ci->sock, resp, rcap - 1, 0);
	if (n <= 0)
		return -1;
	resp[n] = '\0';
	return atoi(resp); /* "200 ..." -> 200 */
}

static const char *hdr(const char *resp, const char *key, char *out, size_t cap)
{
	const char *p = resp;
	size_t klen = strlen(key);
	out[0] = '\0';
	while (p && *p) {
		if (strncmp(p, key, klen) == 0) {
			const char *v = p + klen;
			size_t i = 0;
			while (*v == ' ') v++;
			while (*v && *v != '\r' && *v != '\n' && i < cap - 1)
				out[i++] = *v++;
			out[i] = '\0';
			return out;
		}
		p = strchr(p, '\n');
		if (p) p++;
	}
	return out;
}

static int bm_crcx(const char *gateway, const char *endpoint, const char *mode,
		   bearer_ready_cb cb, void *user, struct bearer_ci **out)
{
	struct bearer_ci *ci = calloc(1, sizeof(*ci));
	char req[512], resp[2048], tmp[128];
	const char *ep = (endpoint && *endpoint) ? endpoint : "rtpbridge/*@mgw";
	int code;

	if (!ci)
		return -1;
	static uint32_t callseq;
	/* osmo-mgw's rtpbridge endpoint bridges the two connections that belong
	 * to ONE call (one CallId, two connection-ids). The two exchanges are
	 * independent MGCF instances, so we must agree on a CallId derived from
	 * the shared bearer endpoint (e.g. "rtpbridge/1@mgw") rather than a
	 * per-instance value — otherwise the second leg's CRCX/MDCX is rejected
	 * with 516 (incorrect CallId). The MGCP transaction-id stays unique per
	 * instance (mix in ISUP_OPC) so the MGW does not mistake the second
	 * leg's CRCX for a retransmission of the first. */
	const char *opc_env = getenv("ISUP_OPC");
	unsigned inst = (unsigned)((opc_env ? atoi(opc_env) : 0) & 0xffff);
	if (!inst) inst = (unsigned)(getpid() & 0xffff);
	ci->sock = socket(AF_INET, SOCK_DGRAM, 0);
	parse_gw(gateway, &ci->dst);
	ci->txid = (inst << 16) | ((++callseq) & 0xffff);
	{
		char cid[32]; size_t k = 0;
		for (const char *s = ep; *s && k < sizeof(cid) - 1; s++)
			if ((*s >= 'a' && *s <= 'z') || (*s >= '0' && *s <= '9'))
				cid[k++] = *s;
		cid[k] = 0;
		snprintf(ci->callid, sizeof(ci->callid), "isup%s", cid);
	}

	snprintf(req, sizeof(req),
		 "CRCX %u %s MGCP 1.0\r\nC: %s\r\nM: %s\r\n\r\n",
		 ci->txid, ep, ci->callid, mode ? mode : "recvonly");
	code = mgcp_txn(ci, req, resp, sizeof(resp));
	if (code != 200) {
		close(ci->sock); free(ci);
		if (cb) cb(user, 0, NULL, 0);
		return -1;
	}
	hdr(resp, "I:", ci->conn, sizeof(ci->conn));
	hdr(resp, "Z:", ci->endpoint, sizeof(ci->endpoint));
	if (!ci->endpoint[0])
		snprintf(ci->endpoint, sizeof(ci->endpoint), "%s", ep);
	if (hdr(resp, "c=IN IP4 ", ci->rtp_ip, sizeof(ci->rtp_ip)), !ci->rtp_ip[0])
		snprintf(ci->rtp_ip, sizeof(ci->rtp_ip), "127.0.0.1");
	{
		const char *m = strstr(resp, "m=audio ");
		if (m) ci->rtp_port = (uint16_t)atoi(m + 8);
	}
	(void)tmp;
	*out = ci;
	if (cb) cb(user, 1, ci->rtp_ip, ci->rtp_port);
	return 0;
}

static int bm_mdcx(struct bearer_ci *ci, const char *mode,
		   const char *remote_ip, uint16_t remote_port)
{
	char req[768], resp[2048], sdp[512] = "";
	int code;
	if (!ci)
		return -1;
	if (remote_ip && remote_port)
		snprintf(sdp, sizeof(sdp),
			 "v=0\r\no=- 0 0 IN IP4 %s\r\ns=-\r\nc=IN IP4 %s\r\n"
			 "t=0 0\r\nm=audio %u RTP/AVP 8\r\na=rtpmap:8 PCMA/8000\r\n",
			 remote_ip, remote_ip, remote_port);
	ci->txid++;
	snprintf(req, sizeof(req),
		 "MDCX %u %s MGCP 1.0\r\nC: %s\r\nI: %s\r\nM: %s\r\n\r\n%s",
		 ci->txid, ci->endpoint, ci->callid, ci->conn,
		 mode ? mode : "sendrecv", sdp);
	code = mgcp_txn(ci, req, resp, sizeof(resp));
	return code == 200 ? 0 : -1;
}

static int bm_dlcx(struct bearer_ci *ci)
{
	char req[512], resp[2048];
	if (!ci)
		return -1;
	ci->txid++;
	snprintf(req, sizeof(req),
		 "DLCX %u %s MGCP 1.0\r\nC: %s\r\nI: %s\r\n\r\n",
		 ci->txid, ci->endpoint, ci->callid, ci->conn);
	mgcp_txn(ci, req, resp, sizeof(resp));
	close(ci->sock);
	free(ci);
	return 0;
}

static const struct bearer_driver MGCP_DRIVER = {
	.name = "mgcp", .crcx = bm_crcx, .mdcx = bm_mdcx, .dlcx = bm_dlcx,
};

const struct bearer_driver *bearer_mgcp_driver(void)
{
	return &MGCP_DRIVER;
}
