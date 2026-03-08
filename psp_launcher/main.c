#include <pspkernel.h>
#include <pspdebug.h>
#include <psputility.h>
#include <psputility_netparam.h>
#include <pspnet.h>
#include <pspnet_inet.h>
#include <pspnet_apctl.h>
#include <pspctrl.h>
#include <pspdisplay.h>
#include <pspkerror.h>
#include <pspmodulemgr.h>
#include <pspsdk.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#ifdef ENABLE_WLAN_SAMPLE_INIT
#include <pspwlan.h>
#endif
#ifdef ENABLE_KUBRIDGE_LOAD
#include <kubridge.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

#include "../nethostfs.h"

#define printf pspDebugScreenPrintf

#define LAUNCHER_NAME "NetHostFSLauncher"
#define DEFAULT_HOST "192.168.1.192"
#define DEFAULT_PORT 7513
#define DEFAULT_PROFILE 1
#define DEFAULT_PRX_FALLBACK "ms0:/nethostfs_psp.prx"
#define MAX_BROWSER_ENTRIES 256
#define BROWSER_VISIBLE_LINES 24

PSP_MODULE_INFO(LAUNCHER_NAME, PSP_MODULE_USER, 1, 0);
PSP_MAIN_THREAD_ATTR(THREAD_ATTR_USER);
PSP_HEAP_SIZE_KB(-2048);
PSP_MAIN_THREAD_STACK_SIZE_KB(128);

typedef struct LauncherConfig {
	char host[64];
	int port;
	int profile;
	char password[32];
	char prx_path[256];
} LauncherConfig;

typedef struct BrowserEntry {
	char name[256];
	int is_dir;
	unsigned int size_lo;
} BrowserEntry;

typedef struct BrowserSession {
	int socks[4];
	int channels;
} BrowserSession;

static int exit_callback(int arg1, int arg2, void *common)
{
	(void)arg1;
	(void)arg2;
	(void)common;
	sceKernelExitGame();
	return 0;
}

static int callback_thread(SceSize args, void *argp)
{
	int cbid;

	(void)args;
	(void)argp;

	cbid = sceKernelCreateCallback("Exit Callback", exit_callback, NULL);
	sceKernelRegisterExitCallback(cbid);
	sceKernelSleepThreadCB();
	return 0;
}

static int setup_callbacks(void)
{
	int thid;

	thid = sceKernelCreateThread(
		"nethostfs_exit_cb",
		callback_thread,
		0x11,
		0x1000,
		PSP_THREAD_ATTR_USER,
		0
	);

	if (thid >= 0) {
		sceKernelStartThread(thid, 0, NULL);
	}

	return thid;
}

static void build_default_prx_path(const char *argv0, char *out, size_t out_size)
{
	const char *default_path = DEFAULT_PRX_FALLBACK;
	char *slash;
	size_t base_len;
	const char *name = "nethostfs_psp.prx";

	if ((out == NULL) || (out_size == 0)) {
		return;
	}

	out[0] = '\0';
	if (argv0 == NULL) {
		strncpy(out, default_path, out_size - 1);
		out[out_size - 1] = '\0';
		return;
	}

	strncpy(out, argv0, out_size - 1);
	out[out_size - 1] = '\0';
	slash = strrchr(out, '/');
	if (slash == NULL) {
		strncpy(out, default_path, out_size - 1);
		out[out_size - 1] = '\0';
		return;
	}

	base_len = (size_t)(slash - out + 1);
	if (base_len + strlen(name) >= out_size) {
		strncpy(out, default_path, out_size - 1);
		out[out_size - 1] = '\0';
		return;
	}

	strcpy(slash + 1, name);
}

