# GLTeapot application trial

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
of `fill_front` or `fill_back`. This points to a missing base-renderer feature;
a smaller diagnostic is being checked to establish the API state and exact
pixel failure independently of the application.

Simply converting triangle indices to lines would not cover face culling,
clipping and different front/back polygon modes. GLTeapot enables culling by
default. Mesa's existing software geometry pipeline is a possible route to
preserving those semantics while retaining GPU rasterization. That integration
has not yet been implemented or tested.

The earlier +245 software trial exposed a controller error: successful
Haiku window-property replies need not contain an `error` field. The controller
was corrected and explicitly linked to the shared unwinder; its exception
cleanup self-test passes. The +246 software run passed, but a corruption test
then exposed an out-of-run transcript-validation gap. The corrected +247
validator rejects that case. All three attempts remain preserved, and none
is presented as a qualified native application milestone.
