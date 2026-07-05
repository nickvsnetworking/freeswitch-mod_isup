/*
 * mod_isup — ITU-T ISUP (Q.763 / Q.764) protocol definitions
 *
 * Message type and parameter code constants per ITU-T Q.762/Q.763,
 * plus the in-memory parsed message representation used by the codec.
 *
 * This header has NO dependency on FreeSWITCH or Osmocom — it is pure
 * protocol so it can be unit-tested and fuzzed standalone.
 */
#ifndef ISUP_PROTO_H
#define ISUP_PROTO_H

#include <stdint.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/* Message type codes — ITU-T Q.762 Table 1                            */
/* ------------------------------------------------------------------ */
enum isup_msg_type {
	ISUP_MT_IAM   = 0x01, /* Initial address */
	ISUP_MT_SAM   = 0x02, /* Subsequent address */
	ISUP_MT_INR   = 0x03, /* Information request */
	ISUP_MT_INF   = 0x04, /* Information */
	ISUP_MT_COT   = 0x05, /* Continuity */
	ISUP_MT_ACM   = 0x06, /* Address complete */
	ISUP_MT_CON   = 0x07, /* Connect */
	ISUP_MT_FOT   = 0x08, /* Forward transfer */
	ISUP_MT_ANM   = 0x09, /* Answer */
	ISUP_MT_REL   = 0x0c, /* Release */
	ISUP_MT_SUS   = 0x0d, /* Suspend */
	ISUP_MT_RES   = 0x0e, /* Resume */
	ISUP_MT_RLC   = 0x10, /* Release complete */
	ISUP_MT_CCR   = 0x11, /* Continuity check request */
	ISUP_MT_RSC   = 0x12, /* Reset circuit */
	ISUP_MT_BLO   = 0x13, /* Blocking */
	ISUP_MT_UBL   = 0x14, /* Unblocking */
	ISUP_MT_BLA   = 0x15, /* Blocking acknowledgement */
	ISUP_MT_UBA   = 0x16, /* Unblocking acknowledgement */
	ISUP_MT_GRS   = 0x17, /* Circuit group reset */
	ISUP_MT_CGB   = 0x18, /* Circuit group blocking */
	ISUP_MT_CGU   = 0x19, /* Circuit group unblocking */
	ISUP_MT_CGBA  = 0x1a, /* Circuit group blocking acknowledgement */
	ISUP_MT_CGUA  = 0x1b, /* Circuit group unblocking acknowledgement */
	ISUP_MT_FAR   = 0x1f, /* Facility request */
	ISUP_MT_FAA   = 0x20, /* Facility accepted */
	ISUP_MT_FRJ   = 0x21, /* Facility reject */
	ISUP_MT_LPA   = 0x24, /* Loop back acknowledgement */
	ISUP_MT_PAM   = 0x28, /* Pass along */
	ISUP_MT_GRA   = 0x29, /* Circuit group reset acknowledgement */
	ISUP_MT_CQM   = 0x2a, /* Circuit group query */
	ISUP_MT_CQR   = 0x2b, /* Circuit group query response */
	ISUP_MT_CPG   = 0x2c, /* Call progress */
	ISUP_MT_USR   = 0x2d, /* User-to-user information */
	ISUP_MT_UCIC  = 0x2e, /* Unequipped circuit identification code */
	ISUP_MT_CFN   = 0x2f, /* Confusion */
	ISUP_MT_OLM   = 0x30, /* Overload */
	ISUP_MT_CRG   = 0x31, /* Charge information */
	ISUP_MT_NRM   = 0x32, /* Network resource management */
	ISUP_MT_FAC   = 0x33, /* Facility */
	ISUP_MT_UPT   = 0x34, /* User part test */
	ISUP_MT_UPA   = 0x35, /* User part available */
	ISUP_MT_IDR   = 0x36, /* Identification request */
	ISUP_MT_IRS   = 0x37, /* Identification response */
	ISUP_MT_SGM   = 0x38, /* Segmentation */
};

