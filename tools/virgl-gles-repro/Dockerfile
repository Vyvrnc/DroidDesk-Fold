FROM debian:trixie
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    git ca-certificates build-essential meson ninja-build pkg-config python3 python3-yaml \
    libepoxy-dev libdrm-dev libgbm-dev libegl-dev libgles-dev libgl-dev libx11-dev \
    libegl-mesa0 libgl1-mesa-dri libglx-mesa0 mesa-utils xvfb xauth gdb \
    && rm -rf /var/lib/apt/lists/*
RUN git clone --depth 1 -b 1.3.0 https://gitlab.freedesktop.org/virgl/virglrenderer.git /src/virglrenderer
WORKDIR /src/virglrenderer
RUN meson setup build -Dbuildtype=debug -Dtests=false -Dvenus=false \
 && ninja -C build
