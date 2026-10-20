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
| `rundebug@<name>` | `rundebug` with `./configs/<name>.conf` | Auto (if port < 1024) |

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
3. Sends `SIGTERM` to a `boat-rudder` already running **on the same config** - another one
   started from its own config (`rundebug@<name>`, `./configs/<name>.conf`) keeps running, so
   several configurations (each with its own ports) can run side by side.
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
   - `scripts/image-optimizer.sh` - the server runs it (relative to its working directory) on
     every media upload; without it every upload fails with `415`. The installer also warns when
     one of its tools - `magick`, `jpegoptim`, `gifsicle` - is missing, which fails the same way.
   - `scripts/manifest.sh` - to check the install against its manifest on the host (below).
   - `scripts/boat-rudder-update` - the updater ([below](#scriptsboat-rudder-update)), also
     linked as `/usr/local/sbin/boat-rudder-update`.
   - `configs/settings.conf` - **only on the first install.** It is per-host (ports, WAP gateway
     addresses, log level…), so a reinstall keeps the installed file, writes the repository's
     version next to it as `settings.conf.dist`, and lists every key the installed file lacks
     (those run with their built-in defaults - see [configuration.md](configuration.md)).
   - `html/` - everything but `html/content/` is merged over the installed copy with `cp -rT`:
     templates, themes and assets are updated, files that only exist on the server (uploaded
     fonts...) are kept. `html/content/` is the site's own and the repository's copy only sample
     content, so it is **seeded on the first install only** - copied over a live site it would
     replace same-named files with the samples. Creates an empty `html/` with a warning if absent.
   - `data/GeoLite2-Country.mmdb` - copied to `data/` if present; otherwise a warning that every
     visitor country will be "Unknown" ([analytics.md](analytics.md#geoip-optional)).
   - `ssl/` - **only seeded on the first install** (when the installed `ssl/` is empty), so
     certificates renewed in place on the server are never overwritten by a stale checkout.
   - `.boat-rudder/` - written last, once every file is in place: `VERSION` and
     `manifest.sha256`, the manifest of the system files just installed
     ([`scripts/manifest.sh`](#scriptsmanifestsh)), and `minisign.pub`, the key releases must be
     signed with. The updater compares against the manifest, and it is the only list of files an
     update may replace or delete.

   This is the one place where the content tree really is copied: the installed service runs
   from `/usr/local/bin/boat-rudder/`, independent of the source checkout.
3. Installs `scripts/boat-rudder.logrotate` as `/etc/logrotate.d/boat-rudder` (if
   `/etc/logrotate.d` exists).
4. Copies `scripts/boat-rudder.service` to `/etc/systemd/system/`.
5. Runs `systemctl daemon-reload`, `systemctl enable` and `systemctl restart` (restart, so a
   reinstall replaces the running old binary).
6. Prints the service status and the log tail command.

**Install path:** `/usr/local/bin/boat-rudder/`, or `--dir <path>`.

**Another directory: `sudo ./scripts/install.sh --dir <path>`.** Builds and copies everything
above (step 2, `.boat-rudder/` included) into `<path>` and stops there: no systemd unit, logrotate
rule or `/usr/local/sbin/boat-rudder-update` link - `boat-rudder.service` always runs
`/usr/local/bin/boat-rudder/`. It is for a tool that runs and updates the install itself: several
sites on one machine, each its own install, are managed that way by the separate **multi-br**
project. The updater works on such an install with `BR_INSTALL_DIR` and `BR_SERVICE` set (below).

`<path>` must be absolute (`a-z A-Z 0-9 . _ -` only), and `install.sh` refuses
`/usr/local/bin/boat-rudder` itself (that's `install.sh` without `--dir`), system directories
(`/usr/local/bin`, `/srv`, `/opt`...) and a non-empty directory that holds no Boat Rudder install
(no `.boat-rudder/`). The directory is an argument rather than an environment variable on
purpose: `sudo` drops the caller's environment, and a lost variable would have sent the install
to the default directory, over the live site.

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
6. Removes the updater's link `/usr/local/sbin/boat-rudder-update` (when it still points into the
   install) and `/etc/logrotate.d/boat-rudder`.
7. Lists any remaining units matching `boat-rudder` for verification.

It does **not** remove `/var/log/boat-rudder.log*` nor the database; delete them by hand if
wanted.

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
    ├── themes/<theme>/...      # Per-theme templates, assets, styles(_admin)_epoch3.css
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
| `scripts/run_debug.sh` | `rundebug`, `rundebug@<name>` | `./scripts/run_debug.sh [<name>]` |
| `scripts/create_local_cert.sh` | `createcert` | `./scripts/create_local_cert.sh` |
| `scripts/install.sh` | `install` | `sudo ./scripts/install.sh [--dir <path>]` - see [`install`](#install) |
| `scripts/uninstall.sh` | `uninstall` | `sudo ./scripts/uninstall.sh` |
| `scripts/image-optimizer.sh` | the server, via `popen()` on every media upload | `./scripts/image-optimizer.sh <in_dir> <out_dir> [file]` |
| `scripts/manifest.sh` | `install` (`create`); manual on a host (`adopt`, `verify`) | `./scripts/manifest.sh create\|adopt\|verify …` |
| `scripts/boat-rudder-update` | nothing (manual, on an installed host) | `sudo boat-rudder-update status\|check\|install\|rollback\|history` |
| `scripts/release.sh` | nothing (manual, by the maintainer) | `./scripts/release.sh <version> [--key <file>] [--publish]` |
| `scripts/minisign.pub` | copied by `install` to `.boat-rudder/` | not executable - the public key releases are signed with |
| `scripts/mongodb_start.sh` | nothing (manual) | `./scripts/mongodb_start.sh` |
| `scripts/mongodb_dump.sh` | nothing (manual) | `./scripts/mongodb_dump.sh [<settings.conf>]` |
| `scripts/mongodb_restore.sh` | nothing (manual) | `./scripts/mongodb_restore.sh [<settings.conf>]` |
| `scripts/check_css_bem.py` | nothing (manual, before committing CSS/templates) | `./scripts/check_css_bem.py` - see [style-guide.md](style-guide.md#15-css-class-names-bem); needs `python3` |
| `scripts/boat-rudder.service` | copied by `install` | not executable |
| `scripts/boat-rudder.logrotate` | copied by `install` | not executable |
| `scripts/show/banner`, `scripts/show/divbar` | sourced (`source ./scripts/show/…`) by `compile_*`, `install`, `uninstall`, `run_debug`, `clean`, `create_local_cert` for console output: the ASCII banner and a divider line | not standalone - plain shell fragments, no shebang |

### `scripts/image-optimizer.sh`

Invoked by the server itself (not by `boat_rudder_builder.sh`) after every media upload. Generates 5 variants
per image - `_full`, `_half`, `_small`, `_medium`, `_micro` - deletes the original; only the five variants are left. Requires `imagemagick`, `jpegoptim` and `gifsicle`; when the script
or one of them is missing, no variant is written and the server rejects the upload with `415`
(the raw file is deleted). Full details in [media-admin.md](media-admin.md).

### `scripts/manifest.sh`

The list of a version's **system files** - the binary, `scripts/image-optimizer.sh`,
`scripts/manifest.sh` and `scripts/boat-rudder-update`, the two default favicons, and every file under `html/templates/`,
`html/themes/` and `html/assets/` - each with its SHA-256, in `sha256sum` format with paths
relative to the install directory (`.boat-rudder/manifest.sha256`). `html/content/`, `configs/`,
`ssl/`, `data/`, the uploaded favicons and fonts are the site's and never listed. Part of the
update system ([update-system-plan.md](../plans/update-system-plan.md)).

| Command | Does |
|---|---|
| `manifest.sh create [<source>]` | Prints the manifest of a built source tree (binary at `bin/boat-rudder`); `install.sh` writes it to `.boat-rudder/` |
| `sudo manifest.sh adopt <install> [<source>]` | One-off, for an install made before manifests: writes `.boat-rudder/VERSION` and `manifest.sha256` from the source of the version it runs, and reports files **edited on the host**, **missing**, and **not part of the version** (added on the host, or left over). The installed binary is listed with its own hash: builds aren't byte-reproducible |
| `manifest.sh verify <install>` | Modified and missing system files; exits `1` if any. The same check as `cd <install> && sha256sum -c .boat-rudder/manifest.sha256` |

Adopting the production install, from the checkout of the version it runs:

```bash
cd /usr/local/src/<checkout>
sudo ./scripts/manifest.sh adopt /usr/local/bin/boat-rudder
```

### `scripts/release.sh`

Run by the maintainer, on their own machine, to publish version `<version>`
([update-system-plan.md](../plans/update-system-plan.md) §3):

1. Checks: `minisign` installed, the secret key exists (default `~/.minisign/boat-rudder.key`,
   `--key` for another), `scripts/minisign.pub` exists, `VERSION` says `<version>`, the work tree
   is clean, the tag `v<version>` doesn't exist. Warns when `CHANGELOG.md` has no `## v<version>`.
2. `git archive` of `HEAD` under `boat-rudder-<version>/`, `gzip -n` →
   `dist/boat-rudder-<version>-src.tar.gz`. Only versioned files; the same commit always gives the
   same bytes. (GitHub's automatic "Source code" archives can't be signed: their bytes aren't
   guaranteed stable.)
3. Signs it (`.minisig`, asks for the key's password) and verifies the signature against
   `scripts/minisign.pub` - a package the installs would reject stops here.
4. Tags `HEAD` as `v<version>` (annotated) - only now, so a failed signature leaves no tag.
5. Writes the release notes - the version's `CHANGELOG.md` section - to
   `dist/boat-rudder-<version>-notes.md`. With `--publish` (needs the `gh` CLI, logged in), pushes
   the tag and creates the GitHub release with the package and its signature; without it, prints
   the steps to do the same on github.com. A release the updater can see has **both** files.

`dist/` is gitignored. The key pair is made once:

```bash
minisign -G -p scripts/minisign.pub -s ~/.minisign/boat-rudder.key
```

`scripts/minisign.pub` is committed; the secret key never enters the repository - keep it, and a
backup, off the servers.

### `scripts/boat-rudder-update`

The updater ([update-system-plan.md](../plans/update-system-plan.md) §4-§6). Runs as root on an
installed host, from `/usr/local/sbin/boat-rudder-update` (the link `install.sh` makes). Works on
`/usr/local/bin/boat-rudder/` and its `.boat-rudder/` state directory; one run at a time (`flock`
on `.boat-rudder/update.lock`).

| Command | Does |
|---|---|
| `status` | Installed version, binary `--version`, service state, whether the signing key is installed, system files modified or missing (`manifest.sh verify`), the last update |
| `check` | The releases on GitHub (`update_repo` in `settings.conf`, default `jpablotoledom/boat-rudder`) that have both `-src.tar.gz` and `.minisig` and are newer than the installed version. The dashboard's `/dashboard/settings/updates` shows the same list with each release's notes |
| `install <version>\|latest [--yes] [--force]` | Downloads that release and runs the stages below. `--force` reinstalls the installed version |
| `install --file <pkg.tar.gz> --sig <pkg.minisig>` | The same from a package already on the host |
| `rollback [--restore-db] [--yes]` | Undoes the newest update not already rolled back, from its backup |
| `history` | Every install and rollback, from `.boat-rudder/history.jsonl` - with who ran it (`by`: the `sudo` user, or `dashboard:<email>`) |
| `run-request` | What the dashboard starts: carries out `.boat-rudder/request.json` (below) |

Stages of `install` - everything that can fail runs before the site is touched:

1. **Package**: the signature is checked against `.boat-rudder/minisign.pub` (never against a key
   inside the package); the archive is extracted to `.boat-rudder/staging/`, refusing anything but
   regular files and directories, absolute paths, `..`, more than one top directory and archives
   over 1 GB.
2. **Build**: prerequisites (`cmake`, `cc`, `make`, `pkg-config`, `strip`, the `libmongoc`,
   `libsodium` and `openssl` headers - missing ones stop the update; a missing `magick`,
   `jpegoptim` or `gifsicle` only warns), then
   `scripts/compile_prod.sh` in the staging copy; the new binary must answer `--version` with the
   package's `VERSION`. A failed build stops here, with the log; the service is never stopped.
3. **Plan**: the installed manifest against the new one gives the files to replace, add and
   delete. A system file whose content differs from the installed manifest was edited on the host:
   listed, replaced, kept in the backup. Files outside both manifests are never touched. A file
   the new version no longer ships is deleted only if the **installed version's official package**
   shipped it: the updater fetches that signed release and checks. The installed manifest lists
   everything `install.sh` found in the tree it ran from, so a site installed from its own
   repository has its additions there too - uploaded fonts, its own theme files - and those are
   kept (*this site's own*) and leave the manifest. If that release can't be had - no network, a
   version never released - nothing is deleted and the files are listed as unconfirmed. Asks to
   continue (unless `--yes`).
4. **Backup** to `.boat-rudder/backups/<date>-<from>-to-<to>/`: the files to replace or delete,
   the list of files to add, the old manifest, `VERSION` and key, and a `mongodump` of the site
   database (skipped with `BR_NO_DB_BACKUP=1`). The last 5 backups are kept.
5. **Apply**: stops the service, writes each file to a temporary name and renames it into place,
   deletes the removed ones, writes the new `manifest.sha256`, `VERSION` and key, starts the
   service.
6. **Health check** (30 s): `/`, `/login` and the active theme's `styles_epoch3.css` must answer
   `200`, asked of `listen_address` (else `127.0.0.1`) on `http_port` - without a PROXY header,
   which a site with `proxy_protocol=1` serves as a direct loopback connection. If not, the update is **rolled back automatically** and recorded as failed.

`rollback` restores the backup's files, deletes the ones the update added, restores manifest,
`VERSION` and key, restarts and checks health. The database is restored only with
`--restore-db` - by default, entries written since the update are kept.

**From the dashboard** (`/dashboard/settings/updates`, admin, password typed again): the server
writes `.boat-rudder/request.json` - `{id, action: install|rollback, version, user}` - and
`.boat-rudder/progress.json` (`queued`), then starts

```
systemd-run --unit=boat-rudder-update --collect --quiet --setenv=BR_INSTALL_DIR=<install> \
            <install>/scripts/boat-rudder-update run-request
```

The unit is named by `BR_UPDATE_UNIT` when the server's own unit sets it (one name per install,
so installs sharing a machine can each run an update), else `boat-rudder-update`.

A transient unit, so it lives outside `boat-rudder.service` - which the update stops and starts -
and a second request can't start while one runs (the unit name is taken; the lock also holds).
`run-request` validates every field of the request and deletes it before acting, runs
`install <version> --yes` or `rollback --yes`, sends its output to `.boat-rudder/last-run.log`, and
rewrites `progress.json` (`{id, action, version, from, user, state, stage, message, started_at,
updated_at}`) at every stage: `running` → `done`, `failed` or `rolled-back`. A run that ends any
other way (`set -e`, a signal) is reported `failed` on exit. The server shows a `queued` run older
than 2 minutes, or a `running` one silent for 2 hours, as failed. No unit file is installed:
`systemd-run` is enough. The server forwards its own `BR_*` variables, for test setups.

Environment overrides, for testing and for installs other than `/usr/local/bin/boat-rudder/`
under `boat-rudder.service`: `BR_INSTALL_DIR`, `BR_REPO`, `BR_SERVICE`, `BR_SERVICE_LOG` (the
unit's log, named in error messages), `BR_SERVICECTL`,
`BR_MINISIGN`, `BR_HEALTH_BASE`, `BR_HEALTH_TIMEOUT`, `BR_KEEP_BACKUPS`, `BR_NO_DB_BACKUP`,
`BR_ALLOW_NONROOT`.

### MongoDB helpers

| Script | Purpose |
|---|---|
| `mongodb_start.sh` | Starts the local MongoDB service (systemd, with a SysV fallback). |
| `mongodb_dump.sh` | `mongodump` of this site's database into `./db_backup/<db>/`. |
| `mongodb_restore.sh` | `mongorestore --drop` of `./db_backup/<db>/` back into the database. |

Both dump and restore read the database name from `mongodb_db` in `configs/settings.conf`, so
they follow whichever site this checkout is configured for - no database name is hardcoded. An
optional argument names another `settings.conf` (another install's); a path that doesn't exist is
an error, never a fallback to this checkout's database.
`mongodb_restore.sh` **drops the existing collections** before restoring.

> The dump directory `db_backup/` (and `db_backup_clean/`) is currently **tracked in git** and
> contains `users.bson` (password hashes) and `sessions.bson` (session tokens). Dumps of a real
> site should stay out of version control - see [security.md](security.md#known-gaps).

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
