# HPWater Parity Roadmap

Source target: https://github.com/AshenOneArt/HPWater, inspected at upstream commit `1253e5b`.

This document is the implementation contract for porting HPWater's Unity HDRP water pipeline into VibeEngine. The current VibeEngine implementation now has the first dedicated HPWater deferred path: a dynamic HPWater component, CPU wave mesh updates, a multi-scale directional spectral ocean layer with matching GBuffer normals, a GPU R16F fluid height ping-pong texture driven by an OpenGL compute wave update, HPWater-style top-down water/scene height-field resources for fluid obstacle boundaries, a dedicated water GBuffer, an explicit water mask, a scene-depth pyramid for refraction, a full-resolution refraction payload with HPWater-style world-space refracted rays projected into NDC for depth-pyramid marching, a half-resolution volumetric accumulation/filter/composite path with an explicit low-resolution motion-vector texture for temporal rejection plus serialized HPWater-style temporal/filter/shadow parameters, a full-resolution caustic energy texture that can be filtered and consumed by both water composite and volume lighting, a directional-light cascade water atlas capture pass, an OpenGL compute caustic irradiance path with four-channel R32UI atomic accumulation plus RGBA16F RGB resolve that samples the real light-space cascade atlas, an HPWater-style compute-first caustic denoise filter with local shared-memory halo staging, and serialized BSDF controls for environment reflection, macro scatter, thin SSS, backlit transmission, forward scatter, preintegrated FGD LUT sampling, and GGX energy compensation. It is not feature-complete.

## Current VibeEngine Coverage

