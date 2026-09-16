# Protocol integration tests

This opt-in suite runs the real `havremote_ftp` and `havremote_sftp` backends.
Its self-contained smoke tests use disposable Linux containers exposed only on
the IPv4 loopback interface (`127.0.0.1`) and require Docker Desktop (or Docker
Engine with Compose v2) and free local test ports. A separate
[active explicit-FTPS test](#external-server-active-explicit-ftps-test) uses
an externally provisioned server, without the Docker fixture. Both are separate
from the default unit tests. The
[synthetic SFTP READ regression](#synthetic-sftp-read-and-eof-regression) below needs
Python and Paramiko instead of Docker or an existing SSH server.

The separate [read-only FTP idle-timeout diagnostic](ftpIdleReadme.md) uses an
explicitly selected saved endpoint and its existing Windows credentials. It
does not use or reconfigure the Docker services below.

The [ordinary Windows test suite](../../docs/building.md#test-configurations)
also runs scripted loopback FTP tests without Docker or an existing server.
Those cover connection replacement, authentication and listing failures,
upload restart, observed storage/processing replies, and data-socket resets.
The same guide explains how to run the longer opt-in upload/download low-speed
tests. These fault-injection tests do not demonstrate real server quota
enforcement or a complete FTPS interoperability matrix.

The credentials, TLS private key, and certificate in this directory are public
test fixtures. They must never be reused outside this disposable harness.

## Running through CTest

For the MinGW example, first follow the [setup instructions](../../docs/building.md#select-the-mingw-installation),
which define the `mingw-debug-local` user preset used below. Configure with both
test options enabled, build, and select the Docker label. On the supported
Windows build, run these commands from the repository root in PowerShell or
`cmd.exe` while Docker Desktop is using Linux containers:

```powershell
cmake --preset mingw-debug-local -DHAVREMOTE_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset mingw-debug-local --target havremote_integration_tests
ctest --test-dir build/mingw-debug -L docker --output-on-failure
```

For the [Visual Studio preset](../../docs/building.md#visual-studio), the
equivalent commands are:

```powershell
cmake --preset msvc -DHAVREMOTE_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset msvc-debug --target havremote_integration_tests
ctest --test-dir build/msvc -C Debug -L docker --output-on-failure
```

The Visual Studio integration suite has not yet been verified. The remaining
Windows examples use MinGW paths. For Visual Studio, select the matching
configuration directory for executables and pass `-C Debug` or `-C Release` to CTest.

The `docker` label selects only the disposable container fixture and its
protocol tests. The broader `integration` label also selects the manual
active-FTPS and saved-endpoint idle tests, which contact their selected servers
when their opt-in environment variables are set.

The same fixture can be run locally with the
[Linux sanitizer preset](../../docs/building.md#linux-headless-checks) by selecting
only the plain FTP and SFTP cases. Linux builds also support FTPS through
OpenSSL. Selecting only FTP and SFTP does not validate FTPS interoperability.

CTest uses a fixture setup/cleanup pair. Setup builds the images and runs
`docker compose up --wait`. Cleanup runs `docker compose down --volumes
--remove-orphans` after the protocol tests, including test failures. If CTest
is terminated before cleanup, follow the cleanup instructions below. The fixed
loopback ports are:

| Service       | Control port | Passive data ports |
| ------------- | -----------: | -----------------: |
| FTP           |         2121 |        30000-30009 |
| Explicit FTPS |         2122 |        30100-30109 |
| SFTP          |         2222 |                n/a |

For server debugging independent of CTest:

```sh
docker compose -f tests/integration/docker-compose.yml up --build --wait
docker compose -f tests/integration/docker-compose.yml logs -f
docker compose -f tests/integration/docker-compose.yml down -v
```

CTest uses a build-directory-specific Compose project named `havremote-<hash>`,
not the standalone project's default name. After an interrupted CTest run,
use the same build directory to inspect its registered cleanup command, then
run that cleanup test:

```powershell
ctest --test-dir build/mingw-debug -N -V -R '^integration\.docker\.cleanup$' -FS '^havremote_protocol_servers$'
ctest --test-dir build/mingw-debug -R '^integration\.docker\.cleanup$' -FS '^havremote_protocol_servers$' --output-on-failure
```

This uses the exact project name recorded by CMake and stops its disposable
fixture containers and removes their volumes. The
[`-FS` option](https://cmake.org/cmake/help/v4.0/manual/ctest.1.html#cmdoption-ctest-FS)
explicitly prevents automatic fixture setup during cleanup. Do not substitute
a different build directory or a production Compose project. The standalone
`down -v` command above cleans up only the independently started fixture.

## Fixtures and automated coverage

- Both FTP services use pyftpdlib over the same UTF-8 fixture tree. FTPS
  requires AUTH TLS for its control and data channels and presents the checked-
  in self-signed certificate, which covers `localhost` and `127.0.0.1`.
- The FTP fixture confines clients to a disposable namespace but starts them in
  `/login` instead of `/`. This verifies that raw commands such as `MKD`,
  `RNFR`/`RNTO`, `DELE`, `RMD`, and `SITE CHMOD` address the same login-relative
  paths as `STOR`, `RETR`, and directory-listing URLs.
- SFTP uses Alpine OpenSSH, password authentication, dynamically generated
  container host keys, and the same fixture tree under
  `/home/havremote/fixture`.
- The tree contains an empty file, a nested directory, a UTF-8 filename, and a
  known text file.
- Each protocol smoke test covers connect/authenticate, MLSD/SFTP browsing,
  UTF-8 listing, recursive directory creation/removal, remote empty-file
  creation, rejection of an existing file unless overwrite is requested,
  upload, stat, rename, permission changes with mode validation, cooperative
  pause, safe upload/download resume, cancellation, and final content comparison.
  Upload resume seeds a deterministic prefix under the recorded temporary
  remote name, queries its size, resumes the full source at that exact offset,
  checks progress through the full source size, and verifies the finalized
  file byte for byte after downloading it. The temporary name must be gone
  after finalization. FTP and FTPS exercise `REST` followed by `STOR` here.
  SFTP runs the same content and finalization checks using its own backend.
  Download finalization also replaces an existing destination and proves that a
  pause at completion leaves the pre-existing destination intact.
- FTP verifies that the insecure-connection diagnostic is emitted.
- FTPS verifies rejection without a trust callback, confirms no real
  credential is requested before the trust decision, accepts the extracted
  leaf SPKI once, rejects a changed stored PIN under the generic accept-once
  action, accepts explicit replacement, and finally performs the smoke test
  using the fixture's endpoint-scoped SPKI PIN.
- SFTP accepts an unknown host key permanently, verifies that an OpenSSH
  `known_hosts` file was atomically written without a leftover temporary file,
  reconnects, and proves that the stored key is reused without another trust
  prompt. Its listings and uploaded-file metadata are also checked for
  permissions and non-empty owner and group values. File-creation checks also
  cover existing directories, a dangling symlink, permission failures, and
  cancellation without creating a file.

FTP/FTPS upload resume follows
[RFC 3659 section 5.3](https://www.rfc-editor.org/rfc/rfc3659#section-5.3):
`REST <remote-partial-size>` followed immediately by `STOR` to the same partial
name, with the local source positioned at that byte offset. The remote size
must match the recorded resume offset and must not exceed the local source
size. The partial file is renamed to its final name only after the remaining
upload succeeds. A server that rejects `REST` or the resumed `STOR` reports
that it does not support upload resume. havRemote has no `APPE` fallback.
For ProFTPD, the test directory must allow store restarts through
[`AllowStoreRestart on`](https://github.com/proftpd/proftpd/blob/master/doc/modules/mod_xfer.html#L68-L89)
and must not enable `HiddenStores`, which blocks `REST`.

## Synthetic SFTP READ and EOF regression

With `HAVREMOTE_BUILD_INTEGRATION_TESTS=ON`, build
`havremote_integration_tests` using the [build instructions above](#running-through-ctest).
Create a temporary Python environment and run the read-only synthetic server/runner
from the repository root:

```powershell
py -m venv "$env:TEMP\havremote-sftp-read-ahead"
& "$env:TEMP\havremote-sftp-read-ahead\Scripts\python.exe" -m pip install paramiko
& "$env:TEMP\havremote-sftp-read-ahead\Scripts\python.exe" tests/integration/sftpReadAheadServer.py .\build\mingw-debug\havremote_integration_tests.exe
```

The runner creates an ephemeral loopback listener, SSH host key, and password,
then runs only `[sftp-read-ahead]` and stops the server. The server exposes no
host files and refuses modifications. Test downloads use disposable temporary
directories. No application configuration or existing SSH trust is changed.
Running the test executable without the runner's environment skips these cases.

The server records actual SFTP READ offsets and lengths. Coverage includes an
empty file, one byte, aligned/unaligned tails, a 209,715,200-byte file, nonzero
resume offsets, and a resume already at EOF. The application clamps each read
buffer to its remaining known byte count and stops calling the read API when
that count is reached. libssh2 speculates up to four buffers
ahead in roughly 30,000-byte READ packets, so already-outstanding wire requests
can cross the STAT size or receive normal EOF responses. The tests require a
bounded tail within that window plus packet rounding, contiguous requests and
an exact delivered byte total.
A one-byte file and resumed one-byte tail must each issue exactly one four-byte
speculative READ, proving both the caller's remaining-length clamp and absence
of a redundant EOF-probe call. Empty files and resumes already at EOF issue no
READ requests. Downloaded content is checked byte for byte, and logical/physical
progress and partial-file finalization are checked too. For large files, the
server temporarily withholds the first reply
until three READ requests arrive: this verifies that pipelining remains enabled,
not merely that final request offsets look correct. A bounded timeout releases
a serialized client so this assertion fails without hanging.

Unknown-size files still use bounded speculative reads until `SSH_FX_EOF`.
Outstanding EOF replies must not turn a successful download into an error.
Resume offsets beyond the known file size and nonzero resumes with an unknown
remote size are rejected before opening the remote file, preserving the local
partial file.
Truncation and growth after STAT must retain the partial file and report a
changed-file error instead of finalizing incomplete content or reporting a
protocol failure. The runner prints request counts, maximum requested ends,
EOF replies, and observed pipeline depth for diagnosis. This is a correctness
regression, not a throughput benchmark or a replacement for OpenSSH/ProFTPD
interoperability tests.

For a Windows executable launched from WSL, the runner adds its three test
variables to `WSLENV` while preserving existing entries. If loopback forwarding
is unavailable, use `--bind <local-WSL-address> --host <same-address>` with a
disposable local test environment where Windows can reach that address.

## Manual active-mode verification

Active FTP requires the server to connect back to the client, so verify
it with a server that can directly reach the client rather than through the
loopback-published Docker fixture. For a server running in the usual WSL 2 NAT
network, obtain its current address with `wsl.exe hostname -I` and use that WSL
address as the saved site's host instead of `localhost`.

1. Select **Active** for the saved FTP or explicit-FTPS site and leave **Active
   mode local address (optional)** empty.
2. Connect, refresh the directory listing, download a small file, and upload a
   small file. The connection check uses `NLST`. Browsing starts with `MLSD`
   and may use `LIST` as a fallback or to supplement owner/group metadata.
   Downloads use `RETR`, and uploads use `STOR`.
3. Confirm that each operation completes and that the server sees an EPRT (or
   legacy IPv4 PORT) command followed by the corresponding `NLST`, `MLSD`,
   `LIST`, `RETR`, or `STOR` command. If browsing, downloading, and uploading
   succeed with explicit FTPS, active explicit FTPS is validated.
4. Confirm that havRemote emits one concise listener-created diagnostic for
   each actual data connection, naming its command and the system-assigned
   listener endpoint. A directory refresh may create multiple data connections.
   It must not emit such a line for rename or another control-only command.
5. Confirm on the server that EPRT (or the legacy IPv4 PORT fallback) contains
   the same address and the system-assigned ephemeral port.

Do not use rename as evidence for active-mode behavior. `RNFR` and `RNTO` are
control-channel commands and do not create a data connection. In particular,
an FTP reply of 550 to `RNFR` is a normal rename or source-path failure, not an
active-mode transport failure and not CURLcode 55.

## External-server active explicit-FTPS test

The data-channel sequence is also available as a hidden, opt-in integration test
after configuring with `HAVREMOTE_BUILD_INTEGRATION_TESTS=ON` and building
`havremote_integration_tests`. It uses automatic active-mode address selection.
Plain active FTP and explicit local-address overrides still require the [manual
procedure above](#manual-active-mode-verification). Set
`HAVREMOTE_ACTIVE_FTPS_HOST`, `HAVREMOTE_ACTIVE_FTPS_USERNAME`, and
`HAVREMOTE_ACTIVE_FTPS_PASSWORD`. Optionally set
`HAVREMOTE_ACTIVE_FTPS_PORT`, `HAVREMOTE_ACTIVE_FTPS_DIRECTORY`, and
`HAVREMOTE_ACTIVE_FTPS_PIN`. Then run:

```powershell
ctest --test-dir build/mingw-debug -R integration.active-ftps --output-on-failure
```

Use a disposable test endpoint and a directory where the test account may
create and delete files. Without a supplied PIN, the test automatically accepts
an eligible self-signed certificate once for that connection. Set
`HAVREMOTE_ACTIVE_FTPS_PIN` to the expected public-key PIN when you need to
verify that endpoint identity.

The test uploads a prefix to a uniquely named file, resumes it with
`REST <offset>` followed by `STOR`, and verifies the complete downloaded
content. Each upload must create its own active `STOR` listener. It uploads
directly to the unique file so rename cannot obscure a data-channel failure.
Temporary-name finalization is covered by the Docker smoke tests. The server
must support [upload restart](#fixtures-and-automated-coverage) as described above.
The test removes the file
after its data-channel checks succeed. A failed assertion or interruption
before cleanup can leave the test file behind. Inspect the selected directory
after a failed run. Without
`HAVREMOTE_ACTIVE_FTPS_HOST`, the test skips without contacting a server.

## Active-mode diagnostics

On an active-mode failure, retain the application message-log excerpt and the
server trace together. When the failing data operation creates a listener,
havRemote reports the exact FTPPORT source, control endpoint, observable
socket/bind/listen stages, available socket-error details, and a bounded
sanitized libcurl trace. File payloads, raw TLS records, and credential
arguments are excluded. A successful observed bind reports the actual
ephemeral address and port through `getsockname()` even if sending EPRT or PORT
subsequently fails. The endpoint is reported as unavailable only when the bind
did not succeed or curl's listener marker/socket could not be observed.
Listener details are not repeated for control-only commands or when no new
listener is created.

An explicit [`ftpActiveAddress`](../../docs/configuration.md#saved-sites-history-and-workspace)
can be tested separately, but it must be assigned to the client. Check the
system's interface listing, such as `Get-NetIPAddress` on Windows, `ip address`
on Linux, or `ifconfig` on macOS. It is both libcurl's local bind address
and the EPRT/PORT address. An address that exists only inside WSL, behind NAT, or
on another host is not an advertised-only override and must be rejected.

Interpret the primary failure codes with the server trace. If no EPRT or PORT
arrives, CURLcode 30 (`CURLE_FTP_PORT_FAILED`) is consistent with local active
setup failing first. CURLcode 55 (`CURLE_SEND_ERROR`) identifies a send failure,
but neither that code nor an earlier FTP reply such as 200 proves that a
listener was prepared or that EPRT/PORT was the failing command. The response
code is the most recent FTP reply, not necessarily a reply to the failed
command. Use the observed socket/bind/listen stages and command trace to locate
the failure.

## Additional test scenarios

These scenarios require testing beyond this smoke harness:

- implicit FTPS and a publicly trusted FTPS certificate chain.
- active FTP/FTPS inside the Docker fixture. Its loopback-published servers
  do not provide a portable callback route to the client's active listener.
  The [external-server test](#external-server-active-explicit-ftps-test)
  automates active explicit FTPS with automatic address selection when a
  separately provisioned server can reach the client. Plain active FTP and
  explicit active-mode local-address overrides remain manual checks against
  such a server. The override must be an IP address assigned to the
  test computer because it controls both the listener bind and the EPRT/PORT
  address. The application does not expose a separate advertised-only NAT
  address.
- encrypted RSA/ECDSA/Ed25519 key-file authentication, SSH agent authentication
  (including Pageant), and keyboard-interactive authentication.
- changed-host-key replacement (the unknown-key and matching-key paths are
  covered, but rotating a live container key needs a separate destructive
  fixture).
- files larger than 4 GiB, sparse-file preservation, filesystem quota/disk-full
  behavior, and server-side quota errors.
- forced network loss, half-open sockets, DNS failure, reconnect sequencing,
  and an uncertain destructive-operation result.
- certificate expiry and hostname mismatch (automatic extraction of a
  self-signed leaf SPKI and changed-PIN replacement are covered).
- a broad matrix of FTP `LIST` dialects (those remain deterministic unit tests)
  or filesystem-specific destination-name collisions.

The large-file and fault-injection cases need a separately provisioned test
environment with explicit storage/time budgets and a controllable network
proxy. They should not be inferred from a passing integration smoke test.
