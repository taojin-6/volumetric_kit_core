# Scenario configuration

The optional CPU-only `volumetric_kit::core_config` target owns the family's
JSON format, typed settings, defaults, and static validation. Applications
load one scenario file and adapt its validated values to their library APIs.
They check resource existence, hardware capabilities and algorithm results at
runtime. JSON and GPU types do not appear in the public config API.

```cmake
set(VKC_WITH_CONFIG ON)
# Declare/fetch the pinned core before its siblings.
vkc_require_core(0.8.0 CONFIG)
target_link_libraries(my_app PRIVATE volumetric_kit::core_config)
```

```cpp
#include "volumetric_kit/core/config/config.hpp"
namespace cfg = volumetric_kit::core::config;
// Inside a function returning Status or Result:
VKC_ASSIGN(const auto settings, cfg::load("dataset.json", cfg::Application::FuseReplica));
```

Desktop entry points accept `--config PATH` or `--help`/`-h`.
`parse_args` returns the request without printing or reading a file; `usage()`
returns a static string. The iOS scanner loads a bundled file through `load`.
No general option registration API is published.

## Required envelope and source

`version` is the integer `1`; `operation` is required. A source object is
required for dataset, live and mesh consumers. Whenever `source` is present,
its `kind` is required. Different files describe different scenarios:

```json
{
  "version": 1,
  "operation": "fuse",
  "source": {
    "kind": "dataset",
    "format": "replica",
    "path": "../datasets/office0"
  },
  "fusion": { "voxel_size_m": 0.02 },
  "run": { "frames": 400 },
  "output": { "path": "results/office0.ply" }
}
```

```json
{
  "version": 1,
  "operation": "stream",
  "source": {
    "kind": "live",
    "driver": "orbbec",
    "sync_config": "rig-sync.json",
    "color_codec": "mjpeg",
    "color_width": 3840,
    "color_height": 2160,
    "depth_width": 640,
    "depth_height": 576,
    "fps": 25
  },
  "run": { "frames": 60000 }
}
```

Paths, including default output paths, are resolved against the file's
containing directory, never against whichever directory the process was
launched from. `parse(text, application, base_directory)` supplies that base
explicitly; an empty base uses the working directory. An omitted dataset
`camera_parameters` becomes `source.path/../cam_params.json`. Existing rig
sync and calibration files retain their domain formats and are referenced by
path. Configuration is separate from captured data, calibration results and
session manifests.

## Application profiles

The application argument selects existing defaults and supported fields; it
does not override the requested operation or source discriminator.

| Profile | Operation | Source | Important defaults |
| --- | --- | --- | --- |
| `FuseReplica` | `fuse` | `dataset`, `replica`; path required | voxel 0.02 m, frames 2^30, mesh every 50, output `fuse_room0.ply` |
| `CodecReplica` | `codec` | `dataset`, `replica`; path required | voxel 0.01 m, frames 2^30, encode every frame, no outputs |
| `CodecMesh` | `codec` | `mesh`; path, one scale (height or unit), one up direction required | voxel 0.01 m, truncation 4 voxels, signed SDF, shell thickness 1.5 voxels, one frame from number 0 |
| `FuseOrbbec` | `fuse` or `stream` | `live`, `orbbec` | colour 1280x720 HEVC, depth 640x576, 30 fps, 300 frames; fusion output `fuse_orbbec.ply` |
| `FuseRender` | `fuse` | `dataset`, `replica`; path required | 400 frames, 1280x720, yaw 45 degrees, pitch 30 degrees, output `fuse_render.png` |
| `FuseViewer` | `fuse` | `dataset`, `replica`; path required | 400 frames, 1280x720, overlay, lit shading, remesh every frame |
| `RigViewer` | `view` | `live`, `orbbec`; sync config required | voxel 0.01 m, dynamic fusion, 1280x720, overlay, unlit shading, unlimited frames/sets |
| `GfxTriangle` | `view` | omitted | 800x600, validation, unlimited frames |
| `GfxImGui`, `GfxImage` | `view` | omitted | 1024x720, validation, unlimited frames |
| `GfxModel` | `view` | omitted for built-in cube; `model` with optional path | 1024x720, validation, unlimited frames; screenshot selects headless rendering |
| `Scanner` | `fuse` | `live`, `arkit` | voxel 0.01 m, truncation 0.03 m, 1024 initial / 32768 maximum buckets, texture and timings enabled |
| `CalibProjectPoint` | `project` | omitted | 1280x720, intrinsics [1000,1000,640,360], Brown distortion [-0.28,0.10,0.001,-0.0005,0], point [0.1,-0.2,2] |