- `HPWaterComponent` creates a runtime water mesh and simulates a visible wave field on CPU.
- `HPWaterComponent` now drives a 16-band directional spectrum for ocean displacement and matching per-fragment GBuffer normals, giving the ocean a visible multi-scale wave response even when mesh tessellation is coarse. The inspected HPWater source preview at `1253e5b` exposes the real-time wave-equation and shader-graph style water stack but not a directly portable FFT/Tessendorf implementation, so this is VibeEngine's spectral ocean foundation rather than a claim of complete upstream FFT parity.
- `Water.shader` provides a visible HPWater-inspired surface material.
- `HPWaterGBuffer.shader` renders water into a dedicated three-target resource set: normal/roughness, scatter/thickness, and absorption/foam.
- `HPWaterMask.shader` builds a full-resolution R8 water coverage mask from the dedicated HPWater depth buffer as the current OpenGL equivalent of HPWater's stencil isolation.
- `HPWaterDepthPyramid.shader` builds an opaque scene-depth mip chain for HPWater refraction.
- `HPWaterComposite.shader` performs Hi-Z-assisted full-resolution refraction payload generation and final water composite, including HPWater-style world-space refracted ray construction, NDC projection, exponential depth-pyramid marching, frame-indexed IGN step jitter, and tunable max refraction cross distance / thickness offset controls.
- `HPWaterVolume.shader` performs a first half-resolution volume accumulation pass for in-scattering, transmittance, and refracted depth. Its volume loop samples the active directional CSM through HPWater-style serialized shadow parameters: shadow softness, minimum filter size, blocker sample count, and filter sample count.
- `HPWaterVolumeMotionVector.shader` generates an explicit half-resolution RG16F volume velocity texture from current/previous view-projection reprojection of the refracted world payload.
- `HPWaterVolumeTemporal.shader` performs low-resolution temporal reprojection/history blend before spatial filtering, consuming the explicit motion-vector texture for velocity rejection while retaining current/previous view-projection reprojection as fallback, plus depth rejection and HPWater/HDRP-style 3x3 current-neighborhood color/transmittance clamp. `HPWaterComponent` now serializes the HPWater defaults for temporal blend, motion-vector usage, velocity scale, temporal depth rejection, and depth threshold.
- `HPWaterVolumeFilter.shader` performs the first multi-iteration depth-aware a-trous-style spatial filter over the half-resolution volume buffers. `HPWaterComponent` now serializes the spatial filter enable flag, iteration count, depth-aware flag, and depth sensitivity so the pass no longer runs a fixed three-iteration filter.
- `HPWaterVolumeUpsample.shader` resolves filtered half-resolution volume buffers into full-resolution joint-bilateral volume textures.
- `HPWaterCaustic.shader` performs a first full-resolution caustic energy accumulation from HPWater normals, thickness, absorption, mask, scene depth, and directional light parameters, including a serialized single-channel/RGB dispersion mode inspired by HPWater's `_Is_Use_RGB_Caustic` and `_CausticDispersionStrength` controls. When compute caustics are available in the square cascade atlas it projects receivers into cascade space, clamps atlas tile samples to half-texel bounds, fades tile edges, and blends into the next cascade near split boundaries.
- `HPWaterCausticFilter.comp` is the primary HPWater-style caustic denoise/filter path. It runs in 16x16 compute groups, stages a 18x18 shared-memory color tile with one-pixel halo/corners, and applies the upstream 3x3 a-trous luminance edge rule (`ComputeEdgeWeight`, `1/16, 2/16, 4/16`, `atrousLuminanceWeight = 0.5`) with HPWater depth and explicit water-mask guards. `HPWaterCausticFilter.shader` remains the fullscreen fallback with the same kernel semantics.
- `HPWaterCausticAtlas.shader` captures water-only surface payloads from the four directional-light cascade views into a 2x2 RGBA16F atlas, reusing the current CSM light view-projection matrices as the first HPWater-style light-space caustic input resource.
- `HPWaterCausticIrradiance.comp` is the first OpenGL compute slice of HPWater caustics: it scatters refracted caustic energy from the HPWater GBuffer, explicit mask, scene depth, light parameters, the current light-space water atlas, and the active directional shadow-depth cascade array into four square R32UI atomic cascade-atlas irradiance textures. The compute pass reconstructs water world positions from the camera depth, selects the matching directional-light cascade from the current CSM split distances, projects the water point through the uploaded cascade VP matrix, samples the real 2x2 water atlas tile instead of synthesizing cascade tiles from screen UVs, performs a HPWater-style exponentially distributed refracted light ray march with frame-indexed R2 dither against the shadow-depth texture array, gates caustic leakage with a serialized `_CausticShadowAlphaClipThreshold`-equivalent receiver visibility threshold, and now writes receiver irradiance in light-space cascade atlas UVs. In RGB caustic mode it projects R/B receivers with HPWater-style per-channel eta offsets in the same atlas receiver space, applies atlas receiver edge falloff, and adds a conservative per-channel spectral energy weighting.
- `HPWaterCausticIrradianceResolve.comp` resolves the atomic R/G/B/A irradiance textures into a full-resolution RGBA16F texture that `HPWaterCaustic.shader` consumes during full-resolution caustic accumulation while still retaining the previous fullscreen path as the visible fallback.
- `HPWaterVolume.shader` can inject filtered caustic energy into the directional in-scattering term so caustics affect volumetric water lighting, not only the final surface composite.
- `HPWaterFluidHeightCapture.shader` captures the first top-down R16F water-height and scene-height inputs used by fluid dynamics. The virtual capture camera now follows HPWater's down-facing camera convention with world +Z as camera up, so captured texture V coordinates match `WorldPosToFluidUV()`. The water target now rasterizes the displaced HPWater surface height from the actual water draw, while the scene target rasterizes actual opaque geometry world height, using max blending to form an obstacle height field. Scene-height capture now excludes the active water layer, matching HPWater's `~waterLayerMask` filtering rule. The CPU fallback also samples the current HPWater dynamic mesh/spectrum height instead of only uploading a flat ocean height. The same 16-band directional spectrum is evaluated in `HPWaterGBuffer.shader` so fluid normals and spectral normals combine in the dedicated water GBuffer instead of only changing CPU vertices. `HPWaterComponent::FluidStartFrameBake` mirrors HPWater's `startFrameBakeProcess`: when enabled, matching resolution/box captures are reused after the first valid bake instead of redrawn every frame.
- `HPWaterFluidDynamics.comp` performs the primary GPU ping-pong wave equation update into an R16F height texture, with `HPWaterFluidDynamics.shader` kept as a fullscreen fallback. It now consumes HPWater-style normalized water-height and scene-height textures first, then falls back to the older R8 obstacle mask if the height fields are unavailable. Its wave equation, boundary damping, and source injection now match HPWater's neighbor-reflection update without VibeEngine-specific obstacle-contact damping, wave-speed clamp, or output-height clamp, while retaining HPWater's X/Y edge absorption formula and positive-only, 0.05-clamped Gaussian source impulse. `HPWaterGBuffer.shader` samples the fluid height texture for fluid normal and foam contribution using HPWater-style world-to-fluid UVs with clamp sampling at the simulation box edge.
- `HPWaterComposite.shader` and `HPWaterVolume.shader` expose the first HPWater-style BSDF/scatter controls: Schlick Fresnel using water IOR 1.33, environment reflection intensity, HPWater's `_IndirectLightStrength` for diffuse indirect water body lighting, a HPWaterBSDFLibary-style 16-step exponential indirect scattering integration, macro volume scatter strength, thin-layer SSS, backlit transmission, forward scattering, preintegrated split-sum FGD LUT sampling, analytic FGD fallback, and GGX energy compensation. The direct diffuse body split now follows HPWaterBSDFLibary's component weighting shape: `diffR = T_entry * G_entry * S_volume`, `diffT = thinLayerSSS + backlitTransmission`, with HPWater-style `G_sss`, `G_backlit`, SSS/backlit path scales, scatter phase, and Beer-Lambert transmittance. The forward-scatter blur now uses HPWater's `CalculateHPWaterMipLevel(depth, FORWARD_SCALING_FACTOR, scatterDensity, 6)` shape with serialized `ForwardScatterBlurDensity`, `MultiScatterScale`, and `_PhaseG = 0.80` controls. The composite pass now samples the active sky texture as an equirectangular reflection source, runs a water-local screen-space reflection ray march before environment fallback, can consume and distance-blend the two nearest baked reflection-probe cubemaps when available, applies HDRP-style box-projected cubemap directions from each probe's world center and box size, weights probes by influence volume before falling back to the sky/ground gradient, and exports the SSR/probe hierarchy state in diagnostics. It also mirrors HPWater's `_WaterDispersionStrength = 0.1` default with clamped refracted-UV long/mid/short color sampling, currently applied in the composite forward-scatter scene-color path as the closest VibeEngine equivalent to HPWater's deferred volume composite. These controls and bindings are serialized or exported in render diagnostics.
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

