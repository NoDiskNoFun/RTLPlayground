# Read-only monitoring API on a VLAN of its own

Monitoring systems such as Home Assistant only need to read the switch. The
monitoring API gives them a second IPv4 address in a VLAN of their own, where
the switch answers exactly one client, and that client can only read the JSON
status pages, with a bearer token. The management address, the web UI, the
login, commands, configuration and firmware uploads stay out of reach from
that VLAN.

## What a client in the API VLAN can do

* ARP and ICMP echo (ping), and TCP to port 80. Everything else in the API
  VLAN is dropped without an answer, as is every frame whose source is not the
  allowed client or whose destination is not the API address. IP fragments
  and IP options are dropped too.
* `GET` of the read-only JSON pages of [the HTTP API](api.md):
  `information.json`, `ports.json`, `traffic.json`, `counters.json?port=<n>`,
  `eee.json`, `bandwidth.json`, `storm.json`, `l2.json?idx=<n>`, `mirror.json`,
  `isolation.json`, `lag.json`, `stp.json`, `stpcnt.json`, `lldp.json`,
  `vlan.json?vid=<n>`, `vlanlist.json`.
* Each request needs `Authorization: Bearer <token>`.

Not reachable: the web pages, `/login`, `/cmd`, `/config`, `/upload`,
`/cmd_log`, `/cmd_log_clear`, `/reset`, `/l2_del.json`. A request other than
`GET` gets `405`, a missing or wrong token `401`, any other path `404`. A login
cookie counts for nothing in the API VLAN, and the token counts for nothing
on the management address.

The switch does not answer for its management address in the API VLAN, and a
TCP connection opened on one side is reset if a segment for it arrives from
the other.

## Setting it up

```
vlan 20 1 2 3 4 5 6t          # a VLAN reaching the monitoring host, as usual
api vlan 20                   # the API VLAN, not the management VLAN
api ip 10.20.0.2 255.255.255.0
api allow 10.20.0.10          # the one client, on the API subnet
api token new                 # prints the token once, keep it
api on
api                           # status
```

The same is in the web UI under System, card "Monitoring API". Save the
running configuration to flash afterwards. The startup configuration stores
`api token <32 hex digits>`: `api token new` is recorded with the token it
generated, so the token survives a reboot.

There is no gateway: the client has to be on the API subnet. `api on` takes
effect once every setting is in place (VLAN differing from the management
VLAN, address, netmask, client on that subnet and different from the API
address, token); until then `api` reports "enabled, not active" and the API
VLAN is not answered at all.

`api token` shows the token, `api token new` replaces it. The token is printed
with the console functions that do not feed syslog, so it never goes to the
syslog server. Typing a token in by hand on the serial console echoes it like
any other input.

`information.json` reports the settings under `api`, never the token:

```json
"api":{"enabled":true,"active":true,"vlan":20,"ip":"10.20.0.2","netmask":"255.255.255.0","allow":"10.20.0.10","token_set":true}
```

## Example

```
curl -H "Authorization: Bearer $TOKEN" http://10.20.0.2/information.json
```

## Limits

* Plain HTTP: the token crosses the API VLAN in clear text. Keep that VLAN to
  the switch and the monitoring host.
* The firmware handles one TCP connection at a time, shared with the web UI.
  Poll at a sensible interval (30 s or more).
* The source address check alone could be spoofed by a host in the same VLAN;
  the token is what keeps it out.

## How it works

uIP keeps a single address. While a frame from the API VLAN is handled,
`api_ctx_enter()` (api.c) swaps the API address and netmask into uIP and
clears the gateway, and `tcpip_output()` tags what goes out with the API VLAN;
`api_ctx_leave()` restores the management addressing. The TCP connection
remembers the side it was opened on (`httpd_state.via_api`), so retransmissions
from the periodic timer take the same path. The switch's own MAC address gets a
static L2 entry in the API VLAN while the API is active.
