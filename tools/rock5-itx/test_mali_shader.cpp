/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
#include "CsfShader.h"
#include <assert.h>
#include <stdio.h>

int main()
{
	alignas(64) unsigned char memory[4096 + 128];
	memset(memory, 0xa5, sizeof(memory));
	MaliCSF::BuildStoreShader(memory + 64);
	for (unsigned i = 0; i < 64; i++) {
		assert(memory[i] == 0xa5);
		assert(memory[64 + 4096 + i] == 0xa5);
	}
	return fwrite(memory + 64, 4096, 1, stdout) == 1 ? 0 : 1;
}
