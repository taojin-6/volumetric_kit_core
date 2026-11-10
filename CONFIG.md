# Scenario configuration

The optional CPU-only `volumetric_kit::core_config` target owns the family's
JSON format, typed settings, defaults, and static validation. Applications
load one scenario file and adapt its validated values to their library APIs.
They check resource existence, hardware capabilities and algorithm results at
runtime. JSON and GPU types do not appear in the public config API.

```cmake
set(VKC_WITH_CONFIG ON)
# Declare/fetch the pinned core before its siblings.
vkc_require_core(0.51.0 CONFIG)
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
required for dataset, live, mesh and stream consumers. Whenever `source` is
present, its `kind` is required. Different files describe different
scenarios:

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
| `FuseOrbbec` | `fuse` or `stream` | `live`, `orbbec` | colour 3840x2160 HEVC, depth 640x576, 25 fps, 300 frames; dynamic fusion, fusion output `fuse_orbbec.ply`; nothing coded |
| `FuseRealsense` | `fuse` | `live`, `realsense`; with `depth` `ffs`, its model and plan cache required | the camera's depth with the projector on; colour 1280x800, depth 1280x720 (FFS: 960x544, projector off), 30 fps, 300 frames; dynamic fusion, fusion output `fuse_realsense.ply`; nothing coded |
| `FuseDecklink` | `fuse` | `live`, `decklink`; two cameras, one pair, calibration, FFS model and plan cache required | colour 3840x2160, FFS depth 960x544, 30 fps, pair skew bound half a frame, 300 frames; dynamic fusion, fusion output `fuse_decklink.ply`; nothing coded |
| `FuseRender` | `fuse` | `dataset`, `replica`; path required | 400 frames, 1280x720, yaw 45 degrees, pitch 30 degrees, output `fuse_render.png` |
| `FuseViewer` | `fuse` | `dataset`, `replica`; path required | 400 frames, 1280x720, overlay, lit shading, remesh every frame |
| `RigViewer` | `view` | `live`, `orbbec`; sync config required | colour 3840x2160 HEVC, depth 640x576, 25 fps; voxel 0.01 m, dynamic fusion, 1280x720 window, overlay, unlit shading, unlimited frames/sets |
| `GfxTriangle` | `view` | omitted | 800x600, validation, unlimited frames |
| `GfxImGui`, `GfxImage` | `view` | omitted | 1024x720, validation, unlimited frames |
| `GfxModel` | `view` | omitted for built-in cube; `model` with optional path | 1024x720, validation, unlimited frames; screenshot selects headless rendering |
| `Scanner` | `fuse` | `live`, `arkit` | voxel 0.01 m, truncation 0.03 m, 1024 initial / 32768 maximum buckets, texture and timings enabled |
| `CalibProjectPoint` | `project` | omitted | 1280x720, intrinsics [1000,1000,640,360], Brown distortion [-0.28,0.10,0.001,-0.0005,0], point [0.1,-0.2,2] |
| `CalibRig` | `calibrate`, `calibrate_intrinsics` or `scan` | `live`, `orbbec` (sync config required) or `decklink` (two cameras, one pair required); `output.path` required | `orbbec`: MJPEG colour 3840x2160, depth 640x576, 25 fps; `decklink`: FuseDecklink's modes; 1280x720 window |
| `Player` | `view` | `stream`; one of `path` and `host` required | 1280x720, unlimited frames |
| `CalibColor` | `calibrate` | `live`, `orbbec`; sync config, calibration, `color_exposure`, `color_gain` and `output.path` required | colour 3840x2160 HEVC, depth 640x576, 25 fps, 50 sets; no window |

The `Config` structs in
[`config.hpp`](include/volumetric_kit/core/config/config.hpp) describe the
resolved settings consumed by applications. The JSON fields below generally
match their member names; mesh `up_axis` is resolved to `up_vector`, and
`source.mobile` and `mobile_registration` to `Source::mobile`, during
decoding. Root sections are
`source`, `fusion`, `codec`, `window`, `render`, `output`, `run`,
`calibration`, and `diagnostics`. There is no generic merge/override or
unknown extension object.

- Dataset fields: `path`, `camera_parameters`, `preload`.
- Live Orbbec fields: `serial`, `sync_config`, `calibration`, `apply_sync`,
  `color_codec` (`hevc`/`mjpeg`), colour/depth widths and heights, `fps`.
  Only `FuseOrbbec` accepts `host_clock`, and only `CalibColor` accepts
  `color_exposure` and `color_gain`. Serial and sync-config selection are
  exclusive; applying sync requires a sync config; host-clock mode forbids one.
  Width/height overrides are pairs. Scanner accepts only `kind`/`driver`.
- Live RealSense fields (core 0.31.0, `FuseRealsense`): `driver`
  `realsense`, `serial` (empty: the only camera attached), the colour/depth
  widths and heights, as pairs, and `fps`. One camera's depth with its
  colour: none of the Orbbec rig's fields, or diagnostics. From core 0.37.0
  it codes and serves its fused sets as FuseOrbbec does (below).
- RealSense depth (core 0.33.0, `FuseRealsense`): `depth` is `camera` (the
  default, the camera's own) or `ffs` (Fast-FoundationStereo on its infrared
  pair, which the consumer runs on CUDA and TensorRT and refuses in a build
  without them).
  `emitter`, the infrared projector, resolves on for `camera` and off for
  `ffs` unless given. `ffs` requires `ffs_model`, the FFS ONNX model, and
  `ffs_plan_cache`, the directory its built TensorRT plans are kept in, both
  config-relative; `camera` refuses both. The depth size is the depth fused:
  the camera's depth mode, or the rectified size FFS runs at, which defaults
  to 960x544 and is the model's own size (the consumer checks the model). The
  infrared cameras run at their own mode; `fusion.min_depth_m` and
  `max_depth_m` gate either depth.
- Live DeckLink fields (core 0.39.0, `FuseDecklink`): one stereo pair of SDI
  cameras on a capture card, its depth from FFS. `driver` `decklink`;
  `cameras`, exactly two objects of an `id`, the camera's key in the
  calibration file, and an `input`, the card connector its SDI output feeds,
  `SDI 1` to `SDI 4` as on the bracket; ids and inputs distinct. `pairs`,
  exactly one object, whose `left` and `right` name the two cameras.
  `calibration` (the rig file: lenses and poses), `ffs_model` and
  `ffs_plan_cache`, config-relative, all required, as FFS's are for
  FuseRealsense. The colour size and `fps` are the cameras' mode, 3840x2160
  at 30 fps by default; `fps` is 24, 25 or 30, the size any pair. The depth
  size is FFS's, 960x544 by default.
  `max_pair_skew_us`, the most a pair's two pictures may lie apart, is from 1
  to half a frame at `fps`, rounded down, and defaults to that bound (16666
  at 30 fps), which pairs free-running cameras. It refuses the Orbbec rig's
  and the RealSense camera's keys and diagnostics. The consumer checks the
  card, its connectors and mode, and that the file names both cameras.
  CalibRig's DeckLink pair (below, core 0.40.0) takes `cameras`, `pairs` and
  the modes under the same rules and defaults.
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
- Stream fields (core 0.17.0): exactly one of `path`, a stream file such as
  `output.stream` writes, and `host`, a live sender's name or IPv4 address.
  `port` goes with `host` only, an integer from 0 to 65535; 0, the default,
  is the sender's default (grid's `networking::kDefaultPort`). A `host`
  containing `:` is refused, as the port is its own field.
- Fusion shared fields: `voxel_size_m`, `truncation_m`, `min_depth_m`,
  `max_depth_m`, `max_weight`. Every dataset profile, FuseOrbbec,
  FuseRealsense, FuseDecklink and RigViewer accept the boolean `dynamic`
  (core 0.6.0): Dynamic integration when true, Classic when false;
  FuseOrbbec (core 0.15.0), FuseRealsense, FuseDecklink and RigViewer default
  to true, the others to false. Other application-specific extraction/dynamic fields
  are accepted only by their actual consumers.
  CodecMesh accepts voxel size and, from core 0.7.0, truncation.
- Codec fields: `coefficient_count`, `quantization_scale`,
  `quantization_table` (`uniform`/`band`/`radial`), `segment_size`,
  `intra_period` (core 0.8.0), `update_size` (core 0.9.0), `sweep`;
  `encode_every` belongs to CodecReplica and `inspect_only` to CodecMesh. The
  encoder writes an I-frame every `intra_period` frames and zero-motion
  P-frames between them; the default, 1, is all-intra. `update_size` is the
  P-frame update-segment size in entries; 0, the default, selects the codec's
  own size, and a period of 1 refuses an explicit size, even 0. Core checks a
  `coefficient_count` in [1, 512], a positive normal scale, a positive
  `intra_period` and an unsigned 32-bit `update_size`; the codec maps the
  table to its weights and checks each kept coefficient's step and the update
  size's range at runtime.
- Window fields: positive integer `width`/`height`, `validation`, and
  `overlay` only where UI is supported. Model dimensions are at most 16384.
  Player refuses `overlay`, since its controls and stats panel are always
  drawn. Scanner accepts no window section. No window/device handles are
  serialized.
- Render fields: `yaw_degrees`, `pitch_degrees`, `follow` for FuseRender;
  `shading` (`unlit`/`lit`, plus `normals` for RigViewer), `texture` for recon
  viewers; `show_sources`/`texture_stats` for RigViewer. Scanner accepts only
  `texture`. Gfx examples accept no render section.
- Output fields: `path` for fusion/export programs; `prefix`, `csv` (per-frame
  rows) and `stream` (coded frames, in one file) for codecs, the last two in
  core 0.6.0; `screenshot` for GfxModel; `serve` and `max_viewers` (core
  0.30.0) for FuseOrbbec, FuseRealsense (core 0.37.0) and FuseDecklink. An
  empty string disables each optional output; fusion/export operations
  require a nonempty `path`. `path`, `csv` and `stream` must differ once resolved, and none may
  be `source.sync_config` or `source.calibration` (core 0.13.0),
  `source.ffs_model` (core 0.33.0), or the phone's session file or
  registration (core 0.42.0).
- Run fields: `frames`, plus `sets` for RigViewer and `first` (core 0.7.0)
  for CodecMesh. Dataset/FuseOrbbec/FuseRealsense/FuseDecklink/CalibColor
  limits are positive; FuseOrbbec's counts camera frames, each set adding one
  per camera it holds, FuseRealsense's its one camera's, FuseDecklink's its
  pair's, and CalibColor's counts sets; RigViewer uses zero for unlimited; gfx uses
  -1 for unlimited and permits zero frames; Player plays until its window
  closes unless `frames` gives a positive count of presents of a stream
  frame. CodecMesh's `frames` is instead an exact count, at least 1, of
  numbered meshes from number `first`, which is nonnegative; the last number,
  `first + frames - 1`, must not exceed INT32_MAX. Scanner has no run limits.
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
- `FuseOrbbec`, `FuseRealsense` (core 0.37.0) and `FuseDecklink` code their
  fused sets, one frame per set, when `output.stream` or `output.csv` names a
  file (core 0.13.0) or `output.serve` a port (core 0.16.0); each still
  writes the PLY at `output.path`. A nonzero `serve` serves the coded frames
  live on that port, on every interface (grid's `networking::StreamServer`);
  0, the default, serves nothing.
  `max_viewers` (core 0.30.0), a positive integer, default 4, is how many
  viewers it serves at once, first come; a connection past them is closed.
  It is refused unless `serve` is nonzero. No upper bound is checked: each
  viewer costs the sender a thread and a socket, and the server takes no
  authentication. Both take the codec fields the codec profiles share, with
  their defaults and checks. A `codec` section without any of the three
  outputs is refused, since nothing would be coded.
- `FuseOrbbec`, `FuseRealsense` and `FuseDecklink` accept `source.audio`
  (core 0.34.0), the audio input recorded beside the cameras, on the host's
  clock, as a track of the coded stream. Omitted, the default, records
  none; `default` is the system's default input; any other value is an input device's name as the
  OS lists it, which the consumer looks up. It requires `output.stream` or
  `output.serve`, since audio is written only to the stream, and FuseOrbbec's
  cameras on the host's clock: a rig (`sync_config`) or `host_clock`. A
  RealSense camera and DeckLink cameras always are, so FuseRealsense and
  FuseDecklink ask only for the stream (FuseRealsense from core 0.37.0, when
  it took the two outputs; 0.34.0 refused its audio).
  Other profiles reject it.
- `FuseOrbbec` accepts `source.mobile` (core 0.42.0), phones' camera
  sessions whose frames fill what the rig's cameras do not see, and
  `source.mobile_registration`, where a session's tracked world sits in
  the rig's. Omitted, the default, there is no phone; the value is never
  empty. A value without `://` is a recorded session's file, config-relative;
  one that reads as an address, starting `tcp:` or only digits, dots and a
  colon, is refused. A live source is `tcp://ADDRESS:PORT`, which the
  consumer listens on for phones to dial in to: the address is numeric
  IPv4, four decimal parts from 0 to 255, and the port is from 1 to 65535,
  each without a leading zero and with nothing after the port. The net
  tier checks both (`net::parse_host_port`, then `net::parse_endpoint`,
  from core 0.44.0), and a refusal gives its reason. Core does
  not check that the address is the host's own; the listen does. The wildcard
  `0.0.0.0`, a name, IPv6 and any other scheme are refused, and so is
  `output.serve`'s port, which binds every interface of the same host.
  `source.mobile` requires `source.calibration`, the world a phone is
  placed in, and operation `stream` refuses it, since the phones fill the
  fused model. `mobile_registration`, config-relative and never empty,
  requires `source.mobile` and names a registration file: a recorded
  session's, or, live, that of the one session whose id it names. Omitted
  for a recorded session, the consumer registers it itself, or refuses it.
  Live, the consumer itself registers each session no registration names,
  or turns that session away, never the run. `diagnostics.track_log`,
  config-relative, is the directory the phone's tracking log is written
  into; live, each session's goes in a directory of its own beneath it,
  beside files of the consumer's own. It requires `source.mobile`, and
  changes nothing that fuses. Other profiles reject all three.

  The address is the consumer's, not one phone's: phones dial in to it,
  each session separate, and sessions join and leave at any time during
  the run, up to a number the consumer sets, so one that comes after
  another has left, an app relaunched or another device, is taken. The rig
  never depends on a live phone (DECISIONS.md, the 2026-11-02 phone
  entry's 2026-11-07 amendment). The consumer runs the rig's capture from
  the start, with no phone, and a phone joining, leaving, failing or never
  dialling neither stops nor delays it. Nor does an address the listen
  cannot bind: the consumer warns and retries, and its report says it is
  not listening. Nor does a phone the consumer cannot take, under the
  run's settings or at all: it turns each phone away and says why. A
  scenario the checks above refuse still fails at load.
- `FuseOrbbec` (core 0.14.0), `FuseRealsense`, `FuseDecklink` and
  `RigViewer` accept `fusion.free_after`, a nonnegative integer, default 30.
  Every that many fused sets, the consumer runs recon's
  `VoxelBlockGrid::free_stale_blocks`, which frees each grid block that none
  of those sets asked to allocate and in which neither this pass nor the one
  before found an observed voxel; 0 never frees. Dynamic
  integration clears the free space a camera sees through, so the blocks a
  surface has left go. Classic integration never clears a voxel, so a run
  fused Classic throughout frees only blocks that never held an observed
  voxel, which the mesher and the codec use nothing from.
  Other profiles reject it.
- `CalibRig` (core 0.11.0) is calib_rig's live view and capture. Its
  `operation` is `calibrate`, which solves the cameras' poses; from core
  0.40.0, `calibrate_intrinsics`, which solves each camera's lens; or, from
  core 0.51.0, `scan`, which solves nothing and saves every camera's picture
  at a key press, e.g. colour stills from a DeckLink pair for
  photogrammetry or NeRF. No other profile takes the latter two. A scan
  takes the calibrations' keys, rules and defaults, below, and none of its
  own (DECISIONS.md, the 2026-11-01 CalibRig entry's 2026-11-09
  amendment). `driver` is `orbbec`, the default, or `decklink` (core
  0.40.0):
  - `orbbec`: its required `source.sync_config` names the rig, of any number
    of cameras; `color_codec`, the colour and depth sizes and `fps` are the
    modes its preset check holds every camera to. Colour must be `mjpeg`,
    since a capture saves each camera's JPEG.
  - `decklink`: one DeckLink pair, with FuseDecklink's `cameras`, `pairs` and
    modes (above). Its depth size is FFS's, which the pair's rectified row
    error is measured at. It refuses `sync_config` and `color_codec`, FFS's
    model and plans, and `max_pair_skew_us`: the driver pairs the pictures
    at its own bound, and a calibration's capture waits for a still board.

  `calibration` (core 0.40.0), optional with either driver and in every
  operation, names the rig file a session starts from. The required
  `output.path` is the directory each session's own directory is created
  in; it has no default, so no session is written beside the scenario
  unasked. It accepts `window` `width`, `height` and `validation`, and
  refuses `overlay`, since its controls and marks are the overlay. It
  refuses every other field and section, `fusion` included: its
  capture-rule and detection thresholds are calib's own, and the board is
  edited in the app.
- `CalibColor` (core 0.26.0) is calib_color's headless fit of each camera's
  colour response. It opens the rig of its required `source.sync_config`,
  posed by its required `source.calibration`, at the Orbbec profiles' modes,
  and fits `run.frames` sets (50 by default, 2 s at 25 fps). Colour must be
  `hevc`, since the fit must see the colour reconstruction decodes
  (DECISIONS.md, the 2026-10-27 entry). Its required `color_exposure` and
  `color_gain` are the manual exposure, in the camera's units, and gain every
  colour camera is set to, with auto exposure off and auto white balance on.
  They have no default, since they depend on the rig's lighting; core checks
  that they are positive integers, and the consumer checks each camera's
  range when it opens. Its required `output.path` is the directory each run's
  own results directory is created in. It refuses every other field and
  section, `window` included: it opens no window.

## Validation and compatibility

Unknown keys, duplicate keys, wrong types, unsupported versions/choices,
nonfinite/overflowing values, embedded NUL paths, unsupported fields and
contradictory settings fail before resource setup. A known section is not
silently ignored by an application that does not consume it. In particular:

- Stream mode rejects an explicit `fusion`, `codec` or `output` section, even
  empty or equal to defaults, and `source.audio` (core 0.34.0). This
  preserves recon's stream-only option exclusions.
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
0.1–8 m for datasets, 0.25–5 m for FuseOrbbec, FuseRealsense and
RigViewer, and 0.5–5 m for FuseDecklink, whose FFS consumer also gates at
the pair's own near limit. Core checks the block-of-eight size, truncation
ratio and bucket-index bounds before library construction.

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
