#!/bin/bash
set -euo pipefail
source "$(dirname "$0")/env.sh"
output="$HAIKU_WORK/build/efi-tools"
mkdir -p "$output"
linker="$HAIKU_WORK/toolchains/host/usr/bin/lld-link-18"
for name in efi-probe roobi-efi; do
    clang++ --target=aarch64-pc-windows-msvc -ffreestanding -fno-builtin \
        -fno-stack-protector -fno-exceptions -fno-rtti -O2 -Wall -Wextra -Werror \
        -I "$HAIKU_SOURCE/headers/private/kernel/platform" \
        -c "$HAIKU_SOURCE/tools/rock5-itx/$name.cpp" -o "$output/$name.obj"
    "$linker" /subsystem:efi_application /entry:efi_main /nodefaultlib /machine:arm64 \
        "/out:$output/$name.efi" "$output/$name.obj"
done
echo "EFI applications built in $output"
