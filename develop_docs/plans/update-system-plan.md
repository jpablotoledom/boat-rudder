# Updates and Rollback - Plan

> **Status**: approved (2026-10-14). Phases 1-3 implemented (2026-10-14): `VERSION`, `--version`,
> `scripts/manifest.sh`, `install.sh` writing `.boat-rudder/`; `scripts/release.sh`;
> `scripts/boat-rudder-update`. Phase 4 implemented (2026-10-15) without *Upload package*:
> `/dashboard/settings/updates` shows the version, checks GitHub, shows the notes, installs and
> rolls back. The updater runs as a transient unit (`systemd-run`) instead of an installed
> `boat-rudder-update.service`. Where the implementation
> differs from this plan, [scripts.md](../reference/scripts.md) describes what it does. References: [scripts.md](../reference/scripts.md),
> [security.md](../reference/security.md), [dashboard.md](../reference/dashboard.md).

Today a site is updated by hand: copy the source to the host, run `install.sh`, which compiles and
copies the binary, `scripts/` and `html/` over the installed tree. There is no record of what is
installed, no backup of what an update replaced and no way back. This plan adds an updater that
fetches **signed source** - from the project's GitHub releases or as an uploaded file - compiles it
on the server, replaces only the system's own files, checks the site still answers, and undoes
itself when it doesn't. The same steps run from the command line and, later, from
`/dashboard/settings/updates`.

---

## 1. Decisions

| Question | Decision |
|---|---|
| What is distributed | **Source**, always compiled on the server (no prebuilt binaries) |
| Integrity | **Mandatory signature** (minisign) on every source package: Boat Rudder is an internet-facing HTTP server, and an update is code it will run as root |
| Source of updates | The public GitHub repository's releases (no token), or the same package uploaded by hand |
| Install layout | **Unchanged**: files are replaced in place under `/usr/local/bin/boat-rudder/` |
| Failed health check | **Automatic rollback** |
| System files edited by hand on the host | Replaced, listed in the update's report, kept in its backup |

---

## 2. Which files are the system's

The installed tree mixes system files with the site's own, and stays that way:

| Path | Owner | An update... |
|---|---|---|
| `boat-rudder`, `scripts/` | system | replaces |
| `html/templates/`, `html/themes/`, `html/assets/` | system | replaces (only files the system shipped - see below) |
| `html/favicon.ico`, `html/favicon-nb.ico` | system (defaults) | replaces |
| `html/content/` | site: uploads, generated QR codes | never touches - the repository's copy is only sample content |
| `html/favicon2.ico`, `html/favicon3.ico` | site (uploaded) | never touches |
| `html/assets/fonts/<uploaded>` | site (uploaded) | never touches |
| `configs/`, `ssl/`, `data/` | host | never touches |

