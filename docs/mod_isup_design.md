# mod_isup — FreeSWITCH ISUP-over-M3UA MGCF

> **Status:** Design. Supersedes the earlier MGCP-external draft of this document.
>
> **Project intent:** Retro-networking. The goal is a **reference-grade, standards-compliant
> ITU-T ISUP implementation** that interworks the PSTN signalling world with FreeSWITCH/SIP.
> Adherence to the specifications and implementation of the **full message set, all parameters,
> and the optional procedures** is an explicit objective — not a "minimum viable interconnect."
> Comprehensive, Wireshark-grade debugging and tracing is a first-class requirement.

---

## 1. Goals & non-goals

### Goals

- **Bidirectional call control:** FreeSWITCH can *originate* calls into the ISUP network and
  *terminate* calls arriving from it, fully scriptable through the dialplan.
- **Full ITU-T ISUP** per Q.761–Q.764 (1999/2003 blue+ series), including the complete message
  set, all mandatory and optional parameters, segmentation (SGM), and the optional procedures:
  overlap, continuity check, circuit supervision (blocking / reset / query), suspend/resume,
  charging, network management, user-to-user signalling, and the Q.730-series supplementary
  service parameters.
- **SIGTRAN transport** via Osmocom: SCTP + M3UA, deployed as an **ASP to an external STP/SG**,
  with SCTP multihoming. ISUP rides M3UA directly as the SI=5 MTP-User.
- **Decomposed bearer (MGCF/MGW):** the module is a Media Gateway Control Function. It controls
  an osmo-mgw via MGCP; the MGW fronts the TDM↔IP conversion and relays RTP to FreeSWITCH, which
  anchors media on the SIP side.
- **Reference-grade observability:** decoded per-message tracing, raw hex, state-transition logs,
  per-CIC/per-message counters, FS events, and pcap export consumable by Wireshark's ISUP/M3UA
  dissectors.
- **Conformance-tested** against the ITU test suites Q.784 (basic call) and Q.785 (supplementary
  services), plus decoder fuzzing.

### Non-goals (v1)

- **ANSI T1.113 ISUP** — ITU-T only. (Parameter/codec layer is structured so ANSI can be added later.)
- **TCAP / MAP / INAP** — SCCP (SI=3) is wired up in the transport but reserved; no transaction layer yet.
- **BICC / bearer-independent IP bearer control** — not needed: osmo-mgw owns the bearer association
  (see §2.3).
- **Node-level failover / state replication** — single node; SCTP multihoming covers link resilience only.
- **MEGACO/H.248 bearer** — designed for from day one as a second bearer driver, but MGCP/osmo-mgw ships first.

---

## 2. Architecture

`mod_isup` is a single **out-of-tree, loadable** FreeSWITCH module. It is **not** a core patch and
requires no FreeSWITCH rebuild — it is compiled against the installed FS dev headers and dropped in
like any third-party module.

> **Licensing note.** FreeSWITCH core is MPL-1.1; `libosmocore` / `libosmo-sigtran` are GPLv2-or-later.
> A module linking them is effectively GPL. Keeping `mod_isup` out-of-tree keeps that GPL boundary
> clean and avoids any attempt to merge it into the MPL core tree.

```
┌───────────────────────────────── mod_isup (.so, GPL, out-of-tree) ────────────────────────────────┐
│                                                                                                    │
│   FreeSWITCH endpoint iface         ISUP layer (ours — Q.763/764)            Bearer driver API      │
│   ┌─────────────────────┐    ┌──────────────────────────────────────┐    ┌──────────────────────┐  │
│   │ channels, dialplan, │    │  Q.763 codec  │  Q.764 per-CIC FSM    │    │ bearer_mgcp (now)    │  │
│   │ originate, channel  │◄──►│  all params   │  timers T1..T39       │◄──►│  libosmo-mgcp-client │  │
│   │ vars, CLI/API/events│    │  segmentation │  CGM / COT / overlap  │    │ bearer_megaco(later) │  │
│   └─────────────────────┘    │               │  SUS/RES / supp svc   │    └──────────┬───────────┘  │
│            ▲ (marshal)        └───────────────┴──────────┬───────────┘               │ MGCP         │
│            │                                             │ MTP-User SI=5             │              │
│   ┌────────┴─────────────────────────────────────────────┴──────────────────┐       ▼              │
│   │   OSMO THREAD — owns ALL ISUP protocol state & the CIC table             │   osmo-mgw           │
│   │   osmo_select() loop  ·  eventfd-woken inbound queue (FS→Osmo)           │   (fronts TDM↔IP,    │
│   │   libosmo-sigtran: SCTP (multihomed) + M3UA AS/ASP                       │    relays RTP to FS) │
│   │   libosmo-sccp: SI=3 SCCP user registered but reserved (future TCAP)     │                      │
│   └─────────────────────────────────┬───────────────────────────────────────┘                      │
└─────────────────────────────────────┼──────────────────────────────────────────────────────────────┘
                                       │ SCTP/M3UA (ASP → STP/SG)                  RTP: osmo-mgw ↔ FS ↔ Sofia
                                       ▼
                                external STP / SG  ───►  PSTN / ISUP network
```

