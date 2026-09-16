# GLTeapot and polygon qualification

The +256 Mesa shader-lifetime fix on the retained +254 Haiku kernel passes two
native boots. All 128 polygon frames, 524,288 pixels and 16,384 guard bytes pass.
Each context records the expected 28 geometry draws, 24 batches and 155 vertices,
with no failures. The two complete polygon transcripts are byte-identical. On
each boot, the fourteen cases rejected by the +255 diagnostic are corrected;
the other fifty images remain byte-identical to that failed baseline.

GLTeapot completes four normal launches, all menu confirmations and all 32
reviewed frames containing 2,014,208 pixels. All 8,732 GPU submissions complete
and allocations return to baseline. Actual frame sheets and HDMI captures show
the original quad wireframe, shaded and unlit rendering, perspective and resizing.
Detailed CPU geometry and NIR logging are disabled during qualification.

The deterministic CPU token-lifetime test also passes ten checks on each native
boot. The earlier graphics suite passes, including windows, OpenGL Kit views,
pipeline operations, process cleanup, heap growth/limits, concurrency, pending
work cleanup, command-fault recovery and retained-context loss notification.
Raw UART accounts for all 58 runtime and heap records across the two boots.

The Mesa source is reconstructed from pinned inputs and fully rebuilt for ARM64.
Only the Mesa EGL library changes among the existing image assets; the CPU test
binary is added. All 37 kernel/system components retain their qualified +254
bytes. Both QEMU EL1 and EL2 pass two boots, the CPU regression and the software
graphics suite: 48 checked captures, 64 application frames and 256 polygon frames.
The reconstructed-source host regression passes under ASan/UBSan. QEMU exercises
software rendering; native Mali results come from the physical ROCK board.

Normal reboot, shutdown, automatic ROOBI recovery, independent eMMC integrity
checks and read-only recovery-image verification pass. NanoKVM's boot identity
remains unchanged. The source/capture audit verifies 259 frozen files (135 Git
sources), all 48 software captures and 112 shared-frame comparisons. The original
final reviewer failed because a derived UART string retained CRLF line endings.
A separate, recorded reviewer copy normalizes that derived text after raw-byte
validation; all 58 expected records pass. Original logs, hashes, frozen controls
and the failed review are preserved.

- Mesa source: `8ad3c3394c07517b658397cf333c1faf002411ee` (+256).
- Retained Haiku source: `6b08a84b31bd9c2a24113cc7d8ca5fb489e7a4ea`, hrev60097+254.
- Original image SHA-256:
  `3b20869f8750d5a4d05245b9103724b9bf2b1b98f5f74bdf7893670d1f00ae26`.
- Native evidence: `artifacts/automated-mali-shader-token-lifetime/20260916T215335Z-2434cf`.
- QEMU EL1: `artifacts/qemu-shell/20260916T213516Z-2c7b09`;
  EL2: `artifacts/qemu-shell/20260916T214353Z-028d1c`.
- Development/build evidence: `artifacts/mali-shader-token-lifetime/20260916T130123Z`.
- Recovery boot: `0d4d0050-800e-4e34-b13b-f01c8909f282`.
- Used-image archive: `artifacts/nanokvm-image-archive/20260916T222138Z-8cea83`;
  SHA-256 `6ec69438f28a2141fb722c10eeaf5b141958d04e9b7e1f5574eda266189e4a37`.
  The used image is independently hashed and preserved locally before removing
  its exact detached remote copy; read-only recovery remains attached.

This qualifies the bounded application and polygon fixture. The private Mesa
installation still requires its launch environment and opt-in CPU geometry;
rasterization runs on Mali. Broader OpenGL coverage, a separate first-provoking
quad-strip boundary defect, default renderer integration and native display
control remain open. The earlier failed candidates below retain their original
results.

## Earlier +252 topology trial

The +252 candidate preserves quads, quad strips and polygon boundaries through
the opt-in CPU geometry path, retaining native Mali rasterization. GLTeapot
passes its original application validator on one native boot: two normal
process exits, fourteen menu confirmations, all sixteen frames and 1,007,104
pixels, 1,386 completed GPU submissions and restored allocation baselines.
Actual contact-sheet review shows the quad wireframe without the earlier extra
diagonals. Controller and renderer logs are separately captured, checked and
combined after both processes exit. All four stream lengths and SHA-256 hashes
pass. **The complete candidate remains unqualified.**

