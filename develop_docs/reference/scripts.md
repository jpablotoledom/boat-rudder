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
| From clean (`./boat_rudder_builder.sh clean` first) | every source (79 today) | a few seconds |
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
   - `boat-rudder` - with `install -m 755`, which replaces the file instead of writing into it,
     so it works while the old binary is running ("Text file busy" with `cp`).
   - `configs/settings.conf` - **only on the first install.** It is per-host (ports, WAP gateway
     addresses, log level…), so a reinstall keeps the installed file, writes the repository's
     version next to it as `settings.conf.dist`, and lists every key the installed file lacks
     (those run with their built-in defaults - see [configuration.md](configuration.md)).
   - `html/` - merged over the installed copy with `cp -rT`: templates and themes are updated,
     files that only exist on the server (media uploaded from the dashboard under
     `html/content/`) are kept. Creates an empty one with a warning if absent.
   - `data/GeoLite2-Country.mmdb` - copied to `data/` if present; otherwise a warning that every
     visitor country will be "Unknown" ([analytics.md](analytics.md#geoip-optional)).
   - `ssl/` - **only seeded on the first install** (when the installed `ssl/` is empty), so
     certificates renewed in place on the server are never overwritten by a stale checkout.

   This is the one place where the content tree really is copied: the installed service runs
   from `/usr/local/bin/boat-rudder/`, independent of the source checkout.
3. Installs `scripts/boat-rudder.logrotate` as `/etc/logrotate.d/boat-rudder` (if
   `/etc/logrotate.d` exists).
4. Copies `scripts/boat-rudder.service` to `/etc/systemd/system/`.
5. Runs `systemctl daemon-reload`, `systemctl enable` and `systemctl restart` (restart, so a
   reinstall replaces the running old binary).
6. Prints the service status and the log tail command.

**Install path:** `/usr/local/bin/boat-rudder/`

**Service name:** `boat-rudder`

**Delegates to:** `scripts/install.sh`

**After install:**
```bash
# Check status
systemctl status boat-rudder

# Follow logs (the unit appends stdout/stderr to this file, not to the journal)
tail -f /var/log/boat-rudder.log

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
5. Removes `/usr/local/bin/boat-rudder/` recursively - **including** the host's
   `configs/settings.conf`, `ssl/`, `data/` and every uploaded file under `html/content/`. Back
   up first if any of it matters.
6. Lists any remaining units matching `boat-rudder` for verification.

It does **not** remove `/etc/logrotate.d/boat-rudder` nor `/var/log/boat-rudder.log*`; delete
them by hand if wanted.

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
├── data/
│   └── GeoLite2-Country.mmdb   # Optional, read at startup for analytics
└── html/                       # Document root: templates, assets and uploaded media
    ├── templates/...           # Shared templates
    ├── themes/<theme>/...      # Per-theme templates, assets, styles_epoch3.css
    ├── assets/fonts/...        # Font library
    └── content/posts/...       # Media uploads land here (content/qr/ caches QR images)
```

The binary is **not** self-contained: it resolves `./configs`, `./html`, `./ssl` and `./data` relative to
its working directory, which is why every script `cd`s to the project root before starting it.
To run it from somewhere else, use `install` - that is what assembles a standalone
`/usr/local/bin/boat-rudder/` tree.

---

## Configuration File (`configs/settings.conf`)

`rundebug` starts the binary with `-c ./configs/settings.conf` from the project root, so the
config can be edited without recompiling or copying. `install` copies it into
`/usr/local/bin/boat-rudder/configs/` on the first install only (see `install` above).

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

### The systemd unit (`scripts/boat-rudder.service`)

| Setting | Value | Why |
|---|---|---|
| `ExecStart` | `/usr/local/bin/boat-rudder/boat-rudder -c …/configs/settings.conf …/html` | Absolute paths into the install directory |
| `WorkingDirectory` | `/usr/local/bin/boat-rudder` | Templates, `data/` and `ssl/` are resolved relative to it |
| `User` / `Group` | `root` | Binds ports 80/443 directly. Consider an unprivileged user plus `AmbientCapabilities=CAP_NET_BIND_SERVICE` - see [security.md](security.md#known-gaps) |
| `KillSignal` / `TimeoutStopSec` / `SendSIGKILL` | `SIGTERM` / `10` / `yes` | Graceful shutdown takes ≤ ~1 s plus up to 10 s draining TLS connections |
| `Restart` / `RestartSec` | `on-failure` / `5` | |
| `StandardOutput` / `StandardError` | `append:/var/log/boat-rudder.log` | One log file, rotated by logrotate |
| `After` / `Wants` | `network-online.target` | |

### `boat-rudder.logrotate`

Installed as `/etc/logrotate.d/boat-rudder`: `/var/log/boat-rudder.log` rotated **weekly** or
when it exceeds **20 MB**, **8** rotations kept, compressed (`delaycompress`), `missingok`,
`notifempty`. It uses **`copytruncate`** because systemd holds the file open in append mode -
moving it away would leave the server writing to the rotated file.

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
| `scripts/migrations/*.js` | nothing (manual, `mongosh`) | see [migrations.md](migrations.md) |
| `scripts/boat-rudder.service` | copied by `install` | not executable |
| `scripts/boat-rudder.logrotate` | copied by `install` | not executable |
| `scripts/show/banner`, `scripts/show/divbar` | sourced (`source ./scripts/show/…`) by `compile_*`, `install`, `uninstall`, `run_debug`, `clean`, `create_local_cert` for console output: the ASCII banner and a divider line | not standalone - plain shell fragments, no shebang |

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

> The dump directory `db_backup/` (and `db_backup_clean/`) is currently **tracked in git** and
> contains `users.bson` (password hashes) and `sessions.bson` (session tokens). Dumps of a real
> site should stay out of version control - see [security.md](security.md#known-gaps).

### Database migrations

One-off `mongosh` scripts under `scripts/migrations/`, named `YYYY-MM-DD-<what>.js`,
idempotent, run by hand against the site's database after a backup:

```bash
./scripts/mongodb_dump.sh
mongosh "mongodb://localhost:27017/<mongodb_db>" scripts/migrations/<file>.js
```

Conventions, catalog and how to write one: [migrations.md](migrations.md).

---

## Prerequisites

### Linux

```bash
# Debian / Ubuntu
sudo apt install cmake gcc pkg-config libssl-dev libmongoc-dev libsodium-dev libqrencode-dev
sudo apt install libmaxminddb-dev          # optional: GeoIP

# Fedora / RHEL
sudo dnf install cmake gcc pkgconf openssl-devel mongo-c-driver-devel libsodium-devel qrencode-devel
sudo dnf install libmaxminddb-devel        # optional: GeoIP

# Arch / Manjaro
sudo pacman -S cmake gcc pkgconf openssl mongo-c-driver libsodium qrencode
sudo pacman -S libmaxminddb                # optional: GeoIP
```

`libmongoc`, `libsodium` and `libqrencode` are not optional: the CMake build links them
unconditionally (the database-backed CMS and dashboard, Argon2id password hashing, QR codes for
retro epochs). `libqrencode` ships no pkg-config file, so CMake looks for the library itself -
the runtime package (`libqrencode4`) is enough. **`libmaxminddb` is optional**: when
`pkg-config` finds it, CMake defines `HAVE_MAXMINDDB` and links it; without it the build still
succeeds and analytics records every country as `Unknown`. `libm` is linked explicitly.

The full dependency and vendored-code list is in [third-party.md](third-party.md).

Runtime dependencies (not needed to compile, but the media library is broken without them):

```bash
sudo apt install mongodb-org imagemagick jpegoptim gifsicle
sudo apt install mongodb-mongosh   # to run scripts/migrations/ (package name per MongoDB's repo)
```

Optional runtime data: `data/GeoLite2-Country.mmdb` (MaxMind GeoLite2, see
[third-party.md](third-party.md#geolite2)).

Optional (for `rundebug` with debugger):
```bash
sudo apt install gdb
```

### macOS

```bash
brew install cmake pkg-config openssl mongo-c-driver libsodium qrencode
brew install libmaxminddb                     # optional: GeoIP
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
