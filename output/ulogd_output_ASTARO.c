/*
 * ulogd_output_ASTARO.c
 *
 * A variant of the SYSLOG plugin with Astaro conformable logging.
 *
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 *
 * H. Eitzenberger, 2006  Astaro AG
 */
#include "config.h"
#include <ulogd/ulogd.h>
#include <ulogd/common.h>
#include <ulogd/plugin.h>
#include <ulogd/ifi.h>
#include <unistd.h>
#include <syslog.h>
#include <netinet/in.h>
#include <netinet/ip.h>
#include <netinet/ether.h>
#include <arpa/inet.h>

/* config accessors */
#define CFG_FACILITY(pi)	((pi)->config_kset->ces[0].u.string)
#define CFG_LEVEL(pi)		((pi)->config_kset->ces[1].u.string)
/*
 * The 'blackhole' config switch is a short-term solution for HA setups
 * where we want a running ulogd which does just no loggging as long
 * as it is in slave mode.  Generally a generic mechanism for all output
 * plugins would be preferable.
 */
#define CFG_BLACKHOLE(pi)	((pi)->config_kset->ces[2].u.value)

/* packetfilter range */
#define __PF_BASE		2000
#define LOG_ID_LOG			(__PF_BASE + 0)
#define LOG_ID_DROP			(__PF_BASE + 1)
#define LOG_ID_ACCEPT		(__PF_BASE + 2)
#define LOG_ID_REJECT		(__PF_BASE + 3)
#define LOG_ID_INVAL_PKT	(__PF_BASE + 4)
#define LOG_ID_SPOOFING_DROP (__PF_BASE + 5)
#define LOG_ID_ICMP_REDIR	(__PF_BASE + 9)
#define LOG_ID_H323_RTP		(__PF_BASE + 10)
#define LOG_ID_Q931_GK		(__PF_BASE + 11)
#define LOG_ID_STRICT_TCP	(__PF_BASE + 12)
#define LOG_ID_FTP_DATA		(__PF_BASE + 13)
#define LOG_ID_DNS_REQ		(__PF_BASE + 14)
#define LOG_ID_SIP_RTP		(__PF_BASE + 16)
#define LOG_ID_AFC_ALERT	(__PF_BASE + 17)
#define LOG_ID_AFC_FT_BLOCK	(__PF_BASE + 18)
#define LOG_ID_AFC_BLOCK	(__PF_BASE + 19)
#define LOG_ID_TRACE		(__PF_BASE + 20)

/* IPS range */
#define __IPS_BASE	2100
#define LOG_ID_PORTSCAN		(__IPS_BASE + 2)
#define LOG_ID_SYN_FLOOD	(__IPS_BASE + 3)
#define LOG_ID_ICMP_FLOOD	(__IPS_BASE + 4)
#define LOG_ID_UDP_FLOOD	(__IPS_BASE + 5)

/* logging flags for custom log handler */
#define LH_F_NOLOG		0x0001	/* never log */
#define LH_F_ALWAYS		0x0002	/* always log */


/* one entry for each entry in astaro_in_keys */
struct log_handler {
	/* alternative logging prefix (instead of ulogd_key name) */
	char *name;

	/* custom log handler */
	int (* fn)(const struct ulogd_pluginstance *, unsigned, char *, size_t);

	unsigned flags;
};


