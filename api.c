/*
 * Read-only monitoring API on a dedicated VLAN, see api.h and doc/api_vlan.md
 *
 * The network stack keeps a single address. While a frame from the API VLAN
 * is handled, api_ctx_enter() swaps the API address into uIP and
 * tcpip_output() tags what goes out with the API VLAN; api_ctx_leave() puts
 * the management address back. uIP handles one frame at a time, so the swap
 * covers everything that frame causes. The one TCP connection remembers the
 * side it was opened on (httpd_state.via_api), so retransmissions from the
 * periodic timer take the same path.
 */

#include <stdint.h>
#include "rtl837x_common.h"
#include "rtl837x_port.h"
#include "uip.h"
#include "api.h"

#pragma codeseg BANK3
#pragma constseg BANK3

extern __xdata uint8_t uip_buf[];
extern __xdata uint16_t rx_packet_vlan;
extern __xdata uint16_t management_vlan;
extern __xdata uip_ipaddr_t uip_hostaddr, uip_draddr, uip_netmask;
extern volatile __xdata uint8_t sfr_data[4];

__xdata struct api_state api;
__xdata uint8_t api_ctx;
__xdata uip_ipaddr_t api_mgmt_host, api_mgmt_mask, api_mgmt_dr;

static __code const char api_hex[] = "0123456789abcdef";
static __code const char api_bearer[] = "bearer";

/* Offsets into a received frame: destination and source MAC, the 8-byte
 * Realtek CPU tag and the 4-byte VLAN tag, then the ether type. The payload
 * starts at UIP_LLH_LEN. */
#define RX_PAYLOAD	(&uip_buf[UIP_LLH_LEN])
#define ARP_LEN		28
#define ARP_OPCODE	6
#define ARP_SIP		14
#define ARP_DIP		24

#define IPH	((__xdata struct uip_tcpip_hdr *)&uip_buf[UIP_LLH_LEN])
#define ICMPH	((__xdata struct uip_icmpip_hdr *)&uip_buf[UIP_LLH_LEN])

/* Compares an address held as bytes in a frame with one held as uip_ipaddr_t:
 * both are in network order, the frame copy may sit on any byte boundary. */
static uint8_t ip_eq_bytes(__xdata uint8_t *b, __xdata uint16_t *a)
{
	__xdata uint8_t *ab = (__xdata uint8_t *)a;

	/* No && chains here and below: SDCC keeps their partial results in
	 * bit memory, which shares internal RAM with the stack */
	return !((b[0] ^ ab[0]) | (b[1] ^ ab[1]) | (b[2] ^ ab[2]) | (b[3] ^ ab[3]));
}


/* The VLAN the management address answers in: 0 there means VLAN 1 for the
 * switch's own L2 entry, see port_l2_static_mgmt() */
static uint16_t mgmt_vlan_effective(void)
{
	return management_vlan ? management_vlan : 1;
}


void api_init(void) __banked
{
	api.enabled = 0;
	api.active = 0;
	api.vlan = 0;
	api.ip[0] = api.ip[1] = 0;
	api.netmask[0] = api.netmask[1] = 0;
	api.allow[0] = api.allow[1] = 0;
	api.token[0] = 0;
	api.l2_vlan = 0;
	api_ctx = 0;
}


/* Removes the switch's L2 entry in the API VLAN, unless the management
 * address uses that VLAN too: its entry is the same one. */
void api_l2_drop(void) __banked
{
	if (api.l2_vlan && api.l2_vlan != mgmt_vlan_effective())
		port_l2_static_mgmt(uip_ethaddr.addr, api.l2_vlan, true);
	api.l2_vlan = 0;
}


/* Whether every setting is in place. A leaf on purpose: SDCC keeps the
 * temporaries of a leaf function in the shared overlay, those of any other
 * function in internal RAM of their own, which the stack cannot spare. */
static uint8_t api_complete(void)
{
	if (!api.enabled || !api.vlan || !api.token[0])
		return 0;
	if (api.vlan == (management_vlan ? management_vlan : 1))
		return 0;
	if (!(api.ip[0] | api.ip[1]) || !(api.netmask[0] | api.netmask[1]) || !(api.allow[0] | api.allow[1]))
		return 0;
	/* The client must be on the API subnet: there is no gateway */
	if (((api.ip[0] ^ api.allow[0]) & api.netmask[0]) | ((api.ip[1] ^ api.allow[1]) & api.netmask[1]))
		return 0;
	if (!((api.ip[0] ^ api.allow[0]) | (api.ip[1] ^ api.allow[1])))
		return 0;
	return 1;
}


/* Re-evaluates whether the API is complete and keeps the switch's own MAC
 * address installed in the API VLAN while it is. To be called after any
 * change of an API setting, the management VLAN or the MAC address. */
void api_update(void) __banked
{
	/* Locals in xdata: internal RAM is used up, see the .mem file */
	static __xdata uint8_t ok;

	ok = api_complete();
	api.active = ok;

	if (api.l2_vlan != (ok ? api.vlan : 0))
		api_l2_drop();
	if (ok && !api.l2_vlan) {
		port_l2_static_mgmt(uip_ethaddr.addr, api.vlan, false);
		api.l2_vlan = api.vlan;
	}
}


/* Whether the frame in uip_buf belongs to the API: it does as soon as it was
 * received in the API VLAN while the API is active. Such a frame is never
 * handled for the management address. */
