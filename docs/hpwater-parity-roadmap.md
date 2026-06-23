# HPWater Parity Roadmap

Source target: https://github.com/AshenOneArt/HPWater, inspected at upstream commit `1253e5b`.

This document is the implementation contract for porting HPWater's Unity HDRP water pipeline into VibeEngine. The current VibeEngine implementation is only the first visible slice: a dynamic HPWater component, CPU wave mesh updates, water flags in the existing GBuffer, and a simplified deferred lighting branch. It is not feature-complete.

## Current VibeEngine Coverage

- `HPWaterComponent` creates a runtime water mesh and simulates a visible wave field on CPU.
- `Water.shader` provides a visible HPWater-inspired surface material.
- `GBuffer.shader` marks water pixels and stores minimal water payload.
- `DeferredLighting.shader` applies water-specific Fresnel, absorption, sky reflection, roughness, and simple scattering.
- The editor can create, edit, serialize, and diagnose HPWater entities.

## HPWater Source Features Not Yet Ported

### Dedicated Water GBuffer

HPWater renders water into a dedicated 3-target GBuffer:

- GBuffer0: world normal plus roughness.
- GBuffer1: scatter color and thickness.
- GBuffer2: absorption color plus foam.
- Water depth is written into the main prepass depth so later screen-space passes can distinguish water surface depth from scene depth.
- Stencil marks water pixels for later full-screen passes.

VibeEngine currently stores only a compact water payload in the existing GBuffer. This must become a dedicated HPWater resource set so refraction, volume, and caustics can read the same data.

### Refraction Buffer

HPWater generates a full-resolution refraction texture where:

- XY stores refracted UV offset.
- Z stores refracted scene depth.
- W stores the water pixel mask.
- It can use either a cheaper normal-offset path or ray marching.
- The ray marching path uses the scene depth pyramid excluding water, exponential stepping, IGN jitter, and a max cross distance.

VibeEngine does not have this pass yet.

### Volumetric Water Lighting

HPWater uses low-resolution volumetric accumulation, then filters and composites it:

- Ray-marched water scattering and transmittance.
- Absorption and scatter coefficients from the HPWater GBuffer.
- Temporal reprojection with history color, transmittance, depth, motion vectors, depth rejection, and velocity weighting.
- A-trous spatial filtering.
- Depth-aware joint bilateral upsampling.
- Final composite with full-resolution specular lighting and refraction.

VibeEngine currently does only a single-pass approximation in deferred lighting.

### Caustics

HPWater caustics are a separate compute-driven pipeline:

- Renders water cascade depth and water GBuffer atlases from directional-light cascade views.
- Reads the shadow cascade atlas / scene depth.
- Refracts light through water normals.
- Accumulates caustic irradiance with atomics.
- Supports single-channel and RGB dispersion modes.
- Applies denoise / filtering and passes caustic data into deferred water lighting.

VibeEngine does not have caustics yet.

### Fluid Dynamics

HPWater includes a GPU wave equation system:

- Top-down virtual orthographic capture of water height.
- Top-down virtual orthographic capture of scene height.
- Ping-pong R16 height textures.
- Compute wave propagation with obstacle boundaries, edge damping, and impulse sources.
- Global wave height texture for water shaders.

VibeEngine currently has a CPU wave equation and procedural spectrum waves, but not the GPU height-texture pipeline.

### BSDF / Light Loop

HPWater's BSDF is more detailed than the current VibeEngine approximation:

- Air-to-water Fresnel with IOR around 1.33.
- GGX specular with preintegrated FGD / energy compensation in HDRP.
- Macro volume scattering.
- Thin-layer subsurface scattering.
- Backlit transmission.
- Forward scatter blur from camera color mips.
- Caustic contribution and volumetric transmittance.
- Indirect lighting strength and environment contribution controlled by global HPWater parameters.

VibeEngine currently has basic Fresnel reflection, sky reflection, absorption, and a simple directional scatter term.

## Porting Order

1. Dedicated HPWater resources
   - Add explicit HPWater render resources and diagnostics.
   - Add water mask/stencil equivalent.
   - Split water payload from the regular GBuffer path.

2. Dedicated HPWater GBuffer pass
   - Render water into three dedicated textures.
   - Preserve main scene depth and water depth semantics needed by later passes.
   - Add debug views for the three water buffers.

3. Refraction pass
   - Add full-resolution refraction UV/depth/mask buffer.
   - First implement normal-offset refraction.
   - Then add ray-marched refraction against a scene depth pyramid.

4. Volumetric pass
   - Add low-resolution water volume accumulation.
   - Add transmittance and volume depth buffers.
   - Add composite into scene color.
   - Add temporal history, a-trous filtering, and depth-aware upsample.

5. GPU fluid dynamics
   - Add top-down height capture passes.
   - Add OpenGL compute ping-pong wave equation textures.
   - Keep the current CPU simulation as a fallback.

6. Caustics
   - Add directional-light cascade water atlas capture.
   - Add compute caustic accumulation.
   - Add single-channel mode first, then RGB dispersion.
   - Feed caustic data into volume and surface lighting.

7. BSDF parity
   - Port HPWater's macro scattering, thin-layer SSS, backlit transmission, and forward-scatter blur.
   - Connect sky/environment reflection as the reflection-probe fallback until a full HDRP-style light loop exists.

## Acceptance Checks

- A scene with `HPWaterOcean` produces non-empty HPWater GBuffer textures.
- The refraction buffer changes when water normals change and remains masked to water pixels.
- Water volume color/transmittance changes with absorption and scatter settings.
- Caustics appear under shallow waves and change with directional light direction.
- Interactive impulses propagate around obstacles.
- Debug diagnostics export all HPWater intermediate targets without user screenshots.
- The final image stays valid with water enabled, disabled, above camera, below camera, and outside the frustum.
