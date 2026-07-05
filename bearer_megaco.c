/*
 * mod_isup — bearer driver: MEGACO / H.248 (RFC 3525) text encoding, over UDP.
 *
 * Second bearer backend behind bearer.h (the first is bearer_mgcp.c). The MGCF
 * acts as the H.248 Media Gateway Controller: CRCX -> ADD a termination to a
 * new context, MDCX -> MODIFY it, DLCX -> SUBTRACT it. Synchronous transactions
 * issued from the Osmo thread, mirroring the MGCP driver.
 *
 * Note: compile-verified; runtime requires a live H.248 MG. Select per-MGW in
 * config (protocol="megaco"); the call logic is unchanged.
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
	char               ctx[32];        /* context id assigned by the MG   */
	char               term[64];       /* termination id assigned by MG   */
	char               rtp_ip[48];
	uint16_t           rtp_port;
	uint32_t           txid;
};

static void parse_gw(const char *gw, struct sockaddr_in *sin)
{
	char ip[48] = "127.0.0.1";
	int port = 2944;                   /* H.248 text default UDP port */
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

static int h248_txn(struct bearer_ci *ci, const char *req, char *resp, size_t rcap)
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
	/* a Reply (not Error) indicates success */
	return strstr(resp, "Reply") ? 0 : -1;
}

/* very small extractor: value following `key` up to a delimiter */
static void grab(const char *s, const char *key, char *out, size_t cap)
{
	const char *p = strstr(s, key);
	size_t i = 0;
	out[0] = '\0';
	if (!p) return;
	p += strlen(key);
	while (*p && (*p == '=' || *p == ' ')) p++;
	while (*p && *p != ',' && *p != '}' && *p != '\r' && *p != '\n' && *p != ' ' && i < cap - 1)
		out[i++] = *p++;
	out[i] = '\0';
}

static const char *h248_mode(const char *mode)
{
	if (mode && !strcmp(mode, "recvonly"))  return "ReceiveOnly";
	if (mode && !strcmp(mode, "sendonly"))  return "SendOnly";
	if (mode && !strcmp(mode, "loopback"))  return "LoopBack";
	return "SendReceive";
}

static int bm_crcx(const char *gateway, const char *endpoint, const char *mode,
		   bearer_ready_cb cb, void *user, struct bearer_ci **out)
{
	struct bearer_ci *ci = calloc(1, sizeof(*ci));
	char req[768], resp[2048];
	const char *term = (endpoint && *endpoint) ? endpoint : "$"; /* $ = MG chooses */

	if (!ci)
		return -1;
	ci->sock = socket(AF_INET, SOCK_DGRAM, 0);
	parse_gw(gateway, &ci->dst);
	ci->txid = (uint32_t)(getpid() & 0xffff);

	/* ADD a termination into a new context ($), request the MG's local SDP */
	snprintf(req, sizeof(req),
		 "MEGACO/1 [%s]\r\nTransaction = %u {\r\n"
		 "  Context = $ {\r\n"
		 "    Add = %s {\r\n"
		 "      Media { Stream = 1 {\r\n"
		 "        LocalControl { Mode = %s },\r\n"
		 "        Local { v=0\r\nc=IN IP4 $\r\nm=audio $ RTP/AVP 8 }\r\n"
		 "      } }\r\n"
		 "    }\r\n  }\r\n}\r\n",
		 "mgc", ci->txid, term, h248_mode(mode));

	if (h248_txn(ci, req, resp, sizeof(resp)) != 0) {
		close(ci->sock); free(ci);
		if (cb) cb(user, 0, NULL, 0);
		return -1;
	}
	grab(resp, "Context =", ci->ctx, sizeof(ci->ctx));
	grab(resp, "Add =", ci->term, sizeof(ci->term));
	{
		const char *c = strstr(resp, "c=IN IP4 ");
		const char *m = strstr(resp, "m=audio ");
		if (c) grab(c, "c=IN IP4", ci->rtp_ip, sizeof(ci->rtp_ip));
		if (m) ci->rtp_port = (uint16_t)atoi(m + 8);
	}
	if (!ci->rtp_ip[0]) snprintf(ci->rtp_ip, sizeof(ci->rtp_ip), "127.0.0.1");
	*out = ci;
	if (cb) cb(user, 1, ci->rtp_ip, ci->rtp_port);
	return 0;
}

static int bm_mdcx(struct bearer_ci *ci, const char *mode,
		   const char *remote_ip, uint16_t remote_port)
{
	char req[768], resp[2048], remote[256] = "";
	if (!ci)
		return -1;
	if (remote_ip && remote_port)
		snprintf(remote, sizeof(remote),
			 ",\r\n        Remote { v=0\r\nc=IN IP4 %s\r\nm=audio %u RTP/AVP 8 }",
			 remote_ip, remote_port);
	snprintf(req, sizeof(req),
		 "MEGACO/1 [%s]\r\nTransaction = %u {\r\n"
		 "  Context = %s {\r\n"
		 "    Modify = %s {\r\n"
		 "      Media { Stream = 1 {\r\n"
		 "        LocalControl { Mode = %s }%s\r\n"
		 "      } }\r\n"
		 "    }\r\n  }\r\n}\r\n",
		 "mgc", ++ci->txid, ci->ctx[0] ? ci->ctx : "-",
		 ci->term[0] ? ci->term : "$", h248_mode(mode), remote);
	return h248_txn(ci, req, resp, sizeof(resp));
}

static int bm_dlcx(struct bearer_ci *ci)
{
	char req[512], resp[2048];
	if (!ci)
		return -1;
	snprintf(req, sizeof(req),
		 "MEGACO/1 [%s]\r\nTransaction = %u {\r\n"
		 "  Context = %s {\r\n    Subtract = %s { }\r\n  }\r\n}\r\n",
		 "mgc", ++ci->txid, ci->ctx[0] ? ci->ctx : "-",
		 ci->term[0] ? ci->term : "$");
	h248_txn(ci, req, resp, sizeof(resp));
	close(ci->sock);
	free(ci);
	return 0;
}

static const struct bearer_driver MEGACO_DRIVER = {
	.name = "megaco", .crcx = bm_crcx, .mdcx = bm_mdcx, .dlcx = bm_dlcx,
};

const struct bearer_driver *bearer_megaco_driver(void)
{
	return &MEGACO_DRIVER;
}
