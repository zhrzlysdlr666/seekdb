# MiniVectorDB - 2026 OceanBase competition, round 2.
#
# The judge starts this image with `docker run -i` (no TTY, no published port,
# no network) and speaks JSON Lines over stdin/stdout, so the default command
# must be the protocol process itself: no shell, no prompt, no server socket.
FROM python:3.12-slim

# The root filesystem is mounted read-only and only /data is writable, so keep
# Python from trying to write bytecode next to the sources and flush every line.
ENV PYTHONDONTWRITEBYTECODE=1 \
    PYTHONUNBUFFERED=1 \
    PYTHONIOENCODING=utf-8

WORKDIR /app
COPY minivectordb.py /app/minivectordb.py

# No pip step on purpose: the build must succeed on a judge machine without
# package-index access. The engine needs only the standard library (it uses
# numpy when it happens to be importable, and math.dist otherwise).

# The judge bind-mounts a writable directory here; creating it keeps the image
# usable when it is started without a mount (local experiments).
RUN mkdir -p /data

# stdout carries protocol frames only; anything diagnostic goes to stderr.
CMD ["python", "-u", "/app/minivectordb.py"]
