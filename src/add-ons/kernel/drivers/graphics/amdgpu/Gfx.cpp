/*
 * Copyright 2014 Advanced Micro Devices, Inc.
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

// Polaris10 CP setup and golden values: Linux 6.18.52 gfx_v8_0.c (MIT).
#include "Gfx.h"
#include "Smu.h"
#include <KernelExport.h>

enum { SECT_NONE, SECT_CONTEXT };
struct cs_extent_def { const unsigned int* extent; uint32 reg_index, reg_count; };
struct cs_section_def { const cs_extent_def* section; uint32 id; };
#include "ClearState.h"

static const uint32 kGolden[][3] = {
	{0xcd4, 0x000c0fc0, 0x000c0200}, // ATC_MISC_CG
	{0x2684, 0x0001f3cf, 0x00007208}, // CB_HW_CONTROL
	{0x2686, 0x0f000000, 0x0f000000}, // CB_HW_CONTROL_2
	{0x2683, 0x000001ff, 0x00000040}, // CB_HW_CONTROL_3
	{0x260d, 0xf00fffff, 0x00000400}, // DB_DEBUG2
	{0x22fc, 0xffffffff, 0x20000001}, // PA_SC_ENHANCE
	{0xc281, 0x0000ff0f, 0x00000000}, // PA_SC_LINE_STIPPLE_STATE
	{0xa0d4, 0x3f3fffff, 0x16000012}, // PA_SC_RASTER_CONFIG
	{0xa0d5, 0x0000003f, 0x0000002a}, // PA_SC_RASTER_CONFIG_1
	{0xec49, 0x00000003, 0x0001003c}, // RLC_CGCG_CGLS_CTRL
	{0xec9d, 0xffffffff, 0x0001003c}, // RLC_CGCG_CGLS_CTRL_3D
	{0x2300, 0x07f80000, 0x07180000}, // SQ_CONFIG
	{0x2542, 0x000f000f, 0x000b0000}, // TA_CNTL_AUX
	{0x2b80, 0x00100000, 0xf31fff7f}, // TCC_CTRL
	{0x2b05, 0x000003ff, 0x000000f7}, // TCP_ADDR_CONFIG
	{0x2b04, 0xffffffff, 0x00000000}, // TCP_CHAN_STEER_HI
	{0x2232, 0x00000004, 0x00000004}, // VGT_RESET_DEBUG
	{0xc200, 0xffffffff, 0xe0000000}, // GRBM_GFX_INDEX
	{0xa0d4, 0xffffffff, 0x16000012}, // PA_SC_RASTER_CONFIG
	{0xa0d5, 0xffffffff, 0x0000002A}, // PA_SC_RASTER_CONFIG_1
	{0x263e, 0xffffffff, 0x22011003}, // GB_ADDR_CONFIG
	{0x31dc, 0xffffffff, 0x00000800}, // SPI_RESOURCE_RESERVE_CU_0
	{0x31dd, 0xffffffff, 0x00000800}, // SPI_RESOURCE_RESERVE_CU_1
	{0x31e6, 0xffffffff, 0x00FF7FBF}, // SPI_RESOURCE_RESERVE_EN_CU_0
	{0x31e7, 0xffffffff, 0x00FF7FAF}, // SPI_RESOURCE_RESERVE_EN_CU_1
};
static const uint32 kHalt = 0x15000000;
static const uint64 kCommandVA = 0x10000;
static const uint64 kMemoryVA = 0x100000;
static const uint32 kDirectSequences = 16;
static uint32 Packet(uint32 op, uint32 count) { return 0xc0000000 | count << 16 | op << 8; }

status_t
GfxEngine::InitializeVM(const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation, const Gart& gart)
{
	// VMID1 is a private diagnostic address space. Only the owned command
	// RAM and GFX scratch are mapped; it cannot reach client BOs or scanout.
	vmArea = -1;
	const uint64 offset = 24ULL << 20;
	if ((regs[0x505] & 1) != 0
		|| !amdgpu_vram_range_is_safe(regs, info, reservation, offset, 8192))
		return B_NOT_ALLOWED;
	volatile uint64* directory;
	vmArea = map_physical_memory("amdgpu private GFX VM", info.bar_address[0] + offset,
		8192, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		(void**)&directory);
	if (vmArea < 0)
		return vmArea;
	for (uint32 i = 0; i < 1024; i++)
		directory[i] = 0;
	volatile uint64* ptes = directory + 512;
	directory[0] = (info.vram_gpu_base + offset + 4096) | 1;
	for (uint32 i = 0; i < 16; i++)
		ptes[kCommandVA / 4096 + i] = gart.table[16 + i];
	for (uint32 i = 0; i < 256; i++)
		ptes[kMemoryVA / 4096 + i] = (gpu + i * 4096) | 0x71;
	__sync_synchronize();
	(void)directory[0];
	regs[0x1520] = 1;
	(void)regs[0x1520];
	regs[0x575] = 0;
	regs[0x576] = 0;
	regs[0x577] = 0;
	regs[0x558] = 0;
	regs[0x560] = 511;
	regs[0x550] = (info.vram_gpu_base + offset) >> 12;
	regs[0x547] = regs[0x546];
	regs[0x50d] = 0; // retain the first fault; no interrupt handler yet
	regs[0x505] = regs[0x504] | 2; // two levels, 512 PTEs per page
	vmEnabled = true;
	regs[0x51e] = 2;
	bigtime_t deadline = system_time() + 100000;
	while ((regs[0x51f] & 2) == 0) {
		if (system_time() >= deadline)
			return B_TIMED_OUT;
		snooze(10);
	}
	uint32 select = regs[0x391];
	regs[0x391] = 1 << 4;
	regs[0x230d] = 1 << 5 | 3 << 8 | 3 << 3; // NC default, UC APE1, unaligned
	regs[0x230a] = 0;
	regs[0x230b] = 1;
	regs[0x230c] = 0;
	regs[0x391] = select;
	return B_OK;
}

void
GfxEngine::Snapshot(amdgpu_gfx_test& result)
{
	result.cp_control = regs[0x21b6];
	result.ring_control = regs[0x3041];
	result.rptr = regs[0x21c0];
	result.wptr = regs[0x3045];
	result.grbm_status = regs[0x2004];
	result.rlc_status = regs[0xec40];
	result.vm_fault_status = regs[0x536];
	result.vm_fault_address = regs[0x53e];
	result.vm_fault_client = regs[0x538];
	if ((result.vm_fault_status & 0xff) == 0 && (regs[0x537] & 0xff) != 0) {
		result.vm_fault_status = regs[0x537];
		result.vm_fault_address = regs[0x53f];
		result.vm_fault_client = regs[0x539];
	}
	dprintf("amdgpu: GFX snapshot stage %u VM %#x page %#x client %#x\n",
		(unsigned)result.stage, (unsigned)result.vm_fault_status,
		(unsigned)result.vm_fault_address, (unsigned)result.vm_fault_client);
	if (result.stage == 4) {
		const uint32 indexes[] = {0xc08d, 0xc092, 0xc093, 0xc0cc, 0xc0cd,
			0xc0ce, 0xc0cf, 0xc0d0, 0xc0d1, 0xc0c3, 0xc0c4, 0xc0c6,
			0xc0c7, 0xc0c8, 0xc0d2, 0xc0d3, 0xeca2, 0xeca3, 0xeca4};
		for (uint32 index : indexes)
			dprintf("amdgpu: GFX IB register %#x = %#x\n", (unsigned)index, (unsigned)regs[index]);
	}
}

status_t
GfxEngine::Test(volatile uint32* r, const amdgpu_info& info,
	const amdgpu::AtomVramReservation& reservation,
	const amdgpu::FirmwareView firmware[4], SdmaEngine& sdma, Gart& gart,
	amdgpu_gfx_test& result)
{
	regs = r;
	Snapshot(result);
	result.stage = 1;
	if (faulted || (attempted && !ready))
		return B_DEV_NOT_READY;
	if (!ready) {
		// BIOS leaves a placeholder base, even with RB_BUFSZ=0. Ownership
		// requires all three processors halted and an empty unconfigured ring.
		dprintf("amdgpu: GFX preflight CP %#x ring %#x base %#x/%#x ptr %u/%u\n",
			(unsigned)r[0x21b6], (unsigned)r[0x3041], (unsigned)r[0x30b1],
			(unsigned)r[0x3040], (unsigned)r[0x21c0], (unsigned)r[0x3045]);
		if (!amdgpu_smc_ready(r) || (r[0x2004] & 0x80000000) != 0
			|| (r[0x21b6] & kHalt) != kHalt || (r[0x3041] & 0x3f) != 0
			|| r[0x21c0] != 0 || r[0x3045] != 0
			|| !amdgpu_vram_range_is_safe(r, info, reservation, 40ULL << 20, 1ULL << 20))
			return B_NOT_ALLOWED;
		area = map_physical_memory("amdgpu GFX kernel ring", info.bar_address[0] + (40ULL << 20),
			1 << 20, B_ANY_KERNEL_ADDRESS, B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
			(void**)&memory);
		if (area < 0)
			return area;
		attempted = true;
		gpu = info.vram_gpu_base + (40ULL << 20);
		ring = gart.commandMemory;
		status_t status = InitializeVM(info, reservation, gart);
		if (status != B_OK) {
			faulted = true;
			return status;
		}
		wptr = sequence = 0;
		for (uint32 i = 0; i < (1 << 20) / 4; i++)
			memory[i] = 0;
		for (uint32 i = 0; i < 0x4000; i++)
			ring[i] = (i & 1) == 0 ? Packet(0x10, 0) : 0;
		result.stage = 2;
		Snapshot(result);
		r[0x306a] = 0; // no interrupts until the IH path exists
		r[0x21b6] |= kHalt;
		(void)r[0x21b6];
		snooze(50);
		for (const auto& value : kGolden)
			r[value[0]] = (r[value[0]] & ~value[1]) | value[2];
		r[0x2000] = (r[0x2000] & ~0xffu) | 0xff; // GRBM read timeout
		// The TC path used by indirect fetches needs the CP/shader memory
		// configuration from gfx_v8_0_constants_init, even before shaders.
		r[0x2581] = 1 | 1 << 1 | 3 << 3; // SH_STATIC_MEM_CONFIG
		uint32 select = r[0x391];
		r[0x391] = 0; // private VMID0
		r[0x230d] = 3 << 5 | 3 << 8 | 3 << 3; // UC types, unaligned access
		r[0x230a] = 0;
		r[0x230b] = 1;
		r[0x230c] = 0;
		r[0x391] = select;
		status = amdgpu_smc_load_gfx(r, firmware,
			sdma.memory + (1 << 20) / 4, sdma.gpu + (1 << 20));
		if (status != B_OK) {
			faulted = true;
			Snapshot(result);
			return status;
		}
		result.stage = 3;
		Snapshot(result);
		// Polaris10 does not use gfx_v8_0_init_pg's RLC save/restore setup.
		r[0xec00] &= ~1u;
		r[0x2008] |= 4;
		(void)r[0x2008];
		snooze(50);
		r[0x2008] &= ~4u;
		(void)r[0x2008];
		snooze(50);
		r[0xec00] |= 1;
		(void)r[0xec00];
		snooze(50);
		Snapshot(result);
		r[0x21c1] = 0;
		r[0x3051] = 0; // ring VMID0
		const uint32 control = 13 | 11 << 8 | 3 << 15 | 1 << 22;
		r[0x3041] = control | 0x80000000;
		r[0x3045] = 0;
		r[0x3043] = (uint32)(gpu + 0x30010);
		r[0x3044] = (gpu + 0x30010) >> 32;
		r[0x3046] = (uint32)(gpu + 0x30020);
		r[0x3047] = (gpu + 0x30020) >> 32;
		snooze(1000);
		r[0x3041] = control;
		r[0x3040] = Gart::kBase >> 8;
		r[0x30b1] = Gart::kBase >> 40;
		r[0x3059] &= ~0x10000000u;
		r[0x30ae] = 7;
		r[0x3050] = 0;
		r[0x304b] = 1;
		r[0x21b6] &= ~kHalt;
		(void)r[0x21b6];
		snooze(50);
		Snapshot(result);
		// Match the VI CP startup sequence before submitting any IB: disable
		// inherited shadow loads, define the hardware's full clear state,
		// select the Polaris raster layout, and initialize CE partitions.
		ring[wptr++] = Packet(0x4a, 0);
		ring[wptr++] = 2u << 28;
		ring[wptr++] = Packet(0x28, 1);
		ring[wptr++] = 0x80000000;
		ring[wptr++] = 0x80000000;
		for (const cs_extent_def* ext = vi_SECT_CONTEXT_defs; ext->extent != NULL; ext++) {
			ring[wptr++] = Packet(0x69, ext->reg_count);
			ring[wptr++] = ext->reg_index - 0xa000;
			for (uint32 i = 0; i < ext->reg_count; i++)
				ring[wptr++] = ext->extent[i];
		}
		ring[wptr++] = Packet(0x69, 2);
		ring[wptr++] = 0xd4;
		ring[wptr++] = 0x16000012;
		ring[wptr++] = 0x2a;
		ring[wptr++] = Packet(0x4a, 0);
		ring[wptr++] = 3u << 28;
		ring[wptr++] = Packet(0x12, 0);
		ring[wptr++] = 0;
		ring[wptr++] = Packet(0x11, 2);
		ring[wptr++] = 3;
		ring[wptr++] = 0x8000;
		ring[wptr++] = 0x8000;
		ready = true;
	}
	result.stage = 4;
	uint32 n = 0;
	volatile uint32* ib = gart.commandMemory + 65536 / 4;
	volatile uint32* data = memory + 0x20000 / 4;
	if (++sequence == 0)
		sequence++;
	result.sequence = sequence;
	const bool direct = sequence <= kDirectSequences;
	const bool minimalIB = sequence == kDirectSequences + 1;
	uint64 destination = direct || minimalIB ? gpu : kMemoryVA;
	for (uint32 i = 0; i < 3072; i++)
		data[(int32)i - 1024] = 0xabcddcba;
	ib[n++] = Packet(0x37, 1026); // WRITE_DATA, 1024 payload DWORDs
	ib[n++] = 5 << 8 | 1 << 20; // memory, write confirmation
	ib[n++] = (uint32)(destination + 0x20000);
	ib[n++] = (destination + 0x20000) >> 32;
	for (uint32 i = 0; i < 1024; i++)
		ib[n++] = 0x71324589 ^ (i * 0x10204081u) ^ sequence;
	// A confirmed memory write in the same IB follows all payload writes.
	memory[0x30000 / 4] = 0;
	ib[n++] = Packet(0x37, 3);
	ib[n++] = 5 << 8 | 1 << 20;
	ib[n++] = (uint32)(destination + 0x30000);
	ib[n++] = (destination + 0x30000) >> 32;
	ib[n++] = sequence;
	// Like Mesa, use one counted NOP for the padding block. Keep this
	// diagnostic independent of the header-only (-1 count) NOP encoding.
	uint32 padding = (-n) & 255;
	if (padding == 1)
		padding += 256;
	if (padding != 0) {
		ib[n++] = Packet(0x10, padding - 2);
		for (uint32 i = 1; i < padding; i++)
			ib[n++] = 0;
	}
	auto emit = [&](uint32 word) { ring[wptr++ & 0x3fff] = word; };
	// Invalidate caches after CPU updates and synchronize PFP before IB reads.
	// gfx_v8_0_emit_mem_sync uses this full-range VI cache operation.
	emit(Packet(0x43, 3));
	emit(1 << 22 | 1 << 23 | 1 << 27 | 1 << 29 | 1 << 18);
	emit(0xffffffff);
	emit(0);
	emit(10);
	emit(Packet(0x42, 0)); // PFP_SYNC_ME
	emit(0);
	if (direct || minimalIB) {
		// The minimal IB obtains its completion marker exclusively from the
		// private VM's five-DWORD IB. Only its bulk payload runs directly.
		uint32 directLength = minimalIB ? 1028 : n;
		for (uint32 i = 0; i < directLength; i++)
			emit(ib[i]);
	}
	// First exercise direct ring packets, then the indirect-buffer fetch path.
	// Both streams and all addresses are private to the kernel.
	if (!direct) {
		uint64 address = kCommandVA;
		uint32 length = n;
		if (minimalIB) {
			// Match the unpadded WRITE_DATA used by Linux's GFX8 ring test.
			// This also proves that the IB actually writes through VMID1.
			address += 0x8000;
			length = 5;
			ib[0x8000 / 4] = Packet(0x37, 3);
			ib[0x8000 / 4 + 1] = 5 << 8 | 1 << 20;
			ib[0x8000 / 4 + 2] = kMemoryVA + 0x30000;
			ib[0x8000 / 4 + 3] = 0;
			ib[0x8000 / 4 + 4] = sequence;
		}
		ring[wptr++ & 0x3fff] = Packet(0x3f, 2);
		ring[wptr++ & 0x3fff] = (uint32)address;
		ring[wptr++ & 0x3fff] = address >> 32;
		ring[wptr++ & 0x3fff] = length | 1 << 24; // private VMID1
	}
	// VI requires a dummy EOP followed by the real event. This fence is
	// outside the IB and covers its return plus cache writeback/invalidation.
	memory[0x30004 / 4] = 0;
	for (uint32 value = 0; value < 2; value++) {
		emit(Packet(0x47, 4));
		emit(0x14 | 5 << 8 | 1 << 15 | 1 << 16 | 1 << 17);
		emit((uint32)(gpu + 0x30004));
		emit((gpu + 0x30004) >> 32 | 1 << 29);
		emit(value == 0 ? sequence - 1 : sequence);
		emit(0);
	}
	padding = (-wptr) & 255;
	if (padding == 1)
		padding += 256;
	if (padding != 0) {
		emit(Packet(0x10, padding - 2));
		for (uint32 i = 1; i < padding; i++)
			emit(0);
	}
	__sync_synchronize();
	(void)ring[(wptr - 1) & 0x3fff];
	r[0x1520] = 1;
	(void)r[0x1520];
	r[0x3045] = wptr & 0x3fff;
	(void)r[0x3045];
	bigtime_t deadline = system_time() + 500000;
	while ((memory[0x30000 / 4] != sequence || memory[0x30004 / 4] != sequence
		|| r[0x21c0] != (wptr & 0x3fff)) && system_time() < deadline)
		snooze(50);
	__sync_synchronize();
	status_t status = memory[0x30000 / 4] == sequence
		&& memory[0x30004 / 4] == sequence && r[0x21c0] == (wptr & 0x3fff)
		? B_OK : B_TIMED_OUT;
	if (status == B_OK) {
		for (uint32 i = 0; i < 3072; i++) {
			uint32 expected = i >= 1024 && i < 2048
				? 0x71324589 ^ ((i - 1024) * 0x10204081u) ^ sequence : 0xabcddcba;
			if (data[(int32)i - 1024] != expected)
				result.mismatches++;
		}
		result.checked_bytes = 12288;
		if (result.mismatches != 0 || ((r[0x536] | r[0x537]) & 0xff) != 0)
			status = B_BAD_DATA;
	}
	Snapshot(result);
	if (status != B_OK) {
		const uint32 registers[] = {0x208d, 0x21c2, 0x3043, 0x3044, 0x3046,
			0x3047, 0x3061, 0x3066, 0x230a, 0x230b, 0x230c, 0x230d,
			0x500, 0x501, 0x502, 0x578, 0x504, 0x50c, 0x54f, 0x546};
		for (uint32 index : registers)
			dprintf("amdgpu: GFX fault register %#x = %#x\n", (unsigned)index, (unsigned)r[index]);
		faulted = true;
		r[0x21b6] |= kHalt;
		(void)r[0x21b6];
	} else
		result.stage = 5;
	dprintf("amdgpu: GFX stage %u status %#x seq %u ring %u/%u GRBM %#x RLC %#x\n",
		(unsigned)result.stage, (unsigned)status, (unsigned)sequence,
		(unsigned)result.rptr, (unsigned)result.wptr,
		(unsigned)result.grbm_status, (unsigned)result.rlc_status);
	return status;
}

void
GfxEngine::Uninitialize()
{
	if (!attempted)
		return;
	regs[0x21b6] |= kHalt;
	(void)regs[0x21b6];
	regs[0xec00] &= ~1u;
	if (vmEnabled) {
		regs[0x505] &= ~1u;
		(void)regs[0x505];
		regs[0x51e] = 2;
		(void)regs[0x51f];
		vmEnabled = false;
	}
	if (vmArea >= 0)
		delete_area(vmArea);
	vmArea = -1;
	delete_area(area);
	ready = false;
	area = -1;
}
