/* Copyright 2026, Haiku, Inc. Distributed under the MIT License. */
// Independent firmware/MMU ordering around the production QueueEngine. The
// separate client fixture walks production heap page tables and their lifetime.
enum HeapEngineCase {
	HeapEngineOK, HeapEngineNoMemory, HeapEngineBadAddress, HeapEngineBadCounters,
	HeapEngineLockTimeout, HeapEngineCommitFailure, HeapEngineFlushTimeout,
	HeapEngineUnlockTimeout
};

struct HeapEngineModel : QueueModel {
	HeapEngineCase trial;
	Guarded heapMemory{37 * 4096};
	uint64_t heapPhysical = UINT64_C(0x480000000);
	bool injected = false, awaiting = false, published = false, prepared = false;
	unsigned locks = 0, commits = 0, flushes = 0, unlocks = 0, replies = 0, aborts = 0;
	HeapEngineModel(FirmwareMemory& memory, PhysicalInventory& physical,
		TestQueue& queue, HeapEngineCase value)
		: QueueModel(memory, physical, QueueNone), trial(value)
	{
		memset(heapMemory.data, 0, heapMemory.size);
		inventory.Add(heapMemory, heapPhysical);
		queue.first.Root()[257] = heapPhysical | 3;
		((uint64_t*)heapMemory.data)[0] = (heapPhysical + 4096) | 3;
		((uint64_t*)(heapMemory.data + 4096))[0] = (heapPhysical + 8192) | 3;
		((uint64_t*)(heapMemory.data + 4096))[1] = (heapPhysical + 12288) | 3;
		((uint64_t*)(heapMemory.data + 8192))[0] = (heapPhysical + 16384) | (UINT64_C(3) << 53) | 0x747;
	}
	void WriteGpu(uint32_t offset, uint32_t value)
	{
		if (awaiting && groupRunning && offset == 0x2458) {
			assert(value == 2 || value == 3);
			if (value == 2) {
				assert(prepared && !published && ReadGpu64(*this, 0x2450) == 47);
				locks++; userLocked = true;
				if (trial == HeapEngineLockTimeout) regs[0x2468] = 1;
			} else {
				assert(userLocked && (commits == 1 || trial == HeapEngineCommitFailure));
				unlocks++;
				if (trial == HeapEngineUnlockTimeout) regs[0x2468] = 1;
				else userLocked = false;
			}
			regs[offset] = value; return;
		}
		if (awaiting && groupRunning && offset == kGpuCommand && userLocked) {
			assert(value == 0x3304 && published && commits == 1 && locks == 1);
			flushes++;
			if (trial != HeapEngineFlushTimeout) regs[kGpuRaw] |= 1u << 17;
			return;
		}
		QueueModel::WriteGpu(offset, value);
	}
	void PauseReset()
	{
		if (configured && pending && groupRunning && !injected && Queue()[0] > Queue()[512]) {
			assert(active == 0 && userMapped && !userLocked);
			injected = awaiting = true; pending = false;
			uint32_t* cso = In(1042);
			cso[0] ^= 1u << 26; In(1026)[3] ^= 1;
			cso[48] = 2; cso[49] = trial == HeapEngineBadCounters ? 2 : 1; cso[51] = 0;
			WriteInterface64(cso, 208, kHeapBase + (trial == HeapEngineBadAddress ? 32 : 0));
			Event(true); elapsed += 100;
			return;
		}
		if (awaiting && pending && groupRunning && ((In(1041)[0] ^ In(1042)[0]) & (1u << 26)) == 0) {
			assert(!prepared && !userLocked && ReadGpu(0x2468) == 0);
			uint64_t chunk = ReadInterface64(In(1041), 32);
			assert(chunk == ReadInterface64(In(1041), 40));
			if (trial == HeapEngineNoMemory) {
				assert(chunk == 0 && !published && locks == 0 && commits == 0 && unlocks == 0);
			} else {
				assert(trial == HeapEngineOK && chunk == ((kHeapBase + 2097152) | 32));
				assert(published && locks == 1 && commits == 1 && flushes == 1 && unlocks == 1);
				for (unsigned i = 0; i < 32; i++) {
					uint8_t* data = inventory.At(currentRoot, kHeapBase + 2097152 + i * 4096, 4096, true);
					assert(data == heapMemory.data + (5 + i) * 4096);
					for (unsigned j = 0; j < 4096; j++) assert(data[j] == 0);
				}
			}
			replies++; awaiting = false;
		}
		// Firmware remains blocked until the OOM ack. It may still consume group
		// termination during cleanup after a failed reply, without running work.
		if (awaiting && pending && groupRunning && (In(1025)[0] & 7) != 0) {
			pending = false; elapsed += 100; return;
		}
		QueueModel::PauseReset();
	}
};

