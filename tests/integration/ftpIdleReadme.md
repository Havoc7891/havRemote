# Read-only FTP idle-timeout test

This opt-in Windows test connects to an explicitly selected saved FTP/FTPS
endpoint, lists its initial directory, waits 12 seconds without calling the
protocol backend, and lists the same directory again. It repeats the idle/list
cycle and prints timestamped, sanitized diagnostics and operation outcomes.
It checks that the normal Info message `Reconnected to the FTP server` appears
once for each replacement control connection, without requiring debug tracing.
The initial connection, owner/group listing fallback, and immediate repeated
refreshes must not produce additional reconnection messages.
Use a dedicated test endpoint configured with a shorter server idle timeout
(for example, 10 seconds), not a production account.

## Temporary application diagnostics

Start havRemote with `HAVREMOTE_FTP_CONTROL_TRACE=1` in its environment to
enable the same diagnostics in the message log. For example, in PowerShell:

```powershell
$env:HAVREMOTE_FTP_CONTROL_TRACE = '1'
.\build\mingw-debug\havRemote-debug.exe
```

Sessions read this flag when they connect. Unset it before the next launch to
disable logging (`Remove-Item Env:HAVREMOTE_FTP_CONTROL_TRACE`). The diagnostic
does not change configuration, inject FTP commands, poll idle sockets, or
change libcurl's connection-reuse/retry policy. Existing TCP keepalive settings
remain unchanged. TCP keepalive is not an FTP `NOOP` command.

The optional `FTP control trace` stream logs at most 32 operations, 256
wire/lifecycle events, and 768 bytes per message body for each session.
Start/end summaries are retained within the operation limit even if the
wire-event budget is exhausted. This trace omits file contents, TLS records,
command arguments, and raw server reply text. Reply codes, connection IDs,
endpoints, elapsed idle time, selected libcurl lifecycle text, and final
CURLcode/FTP response/socket-error details remain visible. An end summary is
the backend result. The UI applies its normal operation context.

Normal upload-failure diagnostics are separate: they can include a bounded,
sanitized final `STOR` reply that libcurl actually delivered, even without this
trace enabled. They do not infer a server reply from a transport error. The
read-only idle test below never uploads.

## Running the read-only endpoint test

Build `havremote_integration_tests` with
`HAVREMOTE_BUILD_INTEGRATION_TESTS=ON` using the
[integration build instructions](README.md#running-through-ctest). After approving use of the chosen
endpoint's saved password for this read-only test, set these non-secret
environment variables in PowerShell:

```powershell
$env:HAVREMOTE_FTP_IDLE_HOST = 'your-test-server-address'
$env:HAVREMOTE_FTP_IDLE_PORT = '2122'
$env:HAVREMOTE_FTP_IDLE_USE_SAVED = '1'
.\build\mingw-debug\havremote_integration_tests.exe '[ftp-idle]' --reporter compact
```

Leave `HAVREMOTE_FTP_CONTROL_TRACE` unset for the normal-message check. It is
optional: set it to `1` to include the
[detailed diagnostics](#temporary-application-diagnostics) described above.
The reconnection Info messages must appear in either mode. The test also checks
that detailed control tracing stays absent when it is disabled.

The test uses the existing
[default application configuration](../../docs/configuration.md#files-and-storage-locations)
through the typed configuration repository. It only matches saved sites with that exact host and
port, excluding SFTP. If more than one site matches, it reports their non-secret
IDs, names, and protocols and stops before retrieving any password. Set
`HAVREMOTE_FTP_IDLE_SITE_ID` to select one unambiguously. Optionally filter by
`HAVREMOTE_FTP_IDLE_PROTOCOL` (`ftp`, `ftps-explicit`, or `ftps-implicit`).

Only the chosen site's password is read from Windows Credential Manager,
inside the test process. Passwords are never passed through environment
variables or command-line arguments, printed, or exported. The test does not
write configuration or credentials, and it will not create defaults if the
configuration is missing. Saved data-connection settings and existing
[endpoint-scoped TLS pins](../../docs/configuration.md#trust-data) are used. New
certificate exceptions are rejected.
If that saved entry does not reference a stored password, the test stops before
connecting. Save the password for that dedicated entry in Site Manager first.
Credentials belonging to another site are never used as a fallback.

Optional non-secret settings:

- `HAVREMOTE_FTP_IDLE_DIRECTORY`: override the directory to list.
- `HAVREMOTE_FTP_IDLE_SECONDS`: idle duration, from 11 to 30 seconds.
- `HAVREMOTE_FTP_IDLE_READ_FILE`: an exact, known-safe remote file to download
  read-only after the second refresh. The test first checks it is a regular file
  no larger than 16 MiB. The local copy is temporary and removed afterward.

No remote create, upload, rename, permission-change, or delete operation is
performed. Directory contents and downloaded file contents are not printed.
Without an explicitly named file, the test performs listings only. No FTP
keepalive command is sent during either idle interval. The reported `Connected()`
value is the backend's cached state, not a claim that an idle control socket is
still live.
The subsequent operation reports a reconnection if libcurl replaces it. Optional
control tracing additionally identifies individual reused/replaced connections.
