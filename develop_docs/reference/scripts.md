# Boat Rudder - Build & Operations Scripts

This document covers `boat_rudder_builder.sh`, the central management script for Boat Rudder, and every sub-script it delegates to.

---

## Overview

`boat_rudder_builder.sh` is the single entry point for all build, run, and deployment operations. It accepts one or more **actions** as positional arguments and executes them in the order they are given.

```
./boat_rudder_builder.sh <action1> [action2] [action3] ...
```

Actions are independent and sequential. If an action fails, the script stops immediately and does not proceed to the next action.

---

## Quick Reference

| Action | Description | Requires sudo |
|---|---|---|
| `compiledebug` | Build with debug symbols + AddressSanitizer | No |
| `compileprod` | Build optimized for production | No |
| `clean` | Remove `build/` and `bin/` | No |
| `rundebug` | Run locally (auto-selects GDB / LLDB / direct) | Auto (if port < 1024) |
| `createcert` | Generate a self-signed TLS certificate | No |
| `install` | Compile prod + install as systemd service | Yes |
| `uninstall` | Stop and remove the systemd service | Yes |

---

## Actions

### `compiledebug`

Compiles the project in **Debug** mode with AddressSanitizer (`-fsanitize=address`) enabled.

**What it does:**
1. Runs `cmake -DCMAKE_BUILD_TYPE=Debug` and `cmake --build` in `build/debug/`.
2. Copies the resulting binary to `bin/boat-rudder`.

`bin/` holds **the binary and nothing else**. The server runs from the project root and reads
`./configs`, `./html` and `./ssl` directly, so there is no second copy of the content tree to
keep in sync - and editing a template, a config value or a certificate takes effect on the next
run with no recompile.

**Delegates to:** `scripts/compile_debug.sh`

**Output:** `bin/boat-rudder` (with ASan instrumentation)

### Incremental builds

`build/` is **kept between runs**, with one directory per build type - `build/debug/` and
`build/release/`. CMake stores an object file per `.c` plus the header dependency graph there, so
each compile only rebuilds what actually changed, and switching between debug and production
does not invalidate the other one's objects.

| | Files recompiled | Time |
|---|---|---|
| From clean (`./boat_rudder_builder.sh clean` first) | 52 | ~2.7 s |
| After editing one `.c` | 1 | ~0.7 s |
| After editing a widely-included header (`utils/log.h`) | 19 | ~1.1 s |

`build/` costs ~6 MB for both build types and is git-ignored. Run `./boat_rudder_builder.sh clean` to remove it
along with `bin/` when a full rebuild is wanted.

**Typical use:**
```bash
./boat_rudder_builder.sh compiledebug
./boat_rudder_builder.sh compiledebug rundebug
```

---

### `compileprod`

Compiles the project in **Release** mode, fully optimized for production.

**What it does:**
1. Runs `cmake -DCMAKE_BUILD_TYPE=Release` and `cmake --build` in `build/release/`.
2. Copies the binary to `bin/boat-rudder` and strips **that copy** (never the build output, which
   would make the next incremental build ship an already-stripped binary).

**Delegates to:** `scripts/compile_prod.sh`

**Output:** `bin/boat-rudder` (optimized, stripped)

**Typical use:**
```bash
./boat_rudder_builder.sh compileprod
./boat_rudder_builder.sh compileprod install
```

---

### `clean`

Removes `build/` and `bin/` - every build artifact and nothing else. `configs/`, `html/`, `ssl/`
and `db_backup/` are live data and are never touched.

**Delegates to:** `scripts/clean.sh`

**Typical use:**
```bash
./boat_rudder_builder.sh clean                # next compile starts from scratch
./boat_rudder_builder.sh clean compiledebug
```

---

### `rundebug`

Runs the debug binary locally, from the project root, against the live `configs/`, `html/` and
`ssl/` directories - so a config change, a template edit or a new certificate applies on the next
run without recompiling.

**What it does:**
1. Verifies `bin/boat-rudder` exists.
2. Creates `html/` if it does not exist.
3. Sends `SIGTERM` to any running instance of `boat-rudder`.
4. Reads `http_port`, `https_port`, and `ssl_enabled` from the config.
5. On **Linux**: re-executes itself with `sudo` if any configured port is < 1024 and the current user is not root.
6. Selects a debugger based on OS and availability:

| Platform | Debugger found | Behavior |
|---|---|---|
| Linux | GDB available | Runs under GDB with SIGPIPE suppressed |
| macOS | LLDB available | Runs under LLDB |
| Any | No debugger | Runs binary directly (ASan still active) |

