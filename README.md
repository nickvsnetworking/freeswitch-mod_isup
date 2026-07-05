# mod_isup

A FreeSWITCH ISUP-over-M3UA MGCF (Media Gateway Control Function). It bridges
SS7 ISUP calls — carried over M3UA/SCTP via `libosmo-sigtran` — to and from
FreeSWITCH, with the bearer controlled over MGCP against an `osmo-mgw`.

See [`docs/mod_isup_design.md`](docs/mod_isup_design.md) for the full
architecture; this README covers what is built, how to build it, and how to
test it.

## Layout

```
isup_proto.h     Q.762/Q.763 message + parameter constants, parsed message struct
isup_codec.[ch]  Q.763 message framing codec (fixed/variable/optional, pointers)
isup_param.[ch]  Q.763 parameter body encode/decode (numbers, cause, range/status)
isup_segment.[ch]Q.764 simple segmentation (SGM split + reassembly)
isup_sm.[ch]     Q.764 per-circuit call state machine (transport/bearer-agnostic)
isup_cgm.[ch]    Circuit group management (BLO/UBL, CGB/CGU, GRS/GRA, RSC, CQM/CQR)
isup_map.[ch]    ISUP <-> SIP / Q.850 interworking helpers
test/            unit, integration, hardening tests + libFuzzer harness
```

The codec, state machine, segmentation, circuit-group manager, and mapping are
**pure protocol logic with no FreeSWITCH or Osmocom dependency**, so they build
and test standalone. The transport (M3UA via `libosmo-sigtran`), bearer (MGCP
via `libosmo-mgcp-client`), and FreeSWITCH endpoint glue are layered on top and
require the live osmo-stp / osmo-mgw / FreeSWITCH environment to run.

## Design notes that matter

* **The state machine never touches sockets.** It consumes already-decoded
  messages via `isup_sm_rx()` and emits via an `ops` vtable. This is why the
  same FSM runs over the in-memory test transport (`test/test_e2e.c`) and, in
  the real module, over M3UA. It also means the codec is reusable if ISUP PDUs
  ever arrive over a non-M3UA carrier.
* **ISUP is the SI=5 MTP-User on M3UA** (`MTP_SI_ISUP` confirmed in the
  installed `libosmo-sigtran`). SCCP (SI=3) shares the same association but is
  reserved for a future TCAP layer — ISUP never runs *over* SCCP.

## Building

### As a FreeSWITCH module (in-tree)

The module builds inside a FreeSWITCH source tree as a git submodule. From the
FreeSWITCH checkout:

```sh
git submodule add https://github.com/nickvsnetworking/freeswitch-mod_isup.git \
    src/mod/endpoints/mod_isup
```

then register it with the build:

* add `src/mod/endpoints/mod_isup/Makefile` to the `AC_CONFIG_FILES` list in
  `configure.ac`, alongside a pkg-config check for the transport library:

  ```m4
  PKG_CHECK_MODULES([LIBOSMO_SIGTRAN], [libosmo-sigtran],
      [have_osmo_sigtran=yes], [have_osmo_sigtran=no])
  AM_CONDITIONAL([HAVE_OSMO_SIGTRAN], [test "x$have_osmo_sigtran" = xyes])
  ```

* add `endpoints/mod_isup` to `modules.conf`.

Then re-run `./bootstrap.sh && ./configure && make`. The bundled `Makefile.am`
links the module against `libosmo-sigtran` and the built `libfreeswitch`.

**Runtime dependencies:** `libosmo-sigtran` (M3UA transport) plus `libosmovty`,
`libosmocore`, and `libtalloc`. The bearer path additionally talks MGCP to an
`osmo-mgw`.

### Standalone protocol tests

The codec, state machine, segmentation, circuit-group manager, and mapping are
pure protocol logic with no FreeSWITCH or Osmocom dependency, so they build and
test on their own via the top-level `Makefile`:

```sh
make check          # build + run all unit/integration/hardening tests
make fuzz           # coverage-guided fuzzing of the whole input path (clang)
make fuzz FUZZ_TIME=600   # longer soak
```

`make check` builds every test with **`-Werror -fsanitize=address,undefined`**.

### Test inventory

| Suite | What it proves |
|---|---|
| `test_codec`     | Q.763 framing: exact-byte vector, round-trips, parameter bodies, parity, + 20k randomized decodes |
| `test_sm`        | Q.764 FSM: inbound/outbound basic call, continuity, glare (win + yield), T7 timeout |
| `test_segment`   | SGM split fits the limit; reassembly reconstructs the original parameter set |
| `test_map`       | TMR↔codec, CPG event↔SIP status, number→E.164, CLIR, Q.850 names |
| `test_e2e`       | Two FSMs complete a full call through the real codec over an async transport |
| `test_cgm`       | Blocking, group blocking (masked), group reset, query — over the wire |
| `test_hardening` | Boundary + regression: 255-octet param, max CIC, full param table, **short-mask CGB over-read**, range/status overflow guard, out-of-range pointer |

## Running inside FreeSWITCH

The module is feature-complete: dedicated osmo thread, eventfd FS↔osmo
marshalling, MGCP bearer, per-circuit timers, and endpoint + dialplan wiring. It
loads via `switch_loadable_module_load_module()` like any other module. Once
built and enabled (above), start FreeSWITCH and `load mod_isup`; the OAM command
`isup status` reports the M3UA ASP state. The full three-container lab in
`docker/` (below) exercises it end to end.

