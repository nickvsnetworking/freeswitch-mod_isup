# mod_isup fs_cli Commands

All commands are issued from the FreeSWITCH console or `fs_cli`. Examples use the
non-interactive form (`fs_cli -x "<command>"`); the same commands work at an
interactive `fs_cli` prompt.

## Module Loading

| Command | Description |
|---------|-------------|
| `load mod_isup` | Load the module. On success returns `+OK`; brings up the M3UA ASP and registers the ISUP endpoint. |
| `unload mod_isup` | Unload the module and tear down the ASP. |
| `reload mod_isup` | Unload then load (picks up cs7 config / environment changes). |
| `module_exists mod_isup` | Returns `true` if the module is currently loaded. |

**Loading:**

```
$ fs_cli -x "load mod_isup"
+OK Reloading XML
+OK
```

If loading returns `-ERR [module load file routine returned an error]`, the
module could not bring up its transport — most commonly the cs7 config is
missing/invalid or the STP is unreachable. See [Troubleshooting](#troubleshooting).

## OAM Commands

The module registers a single API command, `isup`, with subcommands. Running
`isup` with no argument is equivalent to `isup status`.

```
isup status | m3ua | mgw | cic | sccp
```

### `isup status`

Overall health of the exchange — the first thing to check.

```
$ fs_cli -x "isup status"
ISUP profile 'lab'
  M3UA ASP asp-clnt-stp : ACTIVE
  point codes      : OPC=607  peer DPC=608  NI=2
  MGW (mgcp)        : 10.179.1.201:2427
  SCCP             : disabled
  circuits         : 1-4  (0 in use)
```

| Field | Meaning |
|-------|---------|
| `ISUP profile` | The profile name (`lab`). |
| `M3UA ASP … : ACTIVE / down` | The ASP name and its M3UA state. **`ACTIVE`** means the exchange can signal; **`down`** means it cannot. |
| `point codes` | `OPC` = this exchange, `peer DPC` = the destination outbound calls route to, `NI` = network indicator. |
| `MGW (mgcp)` | The bearer driver and the configured Media Gateway `host:port`. |
| `SCCP` | Whether the optional SCCP (SI=3) user is bound (`disabled` unless `sccp-ssn` is set). |
| `circuits` | The CIC range and how many circuits are currently in use (non-idle). |

### `isup m3ua`

Detail on the M3UA transport binding.

```
$ fs_cli -x "isup m3ua"
M3UA ASP asp-clnt-stp: ACTIVE
  local PC (OPC) : 607
  peer  PC (DPC) : 608
  network ind    : 2 (national)
  cs7 config     : /usr/local/freeswitch/conf/isup-cs7.cfg
```

| Field | Meaning |
|-------|---------|
| `M3UA ASP … : ACTIVE / down` | ASP name and state. |
| `local PC (OPC)` | This exchange's point code. |
| `peer PC (DPC)` | The peer exchange's point code. |
| `network ind` | The MTP network indicator applied to outgoing messages. |
| `cs7 config` | Path to the active cs7 transport config file. |

### `isup mgw`

Media Gateway bearer state — one line per circuit that currently has an MGW
connection or an active call.

```
$ fs_cli -x "isup mgw"
MGW 10.179.1.201:2427  driver=mgcp
  CIC 1    ep=rtpbridge/1@mgw   MGW-RTP=10.179.1.201:16002  FS-RTP=10.179.3.60:24140
```

| Field | Meaning |
|-------|---------|
| `MGW … driver=` | The configured gateway and bearer driver (`mgcp`). |
| `CIC` | Circuit identification code for the call. |
| `ep` | The MGW endpoint allocated for this circuit (via MGCP CRCX). |
| `MGW-RTP` | The RTP address/port on the Media Gateway. |
| `FS-RTP` | FreeSWITCH's local RTP address/port for the leg. |

### `isup cic`

Per-circuit state table — the ISUP call state machine for every CIC in range.

```
$ fs_cli -x "isup cic"
CIC   state           session MGW-RTP
1     IDLE            -       :0
2     ANSWERED        yes     10.179.1.201:16004
3     IDLE            -       :0
4     IDLE            -       :0
```

| Column | Meaning |
|--------|---------|
| `CIC` | Circuit identification code. |
| `state` | The Q.764 call state for the circuit (e.g. `IDLE`, `OUTGOING`, `ALERTING`, `ANSWERED`, `RELEASING`). |
| `session` | `yes` if a FreeSWITCH session is bound to this circuit. |
| `MGW-RTP` | The RTP address/port on the MGW for the circuit's bearer (`:0` when none). |

### `isup sccp`

State of the optional SCCP (SI=3) user.

```
$ fs_cli -x "isup sccp"
SCCP (SI=3): disabled
```

When enabled (`sccp-ssn` > 0), it additionally reports the bound Subsystem
Number. SCCP shares the same M3UA association as ISUP and is reserved for a
future TCAP/MAP layer.

## Placing Calls

Outbound ISUP calls are placed with the standard `originate` command against the
`isup` endpoint. See [Call Routing](./call-routing.md) for the dial-string
format and IAM parameters.

```
$ fs_cli -x "originate isup/lab/1002 &echo"
```

## Troubleshooting

### `load mod_isup` returns an error

**Symptoms**: `-ERR [module load file routine returned an error]`, and the log
shows `mod_isup: osmo setup failed` or `failed to read cs7 config`.

**Possible causes**:
- The cs7 config file (`cs7-config`) is missing or has a syntax error.
- The STP is unreachable, so the ASP could not become ready within the start-up
  window.

**Resolution**:
1. Confirm the cs7 file exists at the configured path and parses (a bad line
   aborts the whole file — check for unsupported directives).
2. Verify SCTP reachability to the STP's `remote-ip` on port 2905.
3. Reload once the transport is confirmed: `reload mod_isup`.

### ASP shows `down`

**Symptoms**: `isup status` shows `M3UA ASP … : down`; calls fail.

**Possible causes**:
- `asp-name` (isup.conf.xml) does not match the `asp` name in the cs7 config, so the
  association is never started.
- The STP has not provisioned this exchange (its point code / routing context /
  source IP+port), so it rejects the M3UA ASP after the SCTP association forms.
- SCTP cannot reach the STP.

**Resolution**:
1. Ensure `asp-name` exactly equals the cs7 `asp` name.
2. Confirm the STP is provisioned with this exchange's **routing context**,
   **point code**, and the **source IP/port** it will connect from.
3. Check for an established SCTP association to the STP on port 2905.

### Outbound call rejected with a temporary failure

**Symptoms**: `originate` returns `NORMAL_TEMPORARY_FAILURE`; log shows
`rejecting originate — start-up group reset not yet complete`.

**Possible causes**:
- The start-up circuit-group reset with the peer has not yet completed; circuits
  are not confirmed idle at both ends (Q.764 §2.9.1).

**Resolution**: Wait for the reset to settle (a few seconds after the ASP goes
`ACTIVE`) and retry. Persistent failure indicates the peer is not answering the
group reset — verify the peer exchange and STP routing between the two point
codes.

### All circuits busy

**Symptoms**: `originate` returns `NORMAL_CIRCUIT_CONGESTION`.

**Possible causes**: All CICs in the `1`–`4` range are in use.

**Resolution**: Check `isup cic` for stuck circuits; released calls should return
circuits to `IDLE`.
