# camsim-scene reference build container (REALISM R0). Byte-identical packages need the same image digest:
# build once, keep the image, and reference it by digest (build.json records it). apt packages aren't
# snapshot-pinned, so a rebuilt image may differ.
FROM ubuntu:24.04@sha256:534baea6a22c03a63003dbc8dbe78fe34bc0d7e595d9a9dc9834884ff530eb55

ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8 TZ=UTC PYTHONDONTWRITEBYTECODE=1 PROJ_NETWORK=OFF \
    UV_PYTHON_DOWNLOADS=never UV_LINK_MODE=copy CAMSIM_SCENE_CACHE=/cache
RUN apt-get update \
 && apt-get install -y --no-install-recommends python3.12 python3.12-venv gdal-bin proj-bin squashfs-tools ca-certificates \
 && rm -rf /var/lib/apt/lists/*
COPY --from=ghcr.io/astral-sh/uv:0.12.23 /uv /usr/local/bin/uv

WORKDIR /opt/camsim-scene
COPY pyproject.toml uv.lock .python-version README.md ./
RUN uv sync --frozen --no-dev --no-install-project --python /usr/bin/python3.12
COPY camsim_scene ./camsim_scene
RUN uv sync --frozen --no-dev --python /usr/bin/python3.12
ENV PATH=/opt/camsim-scene/.venv/bin:$PATH

WORKDIR /work
ENTRYPOINT ["camsim-scene"]
