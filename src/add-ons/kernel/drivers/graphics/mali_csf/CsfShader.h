/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#ifndef MALI_CSF_SHADER_H
#define MALI_CSF_SHADER_H

#include "CsfCommandMemory.h"

namespace MaliCSF {

// One compute invocation stores the same eight values as the CS regression.
// This fixed diagnostic is not an arbitrary shader or userspace buffer API.
// Call only on the driver's 4 KiB code allocation before GPU mappings exist.
inline void BuildStoreShader(void* memory)
{
	memset(memory, 0, 4096);
	uint64_t* code = (uint64_t*)memory;
	code[0x300 / 8] = UINT64_C(0x80000018); // Compute SPD, 32 work registers.
	code[0x308 / 8] = kCommandAddress + 0x400;
	code[0x340 / 8] = UINT64_C(31) << 32; // No TLS or workgroup memory.
	for (unsigned round = 0; round < 2; round++) {
		uint32_t* uniforms = (uint32_t*)memory + (0x380 + round * 64) / 4;
		uniforms[0] = uint32_t(kDataAddress);
		uniforms[1] = uint32_t(kDataAddress >> 32);
		for (unsigned i = 0; i < 8; i++)
			uniforms[i + 2] = CommandValue(round, i);
		uint64_t* stream = code + round * 32;
		unsigned n = 0;
		stream[n++] = UINT64_C(0x0300000000ff0000); // WAIT all.
		stream[n++] = UINT64_C(0x1700000000000002); // Endpoint scoreboard 2.
		stream[n++] = UINT64_C(0x2200000000000001); // REQ_RESOURCE compute.
		stream[n++] = UINT64_C(0x0100000000000000); // No SRT resources.
		stream[n++] = UINT64_C(0x0208000000000000) | (0x380 + round * 64);
		stream[n++] = UINT64_C(0x0209000005000001); // FAU: five 64-bit slots, VA high word.
		stream[n++] = UINT64_C(0x0110000000000000) | (kCommandAddress + 0x300);
		stream[n++] = UINT64_C(0x0118000000000000) | (kCommandAddress + 0x340);
		for (unsigned reg = 32; reg <= 36; reg++)
			stream[n++] = UINT64_C(0x0200000000000000) | (uint64_t(reg) << 48);
		for (unsigned reg = 37; reg <= 39; reg++)
			stream[n++] = UINT64_C(0x0200000000000001) | (uint64_t(reg) << 48);
		stream[n++] = UINT64_C(0x0400000000008001); // RUN_COMPUTE, Z axis, increment 1.
		stream[n++] = UINT64_C(0x0300000000040000); // WAIT shader endpoint.
		stream[n++] = UINT64_C(0x0204000000000000); // Flush ID zero.
		stream[n++] = UINT64_C(0x2400040000000211); // Clean L2/LSC, invalidate other.
		stream[n++] = UINT64_C(0x0300000000010000); // WAIT flush.
	}
	uint64_t* shader = code + 0x400 / 8;
	unsigned n = 0;
	shader[n++] = UINT64_C(0x0091c20000000080); // MOV r2, u0 (data address low).
	shader[n++] = UINT64_C(0x0091c30000000081); // MOV r3, u1 (data address high).
	for (unsigned i = 0; i < 8; i++) {
		shader[n++] = UINT64_C(0x0091c00000000080) | (i + 2); // MOV r0, uniform.
		shader[n++] = UINT64_C(0x0061400218000002) | (uint64_t(kStoreWordIndices[i] * 4) << 8);
		shader[n++] = UINT64_C(0x0800c00000000000); // NOP.wait0 before reusing r0.
	}
	shader[n] = UINT64_C(0x7800c00000000000); // NOP.end.
}

} // namespace MaliCSF
#endif
