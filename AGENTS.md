# ROCK 5 ITX fork instructions

This is jmgasper's experimental, explicitly AI-assisted Haiku fork. The owner
has authorized AI-assisted development here. Upstream Haiku's policy does not
accept AI-generated or AI-modified contributions; do not submit this fork's
work upstream or represent it as upstream-approved.

- Keep all local project files, downloads, temporary files, caches, builds,
  credentials and evidence under the mounted `/mnt/HaikuWork` filesystem.
  Do not change HOME or CODEX_HOME. Use `tools/rock5-itx/env.sh`.
- Work on `rock5-itx` and topic branches based on it; keep `master` as the
  upstream baseline. Pin source/toolchain revisions in build manifests.
- Read `docs/rock5-itx/README.md`, `STATUS.md` and `ROADMAP.md` in that directory
  before hardware work. Update status with actual evidence, not assumptions.
- The dedicated ROCK and NanoKVM may be deployed to, restarted and configured
  as authorized by the owner. Use the lab lock for target mutations.
- Preserve a working recovery route. Establish serial capture and a verified
  restore procedure before replacing boot firmware. Raw register/clock work
  needs the exact board revision and matching hardware documentation.
- Do not commit credentials, local configuration, images, logs or captures.
- Run the local build and QEMU smoke test before a hardware iteration.
  A USB attachment or QEMU boot does not prove native ROCK hardware support.
- Keep phase acceptance tests specific to the device and record the Linux
  reference configuration used for comparison. Missing fixtures stay untested.
