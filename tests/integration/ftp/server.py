# SPDX-License-Identifier: MIT

"""Small FTP/explicit-FTPS server used only by havRemote integration tests."""

from __future__ import annotations

import os
import shutil
from pathlib import Path

from pyftpdlib.authorizers import DummyAuthorizer
from pyftpdlib.filesystems import AbstractedFS
from pyftpdlib.handlers import FTPHandler, TLS_FTPHandler
from pyftpdlib.servers import FTPServer


NAMESPACE_ROOT = Path("/srv/havremote-namespace")
ROOT = NAMESPACE_ROOT / "login"
FIXTURES = Path("/opt/havremote/fixtures")


class NonRootLoginFilesystem(AbstractedFS):
    """Expose a confined FTP namespace whose login directory is not '/'."""

    def __init__(self, root: str, cmd_channel: FTPHandler) -> None:
        login_directory = Path(root).resolve()
        namespace_root = login_directory.parent
        super().__init__(str(namespace_root), cmd_channel)
        self.cwd = "/" + login_directory.relative_to(namespace_root).as_posix()


def initialize_root() -> None:
    ROOT.mkdir(parents=True, exist_ok=True)
    shutil.copytree(FIXTURES, ROOT, dirs_exist_ok=True)


def main() -> None:
    initialize_root()
    authorizer = DummyAuthorizer()
    authorizer.add_user(
        os.environ.get("HAVREMOTE_USER", "havremote"),
        os.environ.get("HAVREMOTE_PASSWORD", "havremote-test-password"),
        str(ROOT),
        perm="elradfmwMT",
    )

    use_tls = os.environ.get("HAVREMOTE_TLS", "0") == "1"
    handler_type = TLS_FTPHandler if use_tls else FTPHandler

    class Handler(handler_type):  # type: ignore[misc, valid-type]
        pass

    Handler.authorizer = authorizer
    # This intentionally makes PWD return "/login" while confining all paths
    # to NAMESPACE_ROOT. It catches clients that address URL transfers relative
    # to the login directory but accidentally send raw command paths from the
    # server filesystem root (for example, STOR login-relative followed by an
    # absolute RNFR).
    Handler.abstracted_fs = NonRootLoginFilesystem
    Handler.banner = "havRemote integration fixture"
    Handler.encoding = "utf-8"
    Handler.passive_ports = range(
        int(os.environ["HAVREMOTE_PASSIVE_MIN"]),
        int(os.environ["HAVREMOTE_PASSIVE_MAX"]) + 1,
    )
    Handler.masquerade_address = os.environ.get("HAVREMOTE_ADVERTISE", "127.0.0.1")
    Handler.use_sendfile = False
    if use_tls:
        Handler.certfile = "/opt/havremote/certs/server-cert.pem"
        Handler.keyfile = "/opt/havremote/certs/server-key.pem"
        Handler.tls_control_required = True
        Handler.tls_data_required = True

    FTPServer(("0.0.0.0", 21), Handler).serve_forever(timeout=0.5, blocking=True)


if __name__ == "__main__":
    main()
