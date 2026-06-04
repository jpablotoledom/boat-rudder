# base-http-server - Build & Operations Scripts

This document covers `bhs.sh`, the central management script for `base-http-server`, and every sub-script it delegates to.

---

## Overview

`bhs.sh` is the single entry point for all build, run, and deployment operations. It accepts one or more **actions** as positional arguments and executes them in the order they are given.

```
./bhs.sh <action1> [action2] [action3] ...
```

Actions are independent and sequential. If an action fails, the script stops immediately and does not proceed to the next action.

---

## Quick Reference

| Action | Description | Requires sudo |
|---|---|---|
| `compiledebug` | Build with debug symbols + AddressSanitizer | No |
| `compileprod` | Build optimized for production | No |
| `rundebug` | Run locally (auto-selects GDB / LLDB / direct) | Auto (if port < 1024) |
| `createcert` | Generate a self-signed TLS certificate | No |
| `install` | Compile prod + install as systemd service | Yes |
| `uninstall` | Stop and remove the systemd service | Yes |

---

## Actions

### `compiledebug`

Compiles the project in **Debug** mode with AddressSanitizer (`-fsanitize=address`) enabled.

**What it does:**
1. Deletes `build/` and `bin/` directories.
2. Runs `cmake -DCMAKE_BUILD_TYPE=Debug` and `cmake --build`.
3. Assembles a self-contained `bin/` directory:
   - `bin/base-http-server` - the compiled binary
   - `bin/configs/` - copy of `configs/`
   - `bin/www/` - copy of `www/` (if it exists)
   - `bin/ssl/` - copy of `ssl/` (only if it contains `.pem` files)
4. Deletes `build/` after assembly.

**Delegates to:** `scripts/compile_debug.sh`

**Output:** `bin/base-http-server` (with ASan instrumentation)

**Typical use:**
```bash
./bhs.sh compiledebug
./bhs.sh compiledebug rundebug
```

---

### `compileprod`

Compiles the project in **Release** mode, fully optimized for production.

**What it does:**
1. Deletes `build/` and `bin/` directories.
2. Runs `cmake -DCMAKE_BUILD_TYPE=Release` and `cmake --build`.
3. Strips debug symbols from the binary with `strip` (if available).
4. Assembles `bin/` the same way as `compiledebug`.
5. Deletes `build/` after assembly.

**Delegates to:** `scripts/compile_prod.sh`

**Output:** `bin/base-http-server` (optimized, stripped)

**Typical use:**
```bash
./bhs.sh compileprod
./bhs.sh compileprod install
```

---

### `rundebug`

Runs the debug binary locally. Automatically syncs `configs/` and `ssl/` from the project root to `bin/` before starting, so configuration changes take effect without recompiling.

**What it does:**
1. Verifies `bin/base-http-server` exists.
2. Creates `bin/www/` if it does not exist.
3. Copies `configs/config.txt` → `bin/configs/config.txt`.
4. Copies `ssl/*.pem` → `bin/ssl/` (if `ssl/` contains files).
5. Sends `SIGTERM` to any running instance of `base-http-server`.
6. Reads `http_port`, `https_port`, and `ssl_enabled` from the config.
7. On **Linux**: re-executes itself with `sudo` if any configured port is < 1024 and the current user is not root.
8. Selects a debugger based on OS and availability:

| Platform | Debugger found | Behavior |
|---|---|---|
| Linux | GDB available | Runs under GDB with SIGPIPE suppressed |
| macOS | LLDB available | Runs under LLDB |
| Any | No debugger | Runs binary directly (ASan still active) |

**Environment variable set:** `ASAN_OPTIONS=check_printf=0`

**Binary arguments passed:**
```
base-http-server -c ./bin/configs/config.txt ./bin/www
```

**Delegates to:** `scripts/run_debug.sh`

**Typical use:**
```bash
./bhs.sh rundebug
./bhs.sh compiledebug rundebug
```

---

### `createcert`

Generates a **self-signed RSA 4096-bit TLS certificate** for local development. Requires `openssl` to be installed.

