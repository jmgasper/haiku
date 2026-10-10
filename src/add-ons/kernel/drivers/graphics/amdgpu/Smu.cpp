/*
 * Copyright 2015 Advanced Micro Devices, Inc.
 * Copyright 2026, air/OS.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE COPYRIGHT HOLDER(S) OR AUTHOR(S) BE LIABLE FOR ANY CLAIM, DAMAGES OR
 * OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE,
 * ARISING FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR
 * OTHER DEALINGS IN THE SOFTWARE.
 */

// Protected Polaris startup from Linux polaris10_smumgr.c / smu7_smumgr.c.
// Only the measured hard-key, protected-mode board is admitted. All waits
// have deadlines; no DPM tables, voltage, fan, or clock policy are installed.
#include "Smu.h"
#include <KernelExport.h>

namespace {
const uint32 kIndex = 0x1ac;
const uint32 kData = 0x1ad;
const uint32 kAccess = 0x92;
const uint32 kReset = 0x80000000;
const uint32 kClock = 0x80000004;
const uint32 kPC = 0x80000370;
const uint32 kEvents = 0xc0000004;
const uint32 kStatus = 0xe0003088;
const uint32 kSecurity = 0xe00030a4;
const uint32 kFlags = 0x3f000;

class SmcAccess {
public:
	SmcAccess(volatile uint32* regs)
		: r(regs), index(regs[kIndex]), access(regs[kAccess])
	{
		r[kAccess] = access & ~0x800u;
	}
	~SmcAccess()
	{
		r[kIndex] = index;
		r[kAccess] = access;
		(void)r[kAccess];
	}
	uint32 Read(uint32 address)
	{
		r[kIndex] = address;
		return r[kData];
	}
	void Write(uint32 address, uint32 value)
	{
		r[kIndex] = address;
		r[kData] = value;
		(void)r[kIndex];
	}
	bool Wait(uint32 address, uint32 mask, uint32 expected)
	{
		bigtime_t start = system_time();
		do {
			if ((Read(address) & mask) == expected)
				return true;
			snooze(50);
		} while (system_time() - start < 500000);
		return false;
	}
	void Upload(const amdgpu::FirmwareView& firmware)
	{
		r[kIndex] = 0x20000;
		r[kAccess] = access | 0x800u;
		for (uint32 offset = 0; offset < firmware.codeSize; offset += 4)
			r[kData] = amdgpu::ReadLE32(firmware.code + offset);
		r[kAccess] = access & ~0x800u;
		(void)r[kAccess];
	}
	status_t Send(uint32 message, uint32 argument)
	{
		bigtime_t start = system_time();
		while (r[0x95] == 0 && system_time() - start < 500000)
			snooze(50);
		if (r[0x95] == 0)
			return B_TIMED_OUT;
		r[0xa4] = argument;
		r[0x95] = 0;
		r[0x94] = message;
		(void)r[0x94];
		start = system_time();
		while (r[0x95] == 0 && system_time() - start < 500000)
			snooze(50);
		dprintf("amdgpu: SMC message %#x(%#x) response %#x\n",
			(unsigned)message, (unsigned)argument, (unsigned)r[0x95]);
		return r[0x95] == 1 ? B_OK : r[0x95] == 0 ? B_TIMED_OUT : B_ERROR;
	}
	void Snapshot(amdgpu_smc_bootstrap& result)
	{
		result.clock = Read(kClock);
		result.pc = Read(kPC);
		result.security = Read(kSecurity);
		result.smu_status = Read(kStatus);
		result.response = r[0x95];
		result.soft_registers = Read(0x20030);
	}
	volatile uint32* r;
private:
	uint32 index;
	uint32 access;
};

static status_t
Start(SmcAccess& smc, const amdgpu::FirmwareView& firmware,
	amdgpu_smc_bootstrap& result)
{
	result.stage = 1;
	result.firmware_version = firmware.version;
	smc.Snapshot(result);
	if ((result.security & 0x30000) != 0x30000 || (result.clock & 1) != 0
		|| result.pc >= 0x20000 || (smc.r[0x3412] & 1) == 0
		|| (smc.r[0x3612] & 1) == 0 || smc.r[0x3480] != 0)
		return B_NOT_ALLOWED;
	uint32 reset = smc.Read(kReset);
	smc.Write(kReset, reset | 1);
	smc.Upload(firmware);
	result.stage = 2;
	smc.Write(kStatus, 0);
	smc.Write(kClock, result.clock & ~1u);
	smc.Write(kReset, reset & ~1u);
	if (!smc.Wait(kEvents, 0x10000, 0x10000))
		return B_TIMED_OUT;
	// The protected ROM authenticates the image on Test(0x20000). Check its
	// response and both authentication status bits before restarting the SMC.
	smc.r[0xa4] = 0x20000;
	smc.r[0x95] = 0;
	smc.r[0x94] = 0x100;
	(void)smc.r[0x94];
	if (!smc.Wait(kStatus, 1, 1))
		return B_TIMED_OUT;
	if ((smc.Read(kStatus) & 2) == 0)
		return B_BAD_DATA;
	bigtime_t start = system_time();
	while (smc.r[0x95] == 0 && system_time() - start < 500000)
		snooze(50);
	if (smc.r[0x95] != 1)
		return B_ERROR;
	result.stage = 3;
	smc.Write(kFlags, 0);
	smc.Write(kReset, reset | 1);
	smc.Write(kReset, reset & ~1u);
	if (!smc.Wait(kFlags, 1, 1))
		return B_TIMED_OUT;
	result.stage = 4;
	smc.Snapshot(result);
	if (result.pc < 0x20100 || result.pc >= 0x40000
		|| result.soft_registers < 0x20000
		|| result.soft_registers > 0x40000 - 120
		|| (result.soft_registers & 3) != 0)
		return B_BAD_DATA;
	result.stage = 5;
	return B_OK;
}
} // namespace

