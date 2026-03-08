#include <pspkernel.h>
#include <pspiofilemgr_kernel.h>
#include <pspnet.h>
#include <pspnet_inet.h>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <stdint.h>
#include <string.h>

#include "../nethostfs.h"

PSP_MODULE_INFO("NetHostFS", PSP_MODULE_KERNEL, 1, 0);
#define printf Kprintf

#define NETHOSTFS_DRIVER_NAME      "host"
#define NETHOSTFS_DRIVER_NAME2     "HOST"
#define NETHOSTFS_SESSIONS         4
#define NETHOSTFS_PASSWORD_LEN     8
#define NETHOSTFS_DEFAULT_HOST     "192.168.0.2"
#define NETHOSTFS_DEFAULT_PORT     7513
#define NETHOSTFS_RW_CHUNK         (64 * 1024)

typedef struct NetHostFsConfig {
	char host[32];
	int port;
	char password[NETHOSTFS_PASSWORD_LEN + 1];
} NetHostFsConfig;

static NetHostFsConfig g_cfg = {
	NETHOSTFS_DEFAULT_HOST,
	NETHOSTFS_DEFAULT_PORT,
	""
};

static int g_socks[NETHOSTFS_SESSIONS] = { -1, -1, -1, -1 };
static int g_connected = 0;
static int g_next_socket = 0;
static int g_net_tried_init = 0;
static SceUID g_io_sema = -1;
static int g_last_connect_error = -1;

static int is_space_char(char ch)
{
	return (ch == ' ') || (ch == '\t') || (ch == '\r') || (ch == '\n');
}

static void set_host_value(const char *value)
{
	if ((value == NULL) || (value[0] == '\0')) {
		return;
	}

	strncpy(g_cfg.host, value, sizeof(g_cfg.host) - 1);
	g_cfg.host[sizeof(g_cfg.host) - 1] = '\0';
}

static void set_port_value(const char *value)
{
	int port;
	char c;

	if ((value == NULL) || (value[0] == '\0')) {
		return;
	}

	port = 0;
	while ((c = *value++) != '\0') {
		if ((c < '0') || (c > '9')) {
			return;
		}
		port = (port * 10) + (c - '0');
		if (port > 65535) {
			return;
		}
	}

	if ((port >= 1) && (port <= 65535)) {
		g_cfg.port = port;
	}
}

static void set_password_value(const char *value)
{
	if (value == NULL) {
		return;
	}

	strncpy(g_cfg.password, value, NETHOSTFS_PASSWORD_LEN);
	g_cfg.password[NETHOSTFS_PASSWORD_LEN] = '\0';
}

static char *next_arg_token(char **cursor)
{
	char *start;
	char *p;

	if ((cursor == NULL) || (*cursor == NULL)) {
		return NULL;
	}

	p = *cursor;
	while ((*p != '\0') && is_space_char(*p)) {
		p++;
	}

	if (*p == '\0') {
		*cursor = p;
		return NULL;
	}

	start = p;
	while ((*p != '\0') && !is_space_char(*p)) {
		p++;
	}

	if (*p != '\0') {
		*p = '\0';
		p++;
	}

	*cursor = p;
	return start;
}

static void parse_module_args(SceSize args, void *argp)
{
	char buf[256];
	int copy_len;
	char *cursor;
	char *tok;

	if ((argp == NULL) || (args == 0)) {
		return;
	}

	copy_len = (int)args;
	if (copy_len > (int)(sizeof(buf) - 1)) {
		copy_len = (int)(sizeof(buf) - 1);
	}

	memcpy(buf, argp, copy_len);
	buf[copy_len] = '\0';

	cursor = buf;
	while ((tok = next_arg_token(&cursor)) != NULL) {
		if ((strcmp(tok, "-h") == 0) || (strcmp(tok, "--host") == 0)) {
			char *next = next_arg_token(&cursor);
			if (next != NULL) {
				set_host_value(next);
			}
		} else if ((strcmp(tok, "-p") == 0) || (strcmp(tok, "--port") == 0)) {
			char *next = next_arg_token(&cursor);
			if (next != NULL) {
				set_port_value(next);
			}
		} else if ((strcmp(tok, "-l") == 0) || (strcmp(tok, "--password") == 0)) {
			char *next = next_arg_token(&cursor);
			if (next != NULL) {
				set_password_value(next);
			}
		} else if (strncmp(tok, "host=", 5) == 0) {
			set_host_value(tok + 5);
		} else if (strncmp(tok, "ip=", 3) == 0) {
			set_host_value(tok + 3);
		} else if (strncmp(tok, "port=", 5) == 0) {
			set_port_value(tok + 5);
		} else if (strncmp(tok, "password=", 9) == 0) {
			set_password_value(tok + 9);
		} else if (strncmp(tok, "pass=", 5) == 0) {
			set_password_value(tok + 5);
		}
	}
}