The expanded polygon probe independently rejects the same image. Each context
passes 25 of 32 cases. Immediate-mode quad/polygon lines lose their left edge;
quad strips lose a closing edge; point mode produces three corners instead of
four; and the front-facing quad culling case is empty. Indexed quad/strip
outlines and filled controls pass. Both contexts repeat these failures. All
64 complete RGBA images and 8,192 guard bytes are retained and the diagnostic
sheet was actually reviewed. The unchanged validator rejects equivalent-image
comparison `(0, 17, 19)`; the guest also reports failure. Geometry traces report
28 draws, 23 batches and 138 vertices per context, below the predeclared 24
batches and 155 vertices. No acceptance rule was changed for this run.

The trial also reveals a controller expectation mismatch: the polygon probe
creates two contexts in one retained EGLDisplay, producing one normal runtime
teardown. Its controller expected two. The actual UART contains 29 runtimes,
including two normal GLTeapot retirements and one normal polygon retirement.
This remains separate from the confirmed pixel failure and must be resolved
before another qualification attempt. The second native boot was not reached.

The full Haiku and reconstructed Mesa/application builds pass. Both QEMU modes
pass two boots each: 48 checked capture transfers, 64 reviewed application
frames and 256 reviewed polygon frames. The real geometry helper passes its
existing host ASan/UBSan checks, but those checks did not cover every failing
native input. After the failed trial, normal shutdown, automatic Linux recovery,
independent eMMC file/filesystem/partition/region checks, recovery-image hashing,
source verification and used-image preservation all pass. The original failed
controllers, sources and transcripts remain unchanged.

- Source: `b0d84c5fac8155a19916c24f7df84403eda994b6`, hrev60097+252.
- Original image SHA-256:
  `0ea6826de5783b53caa7c1119a62701b8326f3e7addfb75288ccc4bfc63a25dd`.
- Native evidence: `artifacts/automated-mali-polygon-topology/20260916T104428Z-562dbb`;
  309 preserved source/input files plus the two original Linux reference binaries.
- QEMU EL1: `artifacts/qemu-shell/20260916T102542Z-e0c40f`;
  EL2: `artifacts/qemu-shell/20260916T103454Z-53ba9c`.
- Recovery boot: `e8d4a05a-d1d5-4ffb-b27d-e22ad3d1fb6c`.
- Used-image archive: `artifacts/nanokvm-image-archive/20260916T110321Z-477c2d`;
  SHA-256 `f6e2a9027590ff381306d02d17e5c3d12c56376374c0602cbb88e5967075357b`.
  The exact verified remote copy was removed; read-only recovery remains attached.

## +254 native polygon diagnostics

Two bounded diagnostic boots use the same +254 image and graphics-library
bytes. The first captures all 64 images byte-identical to the rejected +252
trial. Its 56 geometry draws show correct input positions and all four input
edge flags equal to 1.0. For immediate-mode unclipped quad, strip and polygon
lines/points, the final transformed vertex instead has `edgeflag=0`; indexed
and clipped controls retain their flags. The shader NIR is retained.

The follow-up enables `NIR_DEBUG=tgsi` after a QEMU check confirms that this
logging leaves all 64 software-rendered frames unchanged. Native execution
captures 30 translated vertex shaders, including the expected instruction
`MOV OUT[1].x, IN[1].xxxx`. The six missing-edge/point cases still fail in each
context. Case 30 now produces a complete outline in both contexts; 62 of 64
images are unchanged. The frozen controller reports `incomplete` with
`quad-cull-front` because it expected the earlier image hashes. This deviation
is retained, not converted into qualification. Both actual diagnostic sheets
and native desktop captures were visually reviewed.

At this stage the native CPU input-processing fault remained unresolved. Independent checked
pipe-adapter fixtures pass 24 basic layout, 24 interleaved-buffer and 36 repeated
state-transition checks under host ASan/UBSan. Two Haiku ARM64 QEMU runs using
the actual +254 Mesa libraries pass 24 basic and 24 interleaved layout checks.
These tests do not reproduce the failure or exercise native Panfrost callbacks.
The next diagnostic adds bounded CPU shader input/output and buffer-binding
logs only when the existing geometry trace option is enabled.