The `Config` structs in
[`config.hpp`](include/volumetric_kit/core/config/config.hpp) describe the
resolved settings consumed by applications. The JSON fields below generally
match their member names; mesh `up_axis` is resolved to `up_vector` during
decoding. Root sections are
`source`, `fusion`, `codec`, `window`, `render`, `output`, `run`,
`calibration`, and `diagnostics`. There is no generic merge/override or
unknown extension object.

- Dataset fields: `path`, `camera_parameters`, `preload`.
- Live Orbbec fields: `serial`, `sync_config`, `calibration`, `apply_sync`,
  `color_codec` (`hevc`/`mjpeg`), colour/depth widths and heights, `fps`.
  Only `FuseOrbbec` accepts `host_clock`. Serial and sync-config selection are
  exclusive; applying sync requires a sync config; host-clock mode forbids one.
  Width/height overrides are pairs. Scanner accepts only `kind`/`driver`.
- Mesh fields: `path`; exactly one of `height_m` (a height the consumer
  scales the input to) and `metres_per_unit` (the input's declared unit, core
  0.7.0), finite and positive; either `up_axis` (`x`, `y`, `z`, optionally
  signed) or `up_vector` (three finite numbers with finite positive squared
  length); `sdf_mode` (`signed`/`shell`); `shell_voxels`; and `allow_holes`
  (core 0.7.0, default false), which lets signed mode admit a mesh whose only
  defect is boundary edges and is refused with `shell`, even when false. A
  frame-number pattern in `path` is the consumer's to read; core escapes each
  `%` in the scenario's own directory as `%%` (core 0.8.0), so only the path
  the user wrote carries the pattern. Core checks only that `shell_voxels` is
  finite and positive; its bound against the voxel size and truncation is the
  consumer's mesh conversion's.
- Fusion shared fields: `voxel_size_m`, `truncation_m`, `min_depth_m`,
  `max_depth_m`, `max_weight`. Every dataset and Orbbec profile accepts the
  boolean `dynamic` (core 0.6.0): Dynamic integration when true, Classic when
  false; only RigViewer defaults to true. Other application-specific
  extraction/dynamic fields are accepted only by their actual consumers.
  CodecMesh accepts voxel size and, from core 0.7.0, truncation.
- Codec fields: `coefficient_count`, `quantization_scale`,
  `quantization_table` (`uniform`/`band`/`radial`), `segment_size`,
  `intra_period` (core 0.8.0), `sweep`; `encode_every` belongs to
  CodecReplica and `inspect_only` to CodecMesh. The encoder writes an I-frame
  every `intra_period` frames and zero-motion P-frames between them; the
  default, 1, is all-intra. Core checks a `coefficient_count` in [1, 512], a
  positive normal scale and a positive `intra_period`; the codec maps the
  table to its weights and checks each kept coefficient's step at runtime.
- Window fields: positive integer `width`/`height`, `validation`, and
  `overlay` only where UI is supported. Model dimensions are at most 16384.
  Scanner accepts no window section. No window/device handles are serialized.
- Render fields: `yaw_degrees`, `pitch_degrees`, `follow` for FuseRender;
  `shading` (`unlit`/`lit`, plus `normals` for RigViewer), `texture` for recon
  viewers; `show_sources`/`texture_stats` for RigViewer. Scanner accepts only
  `texture`. Gfx examples accept no render section.