static int copy_path(char dst[256], const char *src)
{
	if ((dst == NULL) || (src == NULL)) {
		return -1;
	}

	if (strlen(src) >= 256) {
		return -1;
	}

	strcpy(dst, src);
	return 0;
}

static int send_all(int sock, const void *buf, int len)
{
	const unsigned char *p;
	int sent;

	p = (const unsigned char *)buf;
	sent = 0;

	while (sent < len) {
		int rc = (int)sceNetInetSend(sock, p + sent, (size_t)(len - sent), 0);
		if (rc <= 0) {
			return -1;
		}
		sent += rc;
	}

	return 0;
}

static int recv_all(int sock, void *buf, int len)
{
	unsigned char *p;
	int received;

	p = (unsigned char *)buf;
	received = 0;

	while (received < len) {
		int rc = (int)sceNetInetRecv(sock, p + received, (size_t)(len - received), 0);
		if (rc <= 0) {
			return -1;
		}
		received += rc;
	}

	return 0;
}

static int send_int32(int sock, int value)
{
	return send_all(sock, &value, sizeof(value));
}

static int recv_int32(int sock, int *value)
{
	return recv_all(sock, value, sizeof(*value));
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

static void close_sockets_locked(void)
{
	int i;

	for (i = 0; i < NETHOSTFS_SESSIONS; i++) {
		if (g_socks[i] >= 0) {
			sceNetInetClose(g_socks[i]);
			g_socks[i] = -1;
		}
	}

	g_connected = 0;
	g_next_socket = 0;
}

static void ensure_network_stack(void)
{
	if (g_net_tried_init) {
		return;
	}

	g_net_tried_init = 1;
	(void)sceNetInit(128 * 1024, 42, 4 * 1024, 42, 4 * 1024);
	(void)sceNetInetInit();
}

static int connect_one_socket_locked(int idx)
{
	int sock;
	struct sockaddr_in addr;
	unsigned int ip_addr;

	if ((idx < 0) || (idx >= NETHOSTFS_SESSIONS)) {
		return -1;
	}

	ip_addr = inet_addr(g_cfg.host);
	if (ip_addr == 0xFFFFFFFFU) {
		printf("nethostfs_psp: invalid host IP '%s'\n", g_cfg.host);
		g_last_connect_error = 0x8F010001;
		return -1;
	}

	sock = sceNetInetSocket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) {
		printf("nethostfs_psp: socket() failed on channel %d\n", idx);
		g_last_connect_error = 0x8F010100 | (idx & 0xFF);
		return -1;
	}

	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = ip_addr;
	addr.sin_port = htons((u16)(g_cfg.port + idx));

	printf("nethostfs_psp: connect[%d] -> %s:%d\n", idx, g_cfg.host, g_cfg.port + idx);

	if (sceNetInetConnect(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
		int eno = (int)sceNetInetGetErrno();
		printf("nethostfs_psp: connect[%d] failed errno=%d\n", idx, eno);
		g_last_connect_error = 0x8F020000 | ((idx & 0xFF) << 8) | (eno & 0xFF);
		sceNetInetClose(sock);
		return -1;
	}

	printf("nethostfs_psp: connect[%d] ok\n", idx);
	g_socks[idx] = sock;
	return 0;
}

static int handshake_socket_locked(int sock)
{
	int res;
	IO_LOGIN_CHALLENGE_PARAMS challenge;
	IO_LOGIN_RESPONSE_PARAMS response;

	if (send_int32(sock, NET_HOSTFS_CMD_HELLO) < 0) {
		printf("nethostfs_psp: HELLO send failed\n");
		g_last_connect_error = 0x8F030001;
		return -1;
	}

	if (recv_int32(sock, &res) < 0) {
		printf("nethostfs_psp: HELLO recv failed\n");
		g_last_connect_error = 0x8F030002;
		return -1;
	}

	if (res != NET_HOSTFS_CMD_HELLO) {
		printf("nethostfs_psp: HELLO mismatch 0x%08X\n", res);
		g_last_connect_error = 0x8F030003;
		return -1;
	}

	if (send_int32(sock, NET_HOSTFS_CMD_IOINIT) < 0) {
		printf("nethostfs_psp: IOINIT send failed\n");
		g_last_connect_error = 0x8F030004;
		return -1;
	}

	if (recv_all(sock, &challenge, sizeof(challenge)) < 0) {
		printf("nethostfs_psp: IOINIT challenge recv failed\n");
		g_last_connect_error = 0x8F030005;
		return -1;
	}

	memset(&response, 0, sizeof(response));
	if (g_cfg.password[0] != '\0') {
		memcpy(response.response, challenge.challenge, CHALLENGE_TEXT_LEN);
		encrypt_challenge(response.response, CHALLENGE_TEXT_LEN, (const unsigned char *)g_cfg.password);
	}

	if (send_all(sock, &response, sizeof(response)) < 0) {
		printf("nethostfs_psp: IOINIT response send failed\n");
		g_last_connect_error = 0x8F030006;
		return -1;
	}

	if (recv_int32(sock, &res) < 0) {
		printf("nethostfs_psp: IOINIT result recv failed\n");
		g_last_connect_error = 0x8F030007;
		return -1;
	}

	if (res < 0) {
		printf("nethostfs_psp: IOINIT rejected 0x%08X\n", res);
		g_last_connect_error = 0x8F030008;
		return -1;
	}

	return 0;
}

static int connect_locked(void)
{
	int i;

	if (g_connected) {
		return 0;
	}

	ensure_network_stack();
	g_last_connect_error = -1;

	for (i = 0; i < NETHOSTFS_SESSIONS; i++) {
		if (connect_one_socket_locked(i) < 0) {
			close_sockets_locked();
			return (g_last_connect_error < 0) ? g_last_connect_error : -1;
		}
	}

	for (i = 0; i < NETHOSTFS_SESSIONS; i++) {
		if (handshake_socket_locked(g_socks[i]) < 0) {
			close_sockets_locked();
			return (g_last_connect_error < 0) ? g_last_connect_error : -1;
		}
	}

	g_connected = 1;
	g_next_socket = 0;
	return 0;
}

static int begin_command(int *sock)
{
	int rc;

	if (g_io_sema < 0) {
		return -1;
	}

	rc = sceKernelWaitSema(g_io_sema, 1, NULL);
	if (rc < 0) {
		return -1;
	}

	if (!g_connected) {
		if (connect_locked() < 0) {
			sceKernelSignalSema(g_io_sema, 1);
			return -1;
		}
	}

	*sock = g_socks[g_next_socket];
	g_next_socket = (g_next_socket + 1) % NETHOSTFS_SESSIONS;

	if (*sock < 0) {
		close_sockets_locked();
		sceKernelSignalSema(g_io_sema, 1);
		return -1;
	}

	return 0;
}

static void end_command(void)
{
	if (g_io_sema >= 0) {
		sceKernelSignalSema(g_io_sema, 1);
	}
}

static int exchange_fixed(int cmd, const void *params, int params_len, void *response, int response_len)
{
	int sock;
	int ok;

	if (begin_command(&sock) < 0) {
		return -1;
	}

	ok = 1;

	if (send_int32(sock, cmd) < 0) {
		ok = 0;
	}

	if (ok && (params != NULL) && (params_len > 0) && (send_all(sock, params, params_len) < 0)) {
		ok = 0;
	}

	if (ok && (response != NULL) && (response_len > 0) && (recv_all(sock, response, response_len) < 0)) {
		ok = 0;
	}

	if (!ok) {
		close_sockets_locked();
	}

	end_command();
	return ok ? 0 : -1;
}

static int remote_open(u32 fs_num, const char *file, int flags, SceMode mode)
{
	IO_OPEN_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.file, file) < 0) {
		return -1;
	}
	params.fs_num = fs_num;
	params.flags = flags;
	params.mode = mode;

	if (exchange_fixed(NET_HOSTFS_CMD_IOOPEN, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_close(int fd)
{
	int res = -1;

	if (exchange_fixed(NET_HOSTFS_CMD_IOCLOSE, &fd, sizeof(fd), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_read_once(int fd, void *buf, int len)
{
	IO_READ_PARAMS params;
	int sock;
	int res = -1;
	int ok;

	params.fd = fd;
	params.len = len;

	if (begin_command(&sock) < 0) {
		return -1;
	}

	ok = 1;

	if (send_int32(sock, NET_HOSTFS_CMD_IOREAD) < 0) {
		ok = 0;
	}
	if (ok && (send_all(sock, &params, sizeof(params)) < 0)) {
		ok = 0;
	}
	if (ok && (recv_int32(sock, &res) < 0)) {
		ok = 0;
	}

	if (ok && (res > 0)) {
		if (res > len) {
			ok = 0;
		} else if (recv_all(sock, buf, res) < 0) {
			ok = 0;
		}
	}

	if (!ok) {
		res = -1;
		close_sockets_locked();
	}

	end_command();
	return res;
}

static int remote_write_once(int fd, const void *buf, int len)
{
	IO_WRITE_PARAMS params;
	int sock;
	int res = -1;
	int ok;

	params.fd = fd;
	params.len = len;

	if (begin_command(&sock) < 0) {
		return -1;
	}

	ok = 1;

	if (send_int32(sock, NET_HOSTFS_CMD_IOWRITE) < 0) {
		ok = 0;
	}
	if (ok && (send_all(sock, &params, sizeof(params)) < 0)) {
		ok = 0;
	}
	if (ok && (len > 0) && (send_all(sock, buf, len) < 0)) {
		ok = 0;
	}
	if (ok && (recv_int32(sock, &res) < 0)) {
		ok = 0;
	}

	if (!ok) {
		res = -1;
		close_sockets_locked();
	}

	end_command();
	return res;
}

static int remote_lseek(int fd, SceOff offset, int whence)
{
	IO_LSEEK_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	params.fd = fd;
	params.offset = offset;
	params.whence = whence;

	if (exchange_fixed(NET_HOSTFS_CMD_IOLSEEK, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_remove(u32 fs_num, const char *path)
{
	IO_REMOVE_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.file, path) < 0) {
		return -1;
	}
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IOREMOVE, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_mkdir(u32 fs_num, const char *path, SceMode mode)
{
	IO_MKDIR_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.dir, path) < 0) {
		return -1;
	}
	params.fs_num = fs_num;
	params.mode = mode;

	if (exchange_fixed(NET_HOSTFS_CMD_IOMKDIR, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_rmdir(u32 fs_num, const char *path)
{
	IO_RMDIR_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.dir, path) < 0) {
		return -1;
	}
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IORMDIR, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_dopen(u32 fs_num, const char *dir)
{
	IO_DOPEN_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.dir, dir) < 0) {
		return -1;
	}
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IODOPEN, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_dclose(int did)
{
	int res = -1;

	if (exchange_fixed(NET_HOSTFS_CMD_IODCLOSE, &did, sizeof(did), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_dread(int did, SceIoDirent *dirent)
{
	IO_DREAD_RESULT result;

	memset(&result, 0, sizeof(result));

	if (exchange_fixed(NET_HOSTFS_CMD_IODREAD, &did, sizeof(did), &result, sizeof(result)) < 0) {
		return -1;
	}

	if ((result.res > 0) && (dirent != NULL)) {
		memcpy(dirent, &result.entry, sizeof(*dirent));
	}

	return result.res;
}

static int remote_getstat(u32 fs_num, const char *path, SceIoStat *stat)
{
	IO_GETSTAT_PARAMS params;
	IO_GETSTAT_RESULT result;

	memset(&params, 0, sizeof(params));
	memset(&result, 0, sizeof(result));

	if (copy_path(params.file, path) < 0) {
		return -1;
	}
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IOGETSTAT, &params, sizeof(params), &result, sizeof(result)) < 0) {
		return -1;
	}

	if ((result.res == 0) && (stat != NULL)) {
		memcpy(stat, &result.stat, sizeof(*stat));
	}

	return result.res;
}

static int remote_chstat(u32 fs_num, const char *path, const SceIoStat *stat, int bits)
{
	IO_CHSTAT_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if (copy_path(params.file, path) < 0) {
		return -1;
	}
	params.fs_num = fs_num;
	params.bits = bits;
	if (stat != NULL) {
		memcpy(&params.stat, stat, sizeof(params.stat));
	}

	if (exchange_fixed(NET_HOSTFS_CMD_IOCHSTAT, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_rename(u32 fs_num, const char *oldname, const char *newname)
{
	IO_RENAME_PARAMS params;
	int res = -1;

	memset(&params, 0, sizeof(params));
	if ((copy_path(params.oldfile, oldname) < 0) || (copy_path(params.newfile, newname) < 0)) {
		return -1;
	}
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IORENAME, &params, sizeof(params), &res, sizeof(res)) < 0) {
		return -1;
	}

	return res;
}

static int remote_devctl(u32 fs_num, u32 subcmd, IO_DEVCTL_RESULT *result)
{
	IO_DEVCTL_PARAMS params;

	memset(&params, 0, sizeof(params));
	params.subcmd = subcmd;
	params.fs_num = fs_num;

	if (exchange_fixed(NET_HOSTFS_CMD_IODEVCTL, &params, sizeof(params), result, sizeof(*result)) < 0) {
		return -1;
	}

	return 0;
}

static int io_init(PspIoDrvArg *arg)
{
	(void)arg;
	return 0;
}

static int io_exit(PspIoDrvArg *arg)
{
	(void)arg;
	return 0;
}

static int io_open(PspIoDrvFileArg *arg, char *file, int flags, SceMode mode)
{
	int fd;

	fd = remote_open(arg->fs_num, file, flags, mode);
	if (fd < 0) {
		return fd;
	}

	arg->arg = (void *)(intptr_t)fd;
	return 0;
}

static int io_close(PspIoDrvFileArg *arg)
{
	return remote_close((int)(intptr_t)arg->arg);
}

static int io_read(PspIoDrvFileArg *arg, char *data, int len)
{
	int total;
	int ret;

	if ((data == NULL) || (len < 0)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	total = 0;
	while (total < len) {
		int chunk = len - total;
		if (chunk > NETHOSTFS_RW_CHUNK) {
			chunk = NETHOSTFS_RW_CHUNK;
		}

		ret = remote_read_once((int)(intptr_t)arg->arg, data + total, chunk);
		if (ret < 0) {
			if (total == 0) {
				return ret;
			}
			break;
		}
		if (ret == 0) {
			break;
		}

		total += ret;
		if (ret < chunk) {
			break;
		}
	}

	return total;
}

static int io_write(PspIoDrvFileArg *arg, const char *data, int len)
{
	int total;
	int ret;

	if ((data == NULL) || (len < 0)) {
		return -1;
	}
	if (len == 0) {
		return 0;
	}

	total = 0;
	while (total < len) {
		int chunk = len - total;
		if (chunk > NETHOSTFS_RW_CHUNK) {
			chunk = NETHOSTFS_RW_CHUNK;
		}

		ret = remote_write_once((int)(intptr_t)arg->arg, data + total, chunk);
		if (ret < 0) {
			if (total == 0) {
				return ret;
			}
			break;
		}
		if (ret == 0) {
			break;
		}

		total += ret;
		if (ret < chunk) {
			break;
		}
	}

	return total;
}

static SceOff io_lseek(PspIoDrvFileArg *arg, SceOff ofs, int whence)
{
	int res;

	res = remote_lseek((int)(intptr_t)arg->arg, ofs, whence);
	if (res < 0) {
		return (SceOff)-1;
	}

	return (SceOff)res;
}

static int io_ioctl(PspIoDrvFileArg *arg, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	(void)arg;
	(void)cmd;
	(void)indata;
	(void)inlen;
	(void)outdata;
	(void)outlen;
	return -1;
}

static int io_remove(PspIoDrvFileArg *arg, const char *name)
{
	return remote_remove(arg->fs_num, name);
}

static int io_mkdir(PspIoDrvFileArg *arg, const char *name, SceMode mode)
{
	return remote_mkdir(arg->fs_num, name, mode);
}

static int io_rmdir(PspIoDrvFileArg *arg, const char *name)
{
	return remote_rmdir(arg->fs_num, name);
}

static int io_dopen(PspIoDrvFileArg *arg, const char *dirname)
{
	int did;

	did = remote_dopen(arg->fs_num, dirname);
	if (did < 0) {
		return did;
	}

	arg->arg = (void *)(intptr_t)did;
	return 0;
}

static int io_dclose(PspIoDrvFileArg *arg)
{
	return remote_dclose((int)(intptr_t)arg->arg);
}

static int io_dread(PspIoDrvFileArg *arg, SceIoDirent *dir)
{
	return remote_dread((int)(intptr_t)arg->arg, dir);
}

static int io_getstat(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat)
{
	return remote_getstat(arg->fs_num, file, stat);
}

static int io_chstat(PspIoDrvFileArg *arg, const char *file, SceIoStat *stat, int bits)
{
	return remote_chstat(arg->fs_num, file, stat, bits);
}

static int io_rename(PspIoDrvFileArg *arg, const char *oldname, const char *newname)
{
	return remote_rename(arg->fs_num, oldname, newname);
}

static int io_chdir(PspIoDrvFileArg *arg, const char *dir)
{
	(void)arg;
	(void)dir;
	return -1;
}

static int io_mount(PspIoDrvFileArg *arg)
{
	(void)arg;
	return -1;
}

static int io_umount(PspIoDrvFileArg *arg)
{
	(void)arg;
	return -1;
}

static int io_devctl(PspIoDrvFileArg *arg, const char *name, unsigned int cmd, void *indata, int inlen, void *outdata, int outlen)
{
	IO_DEVCTL_RESULT result;
	struct DevctlGetInfo *info = NULL;

	(void)name;
	(void)inlen;
	(void)outlen;

	if (cmd != DEVCTL_GET_INFO) {
		return -1;
	}

	if (outdata != NULL) {
		info = (struct DevctlGetInfo *)outdata;
	} else if ((indata != NULL) && (inlen >= (int)sizeof(void *))) {
		info = *(struct DevctlGetInfo **)indata;
	}

	if (info == NULL) {
		return -1;
	}

	if (remote_devctl(arg->fs_num, cmd, &result) < 0) {
		return -1;
	}

	if (result.res < 0) {
		return result.res;
	}

	memcpy(info, &result.getinfo, sizeof(*info));
	return 0;
}

static int io_unknown(PspIoDrvFileArg *arg)
{
	(void)arg;
	return -1;
}

static PspIoDrvFuncs g_host_funcs = {
	io_init,
	io_exit,
	io_open,
	io_close,
	io_read,
	io_write,
	io_lseek,
	io_ioctl,
	io_remove,
	io_mkdir,
	io_rmdir,
	io_dopen,
	io_dclose,
	io_dread,
	io_getstat,
	io_chstat,
	io_rename,
	io_chdir,
	io_mount,
	io_umount,
	io_devctl,
	io_unknown,
};

static PspIoDrv g_host_driver = {
	NETHOSTFS_DRIVER_NAME,
	0x10,
	0x800,
	NETHOSTFS_DRIVER_NAME2,
	&g_host_funcs
};

int module_start(SceSize args, void *argp)
{
	int rc;
	int tries;

	(void)sceIoDelDrv(NETHOSTFS_DRIVER_NAME);

	parse_module_args(args, argp);
	printf("nethostfs_psp: start host=%s port=%d pass=%s\n",
	       g_cfg.host,
	       g_cfg.port,
	       (g_cfg.password[0] != '\0') ? "set" : "empty");

	g_io_sema = sceKernelCreateSema("nethostfs_io", 0, 1, 1, NULL);
	if (g_io_sema < 0) {
		return g_io_sema;
	}

	if (sceIoAddDrv(&g_host_driver) < 0) {
		sceKernelDeleteSema(g_io_sema);
		g_io_sema = -1;
		return -1;
	}

	rc = -1;
	if (sceKernelWaitSema(g_io_sema, 1, NULL) >= 0) {
		for (tries = 0; tries < 40; tries++) {
			rc = connect_locked();
			if (rc == 0) {
				rc = 0;
				printf("nethostfs_psp: initial connect ok\n");
				break;
			}
			sceKernelDelayThread(100 * 1000);
		}
		sceKernelSignalSema(g_io_sema, 1);
	}

	if (rc < 0) {
		printf("nethostfs_psp: initial connect failed\n");
		(void)sceIoDelDrv(NETHOSTFS_DRIVER_NAME);
		sceKernelDeleteSema(g_io_sema);
		g_io_sema = -1;
		return -1;
	}

	return 0;
}

int module_stop(SceSize args, void *argp)
{
	(void)args;
	(void)argp;

	if (g_io_sema >= 0) {
		if (sceKernelWaitSema(g_io_sema, 1, NULL) >= 0) {
			close_sockets_locked();
			sceKernelSignalSema(g_io_sema, 1);
		}
		sceKernelDeleteSema(g_io_sema);
		g_io_sema = -1;
	}

	(void)sceIoDelDrv(NETHOSTFS_DRIVER_NAME);
	return 0;
}