## Live integration — validated against the real osmo stack

Both integration paths were run end-to-end against the real Osmocom daemons in
this environment (not simulated). Reproduce with `test/live/run.sh`:

```
TEST 1 — BEARER (live MGCP against osmo-mgw 1.15.0):
  CRCX  -> 200, MGW allocated rtpbridge/1@mgw, conn F53742B6, RTP port 16002
  MDCX  -> 200 (sendrecv, pointed at FreeSWITCH RTP, codec PCMA)
  DLCX  -> 250 OK
  RESULT: PASS — live MGW bearer control works

TEST 2 — TRANSPORT (live ISUP over M3UA/SCTP via osmo-stp 2.2.1):
  ASP associates to osmo-stp over real SCTP, registers SI=5 routing key
  TX IAM to dpc=2 (15 bytes)
  RX MTP-TRANSFER: opc=2 dpc=2 sls=0 len=15   (routed back by the STP)
  decoded IAM CIC=5, called param len=4
  RESULT: PASS — live M3UA loop OK
```

- `test/live/mgcp_probe.py` drives the exact CRCX/MDCX/DLCX sequence `mod_isup`
  performs for a call's bearer, against a live `osmo-mgw`.
- `test/live/m3ua_node.c` brings up a real M3UA ASP association to `osmo-stp`
  over SCTP, registers ISUP as the SI=5 MTP-User, sends an IAM through the live
  stack, and receives it back via the STP's routing — proving the transport
  binding, the SI=5 registration, and the codec on the wire together.

This validates the two hardest integration seams on real infrastructure.

### Full three-container lab (`docker/`)

`docker/` brings up the complete call: **two FreeSWITCH/`mod_isup` exchanges**
calling each other over **M3UA through an `osmo-stp`**, with the bearer on a
shared **`osmo-mgw`**. `docker/validate.sh` builds the images, places a real
FreeSWITCH-originated call (`originate isup/lab/1002`), captures on the SIGTRAN
hub, and asserts 27 properties with `tshark`:

- the full ISUP ladder (IAM → ACM → ANM → REL → RLC) with settable IAM
  parameters (TMR, CPC, calling number/NOA, FCI, hop counter) and ACM backward
  call indicators, routed through the **XML dialplan** (`ring_ready`/`answer`);
- SCCP (SI=3) co-resident on the association, and the `isup status` OAM command
  reporting the M3UA ASP **ACTIVE**;
- **circuit-group supervision live in the rx path** (Q.764 §2.9): each exchange
  issues a start-up **group reset (GRS)** and the peer's circuit-group manager
  answers with **GRA**, in both directions — the GRS is retransmitted until
  acknowledged, so the boot race (first GRS dropped before the reverse MTP route
  is up) is handled. The same manager handles CGB/CGU/CQM/RSC and per-circuit
  blocking, releasing any active call on a reset circuit. Call origination is
  held off until that reset resolves — a call placed during the reset window is
  rejected with a temporary failure (Q.764 §2.9.1);
- **continuity check** (Q.764 §2.1.8): the call is placed with a continuity
  check requested in the IAM (NCI); the originating exchange reports it with a
  **COT** and the terminating exchange loops its bearer back at the `osmo-mgw`
  (`CRCX loopback`) before proceeding;
- **end-to-end through-connected audio**: the terminating exchange generates a
  tone, streams it as PCMA RTP into the `osmo-mgw` bearer (CRCX/MDCX on the
  shared `rtpbridge` endpoint), the gateway **bridges both legs**, and the
  originating exchange consumes the live (non-CNG) audio frames — verified as
  RTP on the wire in both directions *and* at the FreeSWITCH application layer.

```
  $ docker/validate.sh
  ...
  RESULT: 27 passed, 0 failed
  ALL DOCKER VALIDATION CHECKS PASSED
```

## Integration layers

The wiring to the outside world:

| Layer | File | Role |
|---|---|---|
| M3UA transport (SI=5 MTP-User) | `isup_m3ua.c` | ISUP over M3UA/SCTP via `libosmo-sigtran` |
| FreeSWITCH endpoint | `mod_isup.c` | endpoint, OAM API, dialplan + media wiring |
| Bearer abstraction | `bearer.h` | driver seam (MGCP now / MEGACO later) |

The live end-to-end lab (osmo-stp + osmo-mgw + FreeSWITCH + a peer exchange)
lives in `test/integration/`, and the conformance checklist in
`test/conformance/`.

## Bulletproofing

Every byte off the wire is treated as hostile:

* The decoder is fully bounds-checked and never trusts a length or pointer.
* The circuit-group manager validates that a Range-and-status parameter is long
  enough for its advertised range before reading the status bitmap (this fixed
  a real 31-byte over-read on a malformed `CGB`/`CGU`).
* All decoders are exercised by a libFuzzer harness (`test/isup_fuzz.c`) that
  pushes arbitrary input through decode → typed parsers → circuit-group manager
  → state machine → segmentation under ASan+UBSan. **~390k iterations, zero
  crashes** at the time of writing.
* The build is `-Werror` clean, with a `_Static_assert` guaranteeing the
  parameter value buffer can hold any length a single octet can express.