**Environment variable set:** `ASAN_OPTIONS=check_printf=0`

**Binary arguments passed:**
```
boat-rudder -c ./configs/settings.conf ./html
```

**Delegates to:** `scripts/run_debug.sh`

**Typical use:**
```bash
./boat_rudder_builder.sh rundebug
./boat_rudder_builder.sh compiledebug rundebug
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
4. Prints the certificate details (validity dates, subject, SANs).

`rundebug` reads `./ssl/` directly, so the new certificate is picked up on the next run - no
copy step, no recompile.

**Delegates to:** `scripts/create_local_cert.sh`

**Output files:**
```
ssl/cert.pem    - X.509 certificate (PEM)
ssl/key.pem     - RSA private key (PEM, unencrypted)
```

> **Note:** After generating the certificate, make sure `ssl_enabled=1` is set in `configs/settings.conf`.

**Typical use:**
```bash
./boat_rudder_builder.sh createcert
./boat_rudder_builder.sh createcert compiledebug rundebug
```

---

### `install`

Compiles the project for production and installs it as a **systemd service**.

> **Linux only.** Running this action on macOS exits with an error and suggests using `rundebug` instead.

> **Requires `sudo`.** `boat_rudder_builder.sh` calls `sudo ./scripts/install.sh` automatically.

**What it does:**
1. Calls `compile_prod.sh` to produce a fresh release binary.
2. Creates `/usr/local/bin/boat-rudder/` and copies into it, straight from the project root:
   - `boat-rudder` (binary from `bin/`, marked executable)
   - `configs/`
   - `html/` (creates an empty one with a warning if absent)
   - `ssl/` (only if it contains `.pem` files)

   This is the one place where the content tree really is copied: the installed service runs
   from `/usr/local/bin/boat-rudder/`, independent of the source checkout.
3. Copies `scripts/boat-rudder.service` to `/etc/systemd/system/`.
4. Runs `systemctl daemon-reload`, `systemctl enable`, `systemctl start`.
5. Prints the service status and the log tail command.

**Install path:** `/usr/local/bin/boat-rudder/`

**Service name:** `boat-rudder`

**Delegates to:** `scripts/install.sh`

**After install:**
```bash
# Check status
systemctl status boat-rudder

# Follow logs
journalctl -u boat-rudder -f

# Restart after config changes
systemctl restart boat-rudder
```

**Typical use:**
```bash
./boat_rudder_builder.sh install
./boat_rudder_builder.sh compileprod install   # same effect - install always recompiles
```

---

### `uninstall`

Stops and completely removes the systemd service and all installed files.

> **Linux only.** Exits with an error on macOS.

> **Requires `sudo`.** `boat_rudder_builder.sh` calls `sudo ./scripts/uninstall.sh` automatically.

**What it does:**
1. Stops the service with `systemctl stop`.
2. Disables the service with `systemctl disable`.
3. Removes `/etc/systemd/system/boat-rudder.service`.
4. Runs `systemctl daemon-reload`.
5. Removes `/usr/local/bin/boat-rudder/` recursively.
6. Lists any remaining units matching `boat-rudder` for verification.

**Delegates to:** `scripts/uninstall.sh`

**Typical use:**
```bash
./boat_rudder_builder.sh uninstall
```

---

## Combining Actions

Actions are executed left to right. Any combination is valid as long as order is respected (e.g., always compile before running).

```bash
# Compile debug and run immediately
./boat_rudder_builder.sh compiledebug rundebug

# Generate cert, compile debug, run
./boat_rudder_builder.sh createcert compiledebug rundebug

# Compile production and install as service
./boat_rudder_builder.sh compileprod install
```

---

## Project Layout After `compiledebug` or `compileprod`

```
boat-rudder/
├── build/                      # Kept between compiles for incremental builds
│   ├── debug/                  # Objects + dependency graph, Debug + ASan
│   └── release/                # Objects + dependency graph, Release
├── bin/
│   └── boat-rudder             # Compiled binary - the only thing bin/ ever contains
├── configs/
│   └── settings.conf           # Read live at runtime
├── ssl/
│   ├── cert.pem                # Read live at runtime
│   └── key.pem
└── html/                       # Document root: templates, assets and uploaded media
    ├── themes/dark/...
    └── content/posts/...       # Media uploads land here
