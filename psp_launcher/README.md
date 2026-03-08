# NetHostFS user-mode launcher (network + browser mode)

Small user-mode launcher that:

1. Loads PSP network modules
2. Initializes networking
3. Connects a WLAN profile automatically via `sceNetApctlConnect`
4. Verifies NetHostFS server on `port..port+3`
5. Opens a text-mode remote file browser on `host:port`

## Build

```bash
cd psp_launcher
make
```

Outputs include `EBOOT.PBP`.

Optional WLAN sample-style precheck build:

```bash
cd psp_launcher
make WLAN_PROBE=1
```

Optional disable for CFW kernel bridge loader:

```bash
cd psp_launcher
make KERNEL_BRIDGE=0
```

## Usage

Copy `EBOOT.PBP` to your PSP game folder.

Arguments:

- `-h <host_ip>`: host IP (default `192.168.1.192`)
- `-p <port>`: base port (default `7513`)
- `-l <password>`: optional NetHostFS login password (used for browser handshake)
- `-c <profile>`: WLAN profile index (default `1`)
- `-m <path>`: accepted for compatibility (not used in this mode)

Example:

```bash
-h 192.168.1.50 -p 7513 -l secret -c 1
```

## Notes

- This mode does not load any PRX.
- It attempts TCP connections to `<host>:<port>`, `<port+1>`, `<port+2>`, `<port+3>`.
- On each port, it sends `NET_HOSTFS_CMD_HELLO` and expects `NET_HOSTFS_CMD_HELLO` back.
- Browser controls:
- `Up/Down`: move selection
- `Cross`: enter selected directory
- `Circle`: go up one directory
- `Triangle`: refresh current directory
- `Start`: exit browser
- If profile auto-connect fails, launcher stops before PRX start and prints the connect error.
- If the selected profile is invalid (`0x80110601`), launcher auto-falls back to the first configured profile.
- `WLAN_PROBE=1` enables `sceWlan*` checks (switch/power/MAC) like the PSPSDK sample.
- If your loader environment does not expose `sceWlanDrv`, `WLAN_PROBE=1` can fail to load with `0x8002013C`.
