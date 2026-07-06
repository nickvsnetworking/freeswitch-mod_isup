# mod_isup Configuration Reference

`mod_isup` is configured through **two** files plus the FreeSWITCH module loader:

```mermaid
flowchart TB
    XML["isup.conf.xml<br/>(autoload_configs)"] -->|exchange identity:<br/>point codes, MGW, ASP name, CICs| MOD[mod_isup]
    CS7["cs7 config file<br/>(isup-cs7.cfg)"] -->|M3UA/SCTP transport:<br/>ASP, AS, routing key, routes| MOD
    FSMOD["modules.conf.xml<br/>or 'load mod_isup'"] -->|when to load the module| MOD
    MOD -->|inbound calls| DIAL["XML dialplan<br/>(context: default)"]
```

| File | Purpose |
|------|---------|
| **`isup.conf.xml`** | The exchange's identity and behaviour — point codes, MGW address, ASP name, CIC range, inbound context/dialplan. Lives in `autoload_configs/` and is read through the standard FreeSWITCH XML config framework, like every other module. |
| **cs7 config file** | The SIGTRAN transport: the M3UA association (ASP) to the STP, the Application Server (AS), the routing key, and the route table. Standard Osmocom `cs7` VTY syntax. Its path is set by the `cs7-config` setting. |
| **`modules.conf.xml`** | Controls whether the module auto-loads at start-up. It can also be loaded on demand with `load mod_isup`. |

## Configuration File Locations

| Item | Default location | Set by |
|------|------------------|--------|
| Module config | `<conf>/autoload_configs/isup.conf.xml` | — |
| cs7 config file | `/usr/local/freeswitch/conf/isup-cs7.cfg` | `cs7-config` in `isup.conf.xml` |
| Module load config | `<conf>/autoload_configs/modules.conf.xml` | — |

---

## isup.conf.xml Settings

Place `isup.conf.xml` in the FreeSWITCH `autoload_configs/` directory (alongside
`sofia.conf.xml`, `event_socket.conf.xml`, and the rest). All settings are
`param` entries under `<settings>`.