VibeEngine now writes a full-resolution refraction payload and metadata target during `HPWaterComposite.shader`. The current implementation builds a dedicated opaque scene-depth pyramid, computes a HPWater-style world-space refracted ray from the camera-to-water direction, water normal, flat-normal subtraction, and water-cross direction, projects the ray into NDC, then marches that 3D NDC line with exponential steps, depth-pyramid coarse sampling, and mip-0 binary refinement. The march has HPWater-style frame-indexed IGN step jitter plus serialized controls for sample count, max refraction cross distance, and thickness offset. Refraction, volume accumulation, and volume upsample now share the explicit HPWater mask. Remaining refraction parity work is closer validation against HPWater's exact Unity projection conventions and edge-case tuning.

### Volumetric Water Lighting

HPWater uses low-resolution volumetric accumulation, then filters and composites it:

- Ray-marched water scattering and transmittance.
- Absorption and scatter coefficients from the HPWater GBuffer.
- Temporal reprojection with history color, transmittance, depth, motion vectors, depth rejection, and velocity weighting.
- A-trous spatial filtering.
- Depth-aware joint bilateral upsampling.
- Final composite with full-resolution specular lighting and refraction.

VibeEngine now has a half-resolution `HPWaterVolume.shader` accumulation target with in-scattering, transmittance, and refracted depth, plus a temporal reprojection/history pass, a configurable depth-aware a-trous-style spatial filter, and a full-resolution joint-bilateral upsample pass. The accumulation pass now follows the HPWater reference more closely by marching the refracted water ray with exponentially distributed samples and frame-indexed IGN jitter, accumulating directional in-scattering, scene in-scattering, and transmittance through the water volume instead of relying on a single analytic approximation. The volume loop now has a HPWater-style `_HPWaterShadowParams` equivalent: serialized volume shadow softness, minimum filter size, blocker samples, and filter samples are routed through `DeferredRenderer` into a water-specific CSM visibility function. The temporal/filter path now exposes HPWater-style serialized defaults for temporal blend (`0.9`), motion-vector usage, velocity rejection scale (`0.1`), temporal depth rejection and threshold (`0.5`), spatial filter enable, spatial filter iterations (`2`), spatial depth awareness, and spatial depth sensitivity (`100`). The temporal pass generates and consumes an explicit RG16F volume motion-vector texture when enabled, rejects mismatched history with both center-depth and neighborhood-depth tests when enabled, clamps history color/transmittance to the current 3x3 neighborhood before blending, and the spatial filter can be skipped without breaking volume upsample because upsample falls back to temporal/raw volume data. True parity with HPWater/HDRP's full scene/object motion-vector source, validation heuristics, and exact Unity projection conventions remains pending.

### Caustics

HPWater caustics are a separate compute-driven pipeline:

- Renders water cascade depth and water GBuffer atlases from directional-light cascade views.
- Reads the shadow cascade atlas / scene depth.
- Refracts light through water normals.
- Accumulates caustic irradiance with atomics.
- Supports single-channel and RGB dispersion modes.
- Applies denoise / filtering and passes caustic data into deferred water lighting.

VibeEngine now has the first caustic resource slices: `HPWaterCaustic.shader` writes a full-resolution RGBA16F caustic texture, supports a screen-space single-channel/RGB dispersion mode, consumes a compute-generated RGBA16F irradiance texture, `HPWaterCausticFilter.comp` performs the primary compute denoise/filter pass with HPWater-style 16x16 thread groups and 18x18 LDS halo staging, `HPWaterCausticFilter.shader` remains a fullscreen fallback with the same 3x3 a-trous luminance edge kernel, `HPWaterComposite.shader` consumes the filtered texture when available, `HPWaterVolume.shader` injects filtered caustic energy into volume scattering, and `HPWaterCausticAtlas.shader` captures water-only payloads from all four directional-light cascade views into a 2x2 atlas. `HPWaterCausticIrradiance.comp` now uses HPWater-style multi-writer atomic irradiance accumulation through four R32UI square cascade-atlas channel textures, samples the actual light-space water atlas by reconstructing world position and applying the current CSM cascade VP matrices, binds the active CSM shadow-depth texture array for a HPWater-style exponentially distributed refracted light ray march with frame-indexed R2 dither, gates leakage with a serialized `CausticShadowAlphaClipThreshold` defaulting to HPWater's 0.8, writes per-channel R/G/B caustic receivers in cascade atlas UV space instead of camera UV space, fades atlas receiver tile edges, and applies conservative per-channel spectral energy weights in RGB mode. `HPWaterCaustic.shader` projects the current receiver world position back into the matching cascade atlas, clamps samples to tile-safe half-texel bounds, fades tile edges, blends into the next cascade near split boundaries to reduce cascade seam discontinuities, and now uses HPWater GBuffer absorption/scatter plus receiver thickness as a transmittance and leak-reduction gate. Diagnostics export `render_diagnostics_hpwater_caustic.bmp`, `render_diagnostics_hpwater_caustic_compute_irradiance.bmp`, `render_diagnostics_hpwater_caustic_filtered.bmp`, and `render_diagnostics_hpwater_caustic_atlas.bmp`, and report whether the atlas, compute irradiance path, atomic accumulation path, shadow-depth cascade array, exponential light-space stepping, frame-indexed dither, atlas receiver output, RGB receiver projection, cascade blend, atlas edge filtering, spectral weighting, caustic transmittance mask, HPWater-style caustic filter kernel, compute filter, and LDS halo paths were consumed by accumulation. Remaining caustic parity work is exact Unity/HDRP projection validation.

### Fluid Dynamics

HPWater includes a GPU wave equation system:

- Top-down virtual orthographic capture of water height.
- Top-down virtual orthographic capture of scene height.
- Ping-pong R16 height textures.
- Compute wave propagation with obstacle boundaries, edge damping, and impulse sources.
- Global wave height texture for water shaders.

VibeEngine now has a first GPU height-texture pipeline: a persistent R16F ping-pong wave equation pass, serialized component controls, Render Debugger/diagnostic export, and GBuffer sampling for fluid normals/foam. It also has the first HPWater-style obstacle-boundary input: normalized water-height and scene-height textures are generated through top-down R16F capture targets for the ocean simulation volume, and the wave pass treats pixels as blocked when `sceneHeight > waterHeight`. The water-height target now comes from the displaced HPWater water draw, and its CPU fallback samples the current HPWater dynamic mesh/spectrum height. The scene-height target now rasterizes actual opaque geometry world height with max blending instead of writing each object's AABB top height across its full footprint, and scene candidates exclude the active water layer to match HPWater's `~waterLayerMask` capture rule. The top-down GPU capture now matches HPWater's `LookRotation(Vector3.down, Vector3.forward)` convention by using world +Z as the virtual camera up vector, keeping captured V coordinates aligned with `WorldPosToFluidUV()`. The capture path also supports HPWater's start-frame bake/cache mode through `FluidStartFrameBake`, reusing cached water/scene height captures when the simulation resolution and box still match. The GPU and fullscreen fallback wave passes now use HPWater's neighbor-reflection wave equation, 2D edge absorption calculation, positive Gaussian source clamp, raw wave speed, zero height-obstacle epsilon, and unclamped R16F output; diagnostics expose `HPWaterFluidEdgeAbsorptionParityEnabled`, `HPWaterFluidSourceClampEnabled`, and `HPWaterFluidWaveEquationParityEnabled`. The water GBuffer sampling path now matches HPWater's `HPWaterFluidSample.hlsl` clamp-sampler behavior for world-to-fluid UVs instead of zeroing samples outside the box. The dedicated GBuffer also evaluates the serialized directional spectrum per fragment, with diagnostics exposing `HPWaterSpectralOceanEnabled`, `HPWaterSpectralNormalParityEnabled`, `HPWaterSpectrumAmplitude`, and `HPWaterSpectrumNormalStrength`. The older CPU AABB height upload and per-ocean R8 obstacle mask remain as fallback/diagnostic resources. The primary update path now runs through `HPWaterFluidDynamics.comp`, so the fluid simulation no longer depends on the earlier fullscreen fragment pass except as a fallback. True HPWater parity still needs stricter HPWater virtual-camera render-queue rules, a real GPU spectral/FFT source if upstream parity requires it, and closer validation of capture-space conventions.

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

