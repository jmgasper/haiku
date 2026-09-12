/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef ARCH_ARM64_GICV3_ITS_H
#define ARCH_ARM64_GICV3_ITS_H

#include <arch/arm64/gicv3_its_commands.h>
#include <arch/generic/msi.h>
#include <lock.h>

// Initial RK3588 firmware profile: ITS1, Samsung 01:00.0 and CPU 0 only.
// Tables and reservations live for the kernel lifetime, including on failure.
class GICv3Its : public MSIInterface {
public:
	status_t Init(volatile uint8* redistributors, uint32 count, bool trace,
		bool forceHighTables);
	status_t AllocateVectors(uint32, uint32&, uint64&, uint32&) override
		{ return B_NOT_SUPPORTED; }
	status_t AllocateVectorsForDevice(const msi_requester* requester, uint32 count,
		uint32& startVector, uint64& address, uint32& data) override;
	void FreeVectors(uint32 count, uint32 startVector) override;
	void SetEnabled(uint32 vector, bool enabled);
	void TraceInterrupt(uint32 vector);
	bool Contains(uint32 vector) const
		{ return fReady && vector >= Gicv3Its::kFirstLpi
			&& vector < Gicv3Its::kFirstLpi + Gicv3Its::kEventCount; }

private:
	struct Memory {
		area_id area = -1;
		void* address = nullptr;
		phys_addr_t physical = 0;
		size_t size = 0;
		status_t Allocate(const char* name, size_t bytes, size_t alignment,
			phys_addr_t minimumAddress);
	};
	status_t _PrepareTables();
	bool _WriteChecked(size_t offset, uint64 value);
	bool _Submit(const Gicv3Its::Command& command);
	bool _WaitReadOffset(uint32 offset);
	uint32 _Read32(size_t offset) const;
	uint64 _Read64(size_t offset) const;
	void _Write32(size_t offset, uint32 value);
	void _Write64(size_t offset, uint64 value);
	void _Quarantine(const char* reason);

	volatile uint8* fBase = nullptr;
	volatile uint8* fRedistributors = nullptr;
	Memory fDevices;
	Memory fCollections;
	Memory fCommands;
	Memory fProperties;
	Memory fPending;
	Memory fInterrupts;
	uint64 fDeviceBaser = 0;
	uint64 fCollectionBaser = 0;
	uint64 fTarget = 0;
	phys_addr_t fMinimumTableAddress = 0;
	uint32 fWriteOffset = 0;
	uint32 fCount = 0;
	uint32 fEnabledMask = 0;
	int32 fInterruptCounts[Gicv3Its::kEventCount]{};
	bool fReady = false;
	bool fFaulted = false;
	bool fTrace = false;
	spinlock fLock = B_SPINLOCK_INITIALIZER;
};

#endif