**What it does:**
1. Creates `ssl/` if it does not exist.
2. Generates `ssl/key.pem` (private key) and `ssl/cert.pem` (certificate).
3. The certificate is valid for **730 days (2 years)** and includes:
   - `CN=localhost`
   - `SAN: DNS:localhost`, `DNS:127.0.0.1`, `IP:127.0.0.1`
4. If `bin/` exists, copies both files to `bin/ssl/` immediately so `rundebug` picks them up without recompiling.
5. Prints the certificate details (validity dates, subject, SANs).

**Delegates to:** `scripts/create_local_cert.sh`

**Output files:**
```
ssl/cert.pem    - X.509 certificate (PEM)
ssl/key.pem     - RSA private key (PEM, unencrypted)
bin/ssl/cert.pem
bin/ssl/key.pem
```

> **Note:** After generating the certificate, make sure `ssl_enabled=1` is set in `configs/config.txt`.

**Typical use:**
```bash
./bhs.sh createcert
./bhs.sh createcert compiledebug rundebug
```

---

### `install`

Compiles the project for production and installs it as a **systemd service**.

> **Linux only.** Running this action on macOS exits with an error and suggests using `rundebug` instead.

> **Requires `sudo`.** `bhs.sh` calls `sudo ./scripts/install.sh` automatically.

**What it does:**
1. Calls `compile_prod.sh` to produce a fresh release binary.
2. Creates `/usr/local/bin/base-http-server/` and copies into it:
   - `base-http-server` (binary, marked executable)
   - `configs/`
   - `www/` (creates an empty one with a warning if absent)
   - `ssl/` (only if it contains `.pem` files)
3. Copies `scripts/base-http-server.service` to `/etc/systemd/system/`.
4. Runs `systemctl daemon-reload`, `systemctl enable`, `systemctl start`.
5. Prints the service status and the log tail command.

**Install path:** `/usr/local/bin/base-http-server/`

**Service name:** `base-http-server`

**Delegates to:** `scripts/install.sh`

**After install:**
```bash
# Check status
systemctl status base-http-server

# Follow logs
journalctl -u base-http-server -f

# Restart after config changes
systemctl restart base-http-server
```

**Typical use:**
```bash
./bhs.sh install
./bhs.sh compileprod install   # same effect - install always recompiles
```

---

### `uninstall`

Stops and completely removes the systemd service and all installed files.

> **Linux only.** Exits with an error on macOS.

> **Requires `sudo`.** `bhs.sh` calls `sudo ./scripts/uninstall.sh` automatically.

**What it does:**
1. Stops the service with `systemctl stop`.
2. Disables the service with `systemctl disable`.
3. Removes `/etc/systemd/system/base-http-server.service`.
4. Runs `systemctl daemon-reload`.
5. Removes `/usr/local/bin/base-http-server/` recursively.
6. Lists any remaining units matching `base-http-server` for verification.

**Delegates to:** `scripts/uninstall.sh`

**Typical use:**
```bash
./bhs.sh uninstall
```

---

## Combining Actions

Actions are executed left to right. Any combination is valid as long as order is respected (e.g., always compile before running).

```bash
# Compile debug and run immediately
./bhs.sh compiledebug rundebug

# Generate cert, compile debug, run
./bhs.sh createcert compiledebug rundebug

# Compile production and install as service
./bhs.sh compileprod install
```

---

## Project Layout After `compiledebug` or `compileprod`

```
base-http-server/
├── bin/                        # Self-contained runtime directory
│   ├── base-http-server        # Compiled binary
│   ├── configs/
│   │   └── config.txt
│   ├── www/                    # Static files served by the server
│   │   └── index.html
│   └── ssl/                    # TLS certificate and key (if present)
│       ├── cert.pem
│       └── key.pem
├── configs/
│   └── config.txt              # Source config (synced to bin/ on rundebug)
├── ssl/
│   ├── cert.pem                # Source certificate
│   └── key.pem
└── www/
    └── index.html              # Source static files
```