/* map log prefix to descriptive text and ID */
static struct log_type {
	char *prefix;				/* same as LOG target --log-prefix */
	char *desc;					/* descriptive text */
	unsigned id;			 /* Astaro log ID, see Wiki for details */
	char *action;
	size_t prefix_len;
} log_types[] = {
	/* the first entry is the fallback entry */
	{ "LOG:", "Packet logged", LOG_ID_LOG, "log" },
	{ "DROP:", "Packet dropped", LOG_ID_DROP, "drop" },
	{ "ACCEPT:", "Packet accepted", LOG_ID_ACCEPT, "accept" },
	{ "REJECT:", "Packet rejected", LOG_ID_REJECT, "reject" },
	{ "INVALID_PKT:", "Invalid packet", LOG_ID_INVAL_PKT, "invalid packet" },
	{ "IP-SPOOFING DROP:", "IP spoofing drop", LOG_ID_SPOOFING_DROP,
		"IP spoofing drop" },
	{ "SYN_FLOOD:", "SYN flood detected", LOG_ID_SYN_FLOOD, "SYN flood" },
	{ "ICMP_FLOOD:", "ICMP flood detected", LOG_ID_ICMP_FLOOD, "ICMP flood" },
	{ "UDP_FLOOD:", "UDP flood detected", LOG_ID_UDP_FLOOD, "UDP flood" },
	{ "ICMP REDIRECT:", "ICMP redirect", LOG_ID_ICMP_REDIR, "ICMP redirect" },
	{ "H.323  RTP:", "H.323 RTP", LOG_ID_H323_RTP, "H.323 RTP" },
	{ "Q.931 Gatekeeper connection:", "Q.931 Gatekeeper", LOG_ID_Q931_GK,
	  "Q.931 Gatekeeper" },
	{ "STRICT_TCP_STATE:", "strict TCP state", LOG_ID_STRICT_TCP,
	  "strict TCP state" },
	{ "FTP_DATA:", "FTP data", LOG_ID_FTP_DATA, "FTP data" },
	{ "DNS_REQUEST:", "DNS request", LOG_ID_DNS_REQ, "DNS request" },
	{ "PORTSCAN:", "portscan detected", LOG_ID_PORTSCAN, "portscan" },
	{ "SIP Call RTP:", "SIP call RTP", LOG_ID_SIP_RTP, "SIP call RTP" },
	{ "AFC_ALERT ", "AFC Alert", LOG_ID_AFC_ALERT, "log" },
	/* the '-' is correct */
	{ "AFC_FT-BLOCK ", "AFC FT Block", LOG_ID_AFC_FT_BLOCK, "drop" },
	{ "AFC_BLOCK ", "AFC Block", LOG_ID_AFC_BLOCK, "drop" },
	{ "TRACE:", "Packet traced", LOG_ID_TRACE, "log" },
	{ NULL, }
};


enum InKeys {
	InOobPrefix,
	InOobLogmark,
	InOobSeqLocal,
	InOobIfiIn,
	InOobIfiOut,
	InRawMac,
	InIpSAddr,
	InIpDAddr,
	InIpProto,
	InRawPktLen,
	InIpTos,
	InIpTtl,
	InIp6SAddr,
	InIp6DAddr,
	InIp6Len,
	InIp6Hlim,
	InTcpSPort,
	InTcpDPort,
	InUdpSPort,
	InUdpDPort,
	InTcpAck,
	InTcpPsh,
	InTcpRst,
	InTcpSyn,
	InTcpFin,
	InIcmpType,
	InIcmpCode,
};

struct ulogd_key astaro_in_keys[] = {
	[InOobPrefix] = KEY(STRING, "oob.prefix"),
	[InOobLogmark] = KEY(UINT32, "oob.logmark"),
	[InOobSeqLocal] = KEY(UINT32, "oob.seq.local"),
	[InOobIfiIn] = KEY(UINT32, "oob.ifindex_in"),
	[InOobIfiOut] = KEY(UINT32, "oob.ifindex_out"),
	[InRawMac] = KEY(RAW, "raw.mac"),
	[InIpSAddr] = KEY(IPADDR, "ip.saddr"),
	[InIpDAddr] = KEY(IPADDR, "ip.daddr"),
	[InIpProto] = KEY(UINT8, "ip.protocol"),
	[InIp6SAddr] = KEY(IP6ADDR, "ip6.saddr"),
	[InIp6DAddr] = KEY(IP6ADDR, "ip6.daddr"),
	[InIp6Len] = KEY(UINT16, "ip6.len"),
	[InIp6Hlim] = KEY(UINT8, "ip6.hlim"),
	[InRawPktLen] = KEY(UINT32, "raw.pktlen"),
	[InIpTos] = KEY(UINT8, "ip.tos"),
	[InIpTtl] = KEY(UINT8, "ip.ttl"),
	[InTcpSPort] = KEY(UINT16, "tcp.sport"),
	[InTcpDPort] = KEY(UINT16, "tcp.dport"),
	[InUdpSPort] = KEY(UINT16, "udp.sport"),
	[InUdpDPort] = KEY(UINT16, "udp.dport"),
	[InTcpAck] = KEY(BOOL, "tcp.ack"),
	[InTcpPsh] = KEY(BOOL, "tcp.psh"),
	[InTcpRst] = KEY(BOOL, "tcp.rst"),
	[InTcpSyn] = KEY(BOOL, "tcp.syn"),
	[InTcpFin] = KEY(BOOL, "tcp.fin"),
	[InIcmpType] = KEY(UINT8, "icmp.type"),
	[InIcmpCode] = KEY(UINT8, "icmp.code"),
};

