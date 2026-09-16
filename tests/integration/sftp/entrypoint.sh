#!/bin/sh
# SPDX-License-Identifier: MIT

set -eu

mkdir -p /run/sshd /home/havremote/fixture
cp -R /opt/havremote/fixtures/. /home/havremote/fixture/
ln -sf missing-link-target.txt /home/havremote/fixture/dangling-link.txt
chown -R havremote:havremote /home/havremote
ssh-keygen -A

exec /usr/sbin/sshd -D -e -f /etc/ssh/sshd_config