VibeEngine now has a partial BSDF/light-loop bridge: Schlick Fresnel uses an air-to-water IOR of 1.33, environment reflection intensity is serialized, HPWater's `_IndirectLightStrength` is serialized and drives a HPWaterBSDFLibary-style 16-step exponential diffuse indirect scattering integration separately from the scene-wide indirect setting, and the composite pass now weights the direct water body terms using HPWater's `diffR/diffT` split (`T_entry`, `G_entry`, `G_sss`, `G_backlit`, SSS/backlit path scales, scatter phase, and Beer-Lambert transmittance). It also adds HPWater-formula camera-color mip forward-scatter blur, active sky-texture reflection, water-local SSR ray marching that fills the first reflection hierarchy slot, two-nearest baked reflection-probe sampling with distance/influence-weighted blending when available, HDRP-style box-projected reflection directions for each bound probe, a generated 128x128 preintegrated split-sum FGD LUT with analytic fallback, and a serialized GGX energy compensation factor, while the low-resolution volume pass exposes macro scatter strength. Render diagnostics now export HPWater forward-scatter blur density / multi-scatter scale, HPWater indirect light strength, whether exponential indirect scatter integration is active, whether BSDF component weighting is active, whether HPWater SSR hierarchy blending is enabled, and whether reflection probe box projection is active plus the selected primary/secondary probe centers and box sizes. This is still not HDRP/HPWater parity: exact SSR validation, multiple light accumulation, and fuller HDRP light-loop parity remain pending.

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
   - Done: replace the transitional 2D normal-offset march with HPWater-style world-space refracted rays projected into NDC, using exponential/dithered depth-pyramid marching and diagnostics.
   - Pending: closer validation against HPWater's exact Unity projection conventions and edge-case tuning.

4. Volumetric pass
   - Done: add low-resolution water volume accumulation.
   - Done: add transmittance and volume depth buffers.
   - Done: add composite into scene color.
   - Done: add first temporal history/reprojection pass for low-resolution volume buffers.
   - Done: add current/previous view-projection motion reprojection, stronger neighborhood-depth rejection, and HPWater/HDRP-style 3x3 history neighborhood clamp.
   - Done: add an explicit half-resolution motion-vector texture input for the volume temporal filter, with matrix reprojection kept as fallback.
   - Done: replace the transitional single-step volume approximation with a HPWater-style exponentially distributed multi-sample ray integration path, including frame-indexed IGN jitter and diagnostics.
   - Done: add three-iteration depth-aware a-trous-style spatial filtering.
   - Done: add a full-resolution joint-bilateral depth-aware upsample pass.
   - Pending: add HPWater/HDRP directional shadow queries per volume sample, full scene/object motion-vector parity, and closer validation heuristics.

5. GPU fluid dynamics
   - Done: add persistent R16F ping-pong wave equation height textures and diagnostics.
   - Done: feed the current GPU fluid height texture into HPWater GBuffer normal/foam evaluation.
   - Done: add a first R8 obstacle mask by rasterizing non-water mesh AABBs over the ocean XZ footprint.
   - Done: replace the fullscreen ping-pong implementation with an OpenGL compute backend, keeping the old fullscreen shader as fallback.
   - Done: add first HPWater-style R16F water-height and scene-height resources, feed them into the compute obstacle test, and export diagnostics.
   - Done: add first top-down water/scene height capture draw passes and route successful captures into the fluid compute path.
   - Done: replace the transitional constant water-height upload with displaced HPWater surface height capture and CPU fallback sampling.
   - Done: replace the remaining per-object scene top-height approximation with actual top-down geometry height capture.
   - Pending: stricter HPWater layer/filtering parity and validate GPU FFT source/capture-space conventions.
   - Keep the current CPU simulation as a fallback.

