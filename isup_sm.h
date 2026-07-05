/*
 * mod_isup — Q.764 per-circuit call-control state machine.
 *
 * The state machine is deliberately decoupled from FreeSWITCH, the M3UA
 * transport, and the bearer (MGW): all outward actions go through a
 * caller-supplied ops vtable. That keeps the protocol logic pure and
 * unit-testable with mock ops, and lets the real module wire send() to
 * M3UA, the bearer_* hooks to libosmo-mgcp-client, and the to_fs_* hooks
 * to the FreeSWITCH session.
 */
#ifndef ISUP_SM_H
#define ISUP_SM_H

#include "isup_proto.h"
#include "isup_codec.h"

/* Per-circuit call states — Q.764 */
enum isup_cic_state {
	CIC_IDLE = 0,
	CIC_OUT_BEARER,   /* outbound: CRCX in flight before IAM */
	CIC_IAM_SENT,     /* outbound: IAM sent, awaiting ACM/ANM/CON (T7) */
	CIC_OUT_RINGING,  /* outbound: ACM received, awaiting ANM (T9) */
	CIC_IN_BEARER,    /* inbound: CRCX in flight after IAM */
	CIC_IN_WAIT_COT,  /* inbound: awaiting COT after IAM w/ continuity (T8) */
	CIC_IN_PROCEEDING,/* inbound: bearer up, waiting for FS to alert/answer */
	CIC_IN_RINGING,   /* inbound: ACM sent */
	CIC_ACTIVE,       /* answered, through-connected */
	CIC_REL_SENT,     /* REL sent, awaiting RLC (T1/T5) */
	CIC_RELEASING,    /* DLCX in flight */
	CIC_RESET,        /* RSC/GRS in progress */
	CIC_BLOCKED,      /* locally/remotely blocked */
};

/* Timer identifiers — Q.764 Annex A (subset wired into the SM). */
enum isup_timer_id {
	ISUP_T1 = 1,  /* await RLC after REL */
	ISUP_T2,      /* await RES after network SUS */
	ISUP_T5,      /* long REL guard -> RSC */
	ISUP_T7,      /* await ACM after IAM */
	ISUP_T8,      /* await COT after IAM (continuity) */
	ISUP_T9,      /* await ANM after ACM */
	ISUP_T12,     /* await BLA after BLO */
	ISUP_T16,     /* await RLC after RSC */
	ISUP_T22,     /* await GRA after GRS */
};

struct isup_cic; /* fwd */

/* Outward actions. user is the opaque ctx from isup_sm_init(). */
struct isup_sm_ops {
	/* Transmit a fully-built ISUP message towards the peer (via M3UA). */
	void (*send)(void *user, const struct isup_msg *m);

	/* Bearer (MGW) control. crcx is asynchronous: the SM proceeds when
	 * the caller later calls isup_sm_bearer_ready(). mode is "recvonly"
	 * / "sendrecv" / "loopback". */
	void (*bearer_crcx)(void *user, const char *mode);
	void (*bearer_mode)(void *user, const char *mode);
	void (*bearer_dlcx)(void *user);

	/* Towards FreeSWITCH. */
	void (*fs_setup)(void *user, const struct isup_msg *iam); /* inbound new call */
	void (*fs_progress)(void *user, int answered); /* ACM/CPG: ring/early media */
	void (*fs_answer)(void *user);                 /* ANM/CON received */
	void (*fs_release)(void *user, uint8_t cause_value);

	/* Timers, in milliseconds. */
	void (*start_timer)(void *user, int timer_id, int ms);
	void (*stop_timer)(void *user, int timer_id);
};

struct isup_cic {
	uint16_t                  cic;
	enum isup_cic_state       state;
	const struct isup_sm_ops *ops;
	void                     *user;

	int      controlling;     /* 1 if this exchange controls the CIC (glare) */
	uint8_t  last_cause;      /* cause from the last REL */
	uint8_t  t1_retries;      /* REL retransmit count */
	int      continuity;      /* IAM requested continuity on this circuit */
	int      suspended;       /* call suspended (SUS received) */
	uint8_t  bci[2];          /* Backward Call Indicators for ACM/CON */
	int      bci_set;         /* 1 if bci[] overrides the default */

	struct isup_msg pending_iam; /* outbound IAM held until bearer ready */
};

/* Lifecycle */
void isup_sm_init(struct isup_cic *c, uint16_t cic,
		  const struct isup_sm_ops *ops, void *user, int controlling);

/* Set the Backward Call Indicators used in the next ACM/CON (2 octets,
 * Q.763 §3.5). If never set, a sensible default is used. */
void isup_sm_set_bci(struct isup_cic *c, uint8_t b0, uint8_t b1);

/* FreeSWITCH-side stimuli */
void isup_sm_originate(struct isup_cic *c, const struct isup_msg *iam);
void isup_sm_proceeding(struct isup_cic *c);  /* inbound: send ACM (alerting) */
void isup_sm_answer(struct isup_cic *c);      /* inbound: send ANM */
void isup_sm_hangup(struct isup_cic *c, uint8_t cause_value);

/* Force the circuit back to idle on a group reset (GRS): release any call in
 * progress but emit no per-circuit RLC (the GRA covers the range). */
void isup_sm_reset(struct isup_cic *c);

/* Bearer-side stimulus: CRCX completed. */
void isup_sm_bearer_ready(struct isup_cic *c);

/* Transport-side stimulus: a decoded ISUP message arrived for this CIC. */
void isup_sm_rx(struct isup_cic *c, const struct isup_msg *m);

/* Timer expiry. */
void isup_sm_timer(struct isup_cic *c, int timer_id);

const char *isup_state_name(enum isup_cic_state s);

#endif /* ISUP_SM_H */