Both native trials complete normal shutdown, automatic Linux recovery, four
normal GPU runtime/heap retirements, independent eMMC file/partition/region
hashes, source verification and used-image preservation. No kernel fault is
observed. The source snapshots contain 343 and 345 files respectively. The
original validators, controllers and failed results remain unchanged.

- Source: `6b08a84b31bd9c2a24113cc7d8ca5fb489e7a4ea`, hrev60097+254.
- Original image SHA-256:
  `fa7c3761e580cc01438a2c234acebf6b7e3e7c1da81eaa7c95d25e916808c4bd`.
- First diagnostic: `artifacts/automated-mali-polygon-trace/20260916T114812Z-8787b9`;
  transcript SHA-256 `e13a98510a3cf74d7b358ef593cf2c7e2c1333c8e7e8ef6b11bf1cbee5368fea`.
- Shader diagnostic: `artifacts/automated-mali-polygon-tgsi/20260916T122152Z-025763`;
  transcript SHA-256 `888daf10bee24441c5244e3f54366944fe9e6809552392ca0e144b3a6be67b64`.
- Used-image archives: `artifacts/nanokvm-image-archive/20260916T115832Z-3abcbf`
  and `artifacts/nanokvm-image-archive/20260916T123355Z-0facfe`.
- Latest recovery boot: `9497ae0b-ec17-4fa7-9197-36dd3717630e`;
  NanoKVM boot `2597fc80-b134-4c61-9241-c7231d552eb2` remains unchanged.

## +255 shader lifetime diagnosis

A third diagnostic adds bounded logs around the actual CPU vertex interpreter.
It captures correct fetched inputs, but the wrong outputs before clipping. In
both contexts, case 29 writes clip distances `-0.25, 1.25, 1.25, -0.25`. Case 30,
whose shader should write four edge flags of 1.0, instead reproduces those exact
clip distances. The original fourteen pixel failures remain unchanged.

Mesa's `draw_vs_exec.c` shares one TGSI interpreter between vertex shaders.
Preparation compares the token pointer with the interpreter's cached pointer.
Deletion previously freed those tokens without clearing the cache. If the
allocator reuses the address, preparation skips loading the new instructions.
The +256 fix unbinds the interpreter before freeing its currently cached
tokens. Deleting an inactive shader leaves the active binding intact.

The new `shader-token-lifetime-test.c` runs real Mesa draw/TGSI code with only
the token allocator controlled to guarantee address reuse. It checks eight
alternating shader lifetimes and two draws surrounding inactive-shader deletion.
The original code fails four of ten checks; the fix passes all ten under
ASan/UBSan, with ten allocations/releases and eight exact address reuses.
`test-shader-token-lifetime.py` records source/library hashes and commands.
The +256 qualification above verifies the same test on both native boots.

The +255 trace library retains the +254 kernel. Its fresh Mesa reconstruction,
both QEMU modes and native CPU layout fixture pass. Native recovery, independent
integrity checks and preservation of the used image pass; 256 candidate source
files are verified. Both the actual diagnostic sheet and desktop were reviewed.

- Trace source: `6d0a9aa12253a224f0ca77e6744ef9f21d36641c`.
- Trace image SHA-256:
  `1d33bd2f20146fb7e0a58e099e9b5ad8d0e418693d37a1e4a3085ddad79b6a51`.
- Native evidence: `artifacts/automated-mali-polygon-vs-trace/20260916T125251Z-9a195a`.
- Host before/after regression: `artifacts/mali-shader-token-lifetime/20260916T130123Z/host-test`.
- Used-image archive: `artifacts/nanokvm-image-archive/20260916T130701Z-7f70f0`;
  SHA-256 `2dea2d6501293aeb4fcf04d82eae0d3c1b58bac2b5c8db9034903e17eaa43da0`.
- Recovery boot: `9330e48e-fb33-4fed-8204-3fe968515351`.

## Earlier +250 topology and logging trial

The +250 candidate enables the experimental CPU polygon geometry path while
retaining native Mali rasterization. Its first native boot passes the earlier
graphics checks and launches GLTeapot twice. Both processes exit normally;
all 7,922 observed submissions complete and allocation snapshots return to
baseline. **This candidate is not qualified.**

