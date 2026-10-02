# The Mesa side of the graphics work

STATUS.md describes a machine whose OpenGL and Vulkan run on the GPU, and about
half of what makes that true is not in this repository: it is in two Mesa trees
that have nowhere of their own to live. These are those changes, exported so
that the document does not argue from code that exists on one disk.

| | |
| --- | --- |
| `mesa-nvk/` | NVK, the Vulkan driver, on top of X547's `nvrm` back end. Pascal support, page kinds, submission timing, memory the application owns, training the bus, and taking hold of the screen's frame buffer. Applies to `github.com/X547/mesa` at `b28d7c6`. |
| `mesa-hgl/` | Haiku's OpenGL kit through Zink: the renderer add-on, presenting into the window system's memory and then into the screen itself, vertical sync, and the several corrections those needed. Applies to that tree's Haiku branch at `71177f7`. |

Apply with `git am`, oldest first. They are a record rather than a submission:
the NVK series belongs upstream with X547 and the Zink series with Mesa, and
neither has been offered yet.

The two trees are built by `tools/haiku-build-nvk.sh` (on the workstation) and
`tools/build-mesa-hgl.sh` (from the build host). `tools/check-workstation.sh`
says whether what they produce is actually working.