What makes this safe without moving anything is a **manifest**: the exact list of files the
installed version shipped, each with its SHA-256, in `.boat-rudder/manifest.sha256` - plain
`sha256sum` format with paths relative to the install directory, so `sha256sum -c` checks an
install against it and the shell tools read it without a JSON parser. The updater only
ever writes or deletes paths listed there (or in the new version's manifest); any other file -
whatever directory it is in - is outside its reach.

```
/usr/local/bin/boat-rudder/
├── boat-rudder  scripts/  html/  configs/  ssl/  data/     ← as today
└── .boat-rudder/                                           ← new: the updater's state
    ├── VERSION
    ├── manifest.sha256      ← installed system files + SHA-256 (sha256sum format)
    ├── minisign.pub         ← the release signing key
    ├── staging/<version>/   ← src/ and build/ of an update in progress
    ├── backups/<date>-<from>-to-<to>/
    └── history.jsonl
```

---

## 3. Releases and signing

GitHub's automatic "Source code" archives are generated on demand and their bytes are not
guaranteed stable, so they can't carry a signature. Releases instead attach their own archive:

- `scripts/release.sh <version>`, run by the maintainer: checks the tree is clean, tags,
  builds `boat-rudder-<version>-src.tar.gz` with `git archive` (versioned files only, reproducible),
  signs it with **minisign** (`.minisig`), and uploads both to the GitHub release (`gh release`).
- The private key never enters the repository or GitHub; there is no CI involved.
- The public key ships in the source (`scripts/minisign.pub`) and is installed to
  `.boat-rudder/minisign.pub`. A package whose signature doesn't verify against the **installed**
  key is rejected, whichever way it arrived.
- **Key rotation**: a version may ship a new public key only when it is itself signed with the
  current one; the updater then installs the new key (a rollback restores the old one).

The package also carries `VERSION`. *(Not implemented: a `release.json` with the oldest version a
package can update from and the system packages it needs - the updater checks the build
prerequisites itself, and nothing has needed a minimum version yet.)*

---

## 4. The updater

`scripts/boat-rudder-update` (bash), run as root, either from a shell or by the
`boat-rudder-update.service` oneshot unit the dashboard starts.

| Command | Does |
|---|---|
| `check` | Lists newer releases on GitHub (only those with both `-src.tar.gz` and `.minisig`) |
| `install <version>\|latest` | Downloads, then the stages below |
| `install --file <tar.gz> --sig <minisig>` | Same, from an uploaded package |
| `rollback [--restore-db]` | Undoes the last update (see §6) |
| `status`, `history` | Installed version, last result; every update and rollback |

### Stages - everything that can fail happens before the site is touched

1. **Verify**: signature against `.boat-rudder/minisign.pub`.
2. **Extract** into `staging/<version>/src/`, refusing absolute paths, `..`, symlinks, devices and
   archives over a size limit.
3. **Prerequisites**: every tool and library the version needs is present - else stop with the
   list.
4. **Build** in `staging/<version>/build/` (`cmake -DCMAKE_BUILD_TYPE=Release`). A failed build
   stops here, its log kept for the report; the site keeps running.
5. **Smoke test**: the new binary runs `--version`.
6. **New manifest** from the built tree: the binary, `scripts/`, `html/templates/`,
   `html/themes/`, `html/assets/` and the two default favicons - never `html/content/`,
   `configs/`, `ssl/` or `data/`.
7. **Plan**: comparing the installed manifest with the new one gives the files to **replace**,
   **add** and **delete** (system files the new version no longer ships). A file to replace whose
   SHA-256 differs from the installed manifest was **edited on the host**: listed in the report.
   *(Added in 0.0.6: a file is deleted only if the installed version's official, signed package
   shipped it - a site installed from its own repository has its own files in the manifest too.
   See [scripts.md](../reference/scripts.md#scriptsboat-rudder-update).)*

---

## 5. Applying an update

1. **Backup** to `.boat-rudder/backups/<date>-<from>-to-<to>/`: the current content of every file to
   replace or delete (`files.tar.gz`), the list of files to add (`added`), the old manifest, `VERSION` and key, and a
   `mongodump` of the site database.
2. Stop the service - for seconds only, the build is already done - so no request sees a mix of old
   and new templates.
3. Write each file under a temporary name and `rename()` it into place; delete the removed ones;
   write the new `manifest.sha256` and `VERSION`.
4. Start the service.
5. **Health check** (up to ~30 s): `/` and `/login` answer, the active theme's
   `styles_epoch3.css` is served, the log shows MongoDB connected.
6. Pass: record in `history.jsonl`, delete `staging/<version>/`. Fail: **automatic rollback** (§6),
   then record the failure.

The updater replaces itself like any file in `scripts/`: through `rename()`, so the running copy
finishes on the old inode.

---

## 6. Rollback

From an update's backup:

1. Stop the service.
2. Restore `files.tar.gz` (the replaced and deleted files).
3. Delete the files the update added (`added`).
4. Restore the previous `manifest.sha256` and `VERSION`.
5. Start, health check.

The database is restored only with `--restore-db`: by default an entry written after the update is
kept. Each backup undoes **one** update; going back two versions is two rollbacks, newest first.
Backups hold only what changed, so they are small; the last 5 are kept.

---

## 7. Dashboard - `/dashboard/settings/updates`

Admin only, epoch 3, linked from *More settings*:

- Installed version; latest available, with its `CHANGELOG.md` section.
- *Update*, *Upload package* (`.tar.gz` + `.minisig`) and *Roll back*, each confirming the admin's
  password again, on top of the session and CSRF token.
- Progress by stage (verify, build, backup, apply, health), the build log when it fails, the list
  of host-edited files replaced.
- History of updates and rollbacks.

The page never updates the server itself: it writes the request to `.boat-rudder/request.json`,
runs `systemctl start boat-rudder-update.service`, then polls `.boat-rudder/progress.json`, and
reconnects on its own across the restart.

---

## 8. Security

- No unsigned code is ever compiled or installed - a stolen admin session can upload a package,
  but not one the maintainer didn't sign.
- Downloads only over HTTPS from `github.com` / `api.github.com`; the signature, not the transport,
  is what's trusted.
- Archive extraction as in §4 (zip-slip, symlinks, size).
- Every run is logged in `history.jsonl` and the server log: who, what version, from where, result.
- The dashboard upload goes through the existing multipart limits; the updater, not the HTTP
  server, reads the package.

---

## 9. Phases

| Phase | Content | Result |
|---|---|---|
| 1. Baseline - **done** | `VERSION` and `boat-rudder --version`; `scripts/manifest.sh` (`create`, `adopt`, `verify`); `install.sh` writes `.boat-rudder/` (`VERSION`, `manifest.sha256`, and `minisign.pub` once phase 2 adds it); `manifest.sh adopt` builds the manifest of an existing install from the source of its version and reports host edits ([scripts.md](../reference/scripts.md#scriptsmanifestsh)) | Every install knows exactly what it is |
| 2. Releases - **done** | Signing key pair; `scripts/release.sh` ([scripts.md](../reference/scripts.md#scriptsreleasesh)) | Signed source packages on GitHub |
| 3. Command-line updater - **done** | `boat-rudder-update`: check, install, rollback, status, history - §4-§6 ([scripts.md](../reference/scripts.md#scriptsboat-rudder-update)) | Safe updates and rollbacks over SSH |
| 4. Dashboard - **done** (but *Upload package*) | `/dashboard/settings/updates`: version, check, notes, *Update*, *Roll back* through `systemd-run … boat-rudder-update run-request` ([dashboard.md](../reference/dashboard.md), [scripts.md](../reference/scripts.md#scriptsboat-rudder-update)); *Upload package* - later | The same from the browser |

Phases 1-3 remove the risk; phase 4 is convenience.

---

## 10. To settle when implementing

- Where the private key lives (maintainer's machine, passphrase-protected) and its backup.
- How many backups to keep (proposed 5), and whether old `mongodump`s go with them.
- `install.sh` stays for first installs and writes the initial `.boat-rudder/`.

## 11. Documentation to update on implementation

`scripts.md` (release, updater, `install.sh`), `security.md` (signing, extraction rules, the
update route), `routes.md`, `dashboard.md`, `architecture.md` (the `.boat-rudder/` tree), `README.md`
(updating a site), `CHANGELOG.md`.

---

## Contact

For contact or more information:

**Jonathan Pablo Toledo Moya**
theretrocenter.com@gmail.com
