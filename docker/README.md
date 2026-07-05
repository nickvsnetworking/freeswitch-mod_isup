# mod_isup 3-container ISUP call lab

Two FreeSWITCH instances, each running `mod_isup`, place an ISUP call to each
other over **M3UA** through an **osmo-stp**, with the bearer on **osmo-mgw**.

```
 fs-a (PC 0.0.1) ──M3UA/SCTP──┐                  ┌──M3UA/SCTP── fs-b (PC 0.0.2)
   mod_isup ASP               ▼                  ▼               mod_isup ASP (auto-answer)
                          ┌────────── sigtran ──────────┐
                          │  osmo-stp (routes by DPC)   │
                          │  osmo-mgw (RTP bearer 2427) │
                          └─────────────────────────────┘
```

Containers (`docker compose`):
- **sigtran** — osmo-stp (M3UA STP, SCTP 2905) + osmo-mgw (MGCP 2427), 172.30.0.10
- **fs-b** — FreeSWITCH + mod_isup, terminating exchange (answers), 172.30.0.12
- **fs-a** — FreeSWITCH + mod_isup, originating exchange, 172.30.0.11

## Run

```sh
cd mod_isup/docker
docker compose build
docker compose up
```

fs-a waits for its M3UA association to come up, then originates
`isup/lab/1002`. The STP routes the IAM to fs-b, which allocates a bearer on
osmo-mgw and answers; fs-a sees the answer and then clears the call.

## Verified call ladder

```
fs-a  CRCX rtpbridge/*@mgw -> MGW rtp 16002
fs-a  --IAM (cic=1, dpc=0.0.2)--> osmo-stp --> fs-b
fs-b  CRCX rtpbridge/*@mgw -> MGW rtp 16004
fs-b  --ACM--> ... --ANM--> osmo-stp --> fs-a      (call ANSWERED, originate SUCCESS)
fs-a  --REL--> fs-b  ;  fs-b --RLC--> fs-a
```

## How the prebuilt FreeSWITCH is reused

The image is Debian bullseye (OpenSSL 1.1 + libsofia-sip-ua0), into which the
repo's prebuilt `libfreeswitch.so` is copied (it needs only GLIBC ≤ 2.29). A
tiny stub `libspandsp.so.3` satisfies the one DT_NEEDED FreeSWITCH never calls
on this path. `mod_isup` is compiled in-container against `libosmo-sigtran`
(2.2.1) and `libfreeswitch`. A minimal `freeswitch.xml` lets full
`switch_core_init()` complete.

## Notes / known simplifications

- fs-b auto-answers inbound calls (no dialplan modules are built in this image);
  `ISUP_AUTOANSWER=1` makes `mod_isup` answer in-module so the full
  IAM/ACM/ANM/REL/RLC ladder runs.
- Media (RTP) is anchored on osmo-mgw per leg; the two legs are not
  cross-connected for audio in this demo (signalling + bearer control is the
  focus).

## Automated validation

`./docker/validate.sh` builds the lab, captures on the sigtran hub, runs a
parameterised call, and asserts with tshark:

```
PASS: ladder: Initial address / Address complete / Answer / Release / Release complete
PASS: IAM TMR = 3.1 kHz audio        (set via isup_tmr=3k1)
PASS: IAM CPC = payphone             (set via isup_cpc=payphone)
PASS: IAM calling number = 61755500009 (set via isup_calling_number)
PASS: IAM hop counter present
PASS: ACM backward call indicators present
PASS: fs-a: call answered after alerting   (proper IAM->ACM->ANM flow)
PASS: fs-b/fs-a: SCCP enabled (SI=3)       (optional, via ISUP_SCCP_SSN)
RESULT: 13 passed, 0 failed
```

## Build-context note

The lab reuses a prebuilt `libfreeswitch`, so the compose **build context is the
FreeSWITCH source tree** (`context: ../..`). Place this repo at
`<freeswitch>/mod_isup` (the natural spot for an out-of-tree module) and run
`cd mod_isup/docker && docker compose build` — the context then provides
`.libs/libfreeswitch.so*` and `src/include`. Standalone (outside a FreeSWITCH
tree) you must point the context at one.

## Configurable knobs (per container, via environment)

| Env | Meaning |
|---|---|
| `ISUP_OPC` / `ISUP_PEER_DPC` | local / peer point code |
| `ISUP_CS7_CFG` | osmo cs7 (M3UA ASP) config file |
| `ISUP_MGW` | MGW `ip:port` for MGCP bearer |
| `ISUP_AUTOANSWER` | demo: answer inbound calls in-module |
| `ISUP_SCCP_SSN` | enable SCCP (SI=3) bound on this SSN (unset = off) |
| `ISUP_ORIG_VARS` | `{var=val,...}` IAM params for the originate |
