# PSP client (recreated)

This directory contains a recreated PSP-side `nethostfs` client PRX that talks to the host daemon in the repo root.

## What it does

- Opens 4 TCP sessions to the PC host (`port`, `port+1`, `port+2`, `port+3`)
- Performs `HELLO` + `IOINIT` handshake (including password challenge/response)
- Registers PSP I/O driver `host` (usable as `host0:/...`)
- Implements file and directory operations using the protocol in [`nethostfs.h`](../nethostfs.h)

## Build

```bash
cd psp_client
make
```

Output PRX: `nethostfs_psp.prx`

## Runtime configuration

Configuration is passed via module start arguments:

- `-h <ip>` or `host=<ip>` / `ip=<ip>`
- `-p <port>` or `port=<port>`
- `-l <password>` or `password=<password>` / `pass=<password>`

Defaults when omitted:

- `host=192.168.0.2`
- `port=7513`
- `password=` (empty)

## Notes

- `ioctl` and unsupported devctl commands return `-1`.
- `DEVCTL_GET_INFO` is implemented.
- If sockets drop, the client closes all sessions and reconnects lazily on the next operation.
- On module start, client now retries initial server connect for ~4 seconds and fails start if it cannot connect.
- This PRX only initializes `sceNet`/`sceNetInet`; ensure WLAN profile connection is already active before using `host0:`.
- Use [`../psp_launcher`](../psp_launcher) if you want an auto-connect launcher that brings up WLAN and starts this PRX with args.
