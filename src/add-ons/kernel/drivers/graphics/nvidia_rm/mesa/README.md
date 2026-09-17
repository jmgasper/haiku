# NVK for nvidia_rm on pre-Volta GPUs

`nvk-nvrm-pre-volta.patch` applies to X547's Mesa branch `mesa-nvk-r2`
(https://github.com/X547/mesa, NVK with an NVIDIA RM backend) and adapts the
RM backend to GPUs without the Volta/Turing doorbell (Pascal and older):

- no USERMODE object; channels are kicked by writing GP_PUT to the USERD
  page that the RM maps for the channel object
- GF100 channel semaphores instead of the Volta SEM_* methods
- block linear (GENERIC_16BX2) PTE kind for virtual address allocations
- debugging aids: `NVK_NVRM_DEBUG`, `NVK_NVRM_FIFO_IDLE_WAIT`,
  `NVK_NVRM_CTX_VRAM`

The native build on Haiku is scripted in the X399 lab tooling. Status on the
GTX 1080 Ti: the Vulkan device is created and the GPU fetches the channel's
GPFIFO, but the submitted work does not complete yet.
