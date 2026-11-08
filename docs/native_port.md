# Native renderer for Trials Fusion

The aim, as in Skate1Native: no Xenos emulation. Skate takes the game's D3D calls; Fusion has to use its command stream (see below). The first part of this
file is the current state; the rest is what was measured on `default.xex` 0.0.0.3 before writing any.

## Current state

Works in part, Turnip T30 only, off by default (`native_gpu_render = true`). Qualcomm's driver fails every pipeline with
`VK_ERROR_UNKNOWN` although all the SPIR-V validates.

* Title screen, menus and interface: right, title at 122 FPS on the Odin 2 Portal.
* First track: reached, 45 FPS and 22 ms a frame on the renderer thread on RADV (Renoir). The final picture is about as bright as
  the emulated one and the floor and signboards have their pages (the page table and the pages the game asks for now match the emulated run).
* Virtual texture table: **works**. Seed = point list PS 0D26ADDF22CBC739 (not the memory export shaders), then 6398EA and
  95BB84 build each level from the one above. The table lives on the host (`CopyResolveToTable`) as RGBA16F: its fetch is
  integer, the shaders scale by 1/255, so it holds raw values. Blocks resolved at its base address must not replace it.
* `getCompTexLOD` was dropped by XenosRecomp, so setTexLOD got a stale register; fixed in tools/native/xenosrecomp.patch.

* Memory export: **runs natively**. XenosRecomp emits `eA`/`eM0..4` and a `memExport` call at each `alloc export` and at the
  end of the shader (SDK translator's format packing and byte order, `shader_common.h`); the stores go through buffer
  device addresses into one 128 MB buffer of the renderer's, which holds the guest ranges the draws' streams name
  (`mad eA, r#, c0100, c#`: the CPU reads the stream constant at each draw, as the SDK's `AddMemExportRanges` does, and the
  renderer keeps those pages, filled from guest memory the first time). Nothing is written back to guest memory: the
  plugin's `readback_memexport` is off by default and the emulated game works without it. A texture fetched from such a
  range is laid out from the buffer by a compute shader (`shaders/detile.hlsl`) and copied into its image, in place, when an
  export has written the range since. So the frame is not rendered and read back for these draws any more. The plugin way is
  `native_gpu_memexport_plugin = true` (needs `readback_memexport = true` for the textures to see the result). The atlases
  hold 8-byte `k_16_16_16_16` unsigned elements, two for each DXT5 block, one block in each 16 KB tile of the atlas for each
  draw (BCFF66D9 copies a page, 758C4BE0 compresses one). `k_16` integer fetches now get the 65535 scale: the page index
  map they read (32x32 at 1E6DD000) was all zero before, so every block went to the first of its tile. The stale fetch constant
  that looked like a missing 146x146 `k_16` input at slot 4 is not read by any of these shaders.

Established since (2026-11-07, Linux/RADV, Greenhorn's Grove):

* The page request feedback is the 256x128 colour resolve (alpha = mip, page in the other bytes) with its depth, one pass in
  four, read back like the others; its readback worked. What was empty is the pass: its depth was never cleared. Fusion
  clears with rect and point lists whose vertices are window coordinates (`PA_CL_VTE_CNTL` = 0x300, only VS 1E6883FC and
  B6C9863F / DDED7E53 use it) and the depth test is GEQUAL, so the pass only drew while the depth texture was fresh and
  later passes failed every fragment (colour all zero, no new requests). The renderer now sends those vertices through the
  inverse of the viewport transform (`g_ScreenMul` / `g_ScreenAdd`, shared constants). The feedback covers what is visible
  now, about a third of the pixels on the intro camera.
* The table readback to guest memory (what the CPU reads to know which pages sit where) had red and blue swapped in its
  1_5_5_5 words, and truncated 11 to 10; now the same words as the emulated run's for the common entries (42,000 of 1,048,576 differ, as pages move).
* `getCompTexLOD` wrote its result to x whatever the destination swizzle; the feedback shaders read it in y.
* Page draws (BCFF66D9 / 758C4BE0; the renderer counts "memory export draws"), over a whole run on Greenhorn's Grove: emulated 52 at
  the start, then about 2,500 in the 2,000 frames after the track begins, and none after; native 1,410 at the end of a run,
  all in the first 1,100 frames of the track, none after (it was 64 in all before; the stat also counts the one 4 MB export
  draw each frame at 14E9A000, which is not a page draw, so the 4,000 it reached were no pages). The feedback decides, and its coverage was the rectangle of
  the top left 120x64 texels (30 %) where the emulated one has requests in 99 %; native is now 100 % (`fb.py`). Cause: the
  clears of the pass are rect lists in window coordinates (PA_CL_VTE_CNTL 0x300), two to a pass, the second on the 4x
  surface (RB_SURFACE_INFO msaa 2) that covers what the first leaves: (240..256, all rows) and (0..120, 0..64) in 4x samples,
  that is (0..240, 0..128) in the 1x samples the renderer draws at. The renderer doubled that draw's viewport and pitch but
  not the vertices, so only a quarter was cleared, the depth (GEQUAL) held what other passes left and the terrain (VS
  23F30886: positions are 16_16_16_16 integers times c20..c23, nothing wrong there) lost the depth test outside the corner.
  Disabling the terrain's depth test gave 100 % too. `DrawCall::surface_4x` doubles `g_ScreenMul` for window coordinates.
  The same bug cleared a quarter of the shadow caster pass's (4x, 16_16) tiles: the atlas bands now hold casters (about half
  of band 0 differs from the clear word, as in the emulated dump).
* Both modes reach Greenhorn's Grove from a script that waits for screens (scratchpad nav.py: coarse normalised
  front-buffer signatures, Space only on a known menu screen). `native_gpu_observe` now takes the same trace frame as
  the native run (`native_gpu_trace_frame = -1`: the 240th frame of 800+ draws) and writes the front buffer from guest
  memory on `<dump>.trigger`. Guest allocations (so addresses) differ between runs and modes, compare by role.
