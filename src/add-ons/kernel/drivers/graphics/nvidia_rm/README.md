# nvidia_rm

Haiku kernel driver for NVIDIA GPUs that uses NVIDIA's resource manager (RM).

The Haiku OS interface layer in `common/`, `rm/`, `modeset/` and `sdk/` was
imported from X547's nvidia-haiku (https://github.com/X547/nvidia-haiku,
commit cc1849cb2c5c327af4f7eccf993b8616855ad7c5, MIT License, see
`LICENSE.X547`). X547's driver builds the open RM and requires GSP firmware,
which limits it to Turing and newer GPUs.

This fork links the same OS layer with the RM core of NVIDIA's proprietary
Linux driver of the matching release, which still supports Pascal GPUs such
as the GeForce GTX 1080 Ti. NVIDIA binaries are not part of this repository;
`build-cross.sh` downloads and verifies them.