```xml
<configuration name="isup.conf" description="ISUP-over-M3UA MGCF">
  <settings>
    <!-- Exchange identity -->
    <param name="profile-name" value="lab"/>
    <param name="opc" value="607"/>
    <param name="peer-dpc" value="608"/>
    <param name="network-indicator" value="2"/>

    <!-- M3UA transport -->
    <param name="asp-name" value="asp-clnt-stp"/>
    <param name="cs7-config" value="/usr/local/freeswitch/conf/isup-cs7.cfg"/>

    <!-- Media gateway (MGCP) -->
    <param name="mgw" value="10.179.1.201:2427"/>

    <!-- Circuit range -->
    <param name="cic-min" value="1"/>
    <param name="cic-max" value="4"/>

    <!-- Inbound call handling -->
    <param name="context" value="default"/>
    <param name="dialplan" value="XML"/>

    <!-- Optional behaviour -->
    <param name="auto-answer" value="false"/>
    <param name="sccp-ssn" value="0"/>
  </settings>
</configuration>
```

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `profile-name` | String | No | `lab` | Label for this exchange, shown in `isup status`. |
| `opc` | Integer | No | `1` | This exchange's **Originating Point Code** (decimal). Must match the `point-code` in the cs7 config and the point code the STP has provisioned for this exchange. |
| `peer-dpc` | Integer | No | `2` | **Destination Point Code** of the peer exchange that outbound calls (IAM) are routed toward. |
| `network-indicator` | Integer | No | `2` | SS7 network indicator applied to outgoing messages (`0` = international, `2` = national, per [ITU-T Q.704](https://www.itu.int/rec/T-REC-Q.704)). Must be consistent with the STP and peer. |
| `asp-name` | String | No | `asp` | Name of the M3UA ASP to bring up. **Must exactly match the `asp` name in the cs7 config**, otherwise the association is never started and the ASP stays `down`. |
| `cs7-config` | String | No | `/usr/local/freeswitch/conf/isup-cs7.cfg` | Path to the cs7 transport config file (below). If the file is missing or invalid, the module fails to load. |
| `mgw` | String | No | `127.0.0.1:2427` | `host:port` of the MGCP Media Gateway used for the call bearer. |
| `cic-min` | Integer | No | `1` | Lowest Circuit Identification Code managed by this exchange. |
| `cic-max` | Integer | No | `4` | Highest CIC. The circuit count (`cic-max − cic-min + 1`) is the maximum number of concurrent calls. |
| `context` | String | No | `default` | Dialplan context inbound ISUP calls enter — see [Call Routing](./call-routing.md). |
| `dialplan` | String | No | `XML` | Dialplan engine for inbound calls. |
| `auto-answer` | Boolean | No | `false` | When `true`, the module answers inbound calls itself (demo/loopback) instead of leaving answer control to the dialplan. Leave `false` for normal operation. |
| `sccp-ssn` | Integer | No | `0` | If greater than 0, binds an SCCP (SI=3) user on the same M3UA association with this Subsystem Number, reserved for a future TCAP/MAP layer. `0` leaves SCCP off. |

### Environment Variable Overrides

For containerised deployments (where each container is a distinct exchange), any
setting may be overridden at start-up by an environment variable on the
FreeSWITCH process. **`isup.conf.xml` is the canonical configuration; the
environment variable, when set, takes precedence.** This is optional — a normal
deployment configures everything in the XML file.

| Environment variable | Overrides setting |
|----------------------|-------------------|
| `ISUP_OPC` | `opc` |
| `ISUP_PEER_DPC` | `peer-dpc` |
| `ISUP_ASP_NAME` | `asp-name` |
| `ISUP_CS7_CFG` | `cs7-config` |
| `ISUP_MGW` | `mgw` |
| `ISUP_AUTOANSWER` | `auto-answer` (variable set = enabled) |
| `ISUP_SCCP_SSN` | `sccp-ssn` |

---

## cs7 Config File

The cs7 file configures the M3UA/SCTP transport using standard Osmocom `cs7`
VTY syntax (as used by `libosmo-sigtran`). It defines the SCTP association to
the STP, the Application Server, and how ISUP messages are routed. Its path is
the `cs7-config` setting above.

```
log stderr
 logging level set-all notice
line vty
 no login
cs7 instance 0
 network-indicator international
 point-code 0.75.7
 asp asp-clnt-stp 2905 2906 m3ua
  remote-ip 10.179.4.10
  role asp
  sctp-role client
 as as-clnt-isup m3ua
  asp asp-clnt-stp
  routing-key 20 0.75.7
 route-table system
  update route 0.0.0 0.0.0 linkset as-clnt-isup
```

### Global directives

| Directive | Description |
|-----------|-------------|
| `log stderr` / `logging level set-all notice` | Sends the SIGTRAN stack's own log to stderr at `notice` level. Raise to `debug` when diagnosing association problems. |
| `line vty` / `no login` | Enables the embedded VTY without a login prompt. Required boilerplate. |

### `cs7 instance` parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `cs7 instance` | Integer | Yes | `0` | The SS7 instance number. `mod_isup` uses instance `0`. |
| `network-indicator` | Enum | Yes | — | SS7 network indicator: `international`, `national`, `national-spare`, or `reserved`. **Must match the STP and peer** (and the `network-indicator` in `isup.conf.xml`). |
| `point-code` | Point code | Yes | — | This exchange's own point code, in `network.cluster.member` (3-8-3) form. `0.75.7` = decimal `607`, matching `opc`. |

### `asp` parameters (the M3UA association)

Syntax: `asp <name> <remote-port> <local-port> <protocol>`

| Field | Type | Required | Default | Description |
|-------|------|----------|---------|-------------|
| `<name>` | String | Yes | — | ASP name. **Must equal `asp-name`** in `isup.conf.xml` so the module starts this association. |
| `<remote-port>` | Integer | Yes | `2905` | The STP's SCTP port. M3UA standard port is 2905. |
| `<local-port>` | Integer | Yes | — | The SCTP **source port** this exchange binds. Pin a fixed value (e.g. `2906`) when the STP matches peers by source port, or when running more than one exchange from the same source IP. |
| `<protocol>` | Enum | Yes | `m3ua` | Adaptation layer. `mod_isup` uses `m3ua`. |
| `remote-ip` | IP | Yes | — | The STP's IP address. |
| `role` | Enum | Yes | `asp` | M3UA role. This exchange is always the `asp` (the STP is the SG/server). |
| `sctp-role` | Enum | Yes | `client` | SCTP association role. `client` — this exchange initiates the association to the STP. |

### `as` parameters (the Application Server)

Syntax: `as <name> <protocol>`

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `as <name>` | String | Yes | — | Application Server name (local label). |
| `<protocol>` | Enum | Yes | `m3ua` | Adaptation layer; `m3ua`. |
| `asp <name>` | Reference | Yes | — | Binds the ASP (above) into this AS. |
| `routing-key` | `<rctx> <point-code>` | Yes | — | The M3UA routing key: the **routing context** the STP expects (`20`) and this exchange's point code (`0.75.7`). The routing context **must match the STP's provisioned peer**, or the STP rejects the association. |

### `route-table` parameters

| Parameter | Type | Required | Default | Description |
|-----------|------|----------|---------|-------------|
| `route-table system` | — | Yes | — | Selects the system route table. |
| `update route <dpc> <mask> linkset <as>` | Route entry | Yes | — | Directs outbound MTP traffic to the AS. `0.0.0 0.0.0` is a default (catch-all) route sending all ISUP messages to the STP via `as-clnt-isup`; the STP forwards them to the real destination point code. |

---

## Loading the Module in FreeSWITCH

`mod_isup`'s load waits for the M3UA ASP to come up, so on an exchange where the
STP association may not be reachable at start-up it is better to load it on
demand (or after the network is confirmed) with:

```
fs_cli -x "load mod_isup"
```

To auto-load it, add the following to
`<conf>/autoload_configs/modules.conf.xml` on an exchange where the STP
association is expected to be reachable at start-up:

```xml
<load module="mod_isup"/>
```

See [fs_cli Commands](./fs-cli-commands.md) for verifying the load and reading
status.