struct HeapEngineFeed : TestFeed {
	HeapEngineModel& model;
	HeapEngineFeed(TestQueue& a, TestQueue& b, HeapEngineModel& value)
		: TestFeed(a, b, value), model(value) {}
	HeapGrowthResult PrepareHeap(unsigned slot, uint64_t address, uint32_t start,
		uint32_t end, uint32_t fragment, HeapGrowth& growth)
	{
		assert(slot == 0 && model.awaiting && !model.prepared && !model.userLocked);
		assert(((uint64_t*)(model.heapMemory.data + 12288))[0] == 0);
		if (address != kHeapBase || fragment > end || end >= start) return kHeapGrowthInvalid;
		if (model.trial == HeapEngineNoMemory) return kHeapGrowthNoMemory;
		model.prepared = true;
		growth = {this, kHeapBase + 2097152, 131072, (kHeapBase + 2097152) | 32};
		return kHeapGrowthOK;
	}
	bool CommitHeap(HeapGrowth& growth)
	{
		assert(growth.state == this && model.prepared && model.userLocked && model.locks == 1);
		if (model.trial == HeapEngineCommitFailure) return false;
		uint64_t* leaves = (uint64_t*)(model.heapMemory.data + 12288);
		for (unsigned i = 0; i < 32; i++) {
			assert(leaves[i] == 0);
			leaves[i] = (model.heapPhysical + (5 + i) * 4096) | (UINT64_C(3) << 53) | 0x747;
		}
		model.commits++; model.published = true; model.prepared = false; growth = {};
		return true;
	}
	void AbortHeap(HeapGrowth& growth)
	{
		if (growth.state != NULL) {
			assert(growth.state == this && model.prepared && !model.published);
			model.aborts++; model.prepared = false; growth = {};
		}
		// Published data remains owned even after a lost flush or unlock.
	}
};

static void CheckHeapEngine()
{
	auto bytes = container({{0x800000, 4096, 13}, {0x4000000, 65536, 0xc000001b}});
	FirmwareImage image;
	assert(image.Init(bytes.data(), bytes.size()) == FIRMWARE_OK);
	FirmwareMemory memory;
	assert(PlanRuntimeFirmware(memory, image));
	Guarded arena(memory.RequiredBytes());
	for (unsigned trial = HeapEngineOK; trial <= HeapEngineUnlockTimeout; trial++) {
		assert(memory.Build(arena.data, arena.size, UINT64_C(0x182300000)));
		PhysicalInventory inventory;
		TestQueue a(inventory, 0), b(inventory, 1);
		HeapEngineModel model(memory, inventory, a, HeapEngineCase(trial));
		HeapEngineFeed feed(a, b, model);
		QueueEngine<HeapEngineFeed> engine(feed);
		FirmwareRunInfo info{}; info.flags = kFirmwareAllocated;
		CycleFirmware(model, memory, info, engine);
		printf("heap-engine trial=%u events=%u grown=%u declined=%u replies=%u commits=%u aborts=%u jobs=%u engine=%u fw=%x\n",
			trial, engine.HeapEvents(), engine.HeapGrowths(), engine.HeapDeclines(), model.replies,
			model.commits, model.aborts, feed.complete, engine.Error(), info.flags);
		assert(!model.prepared && !model.handlers && engine.HeapEvents() >= 1);
		if (trial == HeapEngineOK || trial == HeapEngineNoMemory) {
			assert(engine.Error() == kQueueEngineOK && info.flags == 255 && model.replies == 1);
			assert(engine.HeapEvents() == 1 && engine.HeapGrowths() == unsigned(trial == HeapEngineOK));
			assert(engine.HeapDeclines() == unsigned(trial == HeapEngineNoMemory));
			assert(feed.complete == 526 && !engine.Mapped() && !model.userMapped);
		} else {
			assert(engine.Error() != kQueueEngineOK && (info.flags & kFirmwareNeedsRecovery));
			assert(model.replies == 0 && feed.complete == 0 && engine.HeapGrowths() == 0);
			assert(model.commits == unsigned(trial == HeapEngineFlushTimeout || trial == HeapEngineUnlockTimeout));
			assert(model.aborts == unsigned(trial == HeapEngineLockTimeout || trial == HeapEngineCommitFailure));
		}
	}
	puts("MALI_CSF_HEAP_ENGINE_TEST_PASS");
}
