# SPDX-License-Identifier: MIT

FROM python:3.13-alpine

RUN python -m pip install --no-cache-dir "pyftpdlib>=2,<3" "pyOpenSSL>=24"

COPY ftp/server.py /opt/havremote/server.py
COPY fixtures/ /opt/havremote/fixtures/
COPY certs/ /opt/havremote/certs/

EXPOSE 21
ENTRYPOINT ["python", "/opt/havremote/server.py"]
