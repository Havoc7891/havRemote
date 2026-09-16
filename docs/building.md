# Building havRemote

havRemote can build its desktop application on Windows and Linux. The source
also provides macOS platform support, which still needs a native build and
runtime verification. The audited release packaging remains Windows-only.
For UI-free checks, use the [Linux headless configuration](#linux-headless-checks).

## Contents

- [Windows build requirements](#windows-build-requirements)
- [Select the MinGW installation](#select-the-mingw-installation)
- [Configure, build, and test with MinGW](#configure-build-and-test-with-mingw)
- [Visual Studio](#visual-studio)
- [Visual Studio Code](#visual-studio-code)
  - [Windows](#windows)
  - [Linux and WSL](#linux-and-wsl)
  - [Simulate update checks](#simulate-update-checks)
- [Dependencies and version overrides](#dependencies-and-version-overrides)
- [Test configurations](#test-configurations)
- [Linux desktop build](#linux-desktop-build)
- [Linux headless checks](#linux-headless-checks)
- [Relevant build files](#relevant-build-files)

## Windows build requirements

Use a C++23 compiler and standard library that pass the project's capability
checks and are supported by the selected dependency versions. Keep the
compiler, linker, Windows SDK or MinGW headers, and dependency binaries
compatible with one another.

| Component                     | Requirement                                                  |
| ----------------------------- | ------------------------------------------------------------ |
| Host                          | Windows 10 or 11, x64                                        |
| CMake                         | 4.0 or newer, using the native Windows executable            |
| Compiler and standard library | Required C++23 features, verified by compile-and-link probes |
| Platform tools                | Matching Windows SDK or MinGW distribution                   |
| Language mode                 | C++23 without compiler extensions                            |

The tested MinGW configuration uses CMake 4.2.3 and GCC 15.2.0 with MinGW-w64
runtime v13. A [Visual Studio 2022 x64 preset](#visual-studio) is also provided.
Use a separate build directory for each toolchain.

The examples below use PowerShell. `cmd.exe` also works with its environment
variable syntax. The Windows presets require Windows `cmake.exe`, Windows
paths, and the selected toolchain's environment.

The build also requires:

- Git, because dependencies are obtained through CMake `FetchContent`,
- an MSYS2 or Git for Windows Bash with `/usr/bin/cygpath` and
  `/usr/bin/perl`, used by LibreSSL's source-generation step even when compiling
  with MSVC.

CMake searches for a compatible Bash on `PATH`, below `MSYS2_ROOT`, in the
usual Git for Windows locations, and in the ancestors of a selected MinGW
installation when one is configured. Set `HAVREMOTE_BASH_EXECUTABLE` as a CMake
cache variable if Bash is installed elsewhere.

## Select the MinGW installation

This section applies only to the `mingw-debug` and `mingw-release` presets,
which use `MinGW Makefiles`. The tested distribution targets
`x86_64-w64-mingw32` with UCRT, POSIX threads, and SEH exceptions.
Both Debug and Release verify the shared C++23 capabilities. MinGW GCC builds
also locate and link-test the required static runtime archives. The MinGW
toolchain rejects a top-level MSYS environment (`MSYSTEM` set to a nonempty
value), but can still invoke Bash for LibreSSL's source preparation.

Keep the selected distribution's GCC license, GCC Runtime Library Exception,
MinGW-w64 runtime license, and winpthreads license files as well. MinGW
configuration locates them for both Debug and Release, not only when packaging.
For nonstandard layouts, see the license-file cache overrides in
[Runtime archive contents](releasing.md#runtime-archive-contents).

Set `HAVREMOTE_MINGW_ROOT` to the distribution's `mingw64` directory, not its
`bin` directory. The checked-in toolchain file resolves `gcc.exe`, `g++.exe`,
`windres.exe`, `mingw32-make.exe`, and the associated binary utilities below
that root. It also places the matching `bin` directory first on the build
process's `PATH`, preventing tools or runtime DLLs from an unrelated MinGW
installation from being selected.

For a persistent, per-checkout setting, create `CMakeUserPresets.json` in the
repository root with this content, replacing the example root:

```json
{
  "version": 9,
  "configurePresets": [
    {
      "name": "mingw-local-environment",
      "hidden": true,
      "environment": {
        "HAVREMOTE_MINGW_ROOT": "C:/Toolchains/MinGW/mingw64"
      }
    },
    {
      "name": "mingw-debug-local",
      "inherits": ["mingw-debug", "mingw-local-environment"]
    },
    {
      "name": "mingw-release-local",
      "inherits": ["mingw-release", "mingw-local-environment"]
    }
  ],
  "buildPresets": [
    {
      "name": "mingw-debug-local",
      "configurePreset": "mingw-debug-local"
    },
    {
      "name": "mingw-release-local",
      "configurePreset": "mingw-release-local"
    }
  ],
  "testPresets": [
    {
      "name": "mingw-debug-local",
      "configurePreset": "mingw-debug-local",
      "output": {
        "outputOnFailure": true
      }
    },
    {
      "name": "mingw-release-local",
      "configurePreset": "mingw-release-local",
      "output": {
        "outputOnFailure": true
      }
    }
  ]
}
```

`CMakeUserPresets.json` is ignored by Git.

Alternatively, set the variable in the current PowerShell session and use the
checked-in presets directly:

```powershell
$env:HAVREMOTE_MINGW_ROOT = 'C:/Toolchains/MinGW/mingw64'
```

The toolchain also accepts an explicit CMake cache value when a custom
configuration is needed:

```powershell
cmake -S . -B build/custom -G 'MinGW Makefiles' `
  -DCMAKE_TOOLCHAIN_FILE=cmake/toolchains/mingw-gcc.cmake `
  -DHAVREMOTE_MINGW_ROOT=C:/Toolchains/MinGW/mingw64
```

## Configure, build, and test with MinGW

With the [local user presets](#select-the-mingw-installation) above, a Debug build is:

```powershell
cmake --preset mingw-debug-local
cmake --build --preset mingw-debug-local
ctest --preset mingw-debug-local
```

The resulting application is `build/mingw-debug/havRemote-debug.exe`.

For Release:

```powershell
cmake --preset mingw-release-local
cmake --build --preset mingw-release-local
ctest --preset mingw-release-local
```

The resulting application is `build/mingw-release/havRemote.exe`. Release
packaging additionally audits the executable and bundled DLLs for unintended
runtime dependencies.

When `HAVREMOTE_MINGW_ROOT` is already in the process environment, use the
shared `mingw-debug` and `mingw-release` presets instead of their `-local`
counterparts:

```powershell
cmake --preset mingw-release
cmake --build --preset mingw-release
ctest --preset mingw-release
```

Set `CMAKE_BUILD_PARALLEL_LEVEL` or use `--parallel` to choose the build job count:

```powershell
cmake --build --preset mingw-debug-local --parallel 8
```

The MinGW PowerShell helper performs the same configure-and-build sequence.
Without a toolchain specified through `-MinGwRoot` or `HAVREMOTE_MINGW_ROOT`,
it uses the matching `mingw-debug-local` or `mingw-release-local` preset from
`CMakeUserPresets.json`.

```powershell
.\scripts\build.ps1 -Configuration Debug -Test
.\scripts\build.ps1 -Configuration Release -Test -Package
```

Set `HAVREMOTE_MINGW_ROOT` in the current session or pass `-MinGwRoot` to use
the shared presets with a specific toolchain:

```powershell
.\scripts\build.ps1 -Configuration Debug -Test `
  -MinGwRoot 'C:/Toolchains/MinGW/mingw64'
```

The helper restores the session's environment and working directory afterward.
[Packaging](releasing.md#build-a-release) is available only for Release and also
generates the separate GNU debug-symbol archive. Configuration, building,
testing, and packaging all use the selected preset's build directory.

The build generates the offline help under `build/mingw-debug/help` or
`build/mingw-release/help`, including when using the `*-local` presets, replacing
`@HAVREMOTE_HELP_VERSION@` in the HTML headers with the project version from
`CMakeLists.txt`. The portable ZIP includes these generated pages. Open the
build-tree help to preview the version label. Changelog versions and dates
remain manually maintained release history.

## Visual Studio

Debug and Release application builds and headless test suites have been verified
with the Visual Studio 2022 MSVC 19.44 toolset and havCSON 0.5.1.

Install Visual Studio 2022 or its Build Tools with the desktop C++ workload and
a Windows SDK. Select a sufficiently recent MSVC toolset and standard library
for the required C++23 features. Git and LibreSSL's Bash/Perl tools
from [Windows build requirements](#windows-build-requirements) are still needed.

The `msvc` configure preset uses the Visual Studio 2022 generator and x64
architecture. Configure once, then select Debug or Release at build time:

```powershell
cmake --preset msvc
cmake --build --preset msvc-debug
ctest --preset msvc-debug
cmake --build --preset msvc-release
ctest --preset msvc-release
```

The presets use the `build/msvc` directory.
The application is written to `build/msvc/Debug/havRemote-debug.exe` or
`build/msvc/Release/havRemote.exe`, with the matching dependency DLLs and
application resources beside it.

MSVC builds retain CMake's default dynamic compiler runtime, `/MD` for Release
and `/MDd` for Debug. A machine running the Release build needs the matching x64
Microsoft Visual C++ Redistributable. Debug builds require the development
runtime and are not redistribution packages.

To stage a Release build and its application dependencies in a separate directory:

```powershell
cmake --install build/msvc --config Release --prefix build/msvc-staged
```

This does not bundle or install the Microsoft runtime redistributable. The
audited portable ZIP and detached GNU symbol workflow remains
[MinGW-specific](releasing.md). An MSVC Release build does not pass through
those packaging checks.

For another compatible toolchain or generator, use an independent build
directory and CMake's normal compiler/toolchain selection. Do not inherit a
MinGW preset for an MSVC or other non-MinGW build.

## Visual Studio Code

Open the checkout in a Windows or Linux VS Code window. For WSL, reopen the
folder in WSL. Install the workspace's recommended CMake Tools and C/C++
extensions in the environment where you will build and debug.

The checked-in tasks use `cmake` and `ctest` from `PATH`. `Ctrl+Shift+B`
configures the Debug preset and builds only the `havRemote` target. To build
Release, use **Tasks: Run Build Task** and choose
**CMake: build havRemote (Release)**. **Tasks: Run Test Task** offers Debug and
Release tasks, each of which builds all targets before running its CTest preset.

### Windows

The Windows tasks use `mingw-debug-local` and `mingw-release-local`, so create
the [local user-preset file](#select-the-mingw-installation) before using them.
Use native Windows CMake and make `gdb.exe` discoverable on the Windows `PATH`
before opening VS Code. For example:

```powershell
$env:HAVREMOTE_MINGW_ROOT = 'C:/Toolchains/MinGW/mingw64'
$env:PATH = "$env:HAVREMOTE_MINGW_ROOT/bin;$env:PATH"
code .
```

Choose **havRemote: Debug with MinGW GDB** and press `F5` to configure, build,
and debug the application. **havRemote: Attach MinGW GDB** attaches to a
running Debug application.

If the VS Code process has no MinGW directory on `PATH`, use
CMake Tools instead: select the `mingw-debug-local` configure preset, select
`havRemote` as the launch target, and run **CMake: Debug**. CMake Tools locates
GDB beside the compiler recorded in the CMake cache and supplies the preset
environment to the application.

For Visual Studio builds, select the `msvc` preset through CMake Tools and use
the Visual Studio debugger.

### Linux and WSL

Install the [Linux desktop build requirements](#linux-desktop-build), plus
GDB for debugging. The Linux tasks use the `linux-debug` and `linux-release`
presets. Make `gdb` available on the Linux `PATH`.

Choose **havRemote: Debug with Linux GDB** and press `F5` to configure, build,
and debug the application. **havRemote: Attach Linux GDB** attaches to a
running Debug application. With CMake Tools, select the `linux-debug`
configure preset, choose `havRemote` as the launch target, and run
**CMake: Debug**.

The application and desktop tests require a graphical session. For tests
without a graphical session, use the [Xvfb commands](#linux-desktop-build).

### Simulate update checks

Debug builds can exercise the real **Help > Check for Updates** worker, response
parser, and dialogs with canned responses, without contacting GitHub. In VS Code,
choose **havRemote: Simulate update check** on Windows or
**havRemote: Simulate update check (Linux)** on Linux, press `F5`, and select a
scenario. Or launch the Debug executable from PowerShell:

```powershell
./build/mingw-debug/havRemote-debug.exe --simulate-update=available
```

On Linux:

```sh
./build/linux-debug/havRemote-debug --simulate-update=available
```

The scenarios are `available`, `up-to-date`, `no-release`, `network-error`, and
`server-error`. This option is available only in Debug builds. Download and release
page actions are intercepted and logged: they do not open a browser or download
files. Real GitHub connectivity, TLS, and downloads still require a live-release
test.

Each scenario keeps its settings, queue, and `known_hosts` beside the executable
under `update-simulation/<scenario>`, without using the normal configuration
files or secure credential store. A fresh simulation configuration starts with automatic
update checks off. Use the Help menu for predictable manual testing, or enable
automatic checks in Settings to test them.
[Daily-check and skipped-version state](configuration.md#settings)
persist between runs of the same scenario. Manual checks bypass both restrictions.
Restarting from Settings retains the selected simulation scenario.

The initial local directory is also inside the simulation directory. This is a
development mode that mocks update traffic, not a sandbox for manual file or
server operations.

## Dependencies and version overrides

Dependency sources are fetched from Git during configuration. The defaults are
centralized in [`cmake/DependencyVersions.cmake`](../cmake/DependencyVersions.cmake):

| Dependency | Default ref      | Build role                                               |
| ---------- | ---------------- | -------------------------------------------------------- |
| wxWidgets  | `v3.3.3`         | UI and portable base helpers, shared libraries           |
| curl       | `curl-8_21_0`    | FTP/FTPS and HTTPS release checks, shared library        |
| libssh2    | `libssh2-1.11.1` | SFTP, shared library                                     |
| LibreSSL   | `v4.3.2`         | curl TLS and libssh2 crypto on Windows, shared libraries |
| Catch2     | `v3.16.0`        | Headless tests                                           |
| havCSON    | `main`           | Header-only configuration library                        |

Every ref is a CMake cache variable and can be overridden without editing the
repository. For example:

```powershell
cmake --preset mingw-debug-local -DHAVREMOTE_HAVCSON_REF=my-feature-branch
```

The variable names are
`HAVREMOTE_WXWIDGETS_REF`, `HAVREMOTE_CURL_REF`,
`HAVREMOTE_LIBSSH2_REF`, `HAVREMOTE_LIBRESSL_REF`,
`HAVREMOTE_CATCH2_REF`, and `HAVREMOTE_HAVCSON_REF`. Each configuration compiles
a havCSON API probe and requires compatibility with the 0.5.x interface.

The public release checker is compiled for `Havoc7891/havRemote` by default.
Downstream builds can select a different public release repository with the
`HAVREMOTE_UPDATE_REPOSITORY_OWNER` and `HAVREMOTE_UPDATE_REPOSITORY_NAME`
cache variables. These values identify a public endpoint. They are not account
credentials and no token is compiled into the application.

curl uses the fetched LibreSSL on Windows, with Windows native CA roots enabled.
curl links LibreSSL's SSL and crypto libraries, and libssh2 uses the same crypto
library. On Linux and macOS, curl and libssh2 use the same system OpenSSL
installation. macOS also enables native certificate trust evaluation. Unused
curl protocols are disabled.
wxWidgets provides access to the operating system's secure credential store.

On Windows, wxWidgets, curl, libssh2, and LibreSSL's crypto and SSL libraries are
shared runtime dependencies. Their required DLLs are staged beside the executable
and included in a MinGW Release ZIP. For MinGW GCC builds, only the libgcc,
libstdc++, libatomic, and winpthreads runtime libraries are linked statically in
the final executables and bundled dependency DLLs. Internal libraries,
test-support targets, and private support libraries may themselves be static
or be absorbed into another target. The application's third-party runtime
dependencies remain shared. MinGW Release builds
[audit PE imports](releasing.md#automated-release-audits) so an accidental dependency
on a MinGW, MSYS, or unreviewed DLL fails the build.

After changing the runtime dependency set, use a fresh build directory. Old
DLLs left by an earlier configuration are rejected by the package
audit, even if the new executable no longer links them.

## Test configurations

Headless Catch2 tests are enabled by default through
`HAVREMOTE_BUILD_TESTS=ON`. The ordinary preset commands above build and run
them. Atomic-write failure tests run in a separate target with havCSON test hooks.

In GUI builds, the ordinary CTest suite also exercises the real transfer
controller with fake protocol sessions, covering renamed destinations,
queue-save failures, and permission-error handling. On Windows, scripted loopback FTP
fixtures exercise the real FTP backend's authentication failures and MLSD/LIST
fallback, upload restart semantics, and brief download stalls. These tests run
without Docker or an external server.

Upload-failure fixtures also cover received storage/quota replies (`452`/`552`),
server processing failures (`451`), and a reset data socket whose queued quota
reply libcurl does not read. They check partial-file retention, no automatic
retry/finalization, and the distinction between a server-sent reply and a reply
actually observed by the client. This is protocol fault injection, not a test
of real server quota enforcement or a disk filled to capacity.

The longer FTP low-speed regressions stall a real libcurl upload
and download, checking the 1-byte/sec, 30-second threshold, CURLcode 28,
retained partial files, and absence of automatic retry or finalization. They
are hidden from the default test run. Request them explicitly (allow roughly
75 seconds for the group):

```powershell
& ./build/mingw-debug/havremote_tests.exe "[low-speed]" --reporter compact --durations yes
```

These isolated wire tests use plain passive FTP. The timeout options are
applied in the shared FTP backend before active/passive and explicit/implicit
TLS setup. FTPS interoperability requires separate integration tests.

The FTP, explicit FTPS, and SFTP container smoke suite is opt-in and requires
Docker Desktop running Linux containers. From native Windows:

```powershell
cmake --preset mingw-debug-local -DHAVREMOTE_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset mingw-debug-local --target havremote_integration_tests
ctest --test-dir build/mingw-debug -L docker --output-on-failure
```

CTest owns the Docker Compose fixture lifecycle for these tests. See the
[`tests/integration` documentation](../tests/integration/README.md) for ports,
coverage, fixtures, cleanup after an interrupted run, and additional manual
tests. Selecting `docker` limits this command to the container suite.
The broader `integration` label also selects manual tests
when their opt-in environment variables are set.

The separate [read-only FTP idle-lifecycle test](../tests/integration/ftpIdleReadme.md)
uses an explicitly selected timeout endpoint and its approved saved credentials
instead of Docker. It verifies libcurl's behavior after an idle control
connection closes and the normal reconnection message, with an optional bounded
control trace. It is available in the integration-test executable even when
Docker is not installed. Follow its endpoint and credential instructions before
running it.

The separate [synthetic SFTP READ/EOF regression](../tests/integration/README.md#synthetic-sftp-read-and-eof-regression)
uses Python and Paramiko instead of Docker. It records actual READ offsets and
lengths, checks 200 MiB downloads and resumed tails byte for byte, verifies
simultaneous requests and bounded speculative read-ahead, and exercises normal
EOF replies and changed file sizes.

## Linux desktop build

The application uses wxWidgets' GTK 3 port. Install the GTK 3, OpenGL, libsecret,
and OpenSSL development packages, together with CMake 4.0 or newer, a C++23
toolchain, Git, `pkg-config`, and Ninja. Debian/Ubuntu
package names include `libgtk-3-dev`, `libsecret-1-dev`, and `libssl-dev`.
GCC 14 or newer can provide the required language features.

The `linux-debug` and `linux-release` presets use Ninja and the default compiler
from `PATH`. To select another compiler, set `CC` and `CXX` before the first
configuration of a new build directory.

Configure, build, and test Debug with the shared presets. The desktop CTest suite
includes GUI tests and requires a graphical session:

```sh
cmake --preset linux-debug
cmake --build --preset linux-debug --parallel
ctest --preset linux-debug
```

Without a graphical session, install Xvfb and run the tests with:

```sh
xvfb-run -a ctest --preset linux-debug
```

Run `build/linux-debug/havRemote-debug` in a graphical desktop session. Saved
credentials require a running desktop secret service. If it is unavailable,
havRemote reports the failure and never falls back to plaintext storage.

To stage a Release runtime and create an archive, use a separate Release build:

```sh
cmake --preset linux-release
cmake --build --preset linux-release --parallel
ctest --preset linux-release
cmake --install build/linux-release --prefix "$PWD/build/linux-staged" --component Runtime
cpack --config build/linux-release/CPackConfig.cmake
```

For Release tests without a graphical session, use:

```sh
xvfb-run -a ctest --preset linux-release
```

The archive is written under `build/linux-release/packages` as
`havRemote-<version>-linux-<architecture>.tar.gz`, with `x86_64` or `arm64`
as the architecture. It bundles the project-built wxWidgets, curl, and libssh2
shared libraries. Keep the `lib`, `translations`, `help`, and `icons`
directories beside the executable. System GTK 3, libsecret, OpenSSL, compiler
runtime libraries, and other operating-system dependencies must be available
on the destination machine.

The Linux build does not yet have the audited packaging workflow described in
[Releasing havRemote](releasing.md).

## Linux headless checks

The `linux-sanitizers` preset sets `HAVREMOTE_BUILD_APP=OFF` and uses Ninja to
build the protocol-neutral and protocol libraries without the
wxWidgets UI, then runs the headless suite with AddressSanitizer and
UndefinedBehaviorSanitizer. The filename checks use wxWidgets' base library,
which is built without a GUI toolkit for these tests. Headless builds do not
require GTK 3 or libsecret development packages and do not create runtime
packages.

Install CMake 4.0 or newer, Ninja, a C/C++ toolchain with C++23 support and
AddressSanitizer/UndefinedBehaviorSanitizer runtimes, Git, and the OpenSSL
development package. LibreSSL and its Bash/Perl preparation step are used only
for Windows builds.

Then configure, build, and test:

```sh
cmake --preset linux-sanitizers
cmake --build --preset linux-sanitizers
ctest --preset linux-sanitizers
```

Container-backed plain FTP and SFTP tests can also be run locally with the
Linux preset:

```sh
cmake --preset linux-sanitizers -DHAVREMOTE_BUILD_INTEGRATION_TESTS=ON
cmake --build --preset linux-sanitizers --target havremote_integration_tests
ctest --preset linux-sanitizers --tests-regex '^integration\.(ftp|sftp)$'
```

This command selects the plain FTP and SFTP container fixtures. curl also has
FTPS support in Linux builds through OpenSSL, but this selection does not
validate an FTPS interoperability matrix.

## Relevant build files

- [`CMakePresets.json`](../CMakePresets.json) defines the shared Windows and
  Linux configure, build, test, and package presets.
- [`cmake/toolchains/mingw-gcc.cmake`](../cmake/toolchains/mingw-gcc.cmake)
  resolves and isolates the selected MinGW toolchain.
- [`cmake/DependencyVersions.cmake`](../cmake/DependencyVersions.cmake) keeps
  all default dependency refs together.
- [`scripts/build.ps1`](../scripts/build.ps1) provides the MinGW
  configure/build/test/package wrapper.