6. Caustics
   - Done: add a first full-resolution caustic energy texture and feed it into surface composite.
   - Done: export caustic texture diagnostics for automated validation.
   - Done: add an edge-aware caustic filter/denoise pass and filtered caustic diagnostics.
   - Done: feed filtered caustic energy into HPWater volume lighting.
   - Done: add directional-light cascade water atlas capture and diagnostics.
   - Done: add the first OpenGL compute caustic irradiance pass and feed its output into fullscreen caustic accumulation.
   - Done: replace the transitional direct-write compute irradiance model with HPWater-style atomic irradiance writes.
   - Done: add RGB dispersion to the compute irradiance path and resolve it into RGB caustic energy.
   - Done: replace the compute irradiance path's synthetic screen-space atlas tile lookup with real light-space water cascade atlas projection using current CSM VP matrices and split distances.
   - Done: bind the active CSM shadow-depth cascade array into the caustic compute path and use it for a first finite-step refracted light ray march from water surface to receiver before camera-space atomic scatter.
   - Done: add first HPWater-style per-channel RGB caustic receiver projection, using green-channel shadow-depth hit distance plus R/B eta offsets before atomic scatter.
   - Done: add tuned HPWater-style exponential light-space stepping with frame-indexed R2 dither and diagnostics.
   - Done: replace the transitional camera-space receiver scatter with first atlas-space receiver UV output and matching atlas-space compute sampling in the fullscreen caustic pass.
   - Done: replace the reused full-resolution atomic texture with a HPWater-style square caustic cascade atlas resource.
   - Done: add HPWater-style absorption/scatter transmittance and receiver-thickness leak reduction controls, serialization, Render Debugger display, and diagnostics.
   - Done: replace the fullscreen-only caustic denoise path with a compute-first HPWater-style 16x16 group / 18x18 LDS halo filter while keeping the fragment shader fallback.
   - Pending: exact Unity/HDRP projection validation.

7. BSDF parity
   - Done: add serialized macro scattering, thin-layer SSS, backlit transmission, forward scatter, and environment reflection controls.
   - Done: add serialized `_IndirectLightStrength` parity for HPWater diffuse indirect body lighting.
   - Done: replace the previous single-term indirect body approximation with a HPWaterBSDFLibary-style 16-step exponential indirect scattering integration path and diagnostics.
   - Done: route the controls through `DeferredRenderer`, `HPWaterComposite.shader`, `HPWaterVolume.shader`, Render Debugger, and `render_diagnostics.txt`.
   - Done: generate a camera-color mip chain for the HPWater composite and use it for thickness-driven forward-scatter blur.
   - Done: feed HPWater composite from the scene light-loop inputs: camera position, directional light, sky/ground indirect colors, indirect intensity, and sky reflection intensity.
   - Done: connect the current sky-gradient environment as the reflection fallback.
   - Done: feed HPWater composite with the active equirectangular sky texture and the nearest baked reflection-probe cubemap when available, with diagnostics for both sources.
   - Done: blend the two nearest baked reflection-probe cubemaps in HPWater composite using distance-weighted secondary probe contribution, with diagnostics for secondary texture and blend factor.
   - Done: add serialized split-sum FGD approximation and GGX energy compensation controls, route them into HPWater composite, and export diagnostics.
   - Done: generate and bind a 128x128 preintegrated split-sum FGD LUT for HPWater composite, keeping the analytic approximation as fallback.
   - Done: replace the transitional forward-scatter blur LOD with HPWater's mip-level formula and serialized `_ForwardScatterBlurDensity` / `_MultiScatterScale` equivalents.
   - Done: add HDRP-style box-projected reflection directions for baked reflection probes.
   - Done: replace the previous ad hoc forward/thin/backlit addends with HPWaterBSDFLibary-style `diffR/diffT` component weighting and diagnostics.
   - Done: add influence-volume weighting for baked reflection probes and blend the probe hierarchy with the sky fallback.
   - Done: add a HPWater composite-local SSR ray march that occupies the first reflection hierarchy slot before probe/sky fallback.
   - Pending: exact HDRP SSR validation, multiple-light accumulation, and full light-loop parity.

## Acceptance Checks

