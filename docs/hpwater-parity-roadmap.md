# HPWater Parity Roadmap

Source target: https://github.com/AshenOneArt/HPWater, inspected at upstream commit `1253e5b`.

This document is the implementation contract for porting HPWater's Unity HDRP water pipeline into VibeEngine. The current VibeEngine implementation now has the first dedicated HPWater deferred path: a dynamic HPWater component, CPU wave mesh updates, a GPU R16F fluid height ping-pong texture, a dedicated water GBuffer, an explicit water mask, a scene-depth pyramid for refraction, a full-resolution refraction payload, a half-resolution volumetric accumulation/filter/composite path, and a full-resolution caustic energy texture that can be filtered and consumed by both water composite and volume lighting. It is not feature-complete.

## Current VibeEngine Coverage

- `HPWaterComponent` creates a runtime water mesh and simulates a visible wave field on CPU.
- `Water.shader` provides a visible HPWater-inspired surface material.
- `HPWaterGBuffer.shader` renders water into a dedicated three-target resource set: normal/roughness, scatter/thickness, and absorption/foam.
- `HPWaterMask.shader` builds a full-resolution R8 water coverage mask from the dedicated HPWater depth buffer as the current OpenGL equivalent of HPWater's stencil isolation.
- `HPWaterDepthPyramid.shader` builds an opaque scene-depth mip chain for HPWater refraction.
- `HPWaterComposite.shader` performs the first Hi-Z-assisted full-resolution refraction payload generation and final water composite, including HPWater-style frame-indexed IGN step jitter and tunable max refraction cross distance / thickness offset controls.
- `HPWaterVolume.shader` performs a first half-resolution volume accumulation pass for in-scattering, transmittance, and refracted depth.
- `HPWaterVolumeTemporal.shader` performs a first low-resolution temporal reprojection/history blend before spatial filtering.
- `HPWaterVolumeFilter.shader` performs the first multi-iteration depth-aware a-trous-style spatial filter over the half-resolution volume buffers.
- `HPWaterVolumeUpsample.shader` resolves filtered half-resolution volume buffers into full-resolution joint-bilateral volume textures.
- `HPWaterCaustic.shader` performs a first full-resolution caustic energy accumulation from HPWater normals, thickness, absorption, mask, scene depth, and directional light parameters.
- `HPWaterCausticFilter.shader` performs a first edge-aware caustic denoise/filter pass using caustic energy, HPWater depth, and the explicit water mask.
- `HPWaterVolume.shader` can inject filtered caustic energy into the directional in-scattering term so caustics affect volumetric water lighting, not only the final surface composite.
- `HPWaterFluidDynamics.shader` performs a first GPU ping-pong wave equation update into an R16F height texture, and `HPWaterGBuffer.shader` samples that texture for fluid normal and foam contribution.
- `DeferredRenderer` exports HPWater GBuffer, explicit water mask, scene-depth pyramid, refraction, volume, caustic, and final composite diagnostics.
- The editor can create, edit, serialize, and diagnose HPWater entities, and auto-export readback BMPs without relying on user screenshots or the Render Debugger panel being open.

## HPWater Source Features Not Yet Ported

### Dedicated Water GBuffer

HPWater renders water into a dedicated 3-target GBuffer:

- GBuffer0: world normal plus roughness.
- GBuffer1: scatter color and thickness.
- GBuffer2: absorption color plus foam.
- Water depth is written into the main prepass depth so later screen-space passes can distinguish water surface depth from scene depth.
- Stencil marks water pixels for later full-screen passes.

VibeEngine now has the dedicated three-target HPWater GBuffer and diagnostics. It also generates a dedicated full-resolution R8 HPWater mask texture from the HPWater depth buffer and uses it as the GPU-side pass-isolation equivalent of HPWater's stencil path. A real platform stencil path can still be added later if exact stencil parity is needed.

### Refraction Buffer

HPWater generates a full-resolution refraction texture where:

