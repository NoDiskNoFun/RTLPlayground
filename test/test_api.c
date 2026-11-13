/*
 * Host checks for api.c: which frames from the API VLAN get through, when the
 * API counts as complete, the switch's L2 entry in the API VLAN, the address
 * swap and the token handling.
 *
 * api.c is compiled UNMODIFIED from the firmware tree.
 */
#include <stdio.h>
#include <string.h>

#undef HTONS
#undef NTOHS

#include "rtl837x_common.h"
#include "uip.h"
#include "api.h"

static int tests_run, tests_failed;
#define CHECK(c, name) do { tests_run++; \
	if (c) printf("PASS  %s\n", name); \
	else { printf("FAIL  %s\n", name); tests_failed++; } } while (0)

/* ---- environment ---- */
uint8_t uip_buf[UIP_CONF_BUFFER_ALLOC_SIZE];
uint16_t uip_len;
uint16_t rx_packet_vlan;
uint16_t management_vlan = 1;
uip_ipaddr_t uip_hostaddr, uip_draddr, uip_netmask;
struct uip_eth_addr uip_ethaddr = { { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 } };
volatile uint8_t sfr_data[4];

static unsigned rnd;
void get_random_32(void)
{
	for (int i = 0; i < 4; i++) {
		rnd = rnd * 1103515245u + 12345u;
		sfr_data[i] = (uint8_t)(rnd >> 16);
	}
}

/* The switch's L2 entries for its own MAC, by VLAN */
static int l2_entry[4096];
static int l2_calls;
void port_l2_static_mgmt(uint8_t *mac, uint16_t vlan, bool remove_entry)
{
	(void)mac;
	if (!vlan)
		vlan = 1;
	l2_entry[vlan] = !remove_entry;
	l2_calls++;
}

/* ---- frames as the NIC delivers them: MACs, CPU tag, VLAN tag, type ---- */
#define PAY (&uip_buf[UIP_LLH_LEN])

static void frame_start(uint16_t vlan, uint16_t type)
{
	memset(uip_buf, 0, sizeof(uip_buf));
	uip_buf[12] = 0x88; uip_buf[13] = 0x99;		/* CPU tag */
	uip_buf[22] = vlan >> 8; uip_buf[23] = vlan;	/* VLAN tag */
	uip_buf[24] = type >> 8; uip_buf[25] = type;
	rx_packet_vlan = vlan;
}

static void put_ip(uint8_t *p, const uint8_t *a) { memcpy(p, a, 4); }

static void arp(uint16_t vlan, int op, const uint8_t *sip, const uint8_t *dip)
{
	frame_start(vlan, 0x0806);
	PAY[0] = 0; PAY[1] = 1; PAY[2] = 0x08; PAY[3] = 0; PAY[4] = 6; PAY[5] = 4;
	PAY[6] = 0; PAY[7] = op;
	put_ip(PAY + 14, sip);
	put_ip(PAY + 24, dip);
	uip_len = UIP_LLH_LEN + 28;
}

static void ipv4(uint16_t vlan, uint8_t proto, const uint8_t *sip, const uint8_t *dip,
		 uint16_t dport, uint8_t icmp_type)
{
	frame_start(vlan, 0x0800);
	PAY[0] = 0x45;
	PAY[9] = proto;
	put_ip(PAY + 12, sip);
	put_ip(PAY + 16, dip);
	if (proto == 6) {
		PAY[22] = dport >> 8; PAY[23] = dport;
		uip_len = UIP_LLH_LEN + 40;
	} else {
		PAY[20] = icmp_type;
		uip_len = UIP_LLH_LEN + 28;
	}
}

static const uint8_t API_IP[4] = { 10, 20, 0, 2 };
static const uint8_t CLIENT[4] = { 10, 20, 0, 10 };
static const uint8_t OTHER[4] = { 10, 20, 0, 11 };
static const uint8_t MGMT_IP[4] = { 192, 168, 2, 1 };