static int
avail(const char *buf, const char *pch, size_t max_len)
{
	return buf + max_len - pch;
}


/* mac address log helper */
static int
lh_log_mac(const struct ulogd_pluginstance *pi, unsigned idx,
		   char *buf, size_t len)
{
	const struct ulogd_key *in = pi->input.keys;
	static const char unknown[] = "00:00:00:00:00:00";
	static char __src[3 * ETH_ALEN], __dst[3 * ETH_ALEN];
	const char *src, *dst;

	if (key_src_valid(&in[InOobIfiIn]))
		dst = ifi_hwaddr2str((int)key_src_u32(&in[InOobIfiIn]), __dst,
							 sizeof(__dst));
	if (!dst)
		dst = unknown;

	if (key_src_valid(&in[InOobIfiOut]))
		src = ifi_hwaddr2str((int)key_src_u32(&in[InOobIfiOut]), __src,
							 sizeof(__src));
	if (!src)
		src = unknown;
		
	return snprintf(buf, len, "dstmac=\"%s\" srcmac=\"%s\" ", src, dst);

	return 0;
}


static int
lh_log_tos(const struct ulogd_pluginstance *pi, unsigned idx,
		   char *buf, size_t len)
{
	const struct ulogd_key *in = pi->input.keys;
	const uint8_t tos = key_src_u8(&in[InIpTos]);
	
	return snprintf(buf, len, "tos=\"0x%02x\" prec=\"0x%02x\" ",
					IPTOS_TOS(tos), IPTOS_PREC(tos));
}


static struct log_handler log_handler[];

static int
lh_log_itf(const struct ulogd_pluginstance *pi, unsigned idx,
		   char *buf, size_t len)
{
	const struct ulogd_key *in = pi->input.keys;
	char __ifname[IF_NAMESIZE], *ifname;
	char *key_name = log_handler[idx].name ? log_handler[idx].name : "itf";

	ifname = ifi_index2name((int)key_src_u32(&in[idx]), __ifname,
							sizeof(__ifname));

	return snprintf(buf, len, "%s=\"%s\" ", key_name, ifname ? ifname
					: "unknown");
	return 0;
}

static struct log_handler log_handler[ARRAY_SIZE(astaro_in_keys)] = {
	[InOobPrefix] = { NULL, NULL, LH_F_NOLOG },
	[InOobLogmark] = { "fwrule", },
	[InOobSeqLocal] = { "seq", },
	[InOobIfiIn] = { "initf", lh_log_itf, },
	[InOobIfiOut] = { "outitf", lh_log_itf, },
	[InRawMac] = { NULL, lh_log_mac, LH_F_ALWAYS },
	[InIpSAddr] = { "srcip", },
	[InIpDAddr] = { "dstip", },
	[InIpProto] = { "proto", },
	[InIp6SAddr] = { "srcip", },
	[InIp6DAddr] = { "dstip", },
	[InRawPktLen] = { "length", },
	[InIpTos] = { "tos", lh_log_tos },
	[InIpTtl] = { "ttl", },
	[InIp6Len] = { "len", },
	[InIp6Hlim] = { "hlim", },
	[InTcpSPort] = { "srcport", },
	[InTcpDPort] = { "dstport", },
	[InUdpSPort] = { "srcport", },
	[InUdpDPort] = { "dstport", },
	[InTcpAck] = { NULL, NULL, LH_F_NOLOG },
	[InTcpPsh] = { NULL, NULL, LH_F_NOLOG },
	[InTcpRst] = { NULL, NULL, LH_F_NOLOG },
	[InTcpSyn] = { NULL, NULL, LH_F_NOLOG },
	[InTcpFin] = { NULL, NULL, LH_F_NOLOG },
	[InIcmpType] = { NULL, NULL, LH_F_NOLOG },
	[InIcmpCode] = { NULL, NULL, LH_F_NOLOG },
};