### 2.1 Layering

| Layer | Provided by | Notes |
|---|---|---|
| SCTP (multihomed) | `libosmo-netif` / kernel SCTP | Association to the STP/SG |
| M3UA (RFC 4666) AS/ASP | `libosmo-sigtran` (`osmo_ss7`) | ASP-side state machine, ASPSM/ASPTM, routing context |
| MTP3-User demux (by SI) | `osmo_ss7` MTP-user registration | We register **SI=5 (ISUP)**; SCCP self-registers SI=3 |
| SCCP (RFC 3868-adjacent) | `libosmo-sccp` | Instantiated but **reserved** — no TCAP yet |
| **ISUP Q.763/Q.764** | **ours** | Codec + per-CIC state machine + procedures |
| ISUP↔SIP/FS mapping | ours | Numbers, cause, codec/TMR, redirection, UUS |
| Bearer control | `libosmo-mgcp-client` (MGCP) | Driver-abstracted; MEGACO driver later |
| FS endpoint | FreeSWITCH core | Channels, dialplan, bridging to Sofia |

### 2.2 Threading model — single protocol-state owner

Decision: **all ISUP protocol state lives on the Osmo `select()` thread.** This is the single
biggest correctness lever, so it is a hard rule:

- The CIC table, every per-CIC FSM, all timers, and the M3UA/bearer interactions execute **only**
  on the Osmo thread. There is exactly one writer of protocol state → no per-CIC locks, no lock
  ordering, no FSM re-entrancy from foreign threads.
- **FS → ISUP** (outbound originate, answer, hangup, DTMF, CLI commands): the FS session/worker
  thread builds a small command struct, pushes it onto a lock-free/mutex-guarded inbound queue,
  and wakes the Osmo loop via an **`eventfd`** registered in `osmo_select()`. The Osmo thread drains
  the queue and applies the command to the FSM.
- **ISUP → FS** (incoming IAM creating a channel, ACM/ANM/CPG/REL driving channel state): the Osmo
  thread never blocks on FS. It uses the non-blocking FS primitives — `switch_core_session_request`,
  then queues messages/events onto the *session's* own queue (`switch_core_session_queue_message`,
  `switch_channel_*` state changes), which FS's session thread consumes. Media setup results
  (CRCX/MDCX responses) come back on the Osmo thread because the MGCP client also runs there.
- Bearer (MGCP) I/O is on the Osmo thread too (libosmo-mgcp-client uses the same select loop), so
  CRCX/MDCX/DLCX completion callbacks land where the FSM lives — no cross-thread bearer state.

```
 FS session thread                  Osmo thread (single owner)                 FS session thread
 ────────────────                   ──────────────────────────                 ────────────────
 originate() ──push cmd──► [inbound queue] ──eventfd wake──► drain ─► FSM ─► CRCX ─► IAM
                                                                  │
 IAM arrives ◄── M3UA ── select() ── FSM ── session_request ──► queue onto new session ──► on_init()
                                                                  └─► ACM/ANM ─► queue channel msgs ──►
```

### 2.3 Bearer model (MGCF / osmo-mgw)

osmo-mgw fronts the TDM↔IP conversion, so the ISUP bearer association is resolved **by the MGW**,
not signalled in ISUP (ITU IAM carries no IP bearer info — this is why BICC is out of scope).

- **CIC → MGW endpoint** is a static, configured map. Each CIC names a deterministic osmo-mgw
  endpoint for its PSTN-side bearer; the FS-facing RTP connection is allocated per call.
