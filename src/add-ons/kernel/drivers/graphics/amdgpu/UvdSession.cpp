/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#include "Uvd.h"
#include "UvdRegisters.h"
#include <KernelExport.h>

namespace {
static void Copy(volatile uint32* memory, uint32 offset, const uint8* data, uint32 bytes)
{
	for (uint32 i = 0; i < bytes; i += 4) {
		uint32 value = 0;
		for (uint32 j = 0; j < 4 && i + j < bytes; j++) value |= (uint32)data[i + j] << (8 * j);
		memory[(offset + i) / 4] = value;
	}
}
}

status_t
UvdEngine::Session(UvdSession& s, uint32 type, const amdgpu_h264_picture* picture,
	const void* bitstream, uint32 bytes, void* output, amdgpu_uvd_test& result)
{
	if (!ready || faulted) return B_DEV_NOT_READY;
	if (type > 2 || (type == 0) == s.created) return B_BAD_VALUE;
	if (type == 1 && (picture == NULL || bitstream == NULL || output == NULL
		|| !amdgpu::UvdH264Validate(s.config, *picture, bytes)
		|| !amdgpu::UvdH264Bitstream((const uint8*)bitstream, bytes,
			(picture->flags & AMDGPU_H264_IDR) != 0)
		|| (s.frames == 0 && (picture->flags & AMDGPU_H264_IDR) == 0)))
		return B_BAD_VALUE;
	if (s.frames == UINT32_MAX) return B_BAD_VALUE;
	const auto& l = s.layout;
	uint8 message[amdgpu::kUvdMessageBytes];
	amdgpu::UvdH264Message(message, type, s.handle, s.config, l, picture, bytes, s.frames + 1);
	Copy(s.cpu, l.message, message, sizeof(message));
	if (type == 0) {
		for (uint32 offset : l.guards)
			for (uint32 i = 0; i < 1024; i++) s.cpu[offset / 4 + i] = 0xabcddcba;
	}
	if (type == 1) {
		for (uint32 i = 0; i < 4096 / 4; i++) s.cpu[l.feedback / 4 + i] = 0;
		s.cpu[l.feedback / 4] = 4096;
		for (uint32 i = 0; i < l.outputBytes / 4; i++) s.cpu[l.target / 4 + i] = 0;
		Copy(s.cpu, l.bitstream, (const uint8*)bitstream, bytes);
		for (uint32 i = (bytes + 3) / 4; i < ((bytes + 127) & ~127u) / 4; i++)
			s.cpu[l.bitstream / 4 + i] = 0;
		Copy(s.cpu, l.scaling, &picture->scaling4x4[0][0], 96);
		Copy(s.cpu, l.scaling + 96, &picture->scaling8x8[0][0], 128);
	}
	volatile uint32* ib = memory + ((3 << 20) + 0x11000) / 4;
	uint32 count = 0;
	auto command = [&](uint32 offset, uint32 cmd) {
		uint64 address = s.gpu + offset;
		ib[count++] = mmUVD_GPCOM_VCPU_DATA0; ib[count++] = (uint32)address;
		ib[count++] = mmUVD_GPCOM_VCPU_DATA1; ib[count++] = address >> 32;
		ib[count++] = mmUVD_GPCOM_VCPU_CMD; ib[count++] = cmd << 1;
	};
	command(l.session, 5); command(l.message, 0);
	if (type == 1) {
		command(l.dpb, 1); command(l.context, 0x206); command(l.bitstream, 0x100);
		command(l.target, 2); command(l.feedback, 3); command(l.scaling, 0x204);
		ib[count++] = mmUVD_ENGINE_CNTL; ib[count++] = 1;
	}
	while ((count & 15) != 0) ib[count++] = 0x80000000;
	result.stage = type == 0 ? 4 : type == 1 ? 5 : 6;
	status_t status = Submit(count, result);
	if (status == B_OK) {
		for (uint32 offset : l.guards)
			for (uint32 i = 0; i < 1024; i++)
				result.guard_mismatches += s.cpu[offset / 4 + i] != 0xabcddcba;
		if (result.guard_mismatches != 0) status = B_BAD_DATA;
	}
	if (status == B_OK) {
		if (type == 1) {
			for (uint32 i = 0; i < l.outputBytes / 4; i++)
				((uint32*)output)[i] = s.cpu[l.target / 4 + i];
			for (uint32 i = 0; i < 8; i++) result.feedback[i] = s.cpu[l.feedback / 4 + i];
			result.checked_bytes = l.outputBytes;
			s.frames++;
		} else s.created = type == 0;
	} else {
		faulted = true;
		Stop();
	}
	return status;
}
