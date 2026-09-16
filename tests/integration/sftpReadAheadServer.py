# SPDX-License-Identifier: MIT

"""Run the opt-in SFTP READ wire regression against a synthetic local server.

Requires Python 3 and Paramiko. No host files are exposed: every remote file is
generated in memory, all modifications are denied, credentials/host keys are
ephemeral, and the listener exists only while the test executable is running.
"""

import argparse
import logging
import os
import secrets
import socket
import stat
import subprocess
import threading

import paramiko
from paramiko.sftp import CMD_READ


PATTERN = bytes((index * 31 + 17) % 251 for index in range(251))
MAX_SIZE = 210 * 1024 * 1024


def attributes(size):
    result = paramiko.SFTPAttributes()
    result.st_mode = stat.S_IFREG | 0o444
    result.st_uid = result.st_gid = 1000
    result.st_atime = result.st_mtime = 1700000000
    if size is not None:
        result.st_size = size
    return result


def payload(offset, length):
    start = offset % len(PATTERN)
    return (PATTERN * ((start + length + len(PATTERN) - 1) // len(PATTERN)))[
        start : start + length
    ]


class GeneratedFile(paramiko.SFTPHandle):
    def __init__(self, size, actual_size, records):
        super().__init__()
        self.size = size
        self.actual_size = actual_size
        self.records = records

    def stat(self):
        # A changed size becomes visible through the opened handle. An absent
        # size remains absent, exercising the normal EOF-driven fallback.
        return attributes(self.actual_size if self.size is not None else None)

    def read(self, offset, length):
        if length > 16 * 1024 * 1024:
            return paramiko.SFTP_FAILURE
        delivered = max(0, min(length, self.actual_size - offset))
        self.records["reads"].append((offset, length, delivered))
        return payload(offset, delivered)

    def close(self):
        reads = self.records["reads"]
        maximum_end = max((offset + length for offset, length, _ in reads), default=0)
        eof_replies = sum(delivered == 0 for _, _, delivered in reads)
        print(
            f"SFTP READ: STAT size={self.size}, actual={self.actual_size}, "
            f"requests={len(reads)}, maxEnd={maximum_end}, EOF replies={eof_replies}, "
            f"simultaneous requests={self.records['pipeline']}",
            flush=True,
        )
        return paramiko.SFTP_OK


class TranscriptFile(paramiko.SFTPHandle):
    def __init__(self, contents):
        super().__init__()
        self.contents = contents

    def stat(self):
        return attributes(len(self.contents))

    def read(self, offset, length):
        return self.contents[offset : offset + length]


class SyntheticSftp(paramiko.SFTPServerInterface):
    def __init__(self, server, *args, **kwargs):
        super().__init__(server, *args, **kwargs)
        self.records = {}

    @staticmethod
    def parse_data_path(path):
        parts = path.split("/")
        if len(parts) != 5 or parts[1] != "data":
            return None
        try:
            known_size = None if parts[3] == "unknown" else int(parts[3])
            actual_size = int(parts[4])
        except ValueError:
            return None
        if not 0 <= actual_size <= MAX_SIZE:
            return None
        if known_size is not None and not 0 <= known_size <= MAX_SIZE:
            return None
        return parts[2], known_size, actual_size

    def transcript(self, path):
        if not path.startswith("/report/"):
            return None
        label = path[len("/report/") :]
        if label not in self.records:
            return None
        record = self.records[label]
        return (f"pipeline {record['pipeline']}\n" + "".join(
            f"{offset} {length} {delivered}\n"
            for offset, length, delivered in record["reads"]
        )).encode("ascii")

    def stat(self, path):
        data = self.parse_data_path(path)
        if data is not None:
            return attributes(data[1])
        contents = self.transcript(path)
        if contents is not None:
            return attributes(len(contents))
        return paramiko.SFTP_NO_SUCH_FILE

    lstat = stat

    def open(self, path, flags, attr):
        if flags & (os.O_WRONLY | os.O_RDWR | os.O_CREAT | os.O_TRUNC | os.O_APPEND):
            return paramiko.SFTP_PERMISSION_DENIED
        data = self.parse_data_path(path)
        if data is not None:
            label, size, actual_size = data
            records = self.records.setdefault(label, {"reads": [], "pipeline": 0})
            return GeneratedFile(size, actual_size, records)
        contents = self.transcript(path)
        if contents is not None:
            return TranscriptFile(contents)
        return paramiko.SFTP_NO_SUCH_FILE


class PipelineProbeServer(paramiko.SFTPServer):
    """Observe genuine simultaneous requests, not inferred request lengths.

    Delay the first response for a large file until three READ packets arrive.
    A one-second timer releases a serialized client too, so the C++ assertion
    fails cleanly instead of hanging. Subsequent responses are not delayed.
    """

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self.probe_lock = threading.RLock()
        self.probed = set()
        self.pending = {}
        self.timers = []

    def flush_probe(self, handle):
        with self.probe_lock:
            pending = self.pending.pop(handle, [])
            if not pending:
                return
            opened = self.file_table.get(handle)
            if isinstance(opened, GeneratedFile):
                opened.records["pipeline"] = len(pending)
            for packet in pending:
                try:
                    super()._process(*packet)
                except (OSError, EOFError):
                    return

    def _process(self, packet_type, request_number, message):
        with self.probe_lock:
            if packet_type == CMD_READ:
                inspected = paramiko.Message(message.get_remainder())
                handle = inspected.get_binary()
                offset = inspected.get_int64()
                opened = self.file_table.get(handle)
                readable_size = (
                    min(opened.size, opened.actual_size)
                    if isinstance(opened, GeneratedFile) and opened.size is not None
                    else opened.actual_size if isinstance(opened, GeneratedFile) else 0
                )
                if handle in self.pending:
                    self.pending[handle].append((packet_type, request_number, message))
                    if len(self.pending[handle]) >= 3:
                        self.flush_probe(handle)
                    return
                if (
                    isinstance(opened, GeneratedFile)
                    and handle not in self.probed
                    and readable_size - offset >= 90000
                ):
                    self.probed.add(handle)
                    self.pending[handle] = [(packet_type, request_number, message)]
                    timer = threading.Timer(1.0, self.flush_probe, args=(handle,))
                    timer.daemon = True
                    self.timers.append(timer)
                    timer.start()
                    return
            super()._process(packet_type, request_number, message)

    def finish_subsystem(self):
        with self.probe_lock:
            for timer in self.timers:
                timer.cancel()
            super().finish_subsystem()


class Authentication(paramiko.ServerInterface):
    def __init__(self, password):
        self.password = password

    def get_allowed_auths(self, username):
        return "password"

    def check_auth_password(self, username, password):
        if username == "havremote-read-ahead" and password == self.password:
            return paramiko.AUTH_SUCCESSFUL
        return paramiko.AUTH_FAILED

    def check_channel_request(self, kind, channel_id):
        if kind == "session":
            return paramiko.OPEN_SUCCEEDED
        return paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("executable", help="Built havremote_integration_tests executable")
    parser.add_argument("--bind", default="127.0.0.1", help="Local listener address")
    parser.add_argument("--host", help="Address the test process uses to reach the listener")
    args = parser.parse_args()
    password = secrets.token_urlsafe(32)
    host_key = paramiko.RSAKey.generate(2048)
    stopped = threading.Event()
    transports = []
    threads = []
    logging.getLogger("paramiko").setLevel(logging.CRITICAL)

    def connection(sock):
        transport = paramiko.Transport(sock)
        transports.append(transport)
        try:
            transport.add_server_key(host_key)
            transport.set_subsystem_handler("sftp", PipelineProbeServer, SyntheticSftp)
            transport.start_server(server=Authentication(password))
            while not stopped.wait(0.05) and transport.is_active():
                pass
        except (OSError, EOFError, paramiko.SSHException):
            pass
        finally:
            transport.close()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as listener:
        listener.bind((args.bind, 0))
        listener.listen(8)
        listener.settimeout(0.2)

        def accept_connections():
            while not stopped.is_set():
                try:
                    sock, _ = listener.accept()
                except socket.timeout:
                    continue
                except OSError:
                    return
                thread = threading.Thread(target=connection, args=(sock,), daemon=True)
                threads.append(thread)
                thread.start()

        accept_thread = threading.Thread(target=accept_connections, daemon=True)
        accept_thread.start()
        environment = os.environ.copy()
        variables = {
            "HAVREMOTE_SFTP_READAHEAD_HOST": args.host or args.bind,
            "HAVREMOTE_SFTP_READAHEAD_PORT": str(listener.getsockname()[1]),
            "HAVREMOTE_SFTP_READAHEAD_PASSWORD": password,
        }
        environment.update(variables)
        if os.name != "nt" and args.executable.lower().endswith(".exe"):
            # WSL does not forward arbitrary Linux variables to Windows children
            environment["WSLENV"] = ":".join(
                filter(None, [environment.get("WSLENV", ""), *variables])
            )
        print(
            f"Synthetic SFTP fixture: {variables['HAVREMOTE_SFTP_READAHEAD_HOST']}:"
            f"{variables['HAVREMOTE_SFTP_READAHEAD_PORT']}",
            flush=True,
        )
        try:
            result = subprocess.run(
                [args.executable, "[sftp-read-ahead]", "--reporter", "compact", "--durations", "yes"],
                env=environment,
                timeout=300,
                check=False,
            )
            return result.returncode
        finally:
            stopped.set()
            accept_thread.join(timeout=2)
            for transport in transports:
                transport.close()
            for thread in threads:
                thread.join(timeout=2)


if __name__ == "__main__":
    raise SystemExit(main())
