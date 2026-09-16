# Releasing havRemote

havRemote releases are unsigned portable Windows x64 ZIP archives built with
the tested MinGW GCC configuration. Other build paths,
including Visual Studio, do not yet have an equivalent release audit and
symbol-packaging workflow. See
[Building and debugging](building.md) for toolchain setup and local presets.

## Contents

- [Build a release](#build-a-release)
- [Linkage policy](#linkage-policy)
- [Automated release audits](#automated-release-audits)
- [Runtime archive contents](#runtime-archive-contents)
- [Manifest and Windows resources](#manifest-and-windows-resources)
- [Regenerate the application icon and help favicon](#regenerate-the-application-icon-and-help-favicon)
- [Release checklist](#release-checklist)

## Build a release

Run the release build in PowerShell:

```powershell
.\scripts\build.ps1 -Configuration Release -Test -Package
```

The helper uses `mingw-release-local` from `CMakeUserPresets.json` unless a
toolchain is specified through `-MinGwRoot` or `HAVREMOTE_MINGW_ROOT`. To use
the shared `mingw-release` preset, set that variable to the selected
distribution's `mingw64` directory or pass the directory explicitly:

```powershell
.\scripts\build.ps1 -Configuration Release -Test -Package `
  -MinGwRoot 'C:/Toolchains/MinGW/mingw64'
```

The helper restores the session's environment and working directory afterward.

With CMake 4.0 or newer available, the script configures and builds the selected
preset, runs its CTest suite, then builds its `package-symbols` and `package`
targets. Configuration verifies the required C++23 capabilities and static
runtime archive availability and linking. This audited packaging workflow
requires a single-configuration MinGW GCC build. GCC 15.2.0 with MinGW-w64
runtime v13, UCRT, POSIX threads,
and SEH is the tested configuration. Run the full build, tests, and package
checks when selecting a different toolchain.

Successful packaging writes two artifacts below the selected build directory's
`packages` folder, which is `build/mingw-release/packages` with the documented
presets:

- `havRemote-<version>-windows-x86_64.zip` - the portable runtime, and
- `havRemote-<version>-windows-x86_64-debug.zip` - detached GNU debug symbols
  for `havRemote.exe` and every bundled dependency DLL.

The Windows [update checker](configuration.md#settings) accepts only the exact runtime name
`havRemote-<version>-windows-x86_64.zip` for the normalized release version. It
does not confuse the similarly named debug-symbol archive with the runnable
package. Other builds select matching
platform and architecture assets: `havRemote-<version>-linux-<architecture>.tar.gz`
or `havRemote-<version>-macos-<architecture>.zip`, with `x86_64` or `arm64`.

If a release has no matching runtime asset, including when the asset was
renamed, havRemote still shows the release with **Download** disabled.
**View release** remains available. This behavior is the same on every platform.

The portable archive is unsigned and does not include an installer.

## Linkage policy

The MinGW release build uses the following linkage:

- libgcc, libstdc++, libatomic, and winpthreads are linked from their static
  archives into every shipped PE,
- havRemote's component libraries are internal static targets folded into the
  executable, while havCSON is header-only,
- wxWidgets, curl, libssh2, and LibreSSL's crypto and SSL libraries remain DLLs
  distributed beside the executable, and
- Catch2 is static and test-only, so it is not part of the runtime bundle.

The exact DLL filenames are derived from CMake targets and written to a
generated runtime manifest. Do not hand-maintain a second list: upstream names
include configuration, compiler, version, and vendor details that may change
between dependency releases.

curl handles FTP transfers and uses LibreSSL for FTPS transfers and HTTPS update
checks on Windows, with Windows native CA roots enabled. curl and libssh2 share
the same LibreSSL crypto library through the project-owned OpenSSL target shim.
wxWidgets' private zlib, PNG, and NanoSVG components are absorbed into the
wxWidgets DLLs and are not shipped as independent libraries. NanoSVG renders the Site Manager
split-button arrow.
JPEG, TIFF, WebP, PCRE2, and Expat support are disabled, along with the unused
XML, XRC, HTML-help, rich-text, and styled-editor modules. The message log uses
the native text control, and offline help opens in the default browser.

Release compilation remaps source, build, and toolchain prefixes and strips
debug data from copied MinGW runtime archives. Debug information for the
application and runtime DLLs is split transactionally into `.debug` files. Each
stripped PE receives a GNU debug link whose filename and CRC are verified before
either artifact is committed.

## Automated release audits

Linking `havRemote.exe` runs the PE audit, and the symbol target runs it again
before archiving. A failure stops packaging. The audit verifies that:

- the runtime directory contains exactly the target-derived DLL set, with no
  missing, extra, duplicate, or case-colliding names,
- neither the executable nor a bundled DLL imports libgcc, libstdc++,
  libatomic, winpthreads, libssp, libgomp, libquadmath, MSYS, or Cygwin runtime
  DLLs,
- every non-system import resolves to a reviewed DLL in the portable bundle,
- Release PEs and detached symbols contain no local checkout, build, or
  toolchain paths,
- every shipped PE has detached source-level debug information and a valid GNU
  debug link,
- packaged translation catalogs exactly match `translations/*.cson`,
- packaged offline-help files match `resources/help` after inserting the CMake
  project version into each HTML header, with other content and the
  high-resolution logo and favicon unchanged, and
- the executable contains manifest resource ID 1, the custom application icon,
  every embedded PNG control icon, and wxWidgets' standard MSW resources.

Before publishing, launch both the build-tree and extracted copies with
`--smoke-test` after reducing `PATH` to Windows system directories. The smoke
test initializes the GUI runtime and all bundled DLLs and decodes every
embedded PNG without reading user configuration or opening a window.

## Runtime archive contents

In addition to `havRemote.exe`, the reviewed DLL set, and the `translations`
directory, the portable ZIP contains:

- the project's `LICENSE.txt` and `thirdPartyNotices.md`,
- `thirdPartyProvenance.txt`, recording each configured dependency ref and
  the exact Git commit resolved during configuration,
- localized offline help below `help`, plus its high-resolution logo and
  favicon below `icons`, and
- a `licenses` directory with the authoritative notices for shipped
  dependencies, MinGW runtime components, and the Oswald icon source font.

[Dependency refs](building.md#dependencies-and-version-overrides) may move.
Use the generated provenance file to identify the sources used for a release.
Retain the configure log with published artifacts as an
additional release record.

Both Debug and Release MinGW configurations locate the GCC GPLv3 text, GCC
Runtime Library Exception, MinGW-w64 runtime license, and winpthreads license
in common `licenses` and `share/licenses` directories below
`HAVREMOTE_MINGW_ROOT`. These files are required during configuration, not only
when creating a package. For another compatible distribution layout, configure
these cache variables with the files provided by that toolchain:

- `HAVREMOTE_GCC_GPLV3_LICENSE_FILE`
- `HAVREMOTE_GCC_RUNTIME_EXCEPTION_FILE`
- `HAVREMOTE_MINGW_RUNTIME_LICENSE_FILE`
- `HAVREMOTE_WINPTHREADS_LICENSE_FILE`

The complete dependency summary is maintained in
[Third-party notices](../thirdPartyNotices.md).

Keep the LibreSSL acknowledgments in that notice and include the unmodified
upstream `COPYING` file as `licenses/LibreSSL-LICENSE.txt`. Release pages and
other advertising materials mentioning features or use of the inherited
OpenSSL/SSLeay software must also display the OpenSSL Project and Eric Young
acknowledgments from the [README's License section](../README.md#license).
These credits do not change the MIT license of havRemote's own code.

## Manifest and Windows resources

[`resources/havRemote.manifest.in`](../resources/havRemote.manifest.in) is
configured with the project version and embedded as `RT_MANIFEST` resource ID 1.
It declares the `Havoc.havRemote` assembly identity, Common Controls 6,
`asInvoker` privileges, per-monitor-v2 DPI awareness, long-path awareness, the
UTF-8 active code page, and Windows 10/11 compatibility.

[`resources/havRemote.rc.in`](../resources/havRemote.rc.in) embeds the
multi-size application icon, the PNG control and file-list icons as `RCDATA`,
the configured manifest, version information, and wxWidgets' standard Windows
resources. wxWidgets' smaller fallback manifest is disabled to ensure there is
only one resource ID 1. No adjacent `.manifest` sidecar is needed or packaged.

When adding or removing a UI image, update all of these together:

1. the resource file list and object dependencies in `CMakeLists.txt`,
2. the named `RCDATA` entry in `resources/havRemote.rc.in`,
3. the image list in [`cmake/HavRemoteImages.cmake`](../cmake/HavRemoteImages.cmake)
   for Linux and macOS,
4. the application's resource lookup or smoke-test list, and
5. the required-resource list in `cmake/scripts/AuditPeImports.cmake`.

## Regenerate the application icon and help favicon

The icon is reproducible with Python 3 and uses only the standard library:

```powershell
python scripts/generateIcon.py
```

The command regenerates `resources/icons/havRemote.ico`, the 512 x 512
`resources/icons/havRemote.png`, the full white logo
[`resources/icons/havRemoteWhite.svg`](../resources/icons/havRemoteWhite.svg),
and the compact white badge logo
[`resources/icons/havRemoteBadge.svg`](../resources/icons/havRemoteBadge.svg).
The PNG is the high-resolution source used by
the README and bundled HTML help, while the ICO retains its individually
rendered Windows frame sizes and also serves as the help's browser-tab icon.
The build copies the same ICO to `icons/havRemote.ico` beside the executable
and includes it in the portable ZIP. Both help languages reference that file.
An alternate output directory can be used for
review without replacing the checked-in assets:

```powershell
python scripts/generateIcon.py --output-dir build/icon-preview
```

The full white SVG keeps the rounded tile white and cuts the outlined "hav"
lettering and angled plug out transparently. The compact badge SVG uses the
same white rounded tile with an enlarged, centered transparent plug and no
lettering, preserving the plug's fine details. In both variants, the background
shows through the cutouts. Both keep a 24 x 24 viewBox. The badge defaults to
a 14 x 14 display size. Neither needs external fonts or images. To regenerate
both SVG variants without touching the
application ICO or PNG:

```powershell
python scripts/generateIcon.py --svg-only
```

For a [custom Shields logo](https://shields.io/docs/logos), embed the compact
badge SVG as a base64 data URI in the `logo` query parameter, not as a hosted
SVG URL. For example, run this from the project root to print a badge URL with
havRemote's red background (`#c40000`), matching the plug in the application logo:

```powershell
$logoPath = (Resolve-Path resources/icons/havRemoteBadge.svg).Path
$logoData = [Convert]::ToBase64String([IO.File]::ReadAllBytes($logoPath))
$logoQuery = [Uri]::EscapeDataString("data:image/svg+xml;base64,$logoData")
"https://img.shields.io/badge/havRemote-c40000?logo=$logoQuery"
```

The custom SVG already supplies its white color. Shields' `logoColor` option
only applies to its built-in Simple Icons logos.

The visual controls near the top of [the script](../scripts/generateIcon.py) include
`PLUG_ANGLE_DEGREES`, `PLUG_FLIP_HORIZONTAL`, `WORDMARK_Y`, and the application
palette. The generator reads the checked-in Oswald source font from
`resources/font`. Its SIL Open Font License 1.1 text is stored beside it
and included in release notices.

After changing the icon, regenerate all four outputs, inspect the smallest ICO
frames as well as the PNG and both SVGs, build a Release executable, and let the
resource audit confirm that Windows received the new embedded icon.

## Release checklist

1. Update the project version and dependency refs.
   Update the changelogs in both
   [`resources/help/en/index.html`](../resources/help/en/index.html) and
   [`resources/help/de/index.html`](../resources/help/de/index.html) with the release version, notes, and date in
   ISO format (`YYYY-MM-DD`). Only the help header version is generated from
   CMake. Changelog entries remain manually maintained.
2. Build from a clean checkout with the [tested MinGW configuration](building.md#select-the-mingw-installation).
3. Run `scripts\build.ps1 -Configuration Release -Test -Package`.
4. Review the runtime ZIP, symbol ZIP, notices, and generated provenance.
5. Test the extracted runtime on a clean Windows 10 or 11 machine with no
   MinGW directory on `PATH`.
6. Create a draft GitHub Release with a tag that is exactly `MAJOR.MINOR.PATCH`
   or `vMAJOR.MINOR.PATCH`, and supply its release notes, including the
   [applicable third-party acknowledgments](#runtime-archive-contents).
7. Attach the portable archive and debug-symbol archive under their exact,
   separate filenames before publishing the release.
8. Publish it as a normal release, not a prerelease.
9. For the first release, run **Check for Updates** from the published version
   and confirm that it reports no newer version. Check the published runtime
   ZIP link and filename directly. For subsequent releases, also check from
   the preceding havRemote version that the new version is offered and that
   **Download** opens the runtime ZIP rather than the debug-symbol archive.