* The shadow atlas, as the game builds it (512 wide, 4 bands of 512 rows; band 0 every frame, 1 every second, 2 and 3 every
  fourth). For each band: a 16_16 pass (the casters, tiles at 0x340, pitch 1040), its resolve to the k_16_16 atlas (4 bytes a
  texel, 512x2048, `15C5F000` here) with a colour clear riding on it (RB_COLOR_CLEAR = FFFFFFFF), then a resolve of the same
  tiles read as 8_8_8_8 to the 1_5_5_5 atlas (2 bytes, `15A5F000`, swap red and blue) with its own clear (7FFF7FFF or 0),
  and for bands 2 and 3 first 8_8_8_8 stamps (B8CF2FCB) drawn over the tiles. So the 1_5_5_5 atlas holds the clear word
  (white) wherever nothing was stamped, which is the emulated dump (white throughout, data in rows 1024 and up). What the renderer
  got wrong: (1) a resolve in another format than the one that wrote the tiles read a surface nothing had written (zero);
  a colour resolve now first clears the tiles owned by another surface to the word the last clear left (`TrackEdram`
  with `read_only`); (2) the clear word was decoded as ARGB bytes for any format, it is the raw sample (red in the low
  byte, the 16_16 formats 1/1024 steps of an int16): `ClearWordColor`; (3) a band resolved at the right offset of another
  resolve's texture was merged into it whatever the texel size, so the k_16_16 atlas's band 1 (+1 MB) fell into the
  1_5_5_5 atlas 2 MB before it: `Texture::texel_bytes` must match; (4) a rectangle clear in the resolve's own pass left
  the rows below the first 256 black on RADV (as read by the resolve shader; a copy of the surface showed them cleared,
  a clear without rectangles works), so one word covering everything clears the whole surface. Not understood.
* Depth is shared in EDRAM too: a depth clear riding on a resolve clears the tiles of every pass that follows, whatever its
  host depth surface. The renderer keeps the raw depth word per tile (`edram_depth_clears_`) and a draw that takes the
  tiles over clears its depth surface to it (the feedback pass needs that besides the 4x fix above).
* Fixed: the window scissor (PA_SC_WINDOW_SCISSOR) was ignored; the lighting pass (3743B184) draws a full-screen quad in 16
  strips of 38 rows, each with its own lights and no blending, so each draw overwrote the screen.