- XY stores refracted UV offset.
- Z stores refracted scene depth.
- W stores the water pixel mask.
- It can use either a cheaper normal-offset path or ray marching.
- The ray marching path uses the scene depth pyramid excluding water, exponential stepping, IGN jitter, and a max cross distance.

VibeEngine now writes a full-resolution refraction payload and metadata target during `HPWaterComposite.shader`. The current implementation builds a dedicated opaque scene-depth pyramid and uses coarse mip sampling with mip-0 binary refinement for the screen-space refraction march. The march now has HPWater-style frame-indexed IGN step jitter plus serialized controls for sample count, max refraction cross distance, and thickness offset. Refraction, volume accumulation, and volume upsample now share the explicit HPWater mask. Closer full 3D NDC ray parity remains pending.

### Volumetric Water Lighting

HPWater uses low-resolution volumetric accumulation, then filters and composites it:

- Ray-marched water scattering and transmittance.
- Absorption and scatter coefficients from the HPWater GBuffer.
- Temporal reprojection with history color, transmittance, depth, motion vectors, depth rejection, and velocity weighting.
- A-trous spatial filtering.
- Depth-aware joint bilateral upsampling.
- Final composite with full-resolution specular lighting and refraction.

VibeEngine now has the first half-resolution `HPWaterVolume.shader` accumulation target with in-scattering, transmittance, and refracted depth, plus a first temporal reprojection/history pass, a three-iteration depth-aware a-trous-style spatial filter, and a full-resolution joint-bilateral upsample pass. Explicit motion-vector input, stronger velocity/depth validation, and a closer HPWater neighborhood-clamping model remain pending.

### Caustics

HPWater caustics are a separate compute-driven pipeline:

- Renders water cascade depth and water GBuffer atlases from directional-light cascade views.
- Reads the shadow cascade atlas / scene depth.
- Refracts light through water normals.
- Accumulates caustic irradiance with atomics.
- Supports single-channel and RGB dispersion modes.
- Applies denoise / filtering and passes caustic data into deferred water lighting.

VibeEngine now has the first caustic resource slice: `HPWaterCaustic.shader` writes a full-resolution RGBA16F caustic texture, `HPWaterCausticFilter.shader` performs a two-pass edge-aware denoise/filter, `HPWaterComposite.shader` consumes the filtered texture when available, `HPWaterVolume.shader` injects filtered caustic energy into volume scattering, and diagnostics export both `render_diagnostics_hpwater_caustic.bmp` and `render_diagnostics_hpwater_caustic_filtered.bmp`. This is intentionally not full HPWater parity yet; the directional-light cascade atlas capture, compute/atomic accumulation, and RGB dispersion stages remain pending.

### Fluid Dynamics

HPWater includes a GPU wave equation system:

- Top-down virtual orthographic capture of water height.
- Top-down virtual orthographic capture of scene height.
- Ping-pong R16 height textures.
- Compute wave propagation with obstacle boundaries, edge damping, and impulse sources.
- Global wave height texture for water shaders.

VibeEngine now has a first GPU height-texture pipeline: a persistent R16F ping-pong wave equation pass, serialized component controls, Render Debugger/diagnostic export, and GBuffer sampling for fluid normals/foam. It also has the first obstacle-boundary input: Scene mesh world AABBs are rasterized into a per-ocean R8 obstacle mask and the wave pass damps/blocks propagation at those pixels. This is currently implemented as OpenGL fullscreen passes plus CPU obstacle rasterization because the engine does not yet expose a compute abstraction or HPWater's virtual top-down height capture. HPWater's true top-down water/scene height textures remain pending; the existing CPU mesh/spectrum path is still kept as the geometry fallback.

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
   - Done: explicit HPWater render resources and diagnostics.
   - Done: add a full-resolution R8 HPWater coverage mask as the GPU-side stencil-equivalent resource for later passes.
   - Done: split water payload from the regular GBuffer path.