static const struct config_keyset astaro_kset = {
	.num_ces = 3,
	.ces = {
		CONFIG_KEY_STR("facility", 0),
		CONFIG_KEY_STR("level", 0),
		CONFIG_KEY_INT("blackhole", 0),
	},
};

struct astaro_priv {
	int level;
	int facility;
};

/* map LOG target prefix to type, use first entry as fallback */
static unsigned
log_prefix2type(const struct log_type *t, const char *prefix)
{
	unsigned n;

	if (!prefix)
		return 0;

	for (n = 0; t[n].prefix; n++) {
		if (strncmp(t[n].prefix, prefix, t[n].prefix_len) == 0)
			return n;
	}

	return 0;
}

/* print key in standard logging format */
static int
print_key(char *buf, size_t len, const struct ulogd_key *key,
		  const char *name)
{
	char str[64], *pch = buf;

	key = key_src(key);

	switch (key->type) {
	case ULOGD_RET_STRING:
		pch += snprintf(pch, avail(buf, pch, len), "%s=\"%s\" ", name,
						key_str(key));
		break;
		
	case ULOGD_RET_UINT8:
		pch += snprintf(pch, avail(buf, pch, len), "%s=\"%u\" ", name,
						key_u8(key));
		break;
		
	case ULOGD_RET_UINT16:
		if (key_u16(key))
			pch += snprintf(pch, avail(buf, pch, len), "%s=\"%u\" ", name,
							key_u16(key));
		break;
		
	case ULOGD_RET_UINT32:
		if (key_u32(key))
			pch += snprintf(pch, avail(buf, pch, len), "%s=\"%u\" ", name,
							key_u32(key));
		break;
		
	case ULOGD_RET_IPADDR:
	{
		struct in_addr addr = (struct in_addr){ key_u32(key), };

		inet_ntop(AF_INET, &addr, str, sizeof(str));
		pch += snprintf(pch, avail(buf, pch, len), "%s=\"%s\" ",
						name, str);
		break;
	}
		
	case ULOGD_RET_IP6ADDR:
	{
		struct in6_addr addr;

		key_in6(key, &addr);
		inet_ntop(AF_INET6, &addr, str, sizeof(str));

		pch += snprintf(pch, avail(buf, pch, len), "%s=\"%s\" ",
						name, str);
		break;
	}

	default:
		break;
	};

	return pch - buf;
}

static int
print_proto_tcp(const struct ulogd_pluginstance *pi, char *buf, size_t len)
{
	const struct ulogd_key *in = pi->input.keys;
	char *pch = buf;
	int delim = 0;

	/* srcport/dstport are handled through generic logging handler */

	strcpy(buf, "tcpflags=\"");
	pch = buf + sizeof("tcpflags=\"") - 1;

	if (key_src_bool(&in[InTcpAck]))
		strncat_delim(&pch, "ACK", sizeof("ACK"), &delim);
	if (key_src_bool(&in[InTcpPsh]))
		strncat_delim(&pch, "PSH", sizeof("PSH"), &delim);
	if (key_src_bool(&in[InTcpRst]))
		strncat_delim(&pch, "RST", sizeof("RST"), &delim);
	if (key_src_bool(&in[InTcpSyn]))
		strncat_delim(&pch, "SYN", sizeof("SYN"), &delim);
	if (key_src_bool(&in[InTcpFin]))
		strncat_delim(&pch, "FIN", sizeof("FIN"), &delim);

	strncat_delim(&pch, "\" ", sizeof("\" "), NULL);
		
	return pch - buf;
}