static void configure(void)
{
	api_init();
	memset(l2_entry, 0, sizeof(l2_entry));
	l2_calls = 0;
	management_vlan = 1;
	api.vlan = 20;
	uip_ipaddr(&api.ip, 10, 20, 0, 2);
	uip_ipaddr(&api.netmask, 255, 255, 255, 0);
	uip_ipaddr(&api.allow, 10, 20, 0, 10);
	strcpy(api.token, "0123456789abcdef0123456789abcdef");
	api.enabled = 1;
	api_update();
}

static void test_complete(void)
{
	configure();
	CHECK(api.active, "complete settings: active");
	CHECK(l2_entry[20], "active: the switch's MAC is installed in the API VLAN");

	api.enabled = 0; api_update();
	CHECK(!api.active && !l2_entry[20], "off: inactive, L2 entry removed");

	configure();
	api.token[0] = 0; api_update();
	CHECK(!api.active, "no token: inactive");

	configure();
	uip_ipaddr(&api.allow, 10, 21, 0, 10); api_update();
	CHECK(!api.active, "client outside the API subnet: inactive (no gateway)");

	configure();
	uip_ipaddr(&api.allow, 10, 20, 0, 2); api_update();
	CHECK(!api.active, "client equal to the API address: inactive");

	configure();
	management_vlan = 20; api_update();
	CHECK(!api.active, "API VLAN equal to the management VLAN: inactive");

	configure();
	management_vlan = 0; api.vlan = 1; api_update();
	CHECK(!api.active, "management filter off answers in VLAN 1: API VLAN 1 inactive");

	/* Changing the VLAN moves the entry */
	configure();
	api.vlan = 30; api_update();
	CHECK(api.active && l2_entry[30] && !l2_entry[20], "VLAN change: the entry moves");

	/* The management VLAN's entry is never removed by the API */
	configure();
	l2_entry[1] = 1;
	api_l2_drop();
	CHECK(l2_entry[1] && !l2_entry[20], "dropping the API entry keeps the management entry");
}

static void test_filters(void)
{
	configure();

	arp(20, 1, CLIENT, API_IP);
	CHECK(api_rx_vlan() && api_arp_accept(), "ARP request from the client for the API address: accepted");
	arp(20, 1, OTHER, API_IP);
	CHECK(!api_arp_accept(), "ARP request from another host: dropped");
	arp(20, 1, CLIENT, MGMT_IP);
	CHECK(!api_arp_accept(), "ARP request for the management address in the API VLAN: dropped");
	arp(20, 2, CLIENT, API_IP);
	CHECK(api_arp_accept(), "ARP reply from the client: accepted");
	arp(20, 2, OTHER, API_IP);
	CHECK(!api_arp_accept(), "ARP reply from another host: dropped");
	arp(20, 1, CLIENT, API_IP);
	uip_len = UIP_LLH_LEN + 20;
	CHECK(!api_arp_accept(), "truncated ARP: dropped");

	ipv4(20, 6, CLIENT, API_IP, 80, 0);
	CHECK(api_ip_accept(), "TCP port 80 from the client: accepted");
	ipv4(20, 6, OTHER, API_IP, 80, 0);
	CHECK(!api_ip_accept(), "TCP port 80 from another host: dropped");
	ipv4(20, 6, CLIENT, API_IP, 22, 0);
	CHECK(!api_ip_accept(), "TCP port 22 from the client: dropped");
	ipv4(20, 6, CLIENT, MGMT_IP, 80, 0);
	CHECK(!api_ip_accept(), "TCP to the management address in the API VLAN: dropped");
	ipv4(20, 17, CLIENT, API_IP, 0, 0);
	uip_len = UIP_LLH_LEN + 28;
	CHECK(!api_ip_accept(), "UDP from the client: dropped");
	ipv4(20, 1, CLIENT, API_IP, 0, 8);
	CHECK(api_ip_accept(), "ping from the client: accepted");
	ipv4(20, 1, CLIENT, API_IP, 0, 0);
	CHECK(!api_ip_accept(), "other ICMP from the client: dropped");
	ipv4(20, 6, CLIENT, API_IP, 80, 0);
	PAY[6] = 0x20;	/* more fragments */
	CHECK(!api_ip_accept(), "fragment: dropped");
	ipv4(20, 6, CLIENT, API_IP, 80, 0);
	PAY[0] = 0x46;	/* IP options */
	CHECK(!api_ip_accept(), "IP options: dropped");

	ipv4(1, 6, CLIENT, API_IP, 80, 0);
	CHECK(!api_rx_vlan(), "a frame from another VLAN is not the API's");
	api.enabled = 0; api_update();
	ipv4(20, 6, CLIENT, API_IP, 80, 0);
	CHECK(!api_rx_vlan(), "API off: frames of its VLAN are not the API's");
}