* The lighting pass (3743B184) was dark for four reasons, all found by sending intermediate registers to its output
  (a dev-only replacement of text in the generated HLSL; no longer in the tree):
  1. Texture heap slots. A resolve to an address that already holds a texture of another size or format (the g-buffer's
     addresses are reused by the half-size passes later in the frame) made a new texture in the old slot, and the descriptor
     update applies to every draw of the frame: the lighting pass read albedo, normals and depth as the later resolve
     (zero). A replaced resolve texture now gets new slots, the old ones free once the frame is done.
  2. Stencil faces. The host's front faces are clockwise; the console's are counter-clockwise when
     PA_SU_SC_MODE_CNTL bit 2 is clear, and the full-screen quad that adds one to the stencil (F0D3EFC1 / E9E94D4C) has that
     bit clear, so its ops fell on the back face's (keep) and the stencil test of the lighting pass (not equal to 0) failed
     everywhere. The front and back ops swap in that case.
  3. Fetch constant sign fields: 2 is unsigned biased (`2 * v - 1`) per component; the normal buffer (2_10_10_10, x and y) was
     read as 0 to 1. `texBias` in the shader, four bits a slot in the shared constants.
  4. The `cube` instruction was translated as `cube(r)` of its first register, unswizzled. The hardware takes the direction
     v as v.zzxy and v.yxzz in its two operands; the translation now takes the second operand's `.yxz`.
  The picture is now lit (albedo, normals, ambient, lights), HDR mean 0.08 to 0.11 with max 2.5 where emulated is 0.13.