status_t
amdgpu_smc_bootstrap_firmware(volatile uint32* regs,
	const amdgpu::FirmwareView& firmware, amdgpu_smc_bootstrap& result)
{
	SmcAccess smc(regs);
	status_t status = Start(smc, firmware, result);
	smc.Snapshot(result);
	// No retry after partial startup: retain evidence and require a cold boot.
	// The lab caller does not hand a partially initialized SMU to applications.
	dprintf("amdgpu: SMC stage %u status %#x PC %#x auth %#x response %#x soft %#x\n",
		(unsigned)result.stage, (unsigned)status, (unsigned)result.pc,
		(unsigned)result.smu_status, (unsigned)result.response,
		(unsigned)result.soft_registers);
	return status;
}


status_t
amdgpu_smc_load_sdma(volatile uint32* regs,
	const amdgpu::FirmwareView& firmware, volatile uint32* workspace, uint64 gpu)
{
	SmcAccess smc(regs);
	amdgpu_smc_bootstrap info = {};
	smc.Snapshot(info);
	if ((info.clock & 1) != 0 || info.pc < 0x20100 || info.pc >= 0x40000
		|| info.soft_registers < 0x20000 || info.soft_registers > 0x40000 - 120
		|| (info.soft_registers & 3) != 0 || firmware.codeSize > 65536)
		return B_DEV_NOT_READY;
	// smu_ucode_xfer_vi.h, little-endian SMU_DRAMData_TOC / SMU_Entry.
	// Use only SDMA0 and keep it halted (flags 0). The PF image includes its
	// 20-byte digest; the SMU authenticates it before reporting completion.
	for (uint32 i = 0; i < 2 * 1024 * 1024 / 4; i++)
		workspace[i] = 0;
	volatile uint32* toc = workspace + 1024 * 1024 / 4;
	uint64 tocGPU = gpu + 1024 * 1024;
	uint64 imageGPU = tocGPU + 4096;
	toc[0] = 1;
	toc[1] = 1;
	toc[2] = (firmware.version & 0xffff) << 16 | 1; // version, SDMA0 ID
	toc[3] = imageGPU >> 32;
	toc[4] = (uint32)imageGPU;
	toc[7] = firmware.codeSize;
	for (uint32 i = 0; i < firmware.codeSize; i += 4)
		toc[1024 + i / 4] = amdgpu::ReadLE32(firmware.code + i);
	__sync_synchronize();
	(void)toc[1024 + firmware.codeSize / 4 - 1];
	regs[0x1520] = 1; // vi_flush_hdp
	(void)regs[0x1520];
	uint32 loadStatus = info.soft_registers + 0x6c; // SMU74_SoftRegisters
	smc.Write(loadStatus, smc.Read(loadStatus) & ~2u);
	const uint32 messages[] = {0x252, 0x253, 0x250, 0x251, 0x254};
	const uint32 arguments[] = {(uint32)(gpu >> 32), (uint32)gpu,
		(uint32)(tocGPU >> 32), (uint32)tocGPU, 2};
	for (uint32 i = 0; i < B_COUNT_OF(messages); i++) {
		status_t status = smc.Send(messages[i], arguments[i]);
		if (status != B_OK)
			return status;
	}
	bool loaded = smc.Wait(loadStatus, 2, 2);
	dprintf("amdgpu: SMC SDMA load status %#x\n", (unsigned)smc.Read(loadStatus));
	return loaded ? B_OK : B_TIMED_OUT;
}