The application validator rejects its transcript because the first launch lacks
the `Filled polygons marked=1` confirmation: thirteen menu records are present
where fourteen are required. No record was inserted or reconstructed to make
it pass. The independent queue checks and all sixteen actual frame payloads
remain available separately. Visual inspection shows a wireframe mesh,
but with extra diagonal edges compared with the software-rendered quad-strip
mesh. The polygon probe and second native boot were not reached.

GLTeapot's checked-in source uses `GL_QUAD_STRIP`. Panfrost's Valhall capability
mask omits both quads and quad strips, so Mesa converts them to triangles before
the new geometry path receives them. The next prototype preserves those original
boundaries. The missing complete menu line is consistent with concurrent writes
from controller stdout and renderer stderr to the same regular file; the Haiku
file-position update is not serialized in `common_user_io`. This cause remains
an inference. Separate streams, combined only after process completion, are the
planned logging correction.

The full Haiku and reconstructed Mesa/application builds pass. Both QEMU modes
pass two boots each; all 48 capture transfers are checked, all 64 application
and 128 polygon frames are reviewed, and the geometry helper passes ASan/UBSan
execution tests. These software results do not qualify the native candidate.
Clean shutdown, automatic recovery and independent eMMC file/partition/region
hashes pass after the failed run. The used USB image is preserved and its exact
remote copy removed.

- Source: `59cfb0ab963dbdc9a8fc5a48e860c47f0cd5155d`, hrev60097+250.
- Original image SHA-256:
  `50daa9904528fb05ba3bac528687ba31429ee07781343b4c3ca4e4c72ed807fc`.
- Native evidence: `artifacts/automated-mali-polygon/20260916T074856Z-edf0f1`.
  It includes the rejected original transcript, a source snapshot, independent
  partial analysis and the actually reviewed sixteen-frame diagnostic sheet.
- QEMU EL1: `artifacts/qemu-shell/20260916T072948Z-734011`;
  EL2: `artifacts/qemu-shell/20260916T073856Z-5029e9`.
- Recovery boot: `2d2c324c-983b-4970-9379-f4595ee3876d`.
- Used-image archive: `artifacts/nanokvm-image-archive/20260916T080411Z-291265`;
  SHA-256 `8cf152bf73620e26fd01a03cd41c98ccd7981b46e0c128149b3d2ba107fc49c7`.

## Earlier +247 application trial

The +247 USB image runs the unchanged GLTeapot application with the native
Mali-G610 renderer. Across two native boots, four application processes
animate, respond to scripted settings and resizing, and exit normally.
All 9,738 GPU submissions complete and allocation snapshots return to baseline.
The application is **not qualified**: selecting wireframe still produces
filled polygons on both boots.

This is an actual visual failure. Every phase-2 image in the two native
16-frame contact sheets shows a solid teapot, despite the controller verifying
that “Filled polygons” is unchecked. The same phase visibly produces wireframe
on all four software-rendered QEMU boots. The broad colour and animation checks
passed; they were insufficient to establish that this setting worked. The
separate visual review prevented the candidate from being accepted.

The trial also checks lit and unlit rendering, perspective, resizing, two
fresh application launches per boot, normal process retirement, completed
native queues and resource cleanup. The earlier GPU fixtures pass on both
native boots. This does not establish general OpenGL conformance or native
display control. Presentation continues through the CPU bitmap copy and EFI
framebuffer.

## Evidence

- Source: `9cb68583a34be170b425fae2064c3c6ffec29a6a`, hrev60097+247.
- Original image SHA-256:
  `7adee2b16c597b0d9dad800a44e4e6b84096bd865036151dceae7a8341500342`.
- Native run:
  `/mnt/HaikuWork/artifacts/automated-mali-application/20260916T045820Z-124d45`.
  It retains both transcripts, complete readbacks, contact sheets, HDMI frames,
  failed visual reviews and the source snapshot.
- QEMU EL1: `artifacts/qemu-shell/20260916T043940Z-a2b17c`.
  QEMU EL2: `artifacts/qemu-shell/20260916T044834Z-ef5d47`.
  Each performs two boots; all 64 application frames were visually reviewed.