* The sun's shadow mask (40768DE1) and the g-buffer normals, fixed (2026-11-07, same moment in both runs, `nav.py` with
  `NAV_TRACES=60`; at 30 s the two modes are at different points of the intro camera):
  1. The mask resolves to a `k_8_8` texture (copy info 0x501), taking the shader's x and y. Resolves of 8888 targets come out
     blue first (mode 3, `value.bgra`, undone by the fetch's swizzle); that also went for the 8 and 8_8 destinations, so the
     lighting pass read the shader's z (always 1) as the shadow term. The atlas, its moments (SNORM, exp adjust -15, the
     0.5 * m + 0.5 in the shader), the light space coordinates from the depth and the constants were right all along; the
     k_16_16 band contents match the emulated dump in shape. Resolves to `k_8` and `k_8_8` now keep the component order.
  2. The g-buffer is two targets: RT0 8888 (albedo, resolved to 15537000) and RT1 2_10_10_10 (normals, resolved with
     RB_COPY_CONTROL source 1 to 152D7000). Draws only claim the first target's EDRAM tiles, so the resolve of RT1 found
     its tiles owned by another surface and cleared them to the clear word: the normal buffer read all zero (biased: (-1,-1,-1)),
     the lighting pass had no sun and ambient/point lights without shape. `ResolveCall::second_target` skips that check.
     (Claiming RT1's tiles in the draw path instead breaks the loading screen: its top half goes black.) The HDR mean went
     from 0.04 to 0.13.
* Exposure, fixed: the ramp 0, 1, 2 ... 15 was the shader, not data. PS 577E0310 is an if / else / endif chain on the predicate
  stack (counter in r15.w), and the translation of `setp_inv` (the else) left the counter at 1 where the hardware sets 0
  (`p0 = src == 1`, result 0 when it is 1, 1 when it is 0, else unchanged). So the nested `setp_gt_push` that follows could never
  run, the adaptation (blend of the new luminance into the history at pixel c0.w, accepted when it falls outside what the
  history holds) was skipped, and the output was `oC0 = r0.x`, the pixel index: each frame wrote its own index into the history.
  Now the history starts at the CPU's 0.0189 and follows the scene (0.125 after 300 frames), c8.y / c8.x is in the emulated
  range and the picture is as bright. Every shader with an else gains from it (xenosrecomp.patch). The k_16_FLOAT resolve and
  `WaitFrame`'s copy were right.
* Tools (also `tbl.py`: page table levels, emulated against native; `tblmask.py`: one level as a mask side by side; `tat.sh`: a
  native run traced once N s after the track starts): the emulated front buffer was decoded as A, R, G, B (the red tint), then as R, G, B, A (red and blue swapped: hologram
  cyan, rider yellow, floor yellow-green). Memory holds R, G, B, A (resolve copy info 0x100000: no swap, no endian) but the game
  presents it through fetch constant 0 with swizzle z, y, x, so the picture is B, G, R: `DumpGuestFrontBuffer` now does that,
  and hues match native (orange sign, blue rider). The emulated side still has its green tint; native is the paler of the two. `<dump>.trace` (both modes) traces the next frame; in observe mode the traced frame also writes
  its front buffer and what each resolve left in guest memory (`<dump>.<n>.<address>_<w>x<h>_guest<format>.bin`, tiled,
  numbered like the native dumps), so a same-moment pair is one `nav.py` run each with `NAV_TRACES`.

* Page table and page requests, fixed (2026-11-08; per level the table now holds the same number of entries as the emulated one in the
  traced frame at 60 s, 688,128 / 172,032 / 43,008 / 10,752 / 2,688 / 672 / 168 / 42 for levels 0 to 7, on the host and in guest memory):
  1. The cascade's first frame draws 6398EA at level 8 over the 8-level table (c0.x = 8, w 0.5) to start level 7 over. A fetch of a
     level past the last returns zero (the emulated run leaves only the seeds there); the clamp to the last level added earlier
     enlarged level 7 into itself, whose 2x blocks then spread through every level (levels 3 and below ended up completely
     filled, the guest table too). `tfetch2DLod` now returns zero.
  2. `CopyResolveToTable` copied each resolve from (0, 0) of the resolve's texture to the address's block, but the second pair of
     blocks of a level (e.g. 1ED50000 below 1ED40000, pitch 512 rows 448) joins the first one's texture as a band: the first
     block was copied twice and the lower quadrants never. It now takes the address the game wrote and the band's own offset.
  3. The feedback resolve (8_8_8_8, 256x128, copy info 0x302: no swap bit) was read back to guest memory blue first; memory
     holds red first unless the copy's swap bit is set (as for 1_5_5_5), so the CPU saw the page flag where it expects the
     page coordinate and requested nothing (64 page draws in a run). Now 1,400 page draws and the floor and signs are textured.
* Tools: `tbl.py` (levels, three columns: emulated, native host, native guest) and `tblcmp.py` (entry by entry) read `<dump>.table.bin` (guest
  memory, written by both modes for the traced frame) and `<dump>.table.<level>_WxH.raw` (the native host table, written at
  the end of the traced frame).

Next, in order:

1. Frame rate: 45 FPS on the track, 22 ms a frame on the renderer thread (RADV, Renoir; 51 and 19 before the floor was textured),
   aim 60. The EDRAM stale-tile clear ran 155,000 times in one 110 s run ("EDRAM: color target overwritten by another surface").
2. Colours: the pair hues agree now (see the tools note above); the emulated green tint and native's paler picture (no grade difference found yet) are left.

How it fits: `games/shared/native_gpu` holds Skate's renderer and `live_commands.cpp`. SDK patch 0008 hands every draw, resolve
and swap to it with the register file; `SnapshotState` lays the registers out as the device snapshot the renderer reads, so
the renderer stays close to Skate's. Draws whose shader has no native form go on to the plugin.

Shaders: run with `gpu_shader_dump_dir`, then `tools/native/build_shaders.sh <dump>`. `make_containers.py` wraps raw microcode in
the D3DX containers XenosRecomp reads. XenosRecomp needs `tools/native/xenosrecomp.patch`: all 256 pixel constants, loop
counts per draw, r0 as the vertex index, which Fusion's shaders use directly (terrain, full screen triangles), and `setTexLOD` with
texel coordinates.

Surfaces marked 4x are drawn at 1x with viewport and pitch doubled. Resolves of 256 KB or less go back to guest memory a frame
or two later, for the exposure and page request readers.

To see a frame: `native_gpu_trace_frame = -1` logs a track frame and `native_gpu_dump_path` writes each resolve in order;
creating `<native_gpu_dump_path>.trace` does it for the next frame, at a moment of your choosing.
`native_gpu_observe = true` with `native_gpu_present = false` leaves rendering to the plugin and logs the same frame, for comparison.
`TRACE guest` lines show guest memory at each resolve destination. Screens that go wholly black in tests are a crash or the
display timing out: use `svc power stayon true` and look at the top activity.

What is left, in order:

1. Base colours: see the first item above. `DDED7E53` and `B4C1B91D` (small, not memory export) draw while the table fills and
   were not in the dump the shader cache was built from; they are in it now.
2. Rect lists (primitive 8) are skipped.
3. Frame pacing: `native_gpu_frame_limit` holds 60, without a real vertical blank.
4. Early present: races on the fence at high frame rates, off by default.
5. Pixel position in 4x draws is not halved here as it was in the plugin test.
6. Destination offsets of resolves in the shadow atlas, surface reinterpretation between formats.
7. A shader cache for the whole game: 93 shaders from one track so far, unknown ones draw nothing.
8. Qualcomm's driver, or shipping with Turnip.

The earlier verdict below assumed the same shaders. Skate goes from 20 to 60 FPS on the Deck with XenosRecomp's, so that
ceiling of about 40 FPS may be low; only a working track will tell.

## Shaders

Skate's way does not apply. Its shaders are D3DX containers (magic `0x102A110x`), found in its archives and read by
XenosRecomp, which needs their constant table and vertex fetch data.

* No container in `data.pak`, `data_tex.pak` or the decoded executable, and none in 3 GB of the running game's memory.
* The executable never loads that magic as a constant. Skate does in many places. The engine does not use containers.
* The microcode is in physical memory on page boundaries, each right after its literal constants: `c251` of
  `3EBCA95DEB27B0CD` (1.0667, 16, 1, 0.9375) sits just before its code. Files are named `shaders\sd\...`.

So the input to a shader recompiler has to be raw microcode, captured or read from the game's own shader files, with what
a container would have said (constants, interpolators, vertex layout) worked out from the microcode. XenosRecomp would need
that as a new input path, or the SDK's own microcode to SPIR-V translator could feed the native renderer instead.

Of 92 shaders seen from the menus to the first track, 4 use memory export, which the virtual textures depend on; the
native renderer handles it now (see the current state).

## D3D entry points

The library is a newer build than Skate's and split in two. Part of it sits in `826C0000` to `826E0000`. The part that runs
for each draw was compiled in with the engine's code, around `82220000` to `82230000`, probably by link time optimization.
The device is passed in r3.

Traced on the first track at 20 FPS, by function. "Sure" means the arguments fit Skate's function.

| Function | Calls per frame | What | Confidence |
| --- | --- | --- | --- |
| `82228DB0` | 1,394 | DrawIndexedVertices: device, 4 (triangle list), base vertex, start index, index count; holds the draw packets | sure |
| `8222FDF8` | 51 | DrawVertices: device, primitive type, start, count; holds the draw packets | sure |
| `822294B0` | 1,396 | SetIndices | likely |
| `82226160` | 58 | Resolve, best match of Skate's `82CC7FF0`, holds a draw packet | likely |
| `8220DD40` | 4 | ClearF, best match of Skate's `82CC7510` | likely |
| `82221AB0` | 150 | SetVertexShader, best match of Skate's `82CBE2E8` | guess |
| `826C91A0` | not traced | SetPixelShader, best match of Skate's `82CBDFE8` | guess |
| `826DA710` | 1 | SetPredication, best match of Skate's `82CC4E78` | likely |
| `82229228`, `8222DA80`, `822287C8` | 1,382, 940, 937 | Engine's draw submission, callers of the above | seen |
| `826CB7B8` | 1 | Swap, device `40852A00`, front buffer `1F830000` in a callee | sure |
| `826C3270` | 1 | Interrupt callback | sure |
| `826C2780` | 2 million+ | Spin wait of the Render thread (caller `8225B138`) | seen |

So draws go through D3D as in Skate, and hooking them is possible. An earlier version of this file said the opposite: it
had traced only the `826C` part. `BeginTiling` has no match.

Draw packet headers have the predicate bit set here (`C0032201`, `C0012201`, `C0003601`), which an early scan for them
missed. Other functions holding them: `822272B8`, `82208F18`, `826C2E00`, `826DA130`, `826D6698`.

The frame's commands: one run of about 12 segments, each executed once: 4 long ones of about 16,000 words and shorter ones,
in a ring from `1FA90000` to `1FC90000`, written on the Render thread by `82228DB0` and `8222DA80`, with 42 small buffers from
`1FC93000` and `1FC94980` in between. There is no replay and no sign of predicated tiling, so Skate's single tile shortcut
does not apply. An earlier version of this file claimed a replay: it had grouped buffers by address prefix.

The small buffers come in 14 groups of 24 `DRAW_INDX_2`, 336 draws. Known draws add up to about 1,840 a frame against the
2,500 to 3,400 the GPU counted, on other frames and on the Odin. That gap is not explained yet. Hardware watchpoints on the
small buffers, set from the menu through track loading, caught no writes.

## What Skate's renderer needs, and what Fusion's device holds

Skate's native layer takes about ten hooks (draws, clear, resolve, tiling, swap, inline vertices). Everything else it reads
at each draw from the D3D device.

The device was `40852A00` in the runs that dumped it; `40852600` appears in one trace, so it can move between runs. Take it
from r3 at the hook. `82228DB0` flushes state exactly as Skate's draw does: dirty masks at `0` to `40`, registers from the
same offsets, constants from `1920` and `6016`. Dumped with gdb, stopped on a real draw on the first track:

| Data | Skate offset | Fusion | State at a draw |
| --- | --- | --- | --- |
| Register groups | `10368`, `10444`, `10528`, `10548`, `10596` | same | Filled, plausible values |
| Fetch constants | `0x480` | same | Filled |
| Index buffer | `12428` | `12812` (+384) | Filled |
| Float constants | `0x780`, `0x1780` | same place in the code | **All zero** |
| Shaders | `12684`, `12688` | `13068`, `13072` (+384) | **0** |

So registers, textures and index buffers can be read as in Skate. Constants and shaders are not in the device at a draw: the
engine must set them another way. `82225058`, called from the draw and holding an `IM_LOAD_IMM` packet, is the first thing to
read for shaders. `8222C768` (1,689 calls a frame) and `8222C6D8` (1,637) are candidates for constants: the XDK has calls
that give the caller a pointer straight into the command buffer, which would leave nothing in the device.

## Where the time goes on the Odin 2 Portal

Measured on the first track, Qualcomm driver, 23 FPS, with `vulkan_gpu_timestamp_buckets = true`.

The GPU is the limit. The Adreno is 97 to 99% busy at its top clock of 680 MHz, at 43 degrees, and the Render thread spends
85% of its time in `8225B0A0`, D3D's block on fence, waiting for it.

| Bucket | ms a frame | What |
| --- | --- | --- |
| PassEntry | 17 | Entering 86 render passes: tile loads, and the deferred work of the pass before |
| Main | 7 | The game's own draws |
| TexUpload | 6.3 | Nearly all copies into images |
| Resolves and dumps | 5.5 | |
| RtTransfer | 4.9 | Moving EDRAM contents between render targets |
| Other | 6 | |

Each frame sends 167 render passes, 106 render target transfers, 260 dispatches, 325 barriers and 2,300 draws.

PassEntry is mostly pixel shading, deferred by the tile based GPU to the end of the pass before: with every draw scissored to
the top half of the screen it falls from 17 to 7.7 ms and the frame from 44 to 34 ms, with tile loads and stores left as
they were. So about 24 ms of the frame is the game's own drawing, and EDRAM emulation is at most about 10 to 15 ms of the
rest. An earlier version of this file said the emulation was nearly all of the cost; that was wrong.

### The frame

1. 4x pass: depth at tile 0, `k_16_16` at tile 832, more than 1,000 draws, then 1x G-buffer pass, about 330 draws, on `@494` and `@988`.
2. Five rounds of one or two 4x depth only draws that write stencil, then a 1x full screen light that reads it.
3. Post processing, each step on EDRAM still holding another format.

The surfaces are 1040 x 608 at 1x. Those labelled 4x are 520 x 304 over the same samples: the depth moves between the two
views eight times a frame, 45% of all transfers by area.

`SetVertexShader` is called only with 0 and `SetPixelShader` never on a track. The engine sends shaders and constants itself,
constants with `SET_SHADER_CONSTANTS` (`0x56`) through `8222C6D8`. **So the device cannot describe a draw and hooks on D3D
calls will not work for Fusion, unlike Skate.** That also explains the draw count gap above. The native renderer has to take
its state from the command stream, where the SDK already has every register, constant and shader address.

### Tried, none kept

| Change | Result |
| --- | --- |
| Skip all transfers | 38 FPS on a broken, emptier scene, so it overstates the gain |
| The 4x as 1x test below with the screen scissor overflowing, or a draw skipped | 27 to 30 FPS: false, those draws were not rendered |
| Skip transfers into depth | 33 FPS, black scene |
| Skip colour to colour transfers | 24 FPS, wrong smoke: they are cheap |
| Keep clean copies of depth views | No gain: the stencil rounds really write |
| Draw depth only 4x draws into the 1x surface, viewport doubled | 2 ms, lighter walls |
| Reload big textures once a frame | 41 ms; the rider looked wrong, not cleared up |
| `vulkan_deferred_resolve_clears` | Nothing |
| `vsync = false` | Nothing |
| `readback_resolve = none` | 27 FPS, no textures |
| All 4x surfaces drawn as the 1x surface of twice the size, on registers doubled per draw | 23 FPS, right picture: transfers lose 2 ms, the 4x passes now shade four times the pixels |
| Shaders through `spirv-opt -O`, a third smaller | No change: the driver's compiler already does this |

The virtual texture table (2048 x 2048, 8 MB) is reloaded whole 4 to 5 times a frame, about 73 MB, most of TexUpload.

## Where the time goes on Linux

Native renderer on the first track (Greenhorn's Grove at the start line), Radeon Renoir (RADV), Release build, no frame
limit, 1,850 draws and 56 resolves a frame. The renderer thread was the limit, not the GPU: it never waited for the fence
(`GPU wait` 0.0 in the 300 frame line), and the game's render thread waits for it. Measured with `perf` (user space only:
`--user-callchains` is the one call graph this kernel's `perf_event_paranoid=2` lets through) and with `rdtsc` timers in the
renderer, because this machine's clock source is HPET and `steady_clock` costs 1.2 us a call. In the logs,
`native_gpu_stats_seconds` gives the renderer thread's rate, and the 300 frame line the GPU's own time (timestamp queries),
`calls` (recording draws, clears and resolves) and `read back` (the CPU's conversion of small resolves).

| ms a frame, renderer thread | Before | After |
| --- | --- | --- |
| Draws (1,850) | 9.4 | 6.5 |
| Read back of small resolves into guest memory (800,000 texels, 41 rects) | 9.8 | 3.3 |
| Resolves (56) | 2.6 | 1.0 |
| Present copy of the front buffer (CPU readback, 2.5 MB) | 0.4 | 0.8 |
| Everything else | 2.6 | 1.6 |
| Renderer thread, timed | 24.8 | 13.2 |
| Renderer thread, as logged (no timers) | 21.6 | 12.0 to 13.5 |
| GPU, frame start to end | 8 to 10 | 8 to 10 |

Of the draws, constants cost 3.5 ms before and 1.5 now, textures 1.4 and 0.4, pipeline lookup 1.2 and 0.7, vertex and index
buffers 1.2, recording commands 1.5, the rest under 1.

Where it went:

1. The read back converted every texel of 41 rects a frame, most of them the virtual texture page table, 71 % of them the same
   as in the frame before. The conversion is one loop per format now, with a table for the page table's half floats (9.8 to
   5.2 ms), and rows that are the same as last time are left alone (to 3.2 ms), unless nothing is known of the memory:
   a write by the game since (`WriteWatcher`), or a rect written before this one over the same tiles (rects overlap: the
   page table's levels are resolved into one another's memory, and the last one in order has to win). A check of every
   skipped row, rect by rect, found none wrong after the first seconds; a check of the memory after the whole pass
   found the skipping not exact, see "Riding and the second track". The buffers are kept, not created and freed for each rect.
2. Two scratch addresses were resolved at five sizes and formats in turn (the post processing chain, the table's smallest
   levels), and a texture was created for each every frame, 0.25 ms apiece. Each size keeps its texture now (1.9 ms).
3. Draws uploaded 8 KB of constants each, byte swapped twice. Only the registers a shader's ALU instructions read are
   uploaded (`ConstantCount`, checked by filling the rest of the upload buffer with NaN: same picture), the game's thread
   works out what changed since the draw before, and the snapshot keeps the constants in the host's order.
4. A draw built its pipeline description and hashed it; it is found from the registers the pipeline is made of now.
   Textures are bound once for draws with the same fetch constants. Counting draws no longer looks up a string.

With `native_gpu_frame_limit = 60` the first track holds 60.0 FPS, the renderer thread at 10 to 11.5 ms (worst frame 13.4 ms),
and 66 to 80 FPS without it.

What stays: the read back (3 ms: the compare and the scatter into the tiled layout, which only the GPU packing the table to
2 bytes a texel would cut), recording the draws (RADV's share is about 2 ms), the present copy (a GPU side present through
the swapchain would save it), the GPU at 9 ms. Xvfb has no DRI3, so what presenting costs on a real display is not shown here.

### Riding and the second track

`nav.py` with `NAV_RIDE=1` rides: `keyd` (XTest, keys held) keeps the throttle down, leans a and d at random, presses B every
25 s and Back (restart track) every 70 s; `NAV_SELECT_KEYS="d"` picks the next track (Training Program 2, unlocked by
finishing the first, which a ride does after about 100 s), `NAV_LIMIT=60` sets the frame limit. `ride.sh <tag> <secs> [limit]
[keys]` runs it and `ridestats.py` gives the numbers: the 5 s windows of the stats line, so minimum and worst are of 5 s
averages. Pictures: snapshots every 50 s.

| Greenhorn's Grove, ridden | FPS min / median / max | Renderer thread ms | GPU ms |
| --- | --- | --- | --- |
| No limit, 4 min | 52 / 60 / 70 | 11.4 / 13.4 / 15.8 | 9 to 11.5 |
| `native_gpu_frame_limit = 60`, 2.5 min | 50 / 59.0 / 60.2 | 10.5 / 12.0 / 15.1 | 9 to 11.2 |

The slow windows come with the checkpoint and track restarts (one frame takes 70 to 80 ms, once every 25 s). Single frames in the 300 frame line: 15 to 30 ms, apart from the restarts. The
renderer thread stays under 16.6 ms except in windows with a restart, so the limit holds 60 most of the time, not always.

The second track (limit 60, 2 min): FPS 58.4 / 59.9 / 60.1, renderer thread 10.5 / 12.1 / 14.0 ms, no shader missing. Pictures of both tracks, ridden: no
black holes, missing decals or flicker seen; the rider, sky, decals, signs, shadows and the finish screen are right. Not
compared with the emulated renderer on the second track: the emulated runs crash at the start there (slower game), so
no frame of the same moment exists.

Shaders: 16 hashes were "not in the cache" (4 at the first track's loading, 12 in the second track); all dumped with
`gpu_shader_dump_dir` from emulated runs (`out/shader_dump`, 119 files now), `tools/native/build_shaders.sh`, rebuilt: none
left on the first track or at the start of the second. Two more (`885C7E9950FFF792`, `D646D1067FE1707E`) showed once, 87 s
into a ride of the second track, in a memory export draw next to the page table's; no run since has reached them again
(3 more of 200 to 400 s, emulated and native with the dump on), so they are not in the cache.

Read back skipping, exactness. The skipping is not exact: converting every rect again after a pass and comparing the
memory (a temporary check, `out/native_tools/verify_read_back.py`) found about 1 rect in 1,000 different (100 of 100,000
in a 40 s ride), through the whole ride, not only at the start. The rects that differ are mostly the page table's levels and
scratch pairs, which share memory. An exact version
is `out/native_tools/exact_read_back.patch`: the memory is owned by the rect that wrote it last (`memory_owner_`, per KB),
and a row is left alone only while its rect owns all of it, or a later entry of the same pass covers it. Checked as above:
0 of 100,000 differ. It costs the gain: read back 3.3 ms at first, and about 7 ms in most rides from the first restart on (a
state in which the 25 rects of the post processing chain, the page table's levels and scratch pairs are all converted every
frame; the ownership never settles in it, and why was not found), so the renderer thread goes to 15.8 ms and the limit
holds 58 FPS. **Not applied**: no wrong picture was seen with the inexact skipping, and 3 ms is the margin the limit needs.

## Verdict so far

60 FPS on the Odin 2 Portal is out of reach by removing emulation alone. With the 44 ms split as above, a renderer with no
EDRAM emulation at all would still spend about 24 ms on the same draws with the same shaders, about 40 FPS at the very best
and with texture uploads and resolves still to remove. The one lever measured to work is fewer pixels: half the area takes
10 ms off. The game renders at 1024 x 608, and where the engine sets that has not been found: no constant pair in the code.

Next, in order of expected gain for the work:

1. Find the engine's render resolution and lower it, aiming at a steady 30 FPS.
2. Load the virtual texture table only where it changed, not whole 4 to 5 times a frame (4 ms).
3. The native renderer, only if the two above fall short.

Native work left, if taken up:

0. Move the native renderer's entry to the command stream: take draws from the SDK's `IssueDraw`, keep the render targets simple.
2. Shaders: the raw microcode, for the whole set, through the SDK's translator.
3. Render the title screen from a frame capture.
4. Memory export: done natively, see the current state.

To find what writes a buffer: attach gdb once in game, `watch -l` on the guest address + `0x1A0000000`.