static void config_init(LauncherConfig *cfg, const char *argv0)
{
	if (cfg == NULL) {
		return;
	}

	memset(cfg, 0, sizeof(*cfg));
	strncpy(cfg->host, DEFAULT_HOST, sizeof(cfg->host) - 1);
	cfg->port = DEFAULT_PORT;
	cfg->profile = DEFAULT_PROFILE;
	cfg->password[0] = '\0';
	build_default_prx_path(argv0, cfg->prx_path, sizeof(cfg->prx_path));
}

static int parse_int_range(const char *s, int min, int max, int *out)
{
	char *end;
	long value;

	if ((s == NULL) || (out == NULL) || (s[0] == '\0')) {
		return -1;
	}

	value = strtol(s, &end, 10);
	if ((end == s) || (*end != '\0') || (value < min) || (value > max)) {
		return -1;
	}

	*out = (int)value;
	return 0;
}

static void parse_args(LauncherConfig *cfg, int argc, char **argv)
{
	int i;

	if (cfg == NULL) {
		return;
	}

	for (i = 1; i < argc; i++) {
		if ((strcmp(argv[i], "-h") == 0) && (i + 1 < argc)) {
			strncpy(cfg->host, argv[++i], sizeof(cfg->host) - 1);
			cfg->host[sizeof(cfg->host) - 1] = '\0';
		} else if ((strcmp(argv[i], "-p") == 0) && (i + 1 < argc)) {
			int port;
			if (parse_int_range(argv[++i], 1, 65535, &port) == 0) {
				cfg->port = port;
			}
		} else if ((strcmp(argv[i], "-l") == 0) && (i + 1 < argc)) {
			strncpy(cfg->password, argv[++i], sizeof(cfg->password) - 1);
			cfg->password[sizeof(cfg->password) - 1] = '\0';
		} else if ((strcmp(argv[i], "-c") == 0) && (i + 1 < argc)) {
			int profile;
			if (parse_int_range(argv[++i], 1, 100, &profile) == 0) {
				cfg->profile = profile;
			}
		} else if ((strcmp(argv[i], "-m") == 0) && (i + 1 < argc)) {
			strncpy(cfg->prx_path, argv[++i], sizeof(cfg->prx_path) - 1);
			cfg->prx_path[sizeof(cfg->prx_path) - 1] = '\0';
		}
	}
}

static int resolve_profile_id(int requested)
{
	int i;

	if ((requested >= 1) && (sceUtilityCheckNetParam(requested) == 0)) {
		return requested;
	}

	if (requested >= 1) {
		printf("Profile %d is invalid (0x%08X)\n", requested, PSP_NETPARAM_ERROR_BAD_NETCONF);
	}

	for (i = 1; i <= 100; i++) {
		if (sceUtilityCheckNetParam(i) == 0) {
			printf("Using configured profile %d\n", i);
			return i;
		}
	}

	return -1;
}

