# StarCraft 64 NX installer image: every tool needed to build the Switch NRO from
# the user's own ROM and to extract the user's own StarCraft PC files. It holds
# no game data: the ROM and the PC install are mounted at run time and all
# game-derived output stays on the user's machine (docs/installer.md).
#
# Built by tools/installer/install.sh / install.ps1 from the repository root:
#   docker build -f tools/installer/Dockerfile -t sc64-installer .

# ---- Mesa for Horizon (EGL + OpenGL on nouveau), built from source.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282 AS mesa
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates python3 python3-pip python3-venv bison flex pkg-config ninja-build \
    && rm -rf /var/lib/apt/lists/*
RUN python3 -m venv /opt/mesa-venv && /opt/mesa-venv/bin/pip install --no-cache-dir meson==1.6.1 mako==1.3.8 pyyaml==6.0.2 packaging
ENV PATH="/opt/mesa-venv/bin:/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:${PATH}"
ARG MESA_COMMIT=2330d007712acbd32e396574203339df39bde153
RUN git clone --filter=blob:none https://github.com/NaGaa95/mesa-switch /mesa \
    && git -C /mesa checkout -q ${MESA_COMMIT}
RUN cd /mesa && MESA_SHADER_CACHE=disabled DESTDIR=/mesa-install ./build-opengl.sh

# ---- The build image.
FROM devkitpro/devkita64@sha256:1fc388c3a0d34bd2045a6dadcb1020e069d5f876a187fd705de14b4440c00282
RUN apt-get update && apt-get install -y --no-install-recommends \
        git ca-certificates curl unzip xz-utils build-essential cmake ninja-build pkg-config \
        python3 python3-venv python3-pip binutils-mips-linux-gnu ffmpeg \
    && rm -rf /var/lib/apt/lists/*
ENV PATH="/opt/devkitpro/devkitA64/bin:/opt/devkitpro/tools/bin:${PATH}"

# Mesa over the image's own portlibs (as tools/switch/import_portlibs.sh does). The
# OpenGL package wants libelf whenever Vulkan is present; it is not, but an empty
# archive keeps the CMake package happy either way.
COPY --from=mesa /mesa-install/opt/devkitpro/portlibs/switch/ /opt/devkitpro/portlibs/switch/
RUN [ -f /opt/devkitpro/portlibs/switch/lib/libelf.a ] || aarch64-none-elf-ar rcs /opt/devkitpro/portlibs/switch/lib/libelf.a

# StormLib, to read the classic StarCraft archives.
RUN git clone -q https://github.com/ladislav-zezula/StormLib /opt/StormLib \
    && git -C /opt/StormLib checkout -q v9.30 \
    && cmake -S /opt/StormLib -B /opt/StormLib/build -DBUILD_SHARED_LIBS=ON -DCMAKE_BUILD_TYPE=Release \
    && cmake --build /opt/StormLib/build -j"$(nproc)"
ENV STORMLIB=/opt/StormLib/build/libstorm.so

# Pinned sources (README "Building the Switch NRO", step 2).
WORKDIR /opt/third_party
RUN pin() { git clone -q "$1" "$2" && git -C "$2" checkout -q "$3"; } \
    && pin https://github.com/N64Recomp/N64Recomp        N64Recomp        ffb39cdad1da5de07eaaa48bd1db4a89a7986771 \
    && pin https://github.com/N64Recomp/N64ModernRuntime N64ModernRuntime cdf5abbd5026fef5c364c676e4667c45e42b6863 \
    && pin https://github.com/shygoo/n64sym              n64sym           ccf4600f3389f1a84bde23339225cf372fdf7712 \
    && pin https://github.com/decompals/ultralib         ultralib         e24c836796df4bf520ff8b11a5c9d2cea3a66cbd \
    && git -C N64Recomp submodule update -q --init --recursive \
    && git -C N64ModernRuntime submodule update -q --init --recursive

# The reference libultra builds and n64sym (step 3; no ROM needed for these).
RUN make -C n64sym -j"$(nproc)" \
    && cd ultralib && make VERSION=J TARGET=libgultra_rom COMPARE=0 setup \
    && for v in I J K L; do \
         make VERSION=$v TARGET=libgultra_rom COMPARE=0 MDEBUG_FILES= AR="$PWD/tools/gcc/ar" -j"$(nproc)" && \
         make VERSION=$v TARGET=libultra_rom COMPARE=0 -j"$(nproc)"; \
       done

# Python tools (the repository's .venv requirements).
RUN python3 -m venv /opt/venv \
    && /opt/venv/bin/pip install --no-cache-dir spimdisasm==1.42.4 pyelftools numpy pillow

# The repository itself, from the build context.
COPY . /opt/src-clean
# Pinned public shell dependencies (miniaudio, stb, font), fetched once here.
RUN cd /opt/src-clean && python3 tools/desktop/bootstrap.py
COPY tools/installer/build_inside.sh /usr/local/bin/sc64-build
RUN chmod +x /usr/local/bin/sc64-build && mkdir -p /work && chmod 1777 /work \
    && chmod -R a+rwX /opt/third_party
ENV HOME=/work
WORKDIR /work
ENTRYPOINT ["/usr/local/bin/sc64-build"]
