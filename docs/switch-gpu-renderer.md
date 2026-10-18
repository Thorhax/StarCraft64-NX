# Switch GPU renderer (implemented in v1.2)

Status: implemented as planned below, with three changes found on hardware:
OpenGL runs on the nouveau Gallium driver (NV12B), not Zink; presentation
stays on the libnx framebuffer (a window-surface context beside the raster
context latched nouveau submission errors, and a single window context made
readbacks wait for vsync fences); the framebuffer is a linear SSBO with a
GPU-side copy to a staging buffer (texture readback was detiled through
uncached memory, ~30 ms a frame).

## Original plan

Goal: move rasterization off the CPU with Mesa for Horizon (NaGaa95/mesa-switch,
NVK on GM20B), so menus/briefings reach the game's 30 fps cap and the port can
run below stock CPU clocks.

## Measured starting point (v0.5, stock clocks, 4-core forwarder)

| Screen | CPU raster | Interpreter | RDRAM snapshot | fps |
|---|---|---|---|---|
| Missions 320x240 | ~19 ms | ~12 ms | ~5 ms | ~22 |
| Menu 640x480 | ~34 ms | ~17 ms | ~5 ms | ~22 |
| Briefing 640x480 | ~41 ms | ~26 ms | ~5 ms | ~13 |

Rasterization is the largest cost everywhere; it is what the GPU replaces.

## What stays the same

- `interpret_gbi` and the `DrawList` (textured rectangles with tile, combine,
  blend, alpha-compare and COPY state) are the renderer input, unchanged.
- RDRAM remains the source of truth: the VI scans out the framebuffer from
  RDRAM, and later texture loads may read it (D-019). The GPU path must leave
  RDRAM exactly where the software path would put it for every pixel it draws.

## Design

1. **One GL thread owns the EGL context and the window surface.** The guest
   thread that submits a graphics task (today it rasterizes synchronously in
   `sc64_capture_submit`) hands the DrawList to the GL thread and waits; the
   main thread asks the same GL thread to present. EGL + desktop GL 4.3 core,
   as in the CBO1-NX renderer (EGL initialization from devkitPro simple_triangle,
   link order from `portlibs/switch/lib/cmake/OpenGL/OpenGLConfig.cmake`).
2. **Target upload.** Upload the target framebuffer region from RDRAM (RGBA16)
   into an RGBA8 texture attached to an FBO, so blending sees prior contents.
3. **Texture cache by content key.** `Texture::key` already identifies decoded
   texture content across frames: decode each new key on the CPU once (the
   software `sample` path over the tile extent / `tex.pixels` for backgrounds),
   upload once, reuse every frame. Tile wrap/mirror/clamp go in the shader.
4. **One quad per Rectangle** with a fragment shader for the observed state:
   combine (1-cycle), alpha compare, COPY alpha bit, blend with the
   framebuffer read zero-extended to 5 bits, RGBA16 quantization and dither.
5. **Readback.** Read the drawn bounds back, encode RGBA16 and write them to
   RDRAM through the existing `commit_rdp_writes` contract. Unchanged pixels
   are rewritten with identical bytes, which has the same effect as no store.
6. **Validation mode.** A config switch renders each frame both ways and logs
   the per-frame pixel difference, to tune the shader against the software
   reference before making GL the default. The software path stays as fallback.
7. **Free the cores.** With GL active, drop the raster helpers; then measure
   at lower CPU clocks.

## Steps

1. `tools/switch/import_portlibs.sh` - Mesa portlibs into the Docker build.
2. EGL bring-up: present the existing RGBA image through GL instead of the
   libnx framebuffer (validates context, surface and link order).
3. GL rasterizer + texture cache + validation mode.
4. GL as default on Switch; remove helpers; clock tests; v1.0.
