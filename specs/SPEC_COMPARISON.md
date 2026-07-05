# mod_isup vs ITU-T Q.762/Q.763 code-point comparison

Reference: Wireshark ISUP dissector (`packet-isup.c`), which encodes the
ITU-T Q.762 message types and Q.763 parameter codes verbatim. (ITU's own
PDF portal blocks automated download.)

## Message types (Q.762)

| code | mod_isup | spec (Wireshark/Q.76x) | match |
|---|---|---|---|
| 0x01 | IAM | LENGTH | ✓ |
| 0x02 | SAM | SUBSEQ_ADDR | ✓ |
| 0x03 | INR | INFO_REQ | ✓ |
| 0x04 | INF | INFO | ✓ |
| 0x05 | COT | CONTINUITY | ✓ |
| 0x06 | ACM | ADDR_CMPL | ✓ |
| 0x07 | CON | CONNECT | ✓ |
| 0x08 | FOT | FORW_TRANS | ✓ |
| 0x09 | ANM | ANSWER | ✓ |
| 0x0c | REL | RELEASE | ✓ |
| 0x0d | SUS | SUSPEND | ✓ |
| 0x0e | RES | RESUME | ✓ |
| 0x10 | RLC | REL_CMPL | ✓ |
| 0x11 | CCR | CONT_CHECK_REQ | ✓ |
| 0x12 | RSC | RESET_CIRCUIT | ✓ |
| 0x13 | BLO | BLOCKING | ✓ |
| 0x14 | UBL | UNBLOCKING | ✓ |
| 0x15 | BLA | BLOCK_ACK | ✓ |
| 0x16 | UBA | UNBLOCK_ACK | ✓ |
| 0x17 | GRS | CIRC_GRP_RST | ✓ |
| 0x18 | CGB | CIRC_GRP_BLCK | ✓ |
| 0x19 | CGU | CIRC_GRP_UNBL | ✓ |
| 0x1a | CGBA | CIRC_GRP_BL_ACK | ✓ |
| 0x1b | CGUA | CIRC_GRP_UNBL_ACK | ✓ |
| 0x1f | FAR | FACILITY_REQ | ✓ |
| 0x20 | FAA | FACILITY_ACC | ✓ |
| 0x21 | FRJ | FACILITY_REJ | ✓ |
| 0x24 | LPA | LOOP_BACK_ACK | ✓ |
| 0x28 | PAM | PASS_ALONG | ✓ |
| 0x29 | GRA | CIRC_GRP_RST_ACK | ✓ |
| 0x2a | CQM | CIRC_GRP_QRY | ✓ |
| 0x2b | CQR | CIRC_GRP_QRY_RSP | ✓ |
| 0x2c | CPG | CALL_PROGRSS | ✓ |
| 0x2d | USR | USER2USER_INFO | ✓ |
| 0x2e | UCIC | UNEQUIPPED_CIC | ✓ |
| 0x2f | CFN | CONFUSION | ✓ |
| 0x30 | OLM | OVERLOAD | ✓ |
| 0x31 | CRG | CHARGE_INFO | ✓ |
| 0x32 | NRM | NETW_RESRC_MGMT | ✓ |
| 0x33 | FAC | FACILITY | ✓ |
| 0x34 | UPT | USER_PART_TEST | ✓ |
| 0x35 | UPA | USER_PART_AVAIL | ✓ |
| 0x36 | IDR | IDENT_REQ | ✓ |
| 0x37 | IRS | IDENT_RSP | ✓ |
| 0x38 | SGM | SEGMENTATION | ✓ |

**45 defined, 45 match spec code-point, 0 mismatched.**
Spec code-points not implemented (5): 0x40 LOOP_PREVENTION, 0x41 APPLICATION_TRANS, 0x42 PRE_RELEASE_INFO, 0x43 SUBSEQUENT_DIR_NUM, 0xfe JAPAN_CHARG_INF

## Parameter codes (Q.763)