- Per-call sequence (inbound): `CRCX(cic-endpoint, recvonly)` → MGW returns its RTP SDP → create FS
  channel with remote SDP = MGW RTP, FS allocates its own RTP → `MDCX(remote = FS RTP)` wires both
  directions → on answer `MDCX(sendrecv)` through-connects. `DLCX` on release.
- **COT loopback** is real: because the MGW owns a genuine bearer, continuity checks loop on the MGW
  (MDCX with a loopback connection option), satisfying CCR/COT properly.
- Bearer control sits behind a thin driver API (`bearer.h`) so the **MEGACO/H.248** driver later is
  a drop-in second backend.

---

## 3. ISUP protocol implementation (the core of the work)

### 3.1 Message set (Q.762 / Q.763)

Full ITU set implemented. Codec encodes/decodes all; the FSM acts on all. (ANSI-only messages such
as CVT/CVR are out of scope for v1.)

| Code | Msg | Code | Msg | Code | Msg |
|---|---|---|---|---|---|
| 0x01 | IAM | 0x12 | RSC | 0x29 | GRA |
| 0x02 | SAM | 0x13 | BLO | 0x2A | CQM |
| 0x03 | INR | 0x14 | UBL | 0x2B | CQR |
| 0x04 | INF | 0x15 | BLA | 0x2C | CPG |
| 0x05 | COT | 0x16 | UBA | 0x2D | USR |
| 0x06 | ACM | 0x17 | GRS | 0x2E | UCIC |
| 0x07 | CON | 0x18 | CGB | 0x2F | CFN |
| 0x08 | FOT | 0x19 | CGU | 0x30 | OLM |
| 0x09 | ANM | 0x1A | CGBA | 0x31 | CRG |
| 0x0C | REL | 0x1B | CGUA | 0x32 | NRM |
| 0x0D | SUS | 0x1F | FAR | 0x33 | FAC |
| 0x0E | RES | 0x20 | FAA | 0x34 | UPT |
| 0x10 | RLC | 0x21 | FRJ | 0x35 | UPA |
| 0x11 | CCR | 0x24 | LPA | 0x36 | IDR |
|  |  | 0x28 | PAM | 0x37 | IRS |
|  |  |  |  | 0x38 | SGM |

### 3.2 Parameters (Q.763)

Every Q.763 parameter is modelled as encode/decode pair + a typed struct, including (non-exhaustive):
Nature of Connection Indicators; Forward/Backward Call Indicators; Calling Party's Category;
Transmission Medium Requirement (+ Prime/Used); Called/Calling Party Number; Optional Forward/Backward
Call Indicators; Redirecting Number; Redirection Information; Original Called Number; Connected Number;
Cause Indicators (Q.850); Event Information; Continuity Indicators; Circuit State Indicator;
Range and Status (group msgs); Generic Number; Generic Digits; Generic Notification; User-to-User
Information; User-to-User Indicators; Access Transport (Q.931 IEs); User Service Information (+ Prime);
User Teleservice Information; Charge/Charged information; Hop Counter; Propagation Delay Counter;
Echo Control Information; MLPP Precedence; Closed User Group Interlock Code; Transit Network Selection;
Call Diversion Information/Treatment; Remote Operations (component for SS); Parameter Compatibility
Information; Message Compatibility Information; Suspend/Resume Indicators; Segmentation; Network
Specific Facility; Service Activation; Display Information.

Codec design rules:
- Strict **fixed / variable / optional** parameter framing per Q.763 §1, with the optional-parameter
  pointer and the End-of-Optional-Parameters octet handled generically.
- **Unknown / vendor parameters** are preserved (stored raw) and honoured per their Parameter
  Compatibility Information instructions (pass-on / discard / release) rather than dropped silently.