2. Dedicated HPWater GBuffer pass
   - Done: render water into three dedicated textures.
   - Done: preserve water depth and expose it to refraction/volume passes.
   - Done: export readback diagnostics for the three water buffers.

3. Refraction pass
   - Done: add full-resolution refraction UV/depth/mask buffer.
   - Done: first implement normal-offset refraction.
   - Done: add a dedicated opaque scene-depth pyramid for Hi-Z-assisted ray-marched refraction.
   - Done: add HPWater-style IGN step jitter and max-cross-distance / thickness controls.
   - Done: route refraction through the explicit HPWater mask.
   - Pending: closer full 3D NDC ray parity.

4. Volumetric pass
   - Done: add low-resolution water volume accumulation.
   - Done: add transmittance and volume depth buffers.
   - Done: add composite into scene color.
   - Done: add first temporal history/reprojection pass for low-resolution volume buffers.
   - Done: add three-iteration depth-aware a-trous-style spatial filtering.
   - Done: add a full-resolution joint-bilateral depth-aware upsample pass.
   - Pending: add explicit motion-vector input, stronger depth rejection, and HPWater-style neighborhood clamping.

5. GPU fluid dynamics
   - Done: add persistent R16F ping-pong wave equation height textures and diagnostics.
   - Done: feed the current GPU fluid height texture into HPWater GBuffer normal/foam evaluation.
   - Done: add a first R8 obstacle mask by rasterizing non-water mesh AABBs over the ocean XZ footprint.
   - Pending: replace the AABB mask with HPWater-style top-down water height and scene height capture passes.
   - Pending: replace the fullscreen ping-pong implementation with an OpenGL compute backend once the engine has compute shader support.
   - Keep the current CPU simulation as a fallback.

6. Caustics
   - Done: add a first full-resolution caustic energy texture and feed it into surface composite.
   - Done: export caustic texture diagnostics for automated validation.
   - Done: add an edge-aware caustic filter/denoise pass and filtered caustic diagnostics.
   - Done: feed filtered caustic energy into HPWater volume lighting.
   - Pending: add directional-light cascade water atlas capture.
   - Pending: add compute caustic accumulation with atomic irradiance writes.
   - Pending: add single-channel mode first, then RGB dispersion.
   - Pending: replace the screen-space caustic approximation with HPWater-style light-space cascade caustics.

7. BSDF parity
   - Port HPWater's macro scattering, thin-layer SSS, backlit transmission, and forward-scatter blur.
   - Connect sky/environment reflection as the reflection-probe fallback until a full HDRP-style light loop exists.

## Acceptance Checks

- A scene with `HPWaterOcean` produces non-empty HPWater GBuffer textures.
- A scene with `HPWaterOcean` produces a non-empty HPWater mask texture and exports `render_diagnostics_hpwater_mask.bmp`.
- A scene with `HPWaterOcean` builds a valid HPWater scene-depth pyramid with more than one mip.
- The refraction buffer changes when water normals change and remains masked to water pixels.
- Water volume color/transmittance changes with absorption and scatter settings.
- Caustics appear under shallow waves, change with directional light direction, and produce non-empty `render_diagnostics_hpwater_caustic.bmp` and `render_diagnostics_hpwater_caustic_filtered.bmp`.
- Filtered caustics remain valid in `render_diagnostics.txt` (`HPWaterCausticFilterRan=1`, `HPWaterCausticFilteredValid=1`) and feed volume lighting through `HPWaterCausticVolumeStrength`.
- Interactive GPU fluid impulses produce a non-empty `render_diagnostics_hpwater_fluid_height.bmp`; overlapping non-water meshes produce a valid `render_diagnostics_hpwater_fluid_obstacle.bmp`.
- Debug diagnostics export all HPWater intermediate targets without user screenshots.
- The final image stays valid with water enabled, disabled, above camera, below camera, and outside the frustum.