`bin/` is self-contained: it can be copied to any location and run directly.

---

## Configuration File (`configs/config.txt`)

`rundebug` always syncs `configs/config.txt` from the project root to `bin/` before starting, so you can edit the config without recompiling.

| Key | Type | Default | Description |
|---|---|---|---|
| `verbose_level` | int | `3` | Log verbosity: `0`=none `1`=error `2`=warn `3`=info `4`=debug |
| `http_port` | int | `8080` | HTTP listening port. Ports < 1024 require root on Linux. |
| `https_port` | int | `8443` | HTTPS listening port. Only used when `ssl_enabled=1`. |
| `ssl_enabled` | int | `0` | Set to `1` to enable HTTPS. Requires valid `ssl_cert` and `ssl_key`. |
| `ssl_cert` | string | `./ssl/cert.pem` | Path to the PEM certificate file (relative to the working directory). |
| `ssl_key` | string | `./ssl/key.pem` | Path to the PEM private key file (relative to the working directory). |
| `trusted_proxies` | string | *(empty)* | Comma-separated list of trusted reverse proxy IPs. When set, `X-Real-IP` and `X-Forwarded-For` headers are honored only from these IPs. |

---

## TLS / HTTPS Setup

### Local development

```bash
# 1. Generate a self-signed certificate
./bhs.sh createcert

# 2. Enable HTTPS in configs/config.txt
#    ssl_enabled=1

# 3. Compile and run
./bhs.sh compiledebug rundebug
```

> **Browser warning:** Self-signed certificates are not trusted by browsers by default.
> You will see a "Your connection is not private" warning. This is expected for local development.
> To suppress it, import `ssl/cert.pem` into your OS or browser trust store.

### Production (Let's Encrypt / CA-signed certificate)

1. Obtain a certificate from your CA (e.g., `certbot --standalone`).
2. Copy the certificate and key to `ssl/cert.pem` and `ssl/key.pem`.
3. Set `ssl_enabled=1` in `configs/config.txt`.
4. Run `./bhs.sh install`.

The systemd service is configured with `Restart=on-failure` - if the process crashes, it restarts automatically after 5 seconds.

---

## Privileged Ports (Linux)

On Linux, binding to ports below 1024 (e.g., 80, 443) requires root privileges.

`rundebug` handles this automatically: if any configured port is < 1024 and the current user is not root, the script re-executes itself with `sudo` without requiring manual intervention.

For production, the systemd service runs as `root` by default, so ports 80 and 443 work without additional configuration.

---

## Sub-scripts Reference

These scripts are not meant to be called directly but can be if needed. All of them resolve their paths relative to the project root, so they work correctly regardless of the current working directory.

| Script | Called by | Direct invocation |
|---|---|---|
| `scripts/compile_debug.sh` | `compiledebug` | `./scripts/compile_debug.sh` |
| `scripts/compile_prod.sh` | `compileprod`, `install` | `./scripts/compile_prod.sh` |
| `scripts/run_debug.sh` | `rundebug` | `./scripts/run_debug.sh` |
| `scripts/create_local_cert.sh` | `createcert` | `./scripts/create_local_cert.sh` |
| `scripts/install.sh` | `install` | `sudo ./scripts/install.sh` |
| `scripts/uninstall.sh` | `uninstall` | `sudo ./scripts/uninstall.sh` |

---

## Prerequisites

### Linux

```bash
# Debian / Ubuntu
sudo apt install cmake gcc libssl-dev

# Fedora / RHEL
sudo dnf install cmake gcc openssl-devel

# Arch / Manjaro
sudo pacman -S cmake gcc openssl
```

Optional (for `rundebug` with debugger):
```bash
sudo apt install gdb
```

### macOS

```bash
brew install cmake openssl

# Pass OpenSSL location to CMake (Homebrew installs it to a non-default path)
cmake -B build -DOPENSSL_ROOT_DIR=$(brew --prefix openssl)
```

Optional (LLDB is bundled with Xcode Command Line Tools):
```bash
xcode-select --install
```

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