- Normal shutdown and automatic recovery to ROOBI boot
  `d74b79ce-0650-4b71-a51f-664e5aab6a11` pass. Independent eMMC partition,
  file and region hashes match their references.
- Used image archive:
  `artifacts/nanokvm-image-archive/20260916T052158Z-44b3d3`.
  The locally preserved used image has SHA-256
  `d2744bbde162bc69723ef3185ba95ee57f1b49e9c4f189d495fed9a5315f5bc3`.
  The verified remote copy was removed after recovery.

## Investigation

The unchanged application calls `glPolygonMode(GL_FRONT_AND_BACK, GL_LINE)`.
The pinned Mesa 25.3.6 Panfrost command-stream source is byte-identical to the
original archive, and its context and command-stream code contain no handling
of `fill_front` or `fill_back`. A smaller diagnostic now confirms correct API
state and the native pixel failure independently of the application.

Simply converting triangle indices to lines would not cover face culling,
clipping and different front/back polygon modes. GLTeapot enables culling by
default. Mesa's existing software geometry pipeline is a possible route to
preserving those semantics while retaining GPU rasterization. An opt-in
experimental integration was built and tested as the +250 candidate above.
It has no native qualification; the retained +247 evidence remains unchanged.

## Independent polygon diagnostic

The diagnostic adds only a probe and launcher to the unchanged +247 image.
It requests desktop OpenGL, verifies the actual Mali-G610 renderer and queried
front/back polygon modes, and checks complete 64×64 RGBA readbacks with guards.
Each boot creates two contexts and runs sixteen cases per context, including
indexed draws, culling, split front/back modes, edge flags, clipping and restored
fill state.

Both native boots give the same result: 16 control cases pass and 16 rendering
cases fail per boot. Every queried polygon mode and GL-error check passes.
Line and point requests nevertheless produce the same 1,152-pixel filled
triangle, including all 64 checked interior pixels. Culling rejects the correct
faces; the clipped line request produces a filled clipped polygon. All 64
complete frames and 8,192 guard bytes are retained, with 32 byte-identical
cross-boot frame comparisons. Both contact sheets and both HDMI desktops were
actually reviewed. This confirms a rendering defect, not a successful feature.

The software oracle passes all 128 frames across two EL1 and two EL2 QEMU
boots; complete corresponding frames match across those four boots. The first
EL2 attempt timed out during reboot and remains preserved. Two native attempts
stopped before Haiku; another controller attempt interrupted startup with an
early power action and produced no probe result. The successful controller waits
for normal startup first and completed both boots without that power action.
The earlier failures are retained and are not counted as rendering tests.

- Original diagnostic image SHA-256:
  `77b3241ca811628c76a4c39f476ade5477608bdb8b673c8f150e85c3f883ea79`.
- Frozen probe, validation and procedure inputs:
  `artifacts/mali-polygon-mode-fix/20260916T051919Z-c8b012`.
- Native transcripts, all RGBA frames, visual reviews and recovery receipts:
  `artifacts/automated-polygon-delayed-start-diagnostic/20260916T063024Z-36b904`.
- Software EL1: `artifacts/qemu-shell/20260916T053657Z-5fc470`;
  accepted EL2 repeat: `artifacts/qemu-shell/20260916T054645Z-9dc39b`.
- Normal shutdown and recovery to ROOBI boot
  `60cd3094-85f4-41ff-b5c3-3a1bdd73fe87` pass. Independent eMMC file, partition
  and region readbacks match their references from that same recovery boot.
- Used image archive: `artifacts/nanokvm-image-archive/20260916T065310Z-23f643`;
  SHA-256 `f615eea8c2db6c6bf942d2bdebc6f9db1942ff4b009ae8154e5c97bc61e0353c`.
  The exact remote copy was removed only after local preservation and matching
  hashes, with the verified read-only recovery image still attached.

## Earlier application preparation

The earlier +245 software trial exposed a controller error: successful
Haiku window-property replies need not contain an `error` field. The controller
was corrected and explicitly linked to the shared unwinder; its exception
cleanup self-test passes. The +246 software run passed, but a corruption test
then exposed an out-of-run transcript-validation gap. The corrected +247
validator rejects that case. All three attempts remain preserved, and none
is presented as a qualified native application milestone.
