#ifndef _API_H_
#define _API_H_

/*
 * Read-only monitoring API on a dedicated VLAN
 *
 * The switch gets a second IPv4 address in a VLAN of its own. In that VLAN it
 * answers exactly one client (api allow), and that client may only read the
 * JSON status pages, authenticated with a bearer token. The management
 * address, the web UI, logins, commands and uploads are not reachable there.
 * See doc/api_vlan.md.
 */

#include <stdint.h>
#include "uip.h"

#define API_TOKEN_LEN 32	/* hex characters, 128 bit */

struct api_state {
	uint8_t enabled;		/* "api on": wanted, active once complete */
	uint8_t active;			/* enabled and every setting valid */
	uint16_t vlan;			/* 0: not set */
	uip_ipaddr_t ip;
	uip_ipaddr_t netmask;
	uip_ipaddr_t allow;		/* the one client allowed in */
	char token[API_TOKEN_LEN + 1];	/* token[0] == 0: no token */
	uint16_t l2_vlan;		/* VLAN of the installed L2 entry, 0: none */
};

extern __xdata struct api_state api;
/* Non-zero while the network stack runs on behalf of the API VLAN */
extern __xdata uint8_t api_ctx;
/* Management addressing, valid while api_ctx is set */
extern __xdata uip_ipaddr_t api_mgmt_host, api_mgmt_mask, api_mgmt_dr;

void api_init(void) __banked;
void api_update(void) __banked;
void api_l2_drop(void) __banked;

uint8_t api_rx_vlan(void) __banked;
uint8_t api_arp_accept(void) __banked;
uint8_t api_ip_accept(void) __banked;
void api_ctx_enter(void) __banked;
void api_ctx_leave(void) __banked;

uint8_t api_token_check(__xdata uint8_t *v) __banked;
void api_token_new(void) __banked;
uint8_t api_token_set(__xdata uint8_t *v) __banked;

void api_parse(void) __banked;

#endif
