# mod_isup integration lab

End-to-end testing of the *integration* layers that the standalone unit tests
cannot cover: the live M3UA association (SCTP), real CRCX/MDCX/DLCX against a
real MGW, and ISUP↔SIP interworking inside a running FreeSWITCH.

The protocol logic itself (codec, state machine, circuit management,
segmentation) is already proven by `make check` and `make fuzz` in the parent
directory — this lab proves the wiring.

## Topology

```
  peer ISUP exchange ──M3UA/SCTP──► osmo-stp ──M3UA/SCTP──► FreeSWITCH+mod_isup ──SIP──► (sipp / UA)
        (scenarios)                    │                          │ MGCP
                                       └────────► osmo-mgw ◄───────┘  ──RTP──► FreeSWITCH
```

Point codes (ITU 14-bit): peer = 1-2-3, mod_isup = 1-1-1, via routing context 101.

## Run

```sh
docker compose up -d osmo-stp osmo-mgw freeswitch
docker compose run --rm scenarios          # drive the scenario suite
docker compose logs -f freeswitch          # watch mod_isup decode traces
```

## Scenario suite (`scenarios/`)

Each scenario drives a call and asserts both the ISUP ladder and the resulting
FreeSWITCH channel state. These map onto the abstract test cases tracked in
`../conformance/q784_matrix.md`.

| Scenario | Direction | Asserts |
|---|---|---|
| `inbound_basic`     | peer → FS  | IAM→ACM→ANM, channel answered, REL→RLC |
| `outbound_basic`    | FS → peer  | originate → IAM, ACM→ringing, ANM→answer |
| `continuity`        | peer → FS  | IAM(cont)→COT loopback on MGW→proceed |
| `glare`             | both       | simultaneous IAM, controlling exchange wins |
| `group_reset`       | both       | GRS→GRA on startup, all CICs idle |
| `blocking`          | both       | BLO/BLA, calls rejected on blocked CIC |
| `early_media`       | peer → FS  | CPG(progress)→183 + in-band audio |
| `suspend_resume`    | peer → FS  | SUS→hold, RES→resume, T2 expiry release |
| `clir`              | peer → FS  | restricted calling number → SIP Privacy |
| `cause_mapping`     | both       | Q.850 cause carried losslessly REL↔BYE |

## What this validates that unit tests cannot

- SCTP multihoming + M3UA ASP state (ASPSM/ASPTM) bring-up and recovery.
- Real MGCP transactions against osmo-mgw (endpoint allocation, RTP relay,
  COT loopback).
- FreeSWITCH session lifecycle, dialplan execution, and bridging to SIP.
- Behaviour under link bounce (point-code pause/resume → circuit blocking).
