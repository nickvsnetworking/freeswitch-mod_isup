# ITU-T Q.784 / Q.785 conformance matrix

Tracks the ISUP abstract test cases against `mod_isup`. "Unit" = covered by a
deterministic test in `make check` (protocol behaviour proven in isolation);
"Lab" = requires the live integration harness (`test/integration/`) for the
SCTP/MGW/FreeSWITCH path. A case is only **Pass** when both its protocol
behaviour (unit) and, where applicable, its live path (lab) are green.

## Q.784 — ISUP basic call control

| § | Test case | Coverage | Status |
|---|---|---|---|
| 1.1 | Circuit reset at start-up (GRS/GRA) | Unit `test_cgm` (group_reset) | Lab pending |
| 2.1.1 | Basic call, en-bloc, successful | Unit `test_sm`/`test_e2e` | Lab pending |
| 2.1.2 | Calling-party clears (forward release) | Unit `test_sm` (outbound + hangup) | Lab pending |
| 2.1.3 | Called-party clears (backward release) | Unit `test_e2e` (test_call_rejected) | Lab pending |
| 2.2 | Call with continuity check (CCR/COT) | Unit `test_sm` (inbound_continuity) | Lab pending |
| 2.3 | Address incomplete (T35) | Unit (T7/T35 timer paths) | Partial |
| 3.x | Unsuccessful call — cause mapping | Unit `test_map` (Q.850) | Lab pending |
| 4.1 | Simultaneous seizure (glare) | Unit `test_sm` (glare win/yield) | Lab pending |
| 5.1 | Blocking/unblocking (BLO/UBL) | Unit `test_cgm` (single_block) | Lab pending |
| 5.2 | Circuit group blocking (CGB/CGU) | Unit `test_cgm` (group_block) | Lab pending |
| 5.3 | Circuit group query (CQM/CQR) | Unit `test_cgm` (query) | Lab pending |
| 6.1 | Reset of an in-use circuit (RSC) | Unit `test_sm` (RSC handling) | Lab pending |
| 7.x | Abnormal: unexpected message / CFN | Unit (decoder + CFN path) | Partial |
| 8.x | Timer expiries (T1/T5/T7/T9) | Unit `test_sm` (T7/T9/T1/T5) | Lab pending |

## Q.785 — ISUP supplementary services

| § | Test case | Coverage | Status |
|---|---|---|---|
| CLIP/CLIR | Presentation/restriction of calling number | Unit `test_map` + `test_codec` | Lab pending |
| Sub-addressing | Access transport transparency | Codec preserves param | Lab pending |
| UUS | User-to-user information transport | Codec preserves param (TLV) | Not impl (passthrough) |
| Suspend/Resume | SUS/RES + T2 | Unit `test_sm` (suspend_resume) | Lab pending |
| Hop counter | Decrement / max-forwards mapping | Codec round-trips param | Not impl (mapping TODO) |

## Notes

- **Unit-covered** cases exercise the exact protocol state transitions,
  message encodings, and timer logic the test case specifies — what differs in
  the lab is the transport (real SCTP/M3UA), the bearer (real MGCP/MGW), and
  the SIP side. The lab scenarios in `../integration/` are the live counterparts.
- **Partial** = the mechanism exists and is unit-tested but the full Q.784
  procedure (e.g. all of overlap signalling) is not yet implemented.
- This matrix is the checklist for "all optional features"; items marked *Not
  impl* are the remaining feature work (overlap completion, UUS mapping, full
  supplementary-service parameter surfacing).