static int
print_proto_icmp(const struct ulogd_pluginstance *pi, char *buf, size_t len)
{
	const struct ulogd_key *in = pi->input.keys;
	char *pch = buf;

	pch += print_key(pch, len, &in[InIcmpType], "type");
	pch += print_key(pch, avail(buf, pch, len), &in[InIcmpCode], "code");

	return pch - buf;
}

/**
 * Print log data resulting from the netfilter TRACE target
 */
static int
print_trace(const struct ulogd_pluginstance *pi, char *buf, size_t max_len)
{
	const struct ulogd_key *in = pi->input.keys;

	return snprintf(buf, max_len, "trace=\"%s\" ",
					key_src_str(&in[InOobPrefix]) + sizeof("TRACE: ") - 1);
}

static int
print_dyn_part(const struct ulogd_pluginstance *pi, unsigned type,
			   char *buf, size_t max_len)
{
	const struct ulogd_key *in = pi->input.keys;
	char *pch = buf;
	int i;

	for (i = 0; i < pi->input.num_keys; i++) {
		struct ulogd_key *key = &pi->input.keys[i];
		char *name;

		if (!(log_handler[i].flags & LH_F_ALWAYS)) {
			if (!key_src_valid(key))
				continue;

			if (log_handler[i].flags & LH_F_NOLOG)
				continue;
		}

		/* log handler name takes precedence */
		name = log_handler[i].name ? log_handler[i].name : key->name;

		/* custom logging handler? */
		if (log_handler[i].fn) {
			pch += (log_handler[i].fn)(pi, i, pch, avail(buf, pch, max_len));
			continue;
		}

		pch += print_key(pch, avail(buf, pch, max_len), key, name);
	}

	/* print proto specific part */
	if (key_src_u8(&in[InIpProto]) == IPPROTO_TCP)
		pch += print_proto_tcp(pi, pch, avail(buf, pch, max_len));
	else if (key_src_u8(&in[InIpProto]) == IPPROTO_ICMP)
		pch += print_proto_icmp(pi, pch, avail(buf, pch, max_len));

	if (__PF_BASE + type == LOG_ID_LOG) {
		/* log Netfilter-internal messages without loss of information */
		if (key_src_valid(&in[InOobPrefix])) {
			pch += snprintf(pch, avail(buf, pch, max_len), "info=\"%s\"",
							key_src_str(&in[InOobPrefix]));
		}
	}

	/* ideally log_prefix2type() would return the real ID, not the
	   index into the array */
	if (__PF_BASE + type == LOG_ID_TRACE)
		pch += print_trace(pi, pch, avail(buf, pch, max_len));

	return pch - buf;
}


/* map log ID to subsystem (packetfilter, ips) */
static const char *
id_to_sub(unsigned id)
{
	if (id >= __IPS_BASE)
		return "ips";
	if (id >= __PF_BASE)
		return "packetfilter";

	return "unknown";
}

static int
astaro_output(struct ulogd_pluginstance *pi, unsigned *flags)
{
	struct astaro_priv *priv = upi_priv(pi);
	const struct ulogd_key *in = pi->input.keys;
	static char buf[1024];
	char *pch = buf, *end = buf + sizeof(buf);
	unsigned type;
	
	if (CFG_BLACKHOLE(pi))
		return ULOGD_IRET_OK;

	type = log_prefix2type(log_types, key_src_valid(&in[InOobPrefix]) ?
						   key_src_str(&in[InOobPrefix]) : NULL);
	
	pch += snprintf(pch, end - pch,
					"id=\"%u\" severity=\"info\" sys=\"SecureNet\" " 
					"sub=\"%s\" name=\"%s\" action=\"%s\" ",
					log_types[type].id, id_to_sub(log_types[type].id),
					log_types[type].desc, log_types[type].action);

	print_dyn_part(pi, type, pch, end - pch);

	syslog(priv->level | priv->facility, "%s\n", buf);

	return ULOGD_IRET_OK;
}


