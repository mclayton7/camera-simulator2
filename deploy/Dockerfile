# CamSim runtime Docker image
# Base: Ubuntu 22.04 (Jammy) — matches UE5 Linux build requirements.
#
# The packaged game binary must be staged before building:
#   <project>/Saved/StagedBuilds/LinuxNoEditor/ → deploy/staged/LinuxNoEditor/
#
# Build:
#   docker build -t camsim:latest deploy/
#
# Run (NVIDIA GPU):
#   docker run --rm --gpus all \
#     -e NVIDIA_VISIBLE_DEVICES=all \
#     -p 8888:8888/udp \
#     --network host \
#     camsim:latest
#
# Run (CPU / Mesa fallback):
#   docker run --rm \
#     --network host \
#     camsim:latest

FROM ubuntu:22.04

# -----------------------------------------------------------------------
# System packages
# -----------------------------------------------------------------------
ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Vulkan loader + NVIDIA ICD stub (real ICD injected by nvidia-container-toolkit)
    libvulkan1 \
    vulkan-tools \
    # Mesa llvmpipe — CPU-side Vulkan fallback
    mesa-vulkan-drivers \
    libllvm15 \
    # X11 libraries needed even in -RenderOffScreen mode (UE initialises display handle)
    libx11-6 \
    libxext6 \
    libxrender1 \
    # SDL2 (UE5 Linux ships its own but this satisfies the loader)
    libsdl2-2.0-0 \
    # Audio stub (we pass -nosound but the audio subsystem still links)
    libasound2 \
    # Shared runtime libs
    libstdc++6 \
    libgcc-s1 \
    libc6 \
    # Utilities
    ca-certificates \
    && rm -rf /var/lib/apt/lists/*

# -----------------------------------------------------------------------
# Non-root runtime user (uid/gid 1000)
# -----------------------------------------------------------------------
RUN groupadd -g 1000 camsim && useradd -u 1000 -g camsim -m -s /bin/bash camsim

# -----------------------------------------------------------------------
# Game binary
# -----------------------------------------------------------------------
COPY staged/LinuxNoEditor/ /opt/camsim/
COPY camsim_config.json    /opt/camsim/camsim_config.json
COPY entrypoint.sh         /opt/camsim/entrypoint.sh

RUN chmod +x /opt/camsim/entrypoint.sh \
    && chown -R camsim:camsim /opt/camsim

# -----------------------------------------------------------------------
# Network ports
# -----------------------------------------------------------------------
# 8888/udp — CIGI input
# 5004/udp  — MPEG-TS / H.264 multicast output
EXPOSE 8888/udp 5004/udp

# -----------------------------------------------------------------------
# Runtime
# -----------------------------------------------------------------------
USER camsim
WORKDIR /opt/camsim

ENTRYPOINT ["/opt/camsim/entrypoint.sh"]
