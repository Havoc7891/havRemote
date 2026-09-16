# SPDX-License-Identifier: MIT

FROM alpine:3.22

RUN apk add --no-cache openssh \
    && adduser -D -h /home/havremote -s /bin/sh havremote \
    && echo 'havremote:havremote-test-password' | chpasswd

COPY sftp/sshdConfig /etc/ssh/sshd_config
COPY sftp/entrypoint.sh /usr/local/bin/havremote-sftp-entrypoint
COPY fixtures/ /opt/havremote/fixtures/

RUN chmod 0755 /usr/local/bin/havremote-sftp-entrypoint

EXPOSE 22
ENTRYPOINT ["/usr/local/bin/havremote-sftp-entrypoint"]
