# Third-party notices

havRemote uses the following third-party projects. Runtime packages include
this notice and license texts for bundled components in the `licenses`
directory. Audited Windows release packages also record the exact configured
refs and resolved commits in `thirdPartyProvenance.txt`.

- wxWidgets - wxWindows Library Licence 3.1
- zlib (bundled with wxWidgets in Windows packages) - zlib License
- libpng (bundled with wxWidgets in Windows packages) - PNG Reference Library License
- NanoSVG (compiled into wxWidgets) - zlib License
- curl - curl license
- OpenSSL (OS-provided dependency for curl and libssh2 in Linux/macOS builds) - the license
  accompanying the selected system installation applies
- libssh2 - BSD 3-Clause license
- LibreSSL - ISC-licensed and public-domain additions, while inherited OpenSSL code
  remains subject to both the legacy OpenSSL and original SSLeay licenses
- havCSON - MIT License
- Catch2 - Boost Software License 1.0 (test builds only)
- GCC runtime libraries (libgcc, libstdc++, libatomic, in MinGW GCC builds) -
  GPLv3 with the GCC Runtime Library Exception
- MinGW-w64 runtime and winpthreads (MinGW builds) - permissive runtime licenses included
  with the selected toolchain
- Oswald - SIL Open Font License 1.1 (icon-generation source only, with its source
  license in `resources/font/OFL.txt`, packaged as
  `licenses/Oswald-OFL-1.1.txt`)

Release builders should also retain the configure log with the artifact.
Linux/macOS packages include license texts but do not currently include the
Windows workflow's provenance report. Their system OpenSSL, GUI toolkit, and
secure-store dependencies remain external and must be accounted for separately.
curl uses LibreSSL for TLS on Windows, with Windows native CA roots enabled.
curl and libssh2 share the same LibreSSL crypto library on Windows. The LibreSSL
crypto and SSL DLLs are included in Windows runtime packages. Native Linux/macOS
builds use the same system OpenSSL for curl and libssh2.

## LibreSSL acknowledgments

LibreSSL supplies curl's Windows TLS backend and libssh2's Windows cryptographic
backend. The following acknowledgments apply to the code it inherits from OpenSSL
and SSLeay:

This product includes software developed by the OpenSSL Project
for use in the OpenSSL Toolkit (http://www.openssl.org/)

This product includes cryptographic software written by Eric Young
(eay@cryptsoft.com).

The unmodified upstream `COPYING` file, including its copyright notices,
license conditions, disclaimers, and additional credits, is included in
Windows release packages as `licenses/LibreSSL-LICENSE.txt`.