- Output fields: `path` for fusion/export programs; `prefix`, `csv` (per-frame
  rows) and `stream` (coded frames, in one file) for codecs, the last two in
  core 0.6.0; `screenshot` for GfxModel. An empty string disables each
  optional output; fusion/export operations require a nonempty `path`. `csv`
  and `stream` must differ once resolved.
- Run fields: `frames`, plus `sets` for RigViewer and `first` (core 0.7.0)
  for CodecMesh. Dataset/FuseOrbbec limits are positive; RigViewer uses zero
  for unlimited; gfx uses -1 for unlimited and permits zero frames. CodecMesh's
  `frames` is instead an exact count, at least 1, of numbered meshes from
  number `first`, which is nonnegative; the last number, `first + frames - 1`,
  must not exceed INT32_MAX. Scanner has no run limits.
- Calibration projection fields: `image_width`, `image_height`,
  `intrinsics` (fx,fy,cx,cy), `distortion` (k1,k2,p1,p2,k3), `point` (x,y,z).
  Focal lengths/dimensions and the point's z must be positive, all values
  finite. No unimplemented calibration solver/board schema is exposed.
- Scanner fusion fields: `voxel_size_m`, `truncation_m`, `buckets`,
  `max_buckets`, `fuse_every`, `remesh_every`, `occlusion_m`;
  `diagnostics.measure_stages` controls timings.
- `FuseOrbbec` accepts the boolean `diagnostics.readback` (core 0.4.0),
  default false. When true, its stream or fuse run reads each set's luma back
  to the host as a separate stage. Other profiles reject this field.
- `FuseOrbbec` accepts the integer `diagnostics.dump_sets` (core 0.5.0),
  default 0 (off), and `diagnostics.dump_directory`. A positive count makes
  its stream or fuse run write the first that many sets' camera JPEGs, and
  with `readback` their luma, into the directory. The directory is a
  `diagnostics` field so that a stream run, which refuses `output`, can use
  it. A positive count requires the directory and `source.color_codec`
  `mjpeg`, since an H.265 frame is not self-contained, and must not exceed
  `run.frames`; a directory requires a positive count. Other profiles reject
  both fields.

## Validation and compatibility

Unknown keys, duplicate keys, wrong types, unsupported versions/choices,
nonfinite/overflowing values, embedded NUL paths, unsupported fields and
contradictory settings fail before resource setup. A known section is not
silently ignored by an application that does not consume it. In particular:

- Stream mode rejects an explicit `fusion` or `output` section, even empty or
  equal to defaults. This preserves recon's stream-only option exclusions.
- Headless model screenshots reject explicit frame limits or overlay settings.
- Mesh inspection rejects fusion, SDF (`sdf_mode`, `shell_voxels`,
  `allow_holes`), encoder and `csv`/`stream` output settings it would not
  use. It accepts `run` and either scale.
- JSON integers remain integers; floating-point syntax cannot silently become
  an integer setting. Directed rounding cannot hide numeric overflow.
- Configuration input is limited to one MiB and 32 nesting levels. `load`
  accepts regular files only and bounds reads even if a file grows.

Recon's existing truncation sentinel is preserved: absent or nonpositive
truncation resolves to four times the chosen voxel size. Scanner's explicit
truncation must be positive and its default stays 0.03 m. Depth defaults are
0.1–8 m for datasets and 0.25–5 m for Orbbec. Core checks the block-of-eight
size, truncation ratio and bucket-index bounds before library construction.

`parse` and `load` return resolved settings for consumption. Field-presence
tracking, defaults and validation remain private to decoding; there is no
separate mutable configuration builder.

nlohmann/json handles JSON syntax and number parsing. Decoding temporarily
uses nearest rounding so overflow is rejected even when the caller uses a
directed rounding mode, and restores that mode on success or failure. Checked
numeric conversions and the family's domain rules are applied before returning.
All fallible public calls contain exceptions and return `Status`/`Result`;
loading never starts devices or writes files. A valid schema does not prove a
requested device mode, file or optional backend exists; those remain runtime
checks in the owning application.
