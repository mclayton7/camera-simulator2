# CamSim runtime Docker image (NVIDIA GPU: Vulkan render + NVENC encode)
#
# The image carries only the packaged game; the NVIDIA driver comes from the
# host at run time via the NVIDIA Container Toolkit (no CUDA or driver
# packages in the image, so one image works with any recent host driver).
#
# Stage the package first (BuildCookRun + entities/):
#   scripts/package_for_docker.sh        # -> deploy/staged/Linux/
#
# Build:
#   docker build -t camsim:latest deploy/
#
# Run:
#   docker run --rm --gpus all --network host --init camsim:latest
# or: docker compose -f deploy/docker-compose.yml up
#
# Host requirements: NVIDIA driver + nvidia-container-toolkit
# (docs/docker.md). A GPU is required: Mesa lavapipe crashes compiling UE 5.8
# SM6 pipelines, so the entrypoint refuses to start without one.

FROM ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

# Ask the toolkit for every GPU and all driver libraries: "graphics" carries
# the Vulkan ICD, "video" libnvidia-encode (NVENC). Hosts in the toolkit's
# legacy mode default to compute,utility, which has neither.
ENV NVIDIA_VISIBLE_DEVICES=all \
    NVIDIA_DRIVER_CAPABILITIES=all

RUN apt-get update && apt-get install -y --no-install-recommends \
        # Vulkan loader. The NVIDIA ICD itself is injected by the toolkit.
        libvulkan1 \
        # glvnd dispatch libs: the NVIDIA Vulkan ICD (libGLX_nvidia) dlopens
        # libEGL.so.1, which is a distro package the toolkit does not inject.
        # Without it vkCreateInstance fails with ERROR_INCOMPATIBLE_DRIVER.
        libegl1 \
        libglx0 \
        # vulkaninfo: the entrypoint logs the selected device
        vulkan-tools \
        # Mesa lavapipe: only for CAMSIM_ALLOW_SOFTWARE_RENDERING=1 experiments
        mesa-vulkan-drivers \
        # SDL needs an X display even with -RenderOffScreen
        xvfb \
        libx11-6 \
        libxext6 \
        libxcursor1 \
        libxi6 \
        libxrandr2 \
        libxrender1 \
        libxss1 \
        libasound2t64 \
        # Health check (GET /live)
        curl \
        ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# Non-root runtime user (uid/gid 1000). Ubuntu 24.04 ships "ubuntu" with 1000.
RUN userdel -r ubuntu 2>/dev/null || true \
    && groupadd -g 1000 camsim \
    && useradd -u 1000 -g camsim -m -s /bin/bash camsim

# Packaged game. The config sits in the project dir, the first place
# FCamSimConfig::GetConfigFilePath() looks; mount over it to override.
COPY --chown=camsim:camsim staged/Linux/ /opt/camsim/
COPY --chown=camsim:camsim camsim_config.yaml /opt/camsim/CamSimTest/camsim_config.yaml
COPY --chmod=755 entrypoint.sh /usr/local/bin/camsim-entrypoint

# Writable state, both volumes in docker-compose.yml:
#   /var/lib/camsim     UE user dir (entrypoint passes -userdir): Saved/
#                       (logs, config) and Cesium's tile cache database
#   /home/camsim/.cache NVIDIA driver shader cache
# They must exist owned by camsim: Docker seeds a new named volume with the
# image directory's ownership, and a missing one comes up root-owned.
RUN mkdir -p /var/lib/camsim /home/camsim/.cache \
    && chown camsim:camsim /var/lib/camsim /home/camsim/.cache

# Host networking is required (UDP multicast), so these are documentation:
# 8888/udp CIGI in, 5004/udp MPEG-TS out, 8080/tcp health HTTP.
EXPOSE 8888/udp 5004/udp 8080/tcp

# /live is the game-thread watchdog; /ready also needs CIGI traffic, so it
# belongs to orchestrators that drive the IG, not to a liveness probe.
HEALTHCHECK --interval=10s --timeout=5s --start-period=180s --retries=3 \
    CMD curl -sf -m 4 "http://127.0.0.1:${CAMSIM_HEALTH_HTTP_PORT:-8080}/live" >/dev/null || exit 1

ARG GIT_SHA=unknown
LABEL org.opencontainers.image.title="CamSim" \
      org.opencontainers.image.description="Synthetic EO/IR sensor IG: CIGI 3.3 in, H.264 MPEG-TS + MISB ST 0601 KLV out" \
      org.opencontainers.image.revision="${GIT_SHA}"

USER camsim
WORKDIR /opt/camsim

ENTRYPOINT ["/usr/local/bin/camsim-entrypoint"]