/* name-value pair */
static struct nv {
	char *name;
	int val;
} nv_facility[] = {
	{ "LOG_DAEMON", LOG_DAEMON },
	{ "LOG_KERN", LOG_KERN },
	{ "LOG_LOCAL0", LOG_LOCAL0 },
	{ "LOG_LOCAL1", LOG_LOCAL1 },
	{ "LOG_LOCAL2", LOG_LOCAL2 },
	{ "LOG_LOCAL3", LOG_LOCAL3 },
	{ "LOG_LOCAL4", LOG_LOCAL4 },
	{ "LOG_LOCAL5", LOG_LOCAL5 },
	{ "LOG_LOCAL6", LOG_LOCAL6 },
	{ "LOG_LOCAL7", LOG_LOCAL7 },
	{ "LOG_USER", LOG_USER },
	{ 0, }
};
static struct nv nv_level[] = {
	{ "LOG_EMERG", LOG_EMERG },
	{ "LOG_ALERT", LOG_ALERT },
	{ "LOG_CRIT", LOG_CRIT },
	{ "LOG_ERR", LOG_ERR },
	{ "LOG_WARNING", LOG_WARNING },
	{ "LOG_NOTICE", LOG_NOTICE },
	{ "LOG_INFO", LOG_INFO },
	{ "LOG_DEBUG", LOG_DEBUG },
	{ 0, }
};

static int
nv_get_value(struct nv *nv, const char *name, int def_val)
{
	if (*name == '\0')
		return def_val;

	for (; nv->name; nv++) {
		if (strcmp(nv->name, name) == 0)
			return nv->val;
	}

	return -1;
};

static int
astaro_configure(struct ulogd_pluginstance *pi)
{
	struct astaro_priv *priv = upi_priv(pi);

	priv->facility = nv_get_value(nv_facility, CFG_FACILITY(pi), LOG_KERN);
	if (priv->facility < 0) {
		upi_log(pi, ULOGD_FATAL, "unknown facility '%s'\n", CFG_FACILITY(pi));
		return -EINVAL;
	}

	priv->level = nv_get_value(nv_level, CFG_LEVEL(pi), LOG_NOTICE);
	if (priv->level < 0) {
		upi_log(pi, ULOGD_FATAL, "unknown level '%s'\n", CFG_LEVEL(pi));
		return -EINVAL;
	}

	return ULOGD_IRET_OK;
}

static int
astaro_fini(struct ulogd_pluginstance *pi)
{
	if (CFG_BLACKHOLE(pi))
		return ULOGD_IRET_OK;

	closelog();

	return ULOGD_IRET_OK;
}

static int
astaro_start(struct ulogd_pluginstance *pi)
{
	int i;

	if (CFG_BLACKHOLE(pi)) {
		upi_log(pi, ULOGD_INFO, "running in blackhole mode\n");
		return ULOGD_IRET_OK;
	}

	openlog("ulogd", LOG_NDELAY | LOG_PID, LOG_DAEMON);

	for (i = 0; log_types[i].prefix; i++)
		log_types[i].prefix_len = strlen(log_types[i].prefix);

	return ULOGD_IRET_OK;
}

static struct ulogd_plugin astaro_plugin = {
	.name = "ASTARO",
	.input = {
		.keys = astaro_in_keys,
		.num_keys = ARRAY_SIZE(astaro_in_keys),
		.type = ULOGD_DTYPE_PACKET | ULOGD_DTYPE_FLOW,
	},
	.output = {
		.type = ULOGD_DTYPE_SINK,
	},
	.config_kset		= &astaro_kset,
	.configure	= astaro_configure,
	.start		= astaro_start,
	.stop		= astaro_fini,
	.interp		= astaro_output,
	.rev		= ULOGD_PLUGIN_REVISION,
	.priv_size	= sizeof(struct astaro_priv),
};

void __upi_ctor init(void);

void
init(void)
{
	ulogd_register_plugin(&astaro_plugin);
}