- **Segmentation (SGM):** messages exceeding the MTP/M3UA payload are split per Q.764 §2.1.12 and
  reassembled on receive (driven by the "optional forward/backward call indicator → simple
  segmentation" + SGM message).
- Decoder is **defensive and fuzzed** (see §7): bounds-checked, never trusts lengths, returns a
  structured parse error that the FSM maps to a Confusion (CFN) or a Q.850-coded REL.

### 3.3 Per-CIC state machine (Q.764)

Each circuit runs an independent FSM on the Osmo thread. Core states:

```
 IDLE
  ├─ originate ─────────► BEARER_OUT (CRCX) ─► IAM_SENT ──ACM──► OUT_RINGING ──ANM──► ACTIVE
  └─ IAM rx ────────────► BEARER_IN  (CRCX) ─► WAIT_CONT? ─► ACM_SENT ─► IN_RINGING ─answer► ACTIVE
                                                  (COT)
 ACTIVE ──REL rx/local hangup──► REL_SENT/REL_RECV ──RLC──► RELEASING (DLCX) ──► IDLE
 any ── BLO/UBL ──► (sub)BLOCKED      any ── GRS/RSC ──► RESET      any ── SUS/RES ──► SUSPENDED
```

The FSM is driven by a single event-dispatch table keyed on `(state, event)`, where events are:
received ISUP message types, FS commands (originate/answer/hangup/info), bearer-driver callbacks
(CRCX/MDCX/DLCX done/fail, COT loop result), and timer expiries. Sub-state machines compose for:

- **Circuit Group Management** (`isup_cgm`): GRS/GRA, RSC, BLO/UBL/BLA/UBA, CGB/CGU/CGBA/CGUA,
  CQM/CQR, UCIC. Maintenance vs hardware blocking tracked independently per CIC and per direction;
  range-and-status bitmaps for the group messages. Mandatory reset of all CICs on startup and after
  M3UA AS becomes ACTIVE / point code RESUME.
- **Continuity** (`isup_cont`): CCR → MGW loopback → COT(success/failure); IAM-with-continuity →
  await COT (T8); failure → recheck (CCR/T24/T25) then REL "continuity check failed."
- **Overlap** (`isup_overlap`): en-bloc vs overlap sending; SAM accumulation; INR/INF
  (info request/response); address-incomplete (T35) handling; minimum-digits / ST handling.
- **Suspend/Resume** (`isup_susres`): network- and user-initiated SUS/RES, T2/T6/T38.
- **Glare / dual seizure** resolution per Q.764 (controlling/non-controlling exchange by point code
  comparison; even/odd CIC convention).

### 3.4 Timers (Q.764 Annex A)

All ISUP timers implemented via `osmo_timer` on the Osmo thread. Representative set (configurable):

| Timer | Purpose | Default |
|---|---|---|
| T1 | Await RLC after REL; resend REL | 15–60 s |
| T2 | Await RES (suspend) | 180 s |
| T5 | Long REL guard → send RSC, alert | 5–15 min |
| T6 | Await RES, network suspend | 30 s |
| T7 | Await ACM/ANM after IAM (address complete) | 20–30 s |
| T8 | Await COT after IAM (continuity) | 10–15 s |
| T9 | Await ANM after ACM | 90–180 s |
| T11 | Await ACM, last SAM sent (overlap) | 15–20 s |
| T12–T15 | (Un)blocking ack repeats | 4–15 s |
| T16/T17 | RSC repeat / guard | 15–60 s / 5 min |
| T18–T21 | Group (un)blocking ack repeats/guards | 4–60 s |
| T22/T23 | GRS repeat / guard | 15–60 s / 5 min |
| T24–T27 | Continuity tone / recheck | per Q.764 |
| T33 | Await INF after INR | 12–15 s |
| T35 | Address incomplete → REL | 15–20 s |
| T38 | Await RES, network | 120 s |

Every timer start/stop/expiry is traceable (§6). Defaults overridable per profile.

### 3.5 ISUP ↔ SIP / FreeSWITCH mapping

| ISUP | FS / SIP |
|---|---|
| Called Party Number (digits, NOA, NPI, ST) | `destination_number`, `isup_called_noa` channel var |
| Calling Party Number (digits, NOA, NPI, APRI, screening) | caller_id_number/name; **APRI=restricted → CLIR** → SIP `Privacy: id` + `P-Asserted-Identity` |
| Calling Party's Category | `isup_cpc` (operator, payphone, ordinary, test…) |
| Cause Indicators (Q.850) | FS native **hangup cause is Q.850** → direct, lossless map both ways |
| TMR / User Service Info | codec/bearer selection: speech / 3.1 kHz audio / 64k unrestricted → A-law passthrough vs data |
| Redirecting Number / Redirection Info / Original Called Number | SIP `Diversion` / `History-Info` |
| Generic Number(s) | additional party info channel vars |
| User-to-User Info / Indicators | SIP `User-to-User` (RFC 7433) or `X-ISUP-UUI` |
| Hop Counter | `Max-Forwards` |
| Event Information (CPG) | early-media / progress, alerting, in-band-info indication |
| Access Transport (embedded Q.931 IEs) | parsed; relevant IEs surfaced as channel vars |

All raw ISUP parameters are also exposed as `isup_*` channel variables (read) and accepted as
dialplan/originate variables (write) so operators can drive any field for testing — essential for a
spec-completeness/retro project.

### 3.6 FreeSWITCH surface

- **Endpoint module** registered as `isup`. Inbound IAM → new channel into the configured
  `context`/`dialplan`. Outbound: `originate isup/<profile>/<number>` with automatic CIC hunting
  (even/odd, controlling-exchange aware to avoid glare); `isup/<profile>/<cic>/<number>` pins a CIC.
- **Channel variables:** full `isup_*` set for every parameter; `sip_h_*`-style passthrough for UUS.
- **API/CLI** (`isup …`): `status`, `profile <p>`, `cic <p> <cic>`, `block/unblock <p> <cic|range>`,
  `reset <p> <cic|range>`, `grs <p> <range>`, `query <p> <range>` (CQM), `loopback <p> <cic>`,
  `trace on|off|pcap <p>`, `linkstatus`, `stats`.
- **Events:** custom FS event subclasses (`isup::link_up`, `isup::link_down`, `isup::pc_pause`,
  `isup::pc_resume`, `isup::cic_blocked`, `isup::cic_reset`, `isup::alarm`) for ESL consumers.

---

## 4. Configuration (`isup.conf.xml`)

```xml
<configuration name="isup.conf" description="ISUP over M3UA (MGCF)">

  <settings>
    <param name="log-level" value="debug"/>
    <param name="pcap-dir" value="/var/log/freeswitch/isup"/>   <!-- per-profile pcap export -->
  </settings>

  <!-- SIGTRAN: one local point code, one or more M3UA ASPs to the STP/SG -->
  <sigtran>
    <local point-code="1-1-1" network-indicator="national"/>
    <m3ua name="stp-primary"
          asp-mode="active" routing-context="101"
          local-addr="10.0.0.10,10.0.0.11" local-port="2905"   <!-- multihomed SCTP -->
          remote-addr="10.0.0.1,10.0.0.2"  remote-port="2905"/>
  </sigtran>

  <!-- MGW for the bearer -->
  <mgw name="mgw1" host="10.0.1.1" port="2427" protocol="mgcp"/>

  <profiles>
    <profile name="stp-east" dpc="2-2-2" routing-context="101" mgw="mgw1"
             context="public" dialplan="XML"
             variant="itu" echo-control="true" continuity="optional">
      <circuits>
        <circuit cic="1"  endpoint="ds/e1-0/1"/>
        <circuit cic="2"  endpoint="ds/e1-0/2"/>
        <!-- ... -->
        <circuit cic="31" endpoint="ds/e1-0/31"/>
      </circuits>
      <timers>            <!-- optional per-profile overrides -->
        <timer name="T7" value="25000"/>
      </timers>
    </profile>
  </profiles>
</configuration>
```

---

## 5. Call flows

### 5.1 Inbound (PSTN → SIP)

```
STP/SG          mod_isup (Osmo thread)           osmo-mgw            FS session/Sofia
  │── IAM(CIC=5) ─►│                                 │                     │
  │                │── CRCX(ds/e1-0/5, recvonly) ───►│                     │
  │                │◄── 200 + MGW RTP ───────────────│                     │
  │                │── session_request, set remote=MGW RTP ───────────────►│ on_init
  │                │── MDCX(remote = FS RTP) ───────►│                     │
  │◄─ ACM ─────────│   (Sofia 180)                                        │ INVITE→ 180
  │◄─ ANM ─────────│── MDCX(sendrecv) ─────────────►│   (Sofia 200)       │ 200 OK
  │   ~~ RTP: PSTN ↔ osmo-mgw ↔ FS ↔ Sofia ~~                              │
  │── REL ─────────►│── channel hangup (Q.850) ─────────────────────────► │ BYE
  │◄─ RLC ─────────│── DLCX ───────────────────────►│                     │
```

### 5.2 Outbound (SIP → PSTN), with continuity check

```
FS/Sofia        mod_isup (Osmo thread)            osmo-mgw            STP/SG
  │─ originate ───►│── hunt CIC, CRCX(sendrecv) ──►│                     │
  │                │── (if continuity) MGW loop ──►│                     │
  │                │── IAM(CIC, [continuity]) ──────────────────────────►│
  │                │◄───────────────────────── COT(success) ────────────│
  │                │◄───────────────────────── ACM ─────────────────────│
  │◄ 180 ──────────│                                                     │
  │                │◄───────────────────────── ANM ─────────────────────│
  │◄ 200 ──────────│── MDCX(sendrecv) ───────────►│                     │
  │─ BYE ──────────►│── REL(Q.850 cause) ─────────────────────────────► │
  │                │◄───────────────────────── RLC ─────────────────────│── DLCX
```

### 5.3 Startup circuit reset (after M3UA AS ACTIVE)

GRS over the full CIC range → await GRA (T22/T23) → mark all IDLE/known → ready. Mirror handling for
inbound GRS. Same pattern for RSC (single CIC) and CGB/CGU group blocking.

---

## 6. Debugging & observability (first-class requirement)

- **Decoded message trace:** at DEBUG, every tx/rx ISUP message is logged fully decoded — message
  type, CIC, OPC/DPC, every parameter expanded to named fields and enum text (Wireshark-style),
  plus the raw hex of the SIF.
- **State-machine trace:** every `(cic, old_state) --event--> new_state` transition logged, with the
  triggering event and any timer started/stopped.
- **pcap export:** raw M3UA-over-SCTP (or synthesised MTP3) frames written to per-profile pcap files
  so traces open directly in Wireshark with its ISUP/M3UA dissectors. `isup trace pcap <p> on`.
- **Osmocom log integration:** libosmocore/`libosmo-sigtran` log targets are routed into the FS log
  with mapped levels, so SCTP/M3UA ASP-state events appear inline.
- **Counters & stats** (`isup stats`): per-profile and per-CIC tx/rx counts by message type, REL by
  Q.850 cause, retransmissions, timer expiries, decode errors, glare events, blocking/reset events.
- **FS events** for state changes (link, point-code, CIC) for external monitoring/ESL.
- **Optional HEP/Homer export** of decoded ISUP for centralised tracing (stretch goal; fits the
  existing Homer tooling in this environment).

---

## 7. Testing & conformance

Testing is part of the deliverable, not an afterthought.

1. **Codec unit tests** (`test/test_codec.c`): encode→decode→re-encode round-trip for every message
   and every parameter; golden byte vectors from real captures in `test/vectors/`; explicit
   boundary cases (max-length, zero-length, missing mandatory, bad optional pointer, segmentation).
2. **State-machine unit tests** (`test/test_sm.c`): drive each FSM with scripted event sequences
   against a **mock transport** and **mock bearer**; assert emitted messages, timer set/cancel, and
   resulting state for basic call, glare, blocking, reset, continuity, overlap, suspend/resume,
   abnormal releases, and every Q.764 timer expiry path.
3. **Decoder fuzzing** (`test/fuzz_isup_decode.c`): libFuzzer/AFL harness over the parameter and
   message decoders — the decoder must never crash, over-read, or hang on hostile input. (Fits the
   existing codec-fuzzing culture in this repo.)
4. **Integration** (`test/integration/`): docker-compose bringing up `osmo-stp` (as the STP/SG),
   `osmo-mgw`, this module in FreeSWITCH, and a **peer ISUP generator** (a second instance, or a
   scripted SIGTRAN tool). Automated scenarios place and receive calls end-to-end and assert media
   and signalling.
5. **Conformance suite:** scenario tests mapped to **ITU-T Q.784** (ISUP basic call control test
   specification) and **Q.785** (supplementary services), tracked as a coverage matrix so "all
   optional features" is demonstrably exercised.
6. **Regression:** golden pcap replays diffed against expected decoded output.

A `make check` target runs unit + fuzz (short) + codec vectors; integration/conformance run under
docker-compose in CI.

---

## 8. Directory layout

```
mod_isup/                         (out-of-tree)
  configure.ac / Makefile.am      — autotools, pkg-config: freeswitch + libosmo*
  mod_isup.c                      — load/unload, endpoint iface, app/API registration
  isup_profile.[ch]               — config parse, profile lifecycle
  isup_m3ua.[ch]                  — osmo_ss7 instance, ASP/AS, SCTP, SI=5 MTP-user, SCCP(reserved)
  isup_thread.[ch]                — Osmo select loop, eventfd inbound queue, FS↔Osmo marshalling
  isup_cic.[ch]                   — CIC table, per-circuit context
  isup_sm.[ch]                    — Q.764 dispatch + core call FSM
  isup_codec.[ch]                 — Q.763 message framing
  isup_params.[ch]                — every Q.763 parameter encode/decode + structs
  isup_segment.[ch]               — SGM segmentation/reassembly
  isup_timers.[ch]                — Q.764 timer table + helpers
  isup_cgm.[ch]                   — blocking / reset / query (GRS/RSC/BLO/CGB/CQM)
  isup_cont.[ch]                  — continuity (CCR/COT/LPA)
  isup_overlap.[ch]               — SAM/INR/INF/overlap
  isup_susres.[ch]                — suspend/resume
  isup_supp.[ch]                  — supplementary-service params (CLIP/CLIR/COLP/UUS/CUG/…)
  isup_map.[ch]                   — ISUP↔SIP/FS mapping (numbers, cause, codec, redirection)
  bearer.h                        — bearer driver API
  bearer_mgcp.c                   — MGCP driver (libosmo-mgcp-client → osmo-mgw)
  bearer_megaco.c                 — H.248 driver (later phase)
  isup_trace.[ch]                 — decoded trace, hex, pcap export, counters, events
  isup_cli.[ch]                   — CLI/API commands
  conf/isup.conf.xml              — sample config
  test/
    test_codec.c  test_sm.c  fuzz_isup_decode.c
    vectors/  integration/  conformance/
  README.md
```

---

## 9. Dependencies

- FreeSWITCH dev headers (matching the target FS version) — `pkg-config freeswitch`.
- `libosmocore`, `libosmo-netif`, `libosmo-sigtran` (osmo_ss7/M3UA/SCCP), `libosmo-mgcp-client`.
- Kernel SCTP (`lksctp`).
- A-law (PCMA) as the default bearer codec; speech/3.1 kHz/64k-data selectable via TMR.

---

## 10. Standards references

Q.700 (overview), Q.704 (MTP3 — superseded by M3UA here), **Q.761–Q.764** (ISUP), Q.730-series
(supplementary services), Q.850 (cause values), Q.931 (access transport IEs); **RFC 4666** (M3UA),
RFC 3868 (SUA — reserved), **RFC 3435** (MGCP), RFC 3525 (MEGACO/H.248 — later), RFC 3550 (RTP),
RFC 7433 (SIP UUI); **Q.784 / Q.785** (ISUP conformance test specifications).

---

## 11. Phased delivery

| Phase | Deliverable |
|---|---|
| 0 | Out-of-tree scaffold: module loads, endpoint registered, build against FS + libosmo* |
| 1 | Bearer driver API + MGCP/osmo-mgw driver; CRCX/MDCX/DLCX proven with a stub channel |
| 2 | SIGTRAN: SCTP/M3UA ASP up to osmo-stp; SI=5 MTP-user; **Osmo↔FS marshalling**; MTP-TRANSFER I/O |
| 3 | Q.763 codec: full message + parameter encode/decode; segmentation; codec unit tests + vectors + fuzz |
| 4 | Q.764 basic call FSM, **bidirectional**, FS endpoint + dialplan + originate; ISUP↔SIP/Q.850 mapping |
| 5 | Circuit supervision: GRS/GRA, RSC, BLO/UBL, CGB/CGU, CQM/CQR, UCIC; startup reset; glare |
| 6 | Continuity (CCR/COT/LPA) with MGW loopback |
| 7 | Overlap (SAM/INR/INF) + suspend/resume + charging (CRG) + network mgmt (UPT/UPA/OLM/NRM/CFN) |
| 8 | Supplementary-service params + UUS + all remaining optional parameters; full channel-var exposure |
| 9 | Debugging/observability: decoded trace, pcap export, counters, events, full CLI |
| 10 | Testing: state-machine suite, integration docker-compose, **Q.784/Q.785 conformance matrix** |
| 11 | MEGACO/H.248 bearer driver (second backend) |
| 12 | Hardening: SCTP multihoming failover, timer/guard soak, alarms, docs |
```