#ifdef ENABLE_WLAN_SAMPLE_INIT
static int wlan_sample_precheck(void)
{
	u8 mac[8];
	int ret;

	memset(mac, 0, sizeof(mac));

	printf("Wlan sample-style precheck:\n");
	printf("Wlan switch is %s\n", sceWlanGetSwitchState() == 0 ? "off" : "on");
	printf("Wlan power is %s\n", sceWlanDevIsPowerOn() == 0 ? "off" : "on");

	ret = sceWlanGetEtherAddr(mac);
	if (ret == 0) {
		printf("Wlan Ethernet Addr: %02X:%02X:%02X:%02X:%02X:%02X\n",
		       mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
	} else {
		printf("Error getting Wlan Ethernet Address (0x%08X)\n", ret);
	}

	if (sceWlanGetSwitchState() == 0) {
		printf("WLAN switch is OFF. Turn it ON and relaunch.\n");
		return 0;
	}

	return 1;
}
#endif

static void load_network_modules(void)
{
	int ret;

	ret = sceUtilityLoadNetModule(PSP_NET_MODULE_COMMON);
	if ((ret < 0) && (ret != 0x80111102)) {
		printf("Load COMMON net module returned 0x%08X\n", ret);
	}

	ret = sceUtilityLoadNetModule(PSP_NET_MODULE_INET);
	if ((ret < 0) && (ret != 0x80111102)) {
		printf("Load INET net module returned 0x%08X\n", ret);
	}
}

static int init_network_stack(void)
{
	int ret;

	ret = sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
	if ((ret < 0) && (ret != 0x80412102)) {
		printf("sceNetInit returned 0x%08X\n", ret);
		return ret;
	}

	ret = sceNetInetInit();
	if ((ret < 0) && (ret != 0x80412102)) {
		printf("sceNetInetInit returned 0x%08X\n", ret);
		return ret;
	}

	ret = sceNetApctlInit(0x1800, 0x30);
	if ((ret < 0) && (ret != 0x80412102)) {
		printf("sceNetApctlInit returned 0x%08X\n", ret);
		return ret;
	}

	return 0;
}

/* Connect to an access point */
static int connect_to_apctl(int config)
{
	int err;
	int stateLast = -1;

	/* Connect using selected profile */
	err = sceNetApctlConnect(config);
	if (err != 0) {
		printf(LAUNCHER_NAME ": sceNetApctlConnect returns %08X\n", err);
		return 0;
	}

	printf(LAUNCHER_NAME ": Connecting...\n");
	while (1) {
		int state;
		err = sceNetApctlGetState(&state);
		if (err != 0) {
			printf(LAUNCHER_NAME ": sceNetApctlGetState returns $%x\n", err);
			break;
		}
		if (state > stateLast) {
			printf("  connection state %d of 4\n", state);
			stateLast = state;
		}
		if (state == 4) {
			break;
		}

		/* Wait a little before polling again. */
		sceKernelDelayThread(50 * 1000);
	}
	printf(LAUNCHER_NAME ": Connected!\n");

	if (err != 0) {
		return 0;
	}

	return 1;
}

static int send_all(int sock, const void *buf, int len)
{
	const unsigned char *p;
	int sent;
	int eagain_spins;

	if ((buf == NULL) || (len < 0)) {
		return -1;
	}

	p = (const unsigned char *)buf;
	sent = 0;
	eagain_spins = 0;

	while (sent < len) {
		int rc = (int)sceNetInetSend(sock, p + sent, (size_t)(len - sent), 0);
		if (rc > 0) {
			sent += rc;
			eagain_spins = 0;
			continue;
		}
		if (rc == 0) {
			return -1;
		}
		if (sceNetInetGetErrno() == 11) {
			if (eagain_spins++ > 250) {
				return -1;
			}
			sceKernelDelayThread(20 * 1000);
			continue;
		}
		if (rc < 0) {
			return -1;
		}
	}

	return 0;
}

static int recv_all(int sock, void *buf, int len)
{
	unsigned char *p;
	int recvd;
	int eagain_spins;

	if ((buf == NULL) || (len < 0)) {
		return -1;
	}

	p = (unsigned char *)buf;
	recvd = 0;
	eagain_spins = 0;

	while (recvd < len) {
		int rc = (int)sceNetInetRecv(sock, p + recvd, (size_t)(len - recvd), 0);
		if (rc > 0) {
			recvd += rc;
			eagain_spins = 0;
			continue;
		}
		if (rc == 0) {
			return -1;
		}
		if (sceNetInetGetErrno() == 11) {
			if (eagain_spins++ > 250) {
				return -1;
			}
			sceKernelDelayThread(20 * 1000);
			continue;
		}
		if (rc < 0) {
			return -1;
		}
	}

	return 0;
}

static int send_int32(int sock, int value)
{
	return send_all(sock, &value, (int)sizeof(value));
}

static int recv_int32(int sock, int *value)
{
	return recv_all(sock, value, (int)sizeof(*value));
}

static void encrypt_challenge(unsigned char *text, int textlen, const unsigned char *key)
{
	int keylen;
	int i;
	int j;
	unsigned char lastkey;

	if ((text == NULL) || (key == NULL)) {
		return;
	}

	keylen = (int)strlen((const char *)key);
	if (keylen <= 0) {
		return;
	}

	lastkey = key[0];
	j = 0;

	for (i = 0; i < textlen; i++) {
		unsigned char a = key[j];
		int b;

		text[i] = (unsigned char)((text[i] ^ a) - j);
		b = lastkey % 8;
		text[i] = (unsigned char)((text[i] >> b) + ((text[i] << (8 - b)) & 0xFF));
		text[i] ^= (unsigned char)(lastkey + key[keylen - j - 1]);
		lastkey = key[j];
		j++;
		if (j == keylen) {
			j = 0;
		}
	}
}

static int nethostfs_handshake(int sock, const char *password, int channel)
{
	int cmd;
	int res;
	IO_LOGIN_CHALLENGE_PARAMS challenge;
	IO_LOGIN_RESPONSE_PARAMS response;

	cmd = NET_HOSTFS_CMD_HELLO;
	if (send_int32(sock, cmd) < 0) {
		return -(1000 + (channel * 10) + 1);
	}
	if (recv_int32(sock, &res) < 0) {
		return -(1000 + (channel * 10) + 2);
	}
	if (res != cmd) {
		return -(1000 + (channel * 10) + 3);
	}

	cmd = NET_HOSTFS_CMD_IOINIT;
	if (send_int32(sock, cmd) < 0) {
		return -(1000 + (channel * 10) + 4);
	}

	if (recv_all(sock, &challenge, (int)sizeof(challenge)) < 0) {
		return -(1000 + (channel * 10) + 5);
	}

	memset(&response, 0, sizeof(response));
	if ((password != NULL) && (password[0] != '\0')) {
		memcpy(response.response, challenge.challenge, CHALLENGE_TEXT_LEN);
		encrypt_challenge(response.response, CHALLENGE_TEXT_LEN, (const unsigned char *)password);
	}

	if (send_all(sock, &response, (int)sizeof(response)) < 0) {
		return -(1000 + (channel * 10) + 6);
	}

	if (recv_int32(sock, &res) < 0) {
		return -(1000 + (channel * 10) + 7);
	}

	if (res < 0) {
		return res;
	}

	return 0;
}

static void close_browser_session(BrowserSession *session)
{
	int i;

	if (session == NULL) {
		return;
	}

	for (i = 0; i < 4; i++) {
		if (session->socks[i] >= 0) {
			sceNetInetClose(session->socks[i]);
			session->socks[i] = -1;
		}
	}
	session->channels = 0;
}

static int connect_browser_session(const LauncherConfig *cfg, BrowserSession *session)
{
	struct sockaddr_in addr;
	unsigned int ip_addr;
	int i;

	if ((cfg == NULL) || (session == NULL)) {
		return -1;
	}

	for (i = 0; i < 4; i++) {
		session->socks[i] = -1;
	}
	session->channels = 0;

	ip_addr = inet_addr(cfg->host);
	if (ip_addr == 0xFFFFFFFFU) {
		return -900;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip_addr;
	addr.sin_port = htons((u16)cfg->port);

	/* Use deterministic 4-channel setup on the base port to avoid stale half-sessions. */
	for (i = 0; i < 4; i++) {
		int sock;
		int tries;
		int connected = 0;
		int last_errno = 0;

		printf("Browser connect[%d] %s:%d\n", i, cfg->host, cfg->port);
		sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
		if (sock < 0) {
			close_browser_session(session);
			return -(910 + i);
		}

		for (tries = 0; tries < 30; tries++) {
			if (sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
				connected = 1;
				break;
			}

			last_errno = (int)sceNetInetGetErrno();
			if ((last_errno == 111) || (last_errno == 11)) {
				sceKernelDelayThread(50 * 1000);
				continue;
			}
			break;
		}

		if (!connected) {
			printf("Browser connect[%d] failed errno=%d\n", i, last_errno);
			sceNetInetClose(sock);
			close_browser_session(session);
			return -(920 + i);
		}

		session->socks[i] = sock;
		session->channels++;
		sceKernelDelayThread(50 * 1000);
	}

	if (session->channels <= 0) {
		close_browser_session(session);
		return -930;
	}

	/* Give server a moment to spawn session threads once channels are connected. */
	sceKernelDelayThread(150 * 1000);
	for (i = 0; i < session->channels; i++) {
		int hs = nethostfs_handshake(session->socks[i], cfg->password, i);
		if (hs < 0) {
			printf("Browser handshake[%d] failed 0x%08X\n", i, hs);
			if (hs == -1001) {
				printf("Tip: stale server session detected; restart nethostfs server or wait 16s.\n");
			}
			close_browser_session(session);
			return hs;
		}
	}

	return 0;
}

static int remote_dopen(int sock, const char *path)
{
	int cmd;
	int did;
	IO_DOPEN_PARAMS params;

	if (path == NULL) {
		return -1;
	}

	memset(&params, 0, sizeof(params));
	snprintf(params.dir, sizeof(params.dir), "%s", path);
	params.fs_num = 0;

	cmd = NET_HOSTFS_CMD_IODOPEN;
	if (send_int32(sock, cmd) < 0) {
		return -1;
	}

	if (send_all(sock, &params, (int)sizeof(params)) < 0) {
		return -1;
	}

	if (recv_int32(sock, &did) < 0) {
		return -1;
	}

	return did;
}

static int remote_dclose(int sock, int did)
{
	int cmd;
	int res;

	cmd = NET_HOSTFS_CMD_IODCLOSE;
	if (send_int32(sock, cmd) < 0) {
		return -1;
	}

	if (send_int32(sock, did) < 0) {
		return -1;
	}

	if (recv_int32(sock, &res) < 0) {
		return -1;
	}

	return res;
}

static int remote_dread(int sock, int did, IO_DREAD_RESULT *out)
{
	int cmd;

	if (out == NULL) {
		return -1;
	}

	memset(out, 0, sizeof(*out));
	cmd = NET_HOSTFS_CMD_IODREAD;
	if (send_int32(sock, cmd) < 0) {
		return -1;
	}

	if (send_int32(sock, did) < 0) {
		return -1;
	}

	if (recv_all(sock, out, (int)sizeof(*out)) < 0) {
		return -1;
	}

	return 0;
}

static int path_is_root(const char *path)
{
	return (path != NULL) && ((strcmp(path, "/") == 0) || (path[0] == '\0'));
}

static void path_go_up(char *path, size_t path_size)
{
	char *last;

	if ((path == NULL) || (path_size == 0)) {
		return;
	}

	if (path_is_root(path)) {
		strncpy(path, "/", path_size - 1);
		path[path_size - 1] = '\0';
		return;
	}

	last = strrchr(path, '/');
	if (last == NULL) {
		strncpy(path, "/", path_size - 1);
		path[path_size - 1] = '\0';
		return;
	}

	if (last == path) {
		path[1] = '\0';
	} else {
		*last = '\0';
	}
}

static void path_join(char *out, size_t out_size, const char *base, const char *name)
{
	size_t needed;
	size_t base_len;
	size_t name_len;

	if ((out == NULL) || (out_size == 0) || (base == NULL) || (name == NULL)) {
		return;
	}

	base_len = strlen(base);
	name_len = strlen(name);
	needed = base_len + name_len + 2;
	if (needed > out_size) {
		out[0] = '\0';
		return;
	}

	if (strcmp(base, "/") == 0) {
		out[0] = '/';
		memcpy(out + 1, name, name_len);
		out[1 + name_len] = '\0';
	} else {
		memcpy(out, base, base_len);
		out[base_len] = '/';
		memcpy(out + base_len + 1, name, name_len);
		out[base_len + 1 + name_len] = '\0';
	}
}

static int load_directory_entries(int sock, const char *path, BrowserEntry *entries, int max_entries)
{
	int did;
	int count;
	IO_DREAD_RESULT result;

	if ((path == NULL) || (entries == NULL) || (max_entries <= 0)) {
		return -1;
	}

	did = remote_dopen(sock, path);
	if (did < 0) {
		return did;
	}

	count = 0;
	while (1) {
		if (remote_dread(sock, did, &result) < 0) {
			(void)remote_dclose(sock, did);
			return -1;
		}

		if (result.res <= 0) {
			break;
		}

		if (count < max_entries) {
			snprintf(entries[count].name, sizeof(entries[count].name), "%s", result.entry.d_name);
			entries[count].is_dir =
				FIO_S_ISDIR(result.entry.d_stat.st_mode) || FIO_SO_ISDIR(result.entry.d_stat.st_attr);
			entries[count].size_lo = (unsigned int)(result.entry.d_stat.st_size & 0xFFFFFFFFU);
			count++;
		}
	}

	(void)remote_dclose(sock, did);
	return count;
}

static void draw_browser(const char *path, const BrowserEntry *entries, int count, int selected, int scroll)
{
	int i;

	pspDebugScreenSetXY(0, 0);
	printf("NetHostFS Browser\n");
	printf("Path: %s\n", path);
	printf("Up/Down Move  X Enter  O Up  TRI Reload  START Exit\n");
	printf("-----------------------------------------------------\n");

	for (i = 0; i < BROWSER_VISIBLE_LINES; i++) {
		int idx = scroll + i;
		if (idx >= count) {
			printf("\n");
			continue;
		}

		printf("%c %s %s", (idx == selected) ? '>' : ' ',
		       entries[idx].is_dir ? "[D]" : "[F]",
		       entries[idx].name);
		if (!entries[idx].is_dir) {
			printf(" (%u)", entries[idx].size_lo);
		}
		printf("\n");
	}
}

static int run_file_browser(const LauncherConfig *cfg)
{
	BrowserEntry entries[MAX_BROWSER_ENTRIES];
	BrowserSession session;
	char path[256];
	SceCtrlData pad;
	SceCtrlData prev_pad;
	int sock;
	int count;
	int selected;
	int scroll;
	int needs_reload;
	int needs_redraw;

	if (cfg == NULL) {
		return -1;
	}

	{
		int cs = connect_browser_session(cfg, &session);
		if (cs < 0) {
			printf("Browser session/connect failed: 0x%08X\n", cs);
			return cs;
		}
	}
	sock = session.socks[0];

	strncpy(path, "/", sizeof(path) - 1);
	path[sizeof(path) - 1] = '\0';

	memset(&prev_pad, 0, sizeof(prev_pad));
	sceCtrlSetSamplingCycle(0);
	sceCtrlSetSamplingMode(PSP_CTRL_MODE_DIGITAL);

	selected = 0;
	scroll = 0;
	needs_reload = 1;
	needs_redraw = 1;
	count = 0;

	while (1) {
		unsigned int pressed;

			if (needs_reload) {
				int rc = load_directory_entries(sock, path, entries, MAX_BROWSER_ENTRIES);
				if (rc < 0) {
					printf("Directory load failed for %s: 0x%08X\n", path, rc);
					close_browser_session(&session);
					return rc;
				}
			count = rc;
			if (selected >= count) {
				selected = (count > 0) ? (count - 1) : 0;
			}
			if (scroll > selected) {
				scroll = selected;
			}
				if (scroll < 0) {
					scroll = 0;
				}
				needs_reload = 0;
				needs_redraw = 1;
			}

			if (needs_redraw) {
				pspDebugScreenClear();
				draw_browser(path, entries, count, selected, scroll);
				needs_redraw = 0;
			}

		sceCtrlReadBufferPositive(&pad, 1);
		pressed = pad.Buttons & ~prev_pad.Buttons;
		prev_pad = pad;

		if (pressed & PSP_CTRL_START) {
			break;
		}

			if ((pressed & PSP_CTRL_UP) && (count > 0)) {
				if (selected > 0) {
					selected--;
					needs_redraw = 1;
				}
			}

			if ((pressed & PSP_CTRL_DOWN) && (count > 0)) {
				if (selected + 1 < count) {
					selected++;
					needs_redraw = 1;
				}
			}

			if (selected < scroll) {
				scroll = selected;
				needs_redraw = 1;
			}
			if (selected >= scroll + BROWSER_VISIBLE_LINES) {
				scroll = selected - BROWSER_VISIBLE_LINES + 1;
				needs_redraw = 1;
			}

		if (pressed & PSP_CTRL_TRIANGLE) {
			needs_reload = 1;
		}

			if (pressed & PSP_CTRL_CIRCLE) {
				path_go_up(path, sizeof(path));
				selected = 0;
				scroll = 0;
				needs_reload = 1;
				needs_redraw = 1;
			}

		if ((pressed & PSP_CTRL_CROSS) && (count > 0)) {
			if (entries[selected].is_dir) {
				if (strcmp(entries[selected].name, ".") == 0) {
					needs_reload = 1;
				} else if (strcmp(entries[selected].name, "..") == 0) {
					path_go_up(path, sizeof(path));
					selected = 0;
					scroll = 0;
					needs_reload = 1;
					} else {
						char next_path[256];
						path_join(next_path, sizeof(next_path), path, entries[selected].name);
						if (next_path[0] != '\0') {
							strncpy(path, next_path, sizeof(path) - 1);
							path[sizeof(path) - 1] = '\0';
							selected = 0;
							scroll = 0;
							needs_reload = 1;
						}
					}
				}
			}

		sceDisplayWaitVblankStart();
	}

	close_browser_session(&session);
	return 0;
}

int main(int argc, char **argv)
{
	LauncherConfig cfg;
	int profile_id;
	int err;

	setup_callbacks();
	pspDebugScreenInit();

	config_init(&cfg, (argc > 0) ? argv[0] : NULL);
	parse_args(&cfg, argc, argv);

	printf("%s\n", LAUNCHER_NAME);
	printf("Host: %s\n", cfg.host);
	printf("Port: %d\n", cfg.port);
	printf("Profile: %d\n", cfg.profile);
	printf("PRX loading disabled: direct network browser mode.\n");

	profile_id = resolve_profile_id(cfg.profile);
	if (profile_id < 0) {
		printf("No valid WLAN profiles configured on this PSP.\n");
		sceKernelSleepThread();
		return 0;
	}

#ifdef ENABLE_WLAN_SAMPLE_INIT
	if (!wlan_sample_precheck()) {
		sceKernelSleepThread();
		return 0;
	}
#endif

	load_network_modules();
	err = init_network_stack();
	if (err != 0) {
		printf("Error: could not initialise the network 0x%08X\n", err);
		sceKernelSleepThread();
		return 0;
	}

	if (!connect_to_apctl(profile_id)) {
		printf("WLAN connect failed for profile %d\n", profile_id);
		sceKernelSleepThread();
		return 0;
	}
	{
		union SceNetApctlInfo info;
		if (sceNetApctlGetInfo(PSP_NET_APCTL_INFO_IP, &info) == 0) {
			printf("WLAN IP: %s\n", info.ip);
		}
	}

	printf("Opening browser...\n");
	sceKernelDelayThread(600 * 1000);
	err = run_file_browser(&cfg);
	if (err != 0) {
		printf("Browser exited with error 0x%08X\n", err);
	}

	printf("Press HOME to exit launcher.\n");
	sceKernelSleepThread();
	return 0;
}