```

The binary is **not** self-contained: it resolves `./configs`, `./html` and `./ssl` relative to
its working directory, which is why every script `cd`s to the project root before starting it.
To run it from somewhere else, use `install` - that is what assembles a standalone
`/usr/local/bin/boat-rudder/` tree.

---

## Configuration File (`configs/settings.conf`)

`rundebug` always syncs `configs/settings.conf` from the project root into `bin/` before
starting, so the config can be edited without recompiling. `install` copies it into
`/usr/local/bin/boat-rudder/configs/`.

Every key is documented in **[configuration.md](configuration.md)**, the single configuration
reference.

---

## TLS / HTTPS Setup

### Local development

```bash
# 1. Generate a self-signed certificate
./boat_rudder_builder.sh createcert

# 2. Enable HTTPS in configs/settings.conf
#    ssl_enabled=1

# 3. Compile and run
./boat_rudder_builder.sh compiledebug rundebug
```

> **Browser warning:** Self-signed certificates are not trusted by browsers by default.
> You will see a "Your connection is not private" warning. This is expected for local development.
> To suppress it, import `ssl/cert.pem` into your OS or browser trust store.

### Production (Let's Encrypt / CA-signed certificate)

1. Obtain a certificate from your CA (e.g., `certbot --standalone`).
2. Copy the certificate and key to `ssl/cert.pem` and `ssl/key.pem`.
3. Set `ssl_enabled=1` in `configs/settings.conf`.
4. Run `./boat_rudder_builder.sh install`.

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
| `scripts/clean.sh` | `clean` | `./scripts/clean.sh` |
| `scripts/run_debug.sh` | `rundebug` | `./scripts/run_debug.sh` |
| `scripts/create_local_cert.sh` | `createcert` | `./scripts/create_local_cert.sh` |
| `scripts/install.sh` | `install` | `sudo ./scripts/install.sh` |
| `scripts/uninstall.sh` | `uninstall` | `sudo ./scripts/uninstall.sh` |
| `scripts/image-optimizer.sh` | the server, via `popen()` on every media upload | `./scripts/image-optimizer.sh <in_dir> <out_dir> [file]` |
| `scripts/mongodb_start.sh` | nothing (manual) | `./scripts/mongodb_start.sh` |
| `scripts/mongodb_dump.sh` | nothing (manual) | `./scripts/mongodb_dump.sh` |
| `scripts/mongodb_restore.sh` | nothing (manual) | `./scripts/mongodb_restore.sh` |
| `scripts/show/banner`, `scripts/show/divbar` | sourced by the other scripts for console output | not standalone |

### `scripts/image-optimizer.sh`

Invoked by the server itself (not by `boat_rudder_builder.sh`) after every media upload. Generates 5 variants
per image - `_full`, `_half`, `_small`, `_medium`, `_micro` - deletes the original and symlinks
the base filename to `_half`. Requires `imagemagick`, `jpegoptim` and `gifsicle`; without them
uploads still succeed but no variants are produced, so thumbnails and the retro epochs break.
Full details in [media-admin.md](media-admin.md).

### MongoDB helpers

| Script | Purpose |
|---|---|
| `mongodb_start.sh` | Starts the local MongoDB service (systemd, with a SysV fallback). |
| `mongodb_dump.sh` | `mongodump` of this site's database into `./db_backup/<db>/`. |
| `mongodb_restore.sh` | `mongorestore --drop` of `./db_backup/<db>/` back into the database. |

Both dump and restore read the database name from `mongodb_db` in `configs/settings.conf`, so
they follow whichever site this checkout is configured for - no database name is hardcoded.
`mongodb_restore.sh` **drops the existing collections** before restoring.

---

## Prerequisites

### Linux

```bash
# Debian / Ubuntu
sudo apt install cmake gcc libssl-dev libmongoc-dev libsodium-dev

# Fedora / RHEL
sudo dnf install cmake gcc openssl-devel mongo-c-driver-devel libsodium-devel

# Arch / Manjaro
sudo pacman -S cmake gcc openssl mongo-c-driver libsodium
```

`libmongoc` and `libsodium` are not optional: the CMake build links them unconditionally for the
database-backed CMS, the dashboard and Argon2id password hashing.

Runtime dependencies (not needed to compile, but the media library is broken without them):

```bash
sudo apt install mongodb-org imagemagick jpegoptim gifsicle
```

Optional (for `rundebug` with debugger):
```bash
sudo apt install gdb
```

### macOS

```bash
brew install cmake openssl mongo-c-driver libsodium
brew install imagemagick jpegoptim gifsicle   # runtime, for the media library

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
