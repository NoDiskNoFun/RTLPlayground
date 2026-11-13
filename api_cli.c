/*
 * Command line interface of the read-only monitoring API, see api.h
 *
 *   api                         status
 *   api on|off                  enable or disable
 *   api vlan <1-4094>           the API VLAN (not the management VLAN)
 *   api ip <address> <netmask>  the switch's address in the API VLAN
 *   api allow <address>         the one client allowed, on the API subnet
 *   api token new               generate a token and show it once
 *   api token <32 hex digits>   set a token (this is what the config stores)
 *   api token                   show the token
 *
 * The token never goes to syslog: it is printed with the _no_syslog
 * functions, which the serial console and the web console still show.
 */

#include <stdint.h>
#include "rtl837x_common.h"
#include "cmd_parser.h"
#include "uip.h"
#include "api.h"

#pragma codeseg BANK2
#pragma constseg BANK2

extern __xdata uint8_t cmd_buffer[CMD_BUF_SIZE];
extern __xdata uint8_t cmd_words_len;
extern __xdata uint8_t cmd_words_b[15];
extern __xdata uint16_t atoi_results_short;
extern __xdata uint8_t ip[4];
uint8_t cmd_compare(uint8_t start, __code const uint8_t * cmd);
uint8_t atoi_short(uint8_t idx);
uint8_t parse_ip(uint8_t idx);

static __code const char api_cmd_prefix[] = "api token ";
static __xdata uip_ipaddr_t api_new_ip;

static void api_err(__code const char *msg)
{
	err_status = ERR_INVALID_ARGUMENT;
	print_string("Error: ");
	print_string(msg);
}


static void api_print_token(void)
{
	static __xdata uint8_t i;

	for (i = 0; api.token[i]; i++)
		write_char_no_syslog(api.token[i]);
	write_char_no_syslog('\n');
}


/* A netmask is a run of ones followed by zeros */
static uint8_t netmask_ok(void)
{
	static __xdata uint8_t i, m, zero;

	zero = 0;
	for (i = 0; i < 4; i++) {
		m = ip[i];
		if (zero) {
			if (m)
				return 0;
			continue;
		}
		if (m != 0xff) {
			zero = 1;
			/* m must look like 1..10..0 */
			if ((uint8_t)(~m + 1) & ~m)
				return 0;
		}
	}
	if (!ip[0])
		return 0;
	return 1;
}


static void api_show(void)
{
	print_string("API: ");
	print_string(api.enabled ? "enabled" : "disabled");
	print_string(api.active ? ", active\n" : ", not active\n");
	print_string("  vlan:    ");
	itoa_short(api.vlan);
	print_string("\n  ip:      ");
	print_ip((__xdata uint8_t *)api.ip);
	print_string("\n  netmask: ");
	print_ip((__xdata uint8_t *)api.netmask);
	print_string("\n  allow:   ");
	print_ip((__xdata uint8_t *)api.allow);
	print_string("\n  token:   ");
	print_string(api.token[0] ? "set\n" : "not set\n");
	if (api.enabled && !api.active)
		print_string("  Needs vlan (not the management VLAN), ip, allow (on the API subnet) and a token\n");
}


static void api_token_cmd(void)
{
	static __xdata uint8_t i;

	if (cmd_words_len == 2) {
		if (!api.token[0]) {
			print_string("No token set\n");
			return;
		}
		print_string("API token: ");
		api_print_token();
		return;
	}
	if (cmd_compare(2, "new")) {
		api_token_new();
		api_update();
		print_string("New API token: ");
		api_print_token();
		/* The command history feeds the startup configuration: record
		 * the token itself, so the same token survives a reboot. */
		for (i = 0; api_cmd_prefix[i]; i++)
			cmd_buffer[i] = api_cmd_prefix[i];
		cmd_words_b[0] = 0;
		cmd_words_b[1] = 4;
		cmd_words_b[2] = i;
		cmd_words_len = 3;
		for (i = 0; i < API_TOKEN_LEN; i++)
			cmd_buffer[cmd_words_b[2] + i] = api.token[i];
		cmd_buffer[cmd_words_b[2] + API_TOKEN_LEN] = NUL;
		return;
	}
	if (!api_token_set(&cmd_buffer[cmd_words_b[2]])) {
		api_err("api token [new|<32 lower-case hex digits>]\n");
		return;
	}
	api_update();
	print_string("API token set\n");
}


void api_parse(void) __banked
{
	if (cmd_words_len < 2) {
		api_show();
		return;
	}
	if (cmd_compare(1, "on")) {
		api.enabled = 1;
		api_update();
		api_show();
		return;
	}
	if (cmd_compare(1, "off")) {
		api.enabled = 0;
		api_update();
		print_string("API disabled\n");
		return;
	}
	if (cmd_compare(1, "token")) {
		api_token_cmd();
		return;
	}
	if (cmd_words_len < 3)
		goto err;
	if (cmd_compare(1, "vlan")) {
		if (!atoi_short(cmd_words_b[2]) || !atoi_results_short || atoi_results_short > 4094)
			goto err;
		api.vlan = atoi_results_short;
		api_update();
		print_string("API VLAN set\n");
		return;
	}
	if (cmd_compare(1, "ip")) {
		if (cmd_words_len < 4 || !parse_ip(cmd_words_b[2]))
			goto err;
		uip_ipaddr(&api_new_ip, ip[0], ip[1], ip[2], ip[3]);
		if (!parse_ip(cmd_words_b[3]) || !netmask_ok() || !(api_new_ip[0] | api_new_ip[1]))
			goto err;
		uip_ipaddr(&api.netmask, ip[0], ip[1], ip[2], ip[3]);
		api.ip[0] = api_new_ip[0];
		api.ip[1] = api_new_ip[1];
		api_update();
		print_string("API address set\n");
		return;
	}
	if (cmd_compare(1, "allow")) {
		if (!parse_ip(cmd_words_b[2]))
			goto err;
		uip_ipaddr(&api.allow, ip[0], ip[1], ip[2], ip[3]);
		api_update();
		print_string("API client set\n");
		return;
	}
err:
	api_err("api [on|off|vlan <1-4094>|ip <ip> <netmask>|allow <ip>|token [new|<hex>]]\n");
}
