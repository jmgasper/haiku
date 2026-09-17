/*
 * OS interface functions that the RM uses when it runs on the CPU (without
 * GSP firmware), as it does for pre-Turing GPUs with the proprietary RM core.
 */

#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>
#include <OS.h>
#include <lock.h>
#include <util/AutoLock.h>

#include "nv-include.h"
extern "C" {
#include <os-interface.h>
}

#include "Driver.h"
#include "Device.h"
#include "RmStack.h"

#include <arch/x86/arch_cpuasm.h>


// #pragma mark - work queue


/*
 * A single worker thread runs RM work items. Items may be queued from
 * interrupt context, so they are kept in a fixed-size ring buffer protected
 * by a spinlock and the worker is woken with a semaphore.
 */
class RmWorkQueue {
public:
	status_t Init()
	{
		fSemaphore = create_sem(0, "nvidia_rm work items");
		if (fSemaphore < B_OK)
			return fSemaphore;

		fThread = spawn_kernel_thread(_Worker, "nvidia_rm worker",
			B_URGENT_DISPLAY_PRIORITY, this);
		if (fThread < B_OK) {
			delete_sem(fSemaphore);
			return fThread;
		}
		resume_thread(fThread);
		return B_OK;
	}

	void Uninit()
	{
		if (fThread < B_OK)
			return;
		fQuit = true;
		release_sem(fSemaphore);
		status_t result;
		wait_for_thread(fThread, &result);
		delete_sem(fSemaphore);
		fThread = -1;
	}

	NV_STATUS Queue(void* data)
	{
		{
			InterruptsSpinLocker locker(fLock);
			if (fCount == kCapacity)
				return NV_ERR_NO_MEMORY;
			fItems[(fHead + fCount) % kCapacity] = data;
			fCount++;
			fPending++;
		}
		release_sem_etc(fSemaphore, 1, B_DO_NOT_RESCHEDULE);
		return NV_OK;
	}

	void Flush(bool isUnload)
	{
		fFlushOngoing = isUnload;
		for (;;) {
			{
				InterruptsSpinLocker locker(fLock);
				if (fPending == 0)
					break;
			}
			snooze(1000);
		}
		fFlushOngoing = false;
	}

	bool IsFlushOngoing() const
	{
		return fFlushOngoing;
	}

private:
	static status_t _Worker(void* arg)
	{
		return static_cast<RmWorkQueue*>(arg)->_Work();
	}

	status_t _Work()
	{
		RmStack stack;
		while (acquire_sem(fSemaphore) == B_OK && !fQuit) {
			void* data;
			{
				InterruptsSpinLocker locker(fLock);
				if (fCount == 0)
					continue;
				data = fItems[fHead];
				fHead = (fHead + 1) % kCapacity;
				fCount--;
			}

			rm_execute_work_item(stack.Get(), data);

			InterruptsSpinLocker locker(fLock);
			fPending--;
		}
		return B_OK;
	}

private:
	static constexpr uint32 kCapacity = 4096;

	spinlock fLock = B_SPINLOCK_INITIALIZER;
	void* fItems[kCapacity] {};
	uint32 fHead = 0;
	uint32 fCount = 0;
	uint32 fPending = 0;
	sem_id fSemaphore = -1;
	thread_id fThread = -1;
	bool fQuit = false;
	bool fFlushOngoing = false;
};


static RmWorkQueue sWorkQueue;


status_t
nv_haiku_work_queue_init()
{
	return sWorkQueue.Init();
}


void
nv_haiku_work_queue_uninit()
{
	sWorkQueue.Uninit();
}


NV_STATUS NV_API_CALL
os_queue_work_item(struct os_work_queue *queue, void *data)
{
	// all GPUs share one queue
	return sWorkQueue.Queue(data);
}


NV_STATUS NV_API_CALL
os_flush_work_queue(struct os_work_queue *queue, NvBool is_unload)
{
	if (!are_interrupts_enabled())
		return NV_ERR_ILLEGAL_ACTION;
	sWorkQueue.Flush(is_unload);
	return NV_OK;
}


NvBool NV_API_CALL
os_is_queue_flush_ongoing(struct os_work_queue *queue)
{
	return sWorkQueue.IsFlushOngoing();
}


// #pragma mark - legacy VGA I/O ports


extern "C" {


void NV_API_CALL
os_io_write_byte(NvU32 address, NvU8 value)
{
	out8(value, address);
}


void NV_API_CALL
os_io_write_word(NvU32 address, NvU16 value)
{
	out16(value, address);
}


NvU8 NV_API_CALL
os_io_read_byte(NvU32 address)
{
	return in8(address);
}


NvU16 NV_API_CALL
os_io_read_word(NvU32 address)
{
	return in16(address);
}


// #pragma mark - miscellaneous


NvBool os_dma_buf_enabled = NV_FALSE;


NvS32 NV_API_CALL
os_string_compare(const char *str1, const char *str2)
{
	return strcmp(str1, str2);
}


char* NV_API_CALL
os_string_copy(char *dst, const char *src)
{
	return strcpy(dst, src);
}


NvU32 NV_API_CALL
os_string_length(const char* str)
{
	return strlen(str);
}


NvU32 NV_API_CALL
os_strtoul(const char *str, char **endp, NvU32 base)
{
	return (NvU32)strtoul(str, endp, base);
}


NV_STATUS NV_API_CALL
os_get_random_bytes(NvU8 *bytes, NvU16 numBytes)
{
	static uint64 sState = 0;
	if (sState == 0)
		sState = system_time() ^ 0x9e3779b97f4a7c15ULL;

	for (NvU16 i = 0; i < numBytes; i++) {
		sState ^= sState << 13;
		sState ^= sState >> 7;
		sState ^= sState << 17;
		bytes[i] = (NvU8)(sState ^ (system_time() >> 3));
	}
	return NV_OK;
}


NV_STATUS NV_API_CALL
os_get_version_info(os_version_info *pOsVersionInfo)
{
	pOsVersionInfo->os_major_version = 1;
	pOsVersionInfo->os_minor_version = 0;
	pOsVersionInfo->os_build_number = 0;
	pOsVersionInfo->os_build_version_str = "Haiku";
	pOsVersionInfo->os_build_date_plus_str = __DATE__;
	return NV_OK;
}


NV_STATUS NV_API_CALL
os_get_smbios_header(NvU64 *pSmbsAddr)
{
	return NV_ERR_NOT_SUPPORTED;
}


void NV_API_CALL
os_dbg_set_level(NvU32 new_debuglevel)
{
}


void NV_API_CALL
os_dump_stack(void)
{
	dprintf("nvidia_rm: os_dump_stack()\n");
}


NvBool NV_API_CALL
os_pci_remove_supported(void)
{
	return NV_FALSE;
}


NV_STATUS NV_API_CALL
os_flush_user_cache(void)
{
	return NV_OK;
}


NV_STATUS NV_API_CALL
os_numa_memblock_size(NvU64 *memblock_size)
{
	return NV_ERR_INVALID_STATE;
}


NvU32 NV_API_CALL
os_count_tail_pages(NvU64 address)
{
	return 1;
}


nv_state_t* NV_API_CALL
nv_get_adapter_state(NvU32 domain, NvU8 bus, NvU8 slot)
{
	NvHaikuDriver& driver = NvHaikuDriver::Instance();
	for (uint32 i = 0; i < driver.DeviceCount(); i++) {
		nv_state_t* nv = driver.DeviceAt(i)->Nv();
		if (nv->pci_info.domain == domain && nv->pci_info.bus == bus
			&& nv->pci_info.slot == slot)
			return nv;
	}
	return NULL;
}


}