/* ------------------------------------------------------------------ */
/* Parameter codes — ITU-T Q.763 Table 5                               */
/* ------------------------------------------------------------------ */
enum isup_param_code {
	ISUP_P_END_OF_OPT          = 0x00,
	ISUP_P_CALL_REF            = 0x01,
	ISUP_P_TMR                 = 0x02, /* Transmission medium requirement */
	ISUP_P_ACCESS_TRANSPORT    = 0x03,
	ISUP_P_CALLED_NUMBER       = 0x04,
	ISUP_P_SUBSEQUENT_NUMBER   = 0x05,
	ISUP_P_NATURE_OF_CONN      = 0x06, /* Nature of connection indicators */
	ISUP_P_FWD_CALL_IND        = 0x07, /* Forward call indicators */
	ISUP_P_OPT_FWD_CALL_IND    = 0x08,
	ISUP_P_CALLING_CATEGORY    = 0x09, /* Calling party's category */
	ISUP_P_CALLING_NUMBER      = 0x0a,
	ISUP_P_REDIRECTING_NUMBER  = 0x0b,
	ISUP_P_REDIRECTION_NUMBER  = 0x0c,
	ISUP_P_CONNECTION_REQ      = 0x0d,
	ISUP_P_INR_IND             = 0x0e, /* Information request indicators */
	ISUP_P_INF_IND             = 0x0f, /* Information indicators */
	ISUP_P_CONTINUITY_IND      = 0x10,
	ISUP_P_BWD_CALL_IND        = 0x11, /* Backward call indicators */
	ISUP_P_CAUSE               = 0x12, /* Cause indicators (Q.850) */
	ISUP_P_REDIRECTION_INFO    = 0x13,
	ISUP_P_CIRC_GRP_SV_TYPE    = 0x15, /* Circuit group supervision msg type */
	ISUP_P_RANGE_AND_STATUS    = 0x16,
	ISUP_P_FACILITY_IND        = 0x18,
	ISUP_P_CUG_INTERLOCK       = 0x1a,
	ISUP_P_USER_SERVICE_INFO   = 0x1d,
	ISUP_P_SIGNALLING_PC       = 0x1e,
	ISUP_P_USER_TO_USER_INFO   = 0x20,
	ISUP_P_CONNECTED_NUMBER    = 0x21,
	ISUP_P_SUSP_RESUME_IND     = 0x22,
	ISUP_P_TRANSIT_NETW_SEL    = 0x23,
	ISUP_P_EVENT_INFO          = 0x24,
	ISUP_P_CIRC_ASSIGN_MAP     = 0x25,
	ISUP_P_CIRC_STATE_IND      = 0x26,
	ISUP_P_AUTO_CONG_LEVEL     = 0x27,
	ISUP_P_ORIG_CALLED_NUMBER  = 0x28,
	ISUP_P_OPT_BWD_CALL_IND    = 0x29,
	ISUP_P_USER_TO_USER_IND    = 0x2a,
	ISUP_P_ORIG_ISC_PC         = 0x2b,
	ISUP_P_GENERIC_NOTIF       = 0x2c,
	ISUP_P_CALL_HISTORY_INFO   = 0x2d,
	ISUP_P_ACCESS_DELIV_INFO   = 0x2e,
	ISUP_P_NETW_SPEC_FACILITY  = 0x2f,
	ISUP_P_USER_SERVICE_INFO_P = 0x30,
	ISUP_P_PROPAGATION_DELAY   = 0x31,
	ISUP_P_REMOTE_OPERATIONS   = 0x32,
	ISUP_P_SERVICE_ACTIVATION  = 0x33,
	ISUP_P_USER_TELESERV_INFO  = 0x34,
	ISUP_P_TRANSMEDIUM_USED    = 0x35,
	ISUP_P_CALL_DIVERSION_INFO = 0x36,
	ISUP_P_ECHO_CONTROL_INFO   = 0x37,
	ISUP_P_MSG_COMPAT_INFO     = 0x38,
	ISUP_P_PARAM_COMPAT_INFO   = 0x39,
	ISUP_P_MLPP_PRECEDENCE     = 0x3a,
	ISUP_P_MCID_REQ_IND        = 0x3b,
	ISUP_P_MCID_RSP_IND        = 0x3c,
	ISUP_P_HOP_COUNTER         = 0x3d,
	ISUP_P_TMR_PRIME           = 0x3e,
	ISUP_P_LOCATION_NUMBER     = 0x3f,
	ISUP_P_REDIR_NUM_RESTRICT  = 0x40,
	ISUP_P_CALL_TRANSFER_REF   = 0x43,
	ISUP_P_LOOP_PREVENTION     = 0x44,
	ISUP_P_CALL_TRANSFER_NUM   = 0x45,
	ISUP_P_CCSS                = 0x4b,
	ISUP_P_FORWARD_GVNS        = 0x4c,
	ISUP_P_BACKWARD_GVNS       = 0x4d,
	ISUP_P_REDIRECT_CAPAB      = 0x4e,
	ISUP_P_GENERIC_NUMBER      = 0xc0,
	ISUP_P_GENERIC_DIGITS      = 0xc1,
};