uint8_t api_rx_vlan(void) __banked
{
	if (!api.active)
		return 0;
	if (rx_packet_vlan != api.vlan)
		return 0;
	return 1;
}


/* ARP in the API VLAN: only from the allowed client, and a request only when
 * it asks for the API address */
uint8_t api_arp_accept(void) __banked
{
	static __xdata uint8_t * __xdata a;

	a = RX_PAYLOAD;
	if (uip_len < UIP_LLH_LEN + ARP_LEN)
		return 0;
	if (!ip_eq_bytes(a + ARP_SIP, api.allow))
		return 0;
	if (a[ARP_OPCODE] != 0)
		return 0;
	if (a[ARP_OPCODE + 1] == 1)	/* request */
		return ip_eq_bytes(a + ARP_DIP, api.ip);
	return a[ARP_OPCODE + 1] == 2;	/* reply */
}


/* IPv4 in the API VLAN: from the allowed client to the API address, not
 * fragmented, and either TCP to port 80 or an ICMP echo request. */
uint8_t api_ip_accept(void) __banked
{
	if (uip_len < UIP_LLH_LEN + 20)
		return 0;
	if (IPH->vhl != 0x45)
		return 0;
	if ((IPH->srcipaddr[0] ^ api.allow[0]) | (IPH->srcipaddr[1] ^ api.allow[1]))
		return 0;
	if ((IPH->destipaddr[0] ^ api.ip[0]) | (IPH->destipaddr[1] ^ api.ip[1]))
		return 0;
	/* More-fragments flag or a fragment offset */
	if ((IPH->ipoffset[0] & 0x3f) | IPH->ipoffset[1])
		return 0;
	/* Only as much length as the fields looked at need: uIP itself checks
	 * the rest against the IP header */
	if (IPH->proto == UIP_PROTO_TCP) {
		if (uip_len < UIP_LLH_LEN + 24)
			return 0;
		if (IPH->destport != HTONS(80))
			return 0;
		return 1;
	}
	if (IPH->proto == UIP_PROTO_ICMP) {
		if (uip_len < UIP_LLH_LEN + 21)
			return 0;
		if (ICMPH->type != 8)
			return 0;
		return 1;
	}
	return 0;
}


void api_ctx_enter(void) __banked
{
	api_mgmt_host[0] = uip_hostaddr[0]; api_mgmt_host[1] = uip_hostaddr[1];
	api_mgmt_mask[0] = uip_netmask[0]; api_mgmt_mask[1] = uip_netmask[1];
	api_mgmt_dr[0] = uip_draddr[0]; api_mgmt_dr[1] = uip_draddr[1];
	uip_hostaddr[0] = api.ip[0]; uip_hostaddr[1] = api.ip[1];
	uip_netmask[0] = api.netmask[0]; uip_netmask[1] = api.netmask[1];
	/* No gateway: the client is on the subnet, nothing is routed */
	uip_draddr[0] = 0; uip_draddr[1] = 0;
	api_ctx = 1;
}


void api_ctx_leave(void) __banked
{
	uip_hostaddr[0] = api_mgmt_host[0]; uip_hostaddr[1] = api_mgmt_host[1];
	uip_netmask[0] = api_mgmt_mask[0]; uip_netmask[1] = api_mgmt_mask[1];
	uip_draddr[0] = api_mgmt_dr[0]; uip_draddr[1] = api_mgmt_dr[1];
	api_ctx = 0;
}


/* Checks an Authorization header value for "Bearer <token>". The token is
 * compared in full whatever the first difference, so the time taken does not
 * tell how much of a guess was right. */
uint8_t api_token_check(__xdata uint8_t *v) __banked
{
	static __xdata uint8_t diff, i, c;

	diff = 0;
	if (!api.token[0])
		return 0;
	for (i = 0; api_bearer[i]; i++) {
		c = v[i];
		if (c >= 'A' && c <= 'Z')
			c += 'a' - 'A';
		if (c != api_bearer[i])
			return 0;
	}
	v += i;
	if (*v != ' ')
		return 0;
	while (*v == ' ')
		v++;
	for (i = 0; i < API_TOKEN_LEN; i++) {
		c = v[i];
		if (!c)
			return 0;
		diff |= c ^ api.token[i];
	}
	c = v[API_TOKEN_LEN];
	if (c && c != '\r' && c != '\n' && c != ' ' && c != '\t')
		return 0;
	return !diff;
}


/* A new token from the hardware random number generator */
void api_token_new(void) __banked
{
	static __xdata uint8_t i, b;

	for (i = 0; i < API_TOKEN_LEN; i++) {
		if (!(i & 7))
			get_random_32();
		b = sfr_data[(i >> 1) & 3];
		if (i & 1)
			b >>= 4;
		api.token[i] = api_hex[b & 0xf];
	}
	api.token[API_TOKEN_LEN] = 0;
}


/* Sets the token from exactly API_TOKEN_LEN lower-case hex digits, as the
 * startup configuration stores it. Returns 0 if the text is not one. */
uint8_t api_token_set(__xdata uint8_t *v) __banked
{
	static __xdata uint8_t i, c;

	for (i = 0; i < API_TOKEN_LEN; i++) {
		c = v[i];
		if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
			return 0;
	}
	c = v[API_TOKEN_LEN];
	if (c && c != ' ')
		return 0;
	for (i = 0; i < API_TOKEN_LEN; i++)
		api.token[i] = v[i];
	api.token[API_TOKEN_LEN] = 0;
	return 1;
}
