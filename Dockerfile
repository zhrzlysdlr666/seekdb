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

# numpy turns the exact TopK scan into a BLAS-shaped loop instead of one Python
# call per row. The wheel is committed under vendor/ because the build machine
# has no package index: `--no-index` never touches the network, so this step
# either installs the pinned wheel or fails immediately. Either way the build
# succeeds, and the engine falls back to math.dist if the import fails at run
# time.
COPY vendor/ /app/vendor/
RUN pip install --no-cache-dir --no-index --find-links=/app/vendor numpy==2.2.6 \
    || echo "numpy unavailable; pure-python fallback stays active"

# The judge bind-mounts a writable directory here; creating it keeps the image
# usable when it is started without a mount (local experiments).
RUN mkdir -p /data

# stdout carries protocol frames only; anything diagnostic goes to stderr.
CMD ["python", "-u", "/app/minivectordb.py"]