/* Nature of address indicator — Q.763 §3.9 */
enum isup_noa {
	ISUP_NOA_SUBSCRIBER       = 1,
	ISUP_NOA_UNKNOWN          = 2,
	ISUP_NOA_NATIONAL         = 3,
	ISUP_NOA_INTERNATIONAL    = 4,
	ISUP_NOA_NETWORK_SPECIFIC = 5,
};

/* Numbering plan indicator */
enum isup_npi {
	ISUP_NPI_ISDN     = 1,
	ISUP_NPI_DATA     = 3,
	ISUP_NPI_TELEX    = 4,
	ISUP_NPI_PRIVATE  = 5,
};

/* Address presentation restricted indicator */
enum isup_apri {
	ISUP_APRI_ALLOWED        = 0,
	ISUP_APRI_RESTRICTED     = 1, /* CLIR */
	ISUP_APRI_NOT_AVAILABLE  = 2,
};

/* Screening indicator */
enum isup_screening {
	ISUP_SCR_USER_NOT_VERIFIED = 0,
	ISUP_SCR_USER_VERIFIED     = 1,
	ISUP_SCR_USER_FAILED       = 2,
	ISUP_SCR_NETWORK_PROVIDED  = 3,
};

/* Transmission medium requirement — Q.763 §3.54 */
enum isup_tmr {
	ISUP_TMR_SPEECH          = 0,
	ISUP_TMR_64K_UNRESTRICTED = 2,
	ISUP_TMR_3K1_AUDIO       = 3,
	ISUP_TMR_64K_PREFERRED   = 6,
};

/* Calling party's category — Q.763 §3.11 (subset) */
enum isup_cpc {
	ISUP_CPC_UNKNOWN          = 0x00,
	ISUP_CPC_OPERATOR_FR      = 0x01,
	ISUP_CPC_OPERATOR_EN      = 0x04,
	ISUP_CPC_ORDINARY         = 0x0a,
	ISUP_CPC_PRIORITY         = 0x0b,
	ISUP_CPC_DATA_CALL        = 0x0c,
	ISUP_CPC_TEST_CALL        = 0x0d,
	ISUP_CPC_PAYPHONE         = 0x0f,
};

/* Event information (CPG) — Q.763 §3.21 */
enum isup_event {
	ISUP_EVENT_ALERTING      = 0x01,
	ISUP_EVENT_PROGRESS      = 0x02,
	ISUP_EVENT_INBAND_INFO   = 0x03,
	ISUP_EVENT_FWD_BUSY      = 0x04,
	ISUP_EVENT_NO_REPLY      = 0x05,
	ISUP_EVENT_UNCONDITIONAL = 0x06,
};

/* ------------------------------------------------------------------ */
/* Parsed message representation                                       */
/* ------------------------------------------------------------------ */
#define ISUP_MAX_PARAMS    32
#define ISUP_MAX_PARAM_LEN 255  /* a length octet can express up to 255 */
#define ISUP_MAX_MSG_LEN   273  /* CIC(2)+MT(1) + classic MTP3 payload     */
#define ISUP_MAX_ENC_LEN   4096 /* scratch ceiling before segmentation     */

struct isup_param {
	uint8_t code;
	uint8_t len;
	uint8_t val[ISUP_MAX_PARAM_LEN];
};

struct isup_msg {
	uint16_t cic;       /* 12-bit circuit identification code */
	uint8_t  msg_type;
	uint8_t  n_params;
	struct isup_param params[ISUP_MAX_PARAMS];
};

/* Codec result codes */
enum isup_rc {
	ISUP_OK              =  0,
	ISUP_ERR_TRUNCATED   = -1, /* ran off the end of the buffer */
	ISUP_ERR_UNKNOWN_MT  = -2, /* no descriptor for this message type */
	ISUP_ERR_MISSING     = -3, /* mandatory parameter absent */
	ISUP_ERR_BADLEN      = -4, /* parameter length wrong for fixed param */
	ISUP_ERR_TOOMANY     = -5, /* exceeded ISUP_MAX_PARAMS */
	ISUP_ERR_NOSPACE     = -6, /* output buffer too small */
	ISUP_ERR_BADPTR      = -7, /* pointer out of range / mandatory var absent */
};

#endif /* ISUP_PROTO_H */