- A scene with `HPWaterOcean` produces non-empty HPWater GBuffer textures.
- A scene with `HPWaterOcean` produces a non-empty HPWater mask texture and exports `render_diagnostics_hpwater_mask.bmp`.
- A scene with `HPWaterOcean` builds a valid HPWater scene-depth pyramid with more than one mip.
- The refraction buffer changes when water normals change, remains masked to water pixels, and reports `HPWaterRefractionNDCMarchEnabled=1` when the NDC depth-pyramid march is active.
- Water volume color/transmittance changes with absorption and scatter settings; accumulation reports `HPWaterVolumeExponentialIntegrationEnabled=1`, `HPWaterVolumeShadowSamplingEnabled=1`, and `HPWaterVolumeSampleCount=16`, and temporal volume filtering reports `HPWaterVolumeTemporalNeighborhoodClampEnabled=1`, `HPWaterVolumeTemporalMotionReprojectionEnabled=1`, `HPWaterVolumeExplicitMotionVectorEnabled=1`, a non-zero `HPWaterVolumeMotionVectorTexture`, and a positive `HPWaterVolumeTemporalNeighborhoodClampStrength` when history filtering runs.
- Water volume shadow filtering reports `HPWaterVolumeShadowParamsEnabled=1` when shadows are initialized, and exports `HPWaterVolumeShadowSoftness`, `HPWaterVolumeShadowMinFilterSize`, `HPWaterVolumeShadowBlockerSamples`, and `HPWaterVolumeShadowFilterSamples`.
- Caustics appear under shallow waves, change with directional light direction, and produce non-empty `render_diagnostics_hpwater_caustic.bmp` and `render_diagnostics_hpwater_caustic_filtered.bmp`.
- Light-space caustic atlas capture runs when HPWater and shadows are enabled, produces a non-empty `render_diagnostics_hpwater_caustic_atlas.bmp`, and reports `HPWaterCausticAtlasRan=1`, `HPWaterCausticAtlasValid=1`, `HPWaterCausticAtlasConsumed=1`, and four cascade draws in `render_diagnostics.txt`.
- Compute caustic irradiance runs when caustics are enabled, produces a non-empty `render_diagnostics_hpwater_caustic_compute_irradiance.bmp`, and reports `HPWaterCausticComputeRan=1`, `HPWaterCausticComputeValid=1`, `HPWaterCausticComputeAtomicEnabled=1`, and a non-zero `HPWaterCausticComputeAtomicTexture`.
- Compute caustic irradiance receives the current camera inverse view-projection, four water cascade VP matrices, and four CSM split distances so atlas sampling remains stable with camera/light movement.
- When shadows are initialized, compute caustic irradiance reports `HPWaterCausticShadowDepthConsumed=1` so the shadow-depth ray-march input is externally verifiable.
- When shadows are initialized, compute caustic irradiance reports `HPWaterCausticExponentialLightStepsEnabled=1` and `HPWaterCausticFrameDitherEnabled=1`, proving the HPWater-style dithered exponential light-space ray march is active.
- When the light-space water atlas and shadow-depth cascade array are valid, compute caustic irradiance reports `HPWaterCausticAtlasReceiverOutputEnabled=1`, proving the receiver writes and fullscreen caustic sampling are using cascade atlas UVs rather than camera UVs.
- In the same state, `HPWaterCausticComputeSize` must match the square caustic atlas dimensions rather than the main viewport dimensions.
- When RGB caustics are enabled with non-zero dispersion and shadow-depth input is valid, compute caustic irradiance reports `HPWaterCausticRGBReceiverProjectionEnabled=1`.
- In the same RGB/atlas state, caustic diagnostics report `HPWaterCausticCascadeBlendEnabled=1`, `HPWaterCausticAtlasEdgeFilterEnabled=1`, and `HPWaterCausticSpectralWeightingEnabled=1`.
- Caustic accumulation reports `HPWaterCausticTransmittanceMaskEnabled=1` and exports serialized `HPWaterCausticTransmittanceStrength`, `HPWaterCausticLeakReduction`, `HPWaterCausticShadowAlphaClipThreshold`, and `HPWaterCausticScatterBoost` values while keeping `HPWaterCausticValid=1`.
- Filtered caustics remain valid in `render_diagnostics.txt` (`HPWaterCausticFilterRan=1`, `HPWaterCausticFilteredValid=1`), report `HPWaterCausticFilterKernelParityEnabled=1`, `HPWaterCausticFilterComputeParityEnabled=1`, `HPWaterCausticFilterLDSHaloEnabled=1`, plus the serialized `HPWaterCausticFilterLuminanceWeight`, and feed volume lighting through `HPWaterCausticVolumeStrength`.
- Interactive GPU fluid impulses run through the compute backend (`HPWaterFluidComputeRan=1`) and produce a non-empty `render_diagnostics_hpwater_fluid_height.bmp`; overlapping non-water meshes produce valid `render_diagnostics_hpwater_fluid_obstacle.bmp`, `render_diagnostics_hpwater_fluid_water_height.bmp`, and `render_diagnostics_hpwater_fluid_scene_height.bmp`, with `HPWaterFluidHeightCaptureRan=1`, `HPWaterFluidHeightCaptureValid=1`, `HPWaterFluidCaptureSpaceParityEnabled=1`, `HPWaterFluidLayerFilteringParityEnabled=1`, `HPWaterFluidDisplacedWaterHeightCapture=1`, `HPWaterFluidSceneGeometryHeightCapture=1`, `HPWaterFluidHeightFieldValid=1`, `HPWaterFluidEdgeAbsorptionParityEnabled=1`, `HPWaterFluidSourceClampEnabled=1`, `HPWaterFluidWaveEquationParityEnabled=1`, and `HPWaterFluidSampleClampParityEnabled=1`. When `FluidStartFrameBake` is enabled and the fluid resolution/box is unchanged after the first valid capture, diagnostics report `HPWaterFluidStartFrameBakeEnabled=1` and `HPWaterFluidHeightCaptureCacheReused=1` while the cached height textures remain valid.
- Spectral ocean normals run in the dedicated water GBuffer and report `HPWaterSpectralOceanEnabled=1`, `HPWaterSpectralNormalParityEnabled=1`, plus positive `HPWaterSpectrumAmplitude` and `HPWaterSpectrumNormalStrength` values. The normal/roughness diagnostic texture remains non-empty so wave normal detail is externally verifiable without relying on a user screenshot.
- BSDF controls are serialized and visible in diagnostics (`HPWaterEnvironmentReflectionIntensity`, `HPWaterIndirectLightStrength`, `HPWaterIndirectScatterIntegrationEnabled`, `HPWaterBSDFComponentWeightingEnabled`, `HPWaterMacroScatterStrength`, `HPWaterThinSSSStrength`, `HPWaterBacklitTransmissionStrength`, and `HPWaterForwardScatterStrength`), and changing them affects composite/volume lighting without disabling the HPWater passes.
- HPWater forward-scatter blur controls are serialized and visible in diagnostics (`HPWaterForwardScatterBlurDensity` and `HPWaterMultiScatterScale`), and the composite shader uses the HPWater mip-level formula for camera-color blur selection.
- FGD and energy compensation controls are serialized and visible in diagnostics (`HPWaterSpecularFGDStrength` and `HPWaterGGXEnergyCompensation`) while HPWater composite remains valid. The generated preintegrated LUT reports `HPWaterPreintegratedFGDLUTValid=1` and `HPWaterPreintegratedFGDLUTResolution=128`.
- Forward-scatter blur uses the generated scene-color mip chain and reports `HPWaterForwardScatterMipEnabled=1` with more than one mip when HPWater composite runs.
- HPWater composite consumes scene light-loop inputs and reports `HPWaterLightLoopInputsValid=1`, plus sky reflection, indirect diffuse, and directional light intensities in `render_diagnostics.txt`.
- HPWater composite reports environment reflection inputs in `render_diagnostics.txt`; the launcher scene binds `HPWaterSkyTextureReflectionBound=1` through `Assets/Skybox/Sky_Sunny.hdr`, while `HPWaterReflectionProbeBound` becomes 1 when the camera is inside a baked reflection probe influence box. Probe blending now exports `HPWaterReflectionProbeInfluenceWeight`, `HPWaterReflectionProbeHierarchyWeight`, and `HPWaterReflectionProbeInfluenceBlendEnabled`; the composite shader uses that hierarchy weight to blend box-projected probe lighting with sky fallback instead of hard-switching away from sky reflection.
- HPWater composite reports `HPWaterSSRReflectionEnabled`, `HPWaterSSRHierarchyBlendEnabled`, `HPWaterSSRMaxSteps`, `HPWaterSSRStepSize`, `HPWaterSSRThickness`, and `HPWaterSSRMaxDistance`; when pipeline SSR is enabled, the water composite performs a screen-space reflection march before falling back to reflection probes and sky lighting.
- Debug diagnostics export all HPWater intermediate targets without user screenshots.
- The final image stays valid with water enabled, disabled, above camera, below camera, and outside the frustum.
