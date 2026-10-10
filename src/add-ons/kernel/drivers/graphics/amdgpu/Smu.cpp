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

bool
amdgpu_smc_ready(volatile uint32* regs)
{
	SmcAccess smc(regs);
	amdgpu_smc_bootstrap info = {};
	smc.Snapshot(info);
	return (info.clock & 1) == 0 && info.pc >= 0x20100 && info.pc < 0x40000
		&& (info.smu_status & 3) == 3 && info.soft_registers >= 0x20000
		&& info.soft_registers <= 0x40000 - 120 && (info.soft_registers & 3) == 0;
}


status_t
amdgpu_smc_load_gfx(volatile uint32* regs, const amdgpu::FirmwareView firmware[4],
	volatile uint32* workspace, uint64 gpu)
{
	if (!amdgpu_smc_ready(regs))
		return B_DEV_NOT_READY;
	SmcAccess smc(regs);
	amdgpu_smc_bootstrap info = {};
	smc.Snapshot(info);
	// Keep the running SMU's first MiB intact; replace only the completed
	// transfer table and images in its second MiB. SDMA is idle/owned by us.
	volatile uint32* toc = workspace + (1 << 20) / 4;
	for (uint32 i = 0; i < (1 << 20) / 4; i++)
		toc[i] = 0;
	toc[0] = 1; toc[1] = 4;
	const uint32 ids[] = {3, 4, 5, 10}; // CE, PFP, ME, RLC
	uint32 offset = 4096;
	for (uint32 i = 0; i < 4; i++) {
		uint32 n = 2 + i * 7;
		uint64 address = gpu + (1 << 20) + offset;
		toc[n] = (firmware[i].version & 0xffff) << 16 | ids[i];
		toc[n + 1] = address >> 32;
		toc[n + 2] = (uint32)address;
		toc[n + 5] = firmware[i].codeSize;
		toc[n + 6] = i == 3 ? 1 : 0; // Linux unhalts RLC, keeps CP halted
		for (uint32 b = 0; b < firmware[i].codeSize; b += 4)
			toc[(offset + b) / 4] = amdgpu::ReadLE32(firmware[i].code + b);
		offset = (offset + firmware[i].codeSize + 4095) & ~4095u;
	}
	__sync_synchronize();
	(void)toc[offset / 4 - 1];
	regs[0x1520] = 1;
	(void)regs[0x1520];
	uint32 loadStatus = info.soft_registers + 0x6c;
	const uint32 mask = 0x438;
	smc.Write(loadStatus, smc.Read(loadStatus) & ~mask);
	status_t status = smc.Send(0x250, (gpu + (1 << 20)) >> 32);
	if (status == B_OK)
		status = smc.Send(0x251, (uint32)(gpu + (1 << 20)));
	if (status == B_OK)
		status = smc.Send(0x254, mask);
	if (status == B_OK && !smc.Wait(loadStatus, mask, mask))
		status = B_TIMED_OUT;
	dprintf("amdgpu: SMC GFX load status %#x\n", (unsigned)smc.Read(loadStatus));
	// Polaris10 golden ACLK divider, as in gfx_v8_0_init_golden_registers.
	if (status == B_OK)
		smc.Write(0xc05000dc, (smc.Read(0xc05000dc) & ~0x7fu) | 0x18);
	return status;
}

void
amdgpu_smc_dump_uvd_clocks(volatile uint32* regs)
{
	SmcAccess smc(regs);
	// Linux smu_7_1_3_d.h: dividers alone do not establish the clock rate
	// when the DFS clock is bypassed. Capture the inherited source state too.
	const uint32 addresses[] = {
		0xc050009c, 0xc05000a0, 0xc05000a4, 0xc05000a8,
		0xc0500118, // GCK_DFS_BYPASS_CNTL
		0xc0500140, 0xc0500144, 0xc0500148, 0xc050014c,
		0xc0500150, 0xc0500154, 0xc0500158, 0xc050015c,
		0xc0500160, // SPLL_CNTL_MODE
		0xc05001a0, 0xc05001a4, // CG_CLKPIN_CNTL / _2
		0xc05001c8, // GCK_ADFS_CLK_BYPASS_CNTL1
		0xc0200000 // GENERAL_PWRMGT
	};
	for (uint32 address : addresses)
		dprintf("amdgpu: UVD clock register %#x = %#x\n", (unsigned)address,
			(unsigned)smc.Read(address));
}