static void test_context(void)
{
	configure();
	uip_ipaddr(&uip_hostaddr, 192, 168, 2, 1);
	uip_ipaddr(&uip_netmask, 255, 255, 255, 0);
	uip_ipaddr(&uip_draddr, 192, 168, 2, 254);
	api_ctx_enter();
	CHECK(api_ctx && !memcmp(uip_hostaddr, api.ip, 4) && !memcmp(uip_netmask, api.netmask, 4)
	      && !(uip_draddr[0] | uip_draddr[1]), "enter: uIP runs on the API address, no gateway");
	CHECK(!memcmp(api_mgmt_host, MGMT_IP, 4), "enter: the management address is kept");
	api_ctx_leave();
	CHECK(!api_ctx && !memcmp(uip_hostaddr, MGMT_IP, 4) && ((uint8_t *)uip_draddr)[3] == 254,
	      "leave: the management addressing is back");
}

static void test_token(void)
{
	uint8_t buf[64];

	configure();
	rnd = 1;
	api_token_new();
	CHECK(strlen(api.token) == API_TOKEN_LEN, "new token: 32 characters");
	CHECK(strspn(api.token, "0123456789abcdef") == API_TOKEN_LEN, "new token: lower-case hex");
	char first[API_TOKEN_LEN + 1];
	strcpy(first, api.token);
	api_token_new();
	CHECK(strcmp(first, api.token), "new token: differs from the previous one");

	strcpy((char *)buf, "fedcba9876543210fedcba9876543210");
	CHECK(api_token_set(buf) && !strcmp(api.token, (char *)buf), "set: 32 hex digits accepted");
	strcpy((char *)buf, "FEDCBA9876543210fedcba9876543210");
	CHECK(!api_token_set(buf), "set: upper case refused (the config stores lower case)");
	strcpy((char *)buf, "fedcba9876543210fedcba987654321");
	CHECK(!api_token_set(buf), "set: 31 digits refused");
	strcpy((char *)buf, "fedcba9876543210fedcba98765432100");
	CHECK(!api_token_set(buf), "set: 33 digits refused");
	CHECK(!strcmp(api.token, "fedcba9876543210fedcba9876543210"), "set: a refused value leaves the token alone");

	strcpy((char *)buf, "Bearer fedcba9876543210fedcba9876543210\r\n");
	CHECK(api_token_check(buf), "check: Bearer <token> CRLF");
	strcpy((char *)buf, "BEARER   fedcba9876543210fedcba9876543210");
	CHECK(api_token_check(buf), "check: scheme in capitals, several spaces");
	strcpy((char *)buf, "Bearerfedcba9876543210fedcba9876543210");
	CHECK(!api_token_check(buf), "check: no space after the scheme");
	strcpy((char *)buf, "Bearer fedcba9876543210fedcba98765432\r\n");
	CHECK(!api_token_check(buf), "check: short token");
}

int main(void)
{
	printf("== api: read-only API VLAN ==\n");
	test_complete();
	test_filters();
	test_context();
	test_token();
	printf("api: %d checks, %d failed\n", tests_run, tests_failed);
	return tests_failed ? 1 : 0;
}
