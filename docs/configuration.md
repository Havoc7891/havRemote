# Configuration

havRemote keeps its application configuration in a comment-preserving CSON
document. Most users should change preferences and saved sites through the
application, but the file remains readable and can be annotated by hand.

## Contents

- [Files and storage locations](#files-and-storage-locations)
- [Format version 1](#format-version-1)
- [Lossless saves and error handling](#lossless-saves-and-error-handling)
- [Transfer recovery state](#transfer-recovery-state)
- [Settings](#settings)
- [Saved sites, history, and workspace](#saved-sites-history-and-workspace)
- [Trust data](#trust-data)
- [Operation errors and authentication failures](#operation-errors-and-authentication-failures)
- [Localization catalogs](#localization-catalogs)
- [External editor](#external-editor)
- [Site exports and imports](#site-exports-and-imports)
- [Transfer-report exports](#transfer-report-exports)

## Files and storage locations

| Data                                                | Location                                                    | Notes                                                                                          |
| --------------------------------------------------- | ----------------------------------------------------------- | ---------------------------------------------------------------------------------------------- |
| Application configuration                           | `havRemote.cson` in the application configuration directory | Settings, update-check state, saved sites, history, workspace state, and FTPS trust exceptions |
| [Transfer recovery state](#transfer-recovery-state) | `queue.cson` in the same directory                          | Unfinished and failed transfers, errors, progress, and safe-resume metadata                    |
| [SFTP host keys](#trust-data)                       | `known_hosts` in the same directory                         | Standard OpenSSH `known_hosts` format, keyed by host and port                                  |
| Saved-site passwords and key passphrases            | Operating system's secure credential store                  | Services named `havRemote/<credential-id>`                                                     |
| [Remote-edit working copies](#external-editor)      | `edit` in the application's local data directory            | Private temporary copies retained when an edit cannot be uploaded safely                       |
| [Translation catalogs](#localization-catalogs)      | `translations/*.cson` beside the executable                 | Read-only application resources discovered at startup                                          |

wxWidgets selects the user's configuration and local-data directories for the
host platform. On Windows, configuration is under `%APPDATA%\havRemote` and
working copies are under `%LOCALAPPDATA%\havRemote\edit`.

When saved-site passwords or key passphrases are retained, they are stored only
through wxWidgets in Windows Credential Manager, macOS Keychain, or the Linux
desktop secret service. If secure storage is unavailable, the operation fails
without a plaintext fallback. Quick Connect passwords and
keyboard-interactive responses remain transient. Secrets, tokens, and private
key contents are never written to `havRemote.cson` or `queue.cson`. A saved site
may contain an opaque credential identifier and a private-key file path, but not
the secret or key contents themselves. Validation also rejects secret-bearing
field names, including password, passphrase, token, and private-key-content
variants, even when they appear inside an otherwise unknown extension object.
The update checker uses the public GitHub Releases API and never needs or stores
a GitHub account, access token, or repository secret.

Saving a changed password or key passphrase creates a new secure-store entry
before the configuration is saved. The previous entry is retained if saving
fails, and is retired after a successful save once no active connection uses it.
Editing only a username or another non-secret field retains the existing entry.

Deleting a saved site also retires its stored password and key passphrase once
no saved site or active connection uses them. The `pendingCredentialDeletions`
list records only their opaque identifiers in the same atomic save as the site
change. Failed or interrupted cleanup is retried on the next launch. Identifiers
are removed from the list after the secure-store deletion succeeds.

## Format version 1

The current and only supported application configuration version is 1. A
representative new document has this shape:

```cson
formatVersion: 1

settings:
  transferConcurrency: 2
  connectionTimeoutSeconds: 20
  commandIdleTimeoutSeconds: 60
  defaultConflictPolicy: "ask"
  theme: "dark"
  language: "en"
  fileLists:
    local:
      sortColumn: "name"
      sortAscending: true
      columnWidths: []
    remote:
      sortColumn: "name"
      sortAscending: true
      columnWidths: []
  updates:
    checkAutomatically: true
    lastCheckUnixSeconds: ""
    skippedVersion: ""
  externalEditor:
    mode: "system"
    executable: ""
    arguments: "{file}"

workspace:
  localDirectory: ""
  connectionDirectories: []
  openTabs: []
  selectedConnectionId: ""
  mainWindow:
    x: null
    y: null
    width: 1410
    height: 930
    maximized: false

quickConnectHistory: []
sites: []
siteFolders: []
siteManagerOrder: []
pendingCredentialDeletions: []
tlsTrust: []
```

`formatVersion`, `settings`, `sites`, and `tlsTrust` are required.
`settings.updates` and its `checkAutomatically`, `lastCheckUnixSeconds`, and
`skippedVersion` members are also required. The `workspace`,
`quickConnectHistory`, `siteFolders`, `siteManagerOrder`, and
`pendingCredentialDeletions` sections, together
with the `fileLists`, `externalEditor`, and `mainWindow` subsections, file-list
`columnWidths`, workspace `openTabs` and `selectedConnectionId`, and the
per-site `ftpDataConnectionMode` and `ftpActiveAddress` fields are optional
within version 1. havRemote supplies their defaults in memory and adds them on
a later save.

Unversioned documents and documents with a version other than 1 are rejected.
All known objects, field types, enum values, integers, ranges, identifiers, and
duplicate identities are validated before the configuration becomes active.
Unknown fields are allowed unless they look like secret-bearing data.

## Lossless saves and error handling

An existing file is parsed into a temporary havCSON lossless document and fully
validated before it replaces the active configuration. Saves use havCSON's
atomic lossless writer with two-space indentation and source-order object keys.
The writer may normalize some whitespace and numeric spelling, but ordinary app
edits preserve:

- leading, inline, block, closing, and trailing comments,
- blank lines and the order of existing members,
- unknown fields and sections,
- saved-site, Site Manager, and Quick Connect history ordering, and
- the lossless nodes belonging to saved sites, Site Manager folders, and
  ordering references, matched by their stable UUIDs.

New known fields are inserted in schema order. When the application removes a
known node, its attached comments are moved to the next logical sibling or the
end of the container instead of being discarded. Removed inline comments become
full-line comments. In bracketed containers they remain before the closing delimiter.

Parsing uses havCSON's source locations and a maximum container depth of 256.
Application configuration and queue files are limited to 64 MiB each. Translation
catalogs and site-import files are limited to 4 MiB each.
[Site imports](#site-exports-and-imports) also enforce their stricter folder-depth
and entry-count limits.

The file is created with defaults only when it does not exist. A parse error,
validation error, unsupported version, or failed read never overwrites an
existing file. The error dialog and message log identify the file and include a
line and column when available. Configuration writes stay disabled for that
application session. Correct or remove the file and then restart havRemote.

Close havRemote before editing `havRemote.cson` manually. The running process
keeps the active lossless document in memory, so a later application save could
replace external edits made after startup.

## Transfer recovery state

`queue.cson` is a separate, application-managed CSON document with format
version 1. It is written atomically but is not part of the lossless configuration
document, so it should not be used for comments or manually maintained settings.
It contains unfinished and failed transfer jobs, their connection and endpoint
identity, paths, progress, errors, and metadata needed to validate a safe resume.
It never contains passwords, key passphrases, or private-key contents.

Conflict renames are saved with the transfer so **Retry** uses the chosen
destination, including after a restart. A single-file job stores the resolved
destination in its job paths. A recursive job keeps its original roots and
records per-file choices in `job.destinationOverrides`. Each mapping contains
`originalLocalPath`, `originalRemotePath`, `localPath`, and `remotePath`. Remote
paths retain both their original bytes and display text. Empty mappings are
omitted. Job paths, destination mappings, and resume checkpoints are validated
together. Stale checkpoints for a previous destination are rejected.

Completed and canceled transfers are omitted rather than kept as permanent
history. Queued, enumerating, running, and paused work is restored in the paused
state after an application restart. Reconnect to the original endpoint and use
**Retry** explicitly. The queue displays live transfer speed and an estimated
time remaining only while enough measurable work is actively running. Resume
offsets and policy-driven skips do not inflate the rate.

**Remove** deletes the selected completed, failed, or canceled entry from the
current session. Removing a failed entry asks for confirmation because it also
discards that transfer's retry and recovery records. **Clear** removes every
entry in the Failed or Completed view and gives the same warning when retryable
failures are included. Safety staging files, including local
`.havremote.part` files and temporary remote uploads, are not deleted by either
command. A malformed or unsupported existing queue file is reported and left
untouched, and queue persistence remains disabled for that application session
instead of overwriting it. New transfers can still run and be retried within
that session, but their recovery state cannot be saved for the next startup.
Close havRemote before repairing or moving the invalid queue file, then restart
to enable persistence again.

## Settings

| Setting                        | Default  | Accepted values                                                                             |
| ------------------------------ | -------- | ------------------------------------------------------------------------------------------- |
| `transferConcurrency`          | `2`      | Integer from 1 through 16                                                                   |
| `connectionTimeoutSeconds`     | `20`     | Integer from 1 through 600                                                                  |
| `commandIdleTimeoutSeconds`    | `60`     | Integer from 1 through 3600                                                                 |
| `defaultConflictPolicy`        | `"ask"`  | `"ask"`, `"overwrite"`, `"skip"`, `"rename"`, or `"resume"`                                 |
| `theme`                        | `"dark"` | `"dark"` or `"light"`                                                                       |
| `language`                     | `"en"`   | 1-35 ASCII letters, digits, or hyphens. See [Localization catalogs](#localization-catalogs) |
| `updates.checkAutomatically`   | `true`   | Boolean                                                                                     |
| `updates.lastCheckUnixSeconds` | `""`     | Empty or a canonical unsigned decimal Unix timestamp string                                 |
| `updates.skippedVersion`       | `""`     | Empty or a canonical `MAJOR.MINOR.PATCH` version                                            |

The `updates` object and all three members are required in a format-version-1
file. The values above are used when havRemote creates a new configuration. The
timestamp is stored as a decimal string so Unix seconds remain exact even though
havCSON numbers use floating-point storage. Empty means that no update check has
completed. `skippedVersion` is normalized without a leading `v` and suppresses
automatic notices only for that exact version.

Automatic checks are enabled by default. At startup, havRemote queries the
latest published, non-prerelease GitHub Release only when no check attempt has
completed in the past 24 hours. Successful and failed completed attempts both
advance this timer to avoid repeatedly contacting GitHub. A manual **Help →
Check for Updates** request ignores both the timer and `skippedVersion`. A newer
release is never hidden by an older skipped version. Release metadata is fetched
over HTTPS without authentication. **Download** and **View release** hand public
URLs to the default browser, and havRemote does not install or replace files.
If the release has no matching download for the current platform and architecture,
the dialog still offers **View release** but disables **Download**.

The local and remote file lists remember their sort column, direction, and
column widths independently. Local lists accept `name`, `size`, `type`, and
`modified`. Remote lists also accept `permissions` and `owner`. `columnWidths`
is either empty to request the UI defaults, or contains four local or six remote
widths in that logical column order. Widths are stored as device-independent
pixels and must be integers from 32 through 4096. Resizing a column updates the
corresponding local or remote layout in every connection tab. Column display
order is fixed and is not a configuration field.

Language, default-conflict-policy, and external-editor changes apply
immediately. The conflict policy is copied into newly queued jobs. Existing
jobs keep the policy with which they were created. Connection and command-idle
timeouts apply to protocol sessions created afterward. Reconnecting a tab
ensures its browser session uses them, while existing browser and transfer
sessions are not interrupted. Theme and simultaneous-transfer changes are saved
immediately but require a restart.

After saving a theme or simultaneous-transfer change, Settings offers to restart
havRemote now or leave the new value for the next launch. The prompt warns about
unfinished transfers and active external-edit monitoring. Before restarting,
havRemote stops the workers and saves recovery state when persistence is
available. Reconnect the restored tabs and use **Retry** to continue transfers.
Finish and save remote edits before restarting, because editor monitoring does
not resume automatically in the new process.

FTP/FTPS file uploads and downloads use a separate, fixed low-speed policy:
libcurl aborts if its measured throughput stays below 1 byte/sec for 30 seconds.
This applies to passive and active FTP, including explicit and implicit FTPS,
and is not a total-transfer timeout. The Settings command-idle timeout still
governs FTP command responses and the existing SFTP policy. A low-speed timeout
leaves the transfer failed without automatically retrying. Partial files and
safe-resume checkpoints are retained. Each FTP upload/download logs the effective
low-speed limit and time at Debug level.

Failed FTP/FTPS uploads also log one bounded, sanitized final `STOR` reply when
libcurl delivers it. Observed `452` (insufficient storage) and `552` (storage
allocation/quota exceeded) replies take precedence over a generic transport
failure. `451` reports a server-local processing error without assuming quota
exhaustion. The original CURLcode, last FTP response code, error buffer, and
socket error remain in the details. If libcurl aborts the data transfer before
reading the final control reply, the log explicitly says it was unavailable and
the transport error remains the fallback.
Partial uploads and recovery checkpoints are retained. Retry remains explicit.

## Saved sites, history, and workspace

Saved sites contain a stable UUID, display name, protocol, endpoint, username,
authentication metadata, initial directories, FTP text encoding, an FTP
data-connection mode, and an optional active-mode local address. Supported
protocol values are `ftp`, `ftpsExplicit`, `ftpsImplicit`, and `sftp`.
Authentication values are `password`, `passwordKeyboardInteractive`,
`privateKey`, `agent`, and `keyboardInteractive`. FTP and FTPS profiles accept
password authentication only. Ad-hoc Quick Connect entries also use password
authentication. The other SFTP methods require a saved Site Manager profile.

`ftpDataConnectionMode` accepts `passive` or `active`. Passive is the default,
tries EPSV before PASV, and is recommended for most networks. Quick Connect
always uses passive mode.

In automatic active mode, havRemote passes `-` to libcurl's
`CURLOPT_FTPPORT`. After establishing the FTP control connection, libcurl reads
that socket's actual local endpoint, binds the data listener to the same local
address with port 0 so the operating system assigns an ephemeral port, and advertises that
address and assigned port with EPRT. PORT is the fallback for legacy IPv4
servers. The address is not derived from the server hostname: entering
`localhost` does not by itself select a loopback address. A loopback address is
used automatically only when it is the actual local endpoint of the established
control connection.

`ftpActiveAddress` is an optional unbracketed IPv4 or IPv6 literal. Its default
and canonical automatic value is the empty string. A non-empty value is passed
to `CURLOPT_FTPPORT`, so it is both the local listener bind address and the
address announced to the server in EPRT or PORT. havRemote verifies that the IP
address is assigned to a local network interface before asking libcurl to use
it. Hostnames, surrounding IPv6 brackets, zone IDs, whitespace, ports, URL
syntax, and non-local addresses are rejected.

Separate local bind and EPRT/PORT addresses are not supported. Leave the field
empty on an ordinary network. On a VPN or multi-homed computer, select it only
when another locally assigned address is also reachable from the server. When
the required server-reachable address is not assigned locally, use passive
mode or change the network configuration. The local firewall must still permit
the incoming connection to the ephemeral listener port.

Raw libcurl failure diagnostics are recorded at Debug level with the numeric
CURLcode, its `curl_easy_strerror()` text, and the error-buffer contents (or
`<empty>` when libcurl supplied none). The action that failed reports the error
with its own context, such as **Change permissions**. Expected cancellation or
pause messages can be shorter, and a recovered compatibility probe need not
become an action error. A successfully created active data listener emits one
concise Debug diagnostic naming the `NLST`, `MLSD`, `LIST`, `RETR`, or `STOR`
operation and its actual assigned listener endpoint. Control-only commands do
not configure or emit active data-channel diagnostics.

For a failed active data-channel operation, havRemote publishes a bounded,
sanitized trace containing the exact value passed to `CURLOPT_FTPPORT`, its
automatic (`-`) or configured origin, the established control connection's
actual local endpoint, and the listener's attempted bind target with requested
port `0`. After libcurl reports a successful bind, havRemote calls
`getsockname()` on the observed listener and records its actual address and
ephemeral port. It also checks `SO_ACCEPTCONN` after libcurl reports a
successful listen. If a stage or socket cannot be observed, the trace says so
instead of inferring it. File payloads, raw TLS records, and credential-bearing
command arguments are excluded. A numeric OS socket error is included when
libcurl exposes one through `CURLINFO_OS_ERRNO`.

When the server receives no EPRT or PORT command,
`CURLE_FTP_PORT_FAILED` (CURLcode 30) is consistent with address resolution,
socket creation, `bind()`, `getsockname()`, or `listen()` failing before the
command could be sent. CURLcode 55 (`CURLE_SEND_ERROR`) reports a send failure.
It does not establish whether listener setup succeeded or which command failed.
The FTP response code, including a successful response such as 200, is only the
most recent reply and need not belong to the failed command. Use the observed
listener stages, socket-error details, and client and server traces to identify
where the failure occurred.

[Validate active FTP and FTPS](../tests/integration/README.md#manual-active-mode-verification)
with commands that actually open a data channel:
`LIST` or `NLST` for a directory listing, `RETR` for a download, and `STOR` for
an upload. If all three succeed with explicit FTPS and active mode, the active
explicit-FTPS data path is validated. `RNFR` and `RNTO` run entirely on the
control connection, so a rename neither exercises nor validates active mode.
An FTP reply of 550 to `RNFR` is a normal server-side rename or source-path
failure and is reported as such. It is unrelated to active-listener setup and
must not be confused with CURLcode 55.

A version-1 saved site without either active-mode field is loaded with passive
mode and an empty local-address override, then gains the explicit fields
on its next save. SFTP sites must use passive mode and an empty override because
neither field applies to SFTP.

havRemote uses only the authentication method selected for a site and does not
silently fall back to another method. `privateKey` reads the selected OpenSSH or
PEM private-key file directly and does not consult an SSH agent. `agent` uses
identities that are already loaded and unlocked in an SSH agent. On Windows,
Pageant is selected when it is running, otherwise the Windows OpenSSH Agent
is used. Linux and macOS use the agent identified by `SSH_AUTH_SOCK`.
After choosing a backend,
it does not try the other one when no identity is accepted, nor does it fall
back to another authentication method. A successful connection records the
selected backend and accepted client public-key fingerprint in the sanitized
message log. This is the fingerprint of the client identity accepted for
authentication, not the server host-key fingerprint verified separately for
[server trust](#trust-data).

`keyboardInteractive` starts keyboard-interactive authentication directly and
passes every server prompt to the user. Choose it when the server supplies all
required prompts, which can include both a password and a TOTP code, within one
keyboard-interactive exchange. `passwordKeyboardInteractive` submits the saved
or requested account password first and then continues with the server's
keyboard-interactive prompts. Choose it when the server requires those two SSH
authentication steps in that order. The first password may be kept in the
operating system's secure credential store, but keyboard-interactive responses such as TOTP codes are
always transient.

`siteFolders` stores the Site Manager tree without changing connection
profiles. Each folder has a stable `id`, a display `name`, an empty or parent
folder `parentId`, and a `siteIds` array containing the stable UUIDs of its
direct sites. Sites absent from every folder remain at the tree root.
`siteManagerOrder` contains every site and folder UUID exactly once. Filtering
that list by parent produces the freely mixed drag-and-drop order at each tree
level. If this optional field is absent, folders appear before sites until the
order is saved. Folder UUIDs and assignments must be unique, every reference
must exist, and folder parent links may not form a cycle. Empty and nested
folders are retained. Site Manager requires distinct site and folder names
among siblings, including at the root. Names can be reused in different
folders. Stable UUIDs, not display names, identify saved sites.

Site Manager validates and saves its changes with either **OK** or **Connect**.
**Connect** then opens the selected site through its stable UUID, reusing an
existing tab when available. Folders cannot be connected. **Cancel** discards
the pending changes.

Duplicating a site creates a new site UUID in the same folder and copies its
connection settings. Duplicating a folder recursively creates fresh folder and
site UUIDs for the entire subtree, retaining names inside the copied folder.
Only the copied root entry needs a new, available name beside its original.
Credential identifiers are not shared, so a password or key
passphrase entered for any copy cannot replace an original site's secure-store
entry.

Successful unsaved Quick Connect endpoints are kept most-recent-first in
`quickConnectHistory`. At most 20 unique entries are stored. Each entry contains
only its protocol, host, port, and username: it never contains a password or a
credential identifier. An endpoint that is already represented by a saved site
is removed from Quick Connect history. Clearing the history also removes the
remembered workspace directories associated with those entries.

Each connection tab owns its local and remote browser state. The workspace
records a directory pair by saved-site UUID or, for an unsaved connection, by
its protocol/host/port/username identity. `workspace.localDirectory` is the
starting local directory for a new tab that does not yet have an identity.
Remembered directories take precedence when a known connection is reopened.
The explicit initial directories in a saved site are used when no remembered
directory pair exists yet. Its initial local directory is also the fallback if
a remembered local path can no longer be opened.

`workspace.openTabs` records up to 64 tabs in notebook order, including each
tab's stable connection ID, local and intended remote directory, and either a
saved-site UUID, a Quick Connect endpoint already retained in history, or no
connection identity for a blank tab. `workspace.selectedConnectionId` records
which of those tabs was active. Tabs are restored as disconnected browser
workspaces and never reconnect automatically. The user must initiate each
connection, so no transient credential is needed or persisted. Closing or
reordering tabs updates this state. A version-1 file without these optional
members opens recovery tabs when unfinished work exists, otherwise one new
blank tab, and gains the members on a later save.

`workspace.mainWindow` stores the restored position and size plus the maximized
state. A missing position centers the configured dimensions on the current
display. The defaults are 1410 x 930 device-independent pixels, scaled and
constrained to the available display area. Saved off-screen geometry is
constrained to a usable display, and child dialogs open centered on their
parent. Open **Preferences > Settings**, choose **Restore default size** in the
**Main window** row, then click **OK** to apply and save the default dimensions
on the current display. **Cancel** leaves the window placement unchanged.

## Trust data

FTPS validates certificate chains and hostnames using LibreSSL on Windows and
OpenSSL on Linux and macOS. Windows builds use native CA roots, while macOS
builds enable native trust evaluation. A permanent exception is an
endpoint-scoped public-key PIN stored in `tlsTrust`. Certificate
material and private keys are not stored there. A matching saved PIN can trust
the same host and port again, while a changed identity is blocked until the
user explicitly replaces the stored value. Normal certificate validation still
runs when a PIN is saved: the exception can override only a missing trust
anchor for a currently valid, cryptographically self-signed leaf certificate.
It cannot bypass expiration, detected revocation, a hostname mismatch, or other
certificate-validation failures. Plain FTP has no trust record and always
presents an insecure-connection warning.

SFTP host keys use the OpenSSH `known_hosts` format. Unknown keys can be trusted
once or saved permanently. Changed keys cannot be accepted transiently and
require explicit replacement.

## Operation errors and authentication failures

FTP/FTPS [control connections closed while idle](../tests/integration/ftpIdleReadme.md)
may be replaced by libcurl on the next operation. The last directory snapshot remains visible while idle.
It is not a live socket-health indicator. Each successfully authenticated
replacement connection produces one normal INFO message,
`Reconnected to the FTP server`, without requiring diagnostic tracing. Initial
connections, explicit disconnect/connect actions, and reuse of the same
connection do not produce this message. Failed authentication does not report
a successful reconnection. havRemote does not send periodic FTP commands
while idle.

When the SFTP server supplies a recognized status reply, operation errors
include its name and number, for example `Permission denied (SFTP status 3)`.
`Failure (SFTP status 4)` is a generic server response, not proof of a permission
problem. Server logs may be needed to identify the cause. Unknown status codes
retain their numeric value. Authentication, transport, and malformed-packet
errors retain the library diagnostic instead of being mislabeled with a status
left over from an earlier operation.

An authentication rejection during a browser operation, such as FTP reply
530, ends that tab's browser connection. A permission-change batch stops on
this session failure instead of continuing through the remaining selections or
refreshing the directory. Fix the credentials or authentication method and
reconnect explicitly. FTP authentication failures are not treated as an
unsupported MLSD command and do not trigger a LIST compatibility fallback.
Ordinary per-file permission failures do not necessarily disconnect the tab.
The batch reports them together and refreshes only when an operation succeeded
or its outcome is uncertain, provided the session remains usable.

## Localization catalogs

havRemote discovers every regular `.cson` file in the `translations` directory
beside the executable. English (`en`) and German (`de`) are shipped. The list in
the Settings dialog comes from the validated catalogs.

Each catalog has exactly these top-level members:

```cson
formatVersion: 1

language:
  code: "en"
  displayName: "English"

strings:
  "menu.file.title": "&File"
```

If the configured code is unavailable,
havRemote selects English when present, otherwise the first valid catalog in
deterministic filename order. Language codes are case-insensitive for selection
and must contain 1-35 ASCII letters, digits, or hyphens. They must start and end
with a letter or digit and cannot contain consecutive hyphens.

All catalogs must contain exactly the reference catalog's translation keys and
must preserve the same numbered placeholder counts, such as `{0}` and `{1}`.
Translation keys use case-sensitive, dot-separated camelCase segments, such as
`siteManager.defaultFolderName`. Each segment starts with an ASCII lowercase
letter and contains only ASCII letters and digits. Underscores are not allowed.
Translated values must be non-empty. One malformed or incomplete
catalog stops application startup with a file-specific error rather than
silently presenting a partially translated interface.

To add a language, copy [`translations/en.cson`](../translations/en.cson), change
only `language.code`,
`language.displayName`, and the string values, then build normally. CMake tracks
the catalog set and copies every source catalog into the runtime and release
bundle. havRemote validates the complete set when it starts.

The Help command follows the effective language selected from these catalogs.
It opens `help/<code>/index.html` from the application bundle and falls back to
the [English guide](../resources/help/en/index.html) when a localized help page is
unavailable. This fallback
applies only to the separate HTML guide. Translation catalogs themselves must
remain complete.

## External editor

The Settings dialog supports two modes:

- `system` opens a local file with its registered application. File
  types that could execute, install, import, or redirect content are blocked in
  this mode.
- `custom` starts the selected editor executable directly. Its argument
  template must contain `{file}` exactly once.

Custom arguments are parsed into an argument vector without invoking a command
shell. Quotes group arguments, and the expanded file path remains a single
literal argument even when it contains spaces or shell metacharacters.

Opening a remote file downloads a private working copy, launches the selected
editor, watches for stable saves, and queues uploads through the normal transfer
system. Before replacement, havRemote compares the recorded remote size and
modification time both before upload and immediately before committing the
temporary upload. A changed, deleted, or unverifiable remote revision requires
an explicit decision for that file. It cannot inherit an apply-to-remaining
choice. Working copies are retained and reported when launching, uploading,
verification, disconnection, or shutdown prevents a safe automatic save. A
failed upload remains available in the Failed queue view and
[persistent recovery state](#transfer-recovery-state), so reconnecting and retrying
it is preferred before manually uploading
the retained working copy.

## Site exports and imports

Site Manager's **Export...** menu saves the selected site, the selected folder
with its entire subtree, or all sites and folders to a dedicated `.cson` file.
It exports the dialog's current validated working copy without committing edits
to the application configuration. Existing files require overwrite confirmation.
The export is written atomically.

This is a separate format, not a copy of `havRemote.cson`. Its only supported
version is 1:

```cson
formatVersion: 1
kind: "havRemoteSites"
entries: [
  {
    kind: "folder"
    name: "Examples"
    entries: [
      {
        kind: "site"
        name: "Example server"
        protocol: "sftp"
        host: "example.com"
        port: 22
        username: "alice"
        authentication: "password"
        initialRemoteDirectory: "/"
        ftpEncoding: "UTF-8"
        ftpDataConnectionMode: "passive"
      }
    ]
  }
]
```

Nested `entries` arrays preserve mixed site/folder ordering, including empty
folders. The export contains only the listed portable connection fields. It
does not include passwords, passphrases, credential identifiers, key-file
contents or paths, initial local directories, active-mode local-address
overrides, trust records, workspace state, history, unknown configuration
fields, or source-configuration comments. Existing comments in the application's
configuration are not modified by an export.

**Import...** accepts this site-export format only. The complete file is validated
before presenting an import preview. New entries are placed inside the selected
folder, beside a selected site, or at the top level when nothing is selected.
Existing entries are never overwritten: conflicting imported names are proposed
as renamed copies and require confirmation. Imported entries receive fresh
UUIDs, and their folder references and ordering are rebuilt. Changes remain in
the Site Manager working copy until **OK** or **Connect** succeeds. **Cancel**
discards them without writing credentials or automatically trusting a server.

The selected authentication method is retained. Because local key-file paths
are not portable, imported private-key sites need a local private-key file
selected before **OK** or **Connect** can save them. Passwords must be entered
again as needed, local directory settings use their defaults, and an active FTP
site uses automatic local-address selection until configured otherwise.

Imports reject unsupported versions, unknown fields, duplicate keys or sibling
names, invalid types/enums/ranges, and excessive input. Limits are 4 MiB per
file, 10,000 entries, 32 folder levels, and 16 KiB per portable text field.
Connection names, hosts, usernames, and remote paths are still private metadata.
Review an export before sharing it.

## Transfer-report exports

In **Queue**, **Failed**, or **Completed**, use **Export...** on the button row or
context menu, then choose **Export selected...** or **Export all...**. Selection
export requires selected rows. Whole-view export requires a nonempty view.
The report is a snapshot taken before the save dialog opens, so subsequent
progress or state changes do not change what is written.

Reports are UTF-8 `.csv` files with a byte-order mark, comma delimiters, quoted
cells, and CRLF line endings. They contain localized column headers in the
displayed order and the displayed values, including complete error details.
These are human-readable, potentially rounded values rather than exact raw
job metadata. The Item column contains the displayed filename. Recursive jobs
are exported as one row. When importing a report into a spreadsheet, select
UTF-8 and comma as the delimiter if needed.

Cells are sanitized for secrets and control characters. A leading apostrophe
is added to formula-like cells so server-supplied text is not treated as a
spreadsheet formula. Exports preserve the current queue and do not contain
resume checkpoints or credentials. They cannot be imported to recreate or
resume transfers, and only entries still present in the selected view are
available.

Saving requires overwrite confirmation and uses a sibling temporary file before
atomic replacement. Failed writes preserve an existing report. Both site and
report exports block overwriting havRemote's active configuration, queue
recovery file, or trusted-host file, including equivalent paths.