| code | mod_isup | spec (Wireshark/Q.76x) | match |
|---|---|---|---|
| 0x00 | END_OF_OPT | End of optional parameters | ✓ |
| 0x01 | CALL_REF | Call Reference (national use) | ✓ |
| 0x02 | TMR | Transmission medium requirement | ✓ |
| 0x03 | ACCESS_TRANSPORT | Access transport | ✓ |
| 0x04 | CALLED_NUMBER | Called party number | ✓ |
| 0x05 | SUBSEQUENT_NUMBER | Subsequent number | ✓ |
| 0x06 | NATURE_OF_CONN | Nature of connection indicators | ✓ |
| 0x07 | FWD_CALL_IND | Forward call indicators | ✓ |
| 0x08 | OPT_FWD_CALL_IND | Optional forward call indicators | ✓ |
| 0x09 | CALLING_CATEGORY | Calling party's category | ✓ |
| 0x0a | CALLING_NUMBER | Calling party number | ✓ |
| 0x0b | REDIRECTING_NUMBER | Redirecting number | ✓ |
| 0x0c | REDIRECTION_NUMBER | Redirection number | ✓ |
| 0x0d | CONNECTION_REQ | Connection request | ✓ |
| 0x0e | INR_IND | Information request indicators (national use) | ✓ |
| 0x0f | INF_IND | Information indicators (national use) | ✓ |
| 0x10 | CONTINUITY_IND | Continuity request | ✓ |
| 0x11 | BWD_CALL_IND | Backward call indicators | ✓ |
| 0x12 | CAUSE | Cause indicators | ✓ |
| 0x13 | REDIRECTION_INFO | Redirection information | ✓ |
| 0x15 | CIRC_GRP_SV_TYPE | Circuit group supervision message type | ✓ |
| 0x16 | RANGE_AND_STATUS | Range and Status | ✓ |
| 0x18 | FACILITY_IND | Facility indicator | ✓ |
| 0x1a | CUG_INTERLOCK | Closed user group interlock code | ✓ |
| 0x1d | USER_SERVICE_INFO | User service information | ✓ |
| 0x1e | SIGNALLING_PC | Signalling point code (national use) | ✓ |
| 0x20 | USER_TO_USER_INFO | User-to-user information | ✓ |
| 0x21 | CONNECTED_NUMBER | Connected number | ✓ |
| 0x22 | SUSP_RESUME_IND | Suspend/Resume indicators | ✓ |
| 0x23 | TRANSIT_NETW_SEL | Transit network selection (national use) | ✓ |
| 0x24 | EVENT_INFO | Event information | ✓ |
| 0x25 | CIRC_ASSIGN_MAP | Circuit assignment map | ✓ |
| 0x26 | CIRC_STATE_IND | Circuit state indicator (national use) | ✓ |
| 0x27 | AUTO_CONG_LEVEL | Automatic congestion level | ✓ |
| 0x28 | ORIG_CALLED_NUMBER | Original called number | ✓ |
| 0x29 | OPT_BWD_CALL_IND | Backward call indicators | ✓ |
| 0x2a | USER_TO_USER_IND | User-to-user indicators | ✓ |
| 0x2b | ORIG_ISC_PC | Origination ISC point code | ✓ |
| 0x2c | GENERIC_NOTIF | Generic notification indicator | ✓ |
| 0x2d | CALL_HISTORY_INFO | Call history information | ✓ |
| 0x2e | ACCESS_DELIV_INFO | Access delivery information | ✓ |
| 0x2f | NETW_SPEC_FACILITY | Network specific facility (national use) | ✓ |
| 0x30 | USER_SERVICE_INFO_P | User service information prime | ✓ |
| 0x31 | PROPAGATION_DELAY | Propagation delay counter | ✓ |
| 0x32 | REMOTE_OPERATIONS | Remote operations (national use) | ✓ |
| 0x33 | SERVICE_ACTIVATION | Service activation | ✓ |
| 0x34 | USER_TELESERV_INFO | User teleservice information | ✓ |
| 0x35 | TRANSMEDIUM_USED | Transmission medium used | ✓ |
| 0x36 | CALL_DIVERSION_INFO | Call diversion information | ✓ |
| 0x37 | ECHO_CONTROL_INFO | Echo control information | ✓ |
| 0x38 | MSG_COMPAT_INFO | Message compatibility information | ✓ |
| 0x39 | PARAM_COMPAT_INFO | Parameter compatibility information | ✓ |
| 0x3a | MLPP_PRECEDENCE | MLPP precedence | ✓ |
| 0x3b | MCID_REQ_IND | MCID request indicators | ✓ |
| 0x3c | MCID_RSP_IND | MCID response indicators | ✓ |
| 0x3d | HOP_COUNTER | Hop counter | ✓ |
| 0x3e | TMR_PRIME | Transmission medium requirement prime | ✓ |
| 0x3f | LOCATION_NUMBER | Location number | ✓ |
| 0x40 | REDIR_NUM_RESTRICT | Redirection number restriction | ✓ |
| 0x43 | CALL_TRANSFER_REF | Call transfer reference | ✓ |
| 0x44 | LOOP_PREVENTION | Loop prevention indicators | ✓ |
| 0x45 | CALL_TRANSFER_NUM | Call transfer number | ✓ |
| 0x4b | CCSS | CCSS | ✓ |
| 0x4c | FORWARD_GVNS | Forward GVNS | ✓ |
| 0x4d | BACKWARD_GVNS | Backward GVNS | ✓ |
| 0x4e | REDIRECT_CAPAB | Redirect capability (reserved for national use) | ✓ |
| 0xc0 | GENERIC_NUMBER | Generic number | ✓ |
| 0xc1 | GENERIC_DIGITS | Generic digits (national use) | ✓ |

**68 defined, 68 match spec code-point, 0 mismatched.**
Spec code-points not implemented (15): 0x5b Network management controls, 0x65 Correlation id, 0x66 SCF id, 0x6e Call diversion treatment indicators, 0x6f Called IN number, 0x70 Call offering treatment indicators, 0x71 Charged party identification (national use), 0x72 Conference treatment indicators, 0x73 Display information, 0x74 UID action indicators, 0x75 UID capability indicators, 0x77 Redirect counter (reserved for national use), 0x78 Application transport, 0x79 Collect call request, 0x81 Calling geodetic location
