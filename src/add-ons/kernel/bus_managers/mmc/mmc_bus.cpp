/*
 * Copyright 2018-2020 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * Authors:
 *		B Krishnan Iyer, krishnaniyer97@gmail.com
 */
#include "mmc_bus.h"

#include <Errors.h>

#include <stdint.h>


MMCBus::MMCBus(device_node* node)
	:
	fNode(node),
	fController(NULL),
	fCookie(NULL),
	fStatus(B_OK),
	fWorkerThread(-1),
	fScanSemaphore(-1),
	fLockSemaphore(-1),
	fActiveDevice(0),
	fCardType(CARD_TYPE_UNKNOWN)
{
	CALLED();

	// Get the parent info, it includes the API to send commands to the hardware
	device_node* parent = gDeviceManager->get_parent_node(node);
	fStatus = gDeviceManager->get_driver(parent,
		(driver_module_info**)&fController, &fCookie);
	gDeviceManager->put_node(parent);

	if (fStatus != B_OK) {
		ERROR("Not able to establish the bus %s\n",
			strerror(fStatus));
		return;
	}

	fScanSemaphore = create_sem(0, "MMC bus scan");
	if (fScanSemaphore < B_OK) {
		fStatus = fScanSemaphore;
		return;
	}
	fLockSemaphore = create_sem(1, "MMC bus lock");
	if (fLockSemaphore < B_OK) {
		fStatus = fLockSemaphore;
		return;
	}
	fWorkerThread = spawn_kernel_thread(_WorkerThread, "SD bus controller",
		B_NORMAL_PRIORITY, this);
	if (fWorkerThread < B_OK) {
		fStatus = fWorkerThread;
		return;
	}
	fStatus = resume_thread(fWorkerThread);
	if (fStatus != B_OK) {
		kill_thread(fWorkerThread);
		fWorkerThread = -1;
		return;
	}

	fController->set_scan_semaphore(fCookie, fScanSemaphore);
}


MMCBus::~MMCBus()
{
	CALLED();

	// Tell the worker thread we want to stop
	fStatus = B_SHUTTING_DOWN;

	if (fController != NULL)
		fController->set_scan_semaphore(fCookie, -1);
	// Wake the scanner, keeping its bus lock alive until it has exited.
	if (fScanSemaphore >= B_OK)
		delete_sem(fScanSemaphore);

	// Wait for the worker thread to terminate
	status_t result;
	if (fWorkerThread >= B_OK)
		wait_for_thread(fWorkerThread, &result);
	if (fLockSemaphore >= B_OK)
		delete_sem(fLockSemaphore);
}


status_t
MMCBus::InitCheck()
{
	return fStatus;
}


void
MMCBus::Rescan()
{
	// Just wake up the thread for a scan
	release_sem(fScanSemaphore);
}


status_t
MMCBus::ExecuteCommand(uint16_t rca, uint8_t command, uint32_t argument,
	uint32_t* response)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->execute_command(fCookie, command, argument, response);
}


status_t
MMCBus::DoIO(uint16_t rca, uint8_t command, IOOperation* operation,
	bool offsetAsSectors)
{
	status_t status = _ActivateDevice(rca);
	if (status != B_OK)
		return status;
	return fController->do_io(fCookie, command, operation, offsetAsSectors);
}


status_t
MMCBus::SetClock(int frequency)
{
	return fController->set_clock(fCookie, frequency);
}


void
MMCBus::SetBusWidth(int width)
{
	fController->set_bus_width(fCookie, width);
}


void
MMCBus::SetCardType(card_type type)
{
	fCardType = type;
	fController->set_card_type(fCookie, type);
}


status_t
MMCBus::_ActivateDevice(uint16_t rca)
{
	// Do nothing if the device is already activated
	if (fActiveDevice == rca)
		return B_OK;

	uint32_t response;
	status_t result;
	result = fController->execute_command(fCookie, SELECT_DESELECT_CARD, ((uint32)rca) << 16,
		&response);

	if (result == B_OK && rca != 0 && (response & kMmcR1ErrorMask) != 0)
		result = B_IO_ERROR;
	if (result == B_OK)
		fActiveDevice = rca;

	return result;
}


void MMCBus::_AcquireScanSemaphore()
{
	ReleaseBus();
	acquire_sem(fScanSemaphore);
	AcquireBus();
}


void
MMCBus::_TerminateBus()
{
	fController->terminate_bus(fCookie);
}


status_t
MMCBus::_WorkerThread(void* cookie)
{
	MMCBus* bus = (MMCBus*)cookie;
	uint32_t response;

	bus->AcquireBus();

	// Reset all cards on the bus
	// This does not work if the bus has not been powered on yet (the command
	// will timeout), in that case we wait until asked to scan again when a
	// card has been inserted and powered on.
	status_t result;
	do {
		bus->_AcquireScanSemaphore();

		// Check if we need to exit early (possible if the parent device did
		// not manage initialize itself correctly)
		if (bus->fStatus == B_SHUTTING_DOWN) {
			bus->ReleaseBus();
			return B_OK;
		}

		TRACE("Reset the bus...\n");
		// Card power and clock stabilization run in this worker, not in
		// the host controller's card-insertion interrupt handler.
		result = bus->SetClock(400);
		if (result != B_OK) {
			bus->_TerminateBus();
			bus->ReleaseBus();
			return result;
		}
		bus->SetBusWidth(1);
		result = bus->ExecuteCommand(0, GO_IDLE_STATE, 0, NULL);
		TRACE("CMD0 result: %s\n", strerror(result));
	} while (result != B_OK);

	// Need to wait at least 8 clock cycles after CMD0 before sending the next
	// command. With the default 400kHz clock that would be 20 microseconds,
	// but we need to wait at least 20ms here, otherwise the next command times
	// out
	snooze(30000);

	while (bus->fStatus != B_SHUTTING_DOWN) {
		TRACE("Scanning the bus\n");

		// Use the low speed clock and 1bit bus width for scanning
		status_t clockStatus = bus->SetClock(400);
		if (clockStatus != B_OK) {
			bus->_TerminateBus();
			bus->ReleaseBus();
			return clockStatus;
		}
		bus->SetBusWidth(1);
		bus->SetCardType(CARD_TYPE_UNKNOWN);

		// Probe the voltage range
		enum {
			// Table 4-40 in physical layer specification v8.00
			// All other values are currently reserved
			HOST_27_36V = 1, // Host supplied voltage 2.7-3.6V
		};

		// An arbitrary value, we just need to check that the response
		// containts the same.
		static const uint8 kVoltageCheckPattern = 0xAA;
		uint8_t cardType = CARD_TYPE_SD;
		// FIXME MMC cards will not reply to this! They expect CMD1 instead
		// SD v1 cards will also not reply, but we can proceed to ACMD41
		// If ACMD41 also does not work, it may be an SDIO card, too
		uint32_t probe = (HOST_27_36V << 8) | kVoltageCheckPattern;
		uint32_t hcs = 1 << 30;
		uint32_t ocr = 0;
		status_t status = bus->ExecuteCommand(0, SD_SEND_IF_COND, probe, &response);
		if (status != B_OK) {
			TRACE("Card does not implement CMD8, may be a V1 SD or MMC card\n");
			// Do not check for SDHC support in this case
			hcs = 0;

			TRACE("Trying MMC CMD1 initialization...\n");
			bigtime_t deadline = system_time() + 2000000;
			do {
				status = bus->ExecuteCommand(0, MMC_SEND_OP_COND, 0x40FF8000, &ocr);
				// Request sector addressing; the reply determines the actual mode.
				if (status != B_OK) {
					TRACE("MMC CMD1 failed\n");
					break;
				}
				if ((ocr & (1 << 31)) == 0) {
					TRACE("MMC card is busy\n");
					snooze(100000);
				}
			} while ((ocr & (1 << 31)) == 0 && system_time() < deadline);

			if (status == B_OK && (ocr & (1 << 31)) != 0) {
				TRACE("Detected MMC card after CMD1\n");
				if ((ocr & (1 << 30)) != 0)
					cardType = CARD_TYPE_MMC_EXTENDED_CAPACITY;
				else
					cardType = CARD_TYPE_MMC;
			}
		} else if (response != probe) {
			ERROR("Card does not support voltage range (expected %x, "
				"reply %x)\n", probe, response);
			bus->_TerminateBus();
			bus->ReleaseBus();
			return B_ERROR;
		}

		// Probe OCR, waiting for card to become ready
		// We keep repeating ACMD41 until the card replies that it is
		// initialized. For MMC we already probed using CMD1 above.
		if ((cardType != CARD_TYPE_MMC) && (cardType != CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			bigtime_t deadline = system_time() + 2000000;
			ocr = 0;
			do {
				uint32_t cardStatus = 0;
				status = bus->ExecuteCommand(0, SD_APP_CMD, 0, &cardStatus);
				if (status != B_OK)
					break;
				if ((cardStatus & 0xfff9a000) != 0 || (cardStatus & (1 << 5)) == 0) {
					status = B_BAD_DATA;
					break;
				}
				status = bus->ExecuteCommand(0, SD_SEND_OP_COND, hcs | 0xFF8000, &ocr);
				if (status != B_OK)
					break;

				if ((ocr & (1 << 31)) == 0) {
					TRACE("Card is busy\n");
					snooze(100000);
				}
			} while ((ocr & (1 << 31)) == 0 && system_time() < deadline);
			if (status != B_OK || (ocr & (1 << 31)) == 0) {
				ERROR("Card initialization did not complete: %s\n", strerror(status));
				bus->_TerminateBus();
				bus->ReleaseBus();
				return status == B_OK ? B_TIMED_OUT : status;
			}
		}

		// FIXME this should be asked to each card, when there are multiple
		// ones. So ACMD41 should be moved inside the probing loop below?
		if (cardType == CARD_TYPE_SD) {
			if ((ocr & hcs) != 0)
				cardType = CARD_TYPE_SDHC;
			if ((ocr & (1 << 29)) != 0)
				cardType = CARD_TYPE_UHS2;
			if ((ocr & (1 << 24)) != 0)
				TRACE("Card supports 1.8v");
		}
		TRACE("Voltage range: %x\n", ocr & 0xFFFFFF);

		// Set the card type so the next commands have the correct reply types
		// (MMC and SD commands with the same identifier sometime expect different responses)
		bus->SetCardType((card_type)cardType);

		// TODO send CMD11 to switch to low voltage mode if card supports it?

		// We use CMD2 (ALL_SEND_CID) and CMD3 (SEND_RELATIVE_ADDR) to assign
		// an RCA to all cards. Initially all cards have an RCA of 0 and will
		// all receive CMD2. But only one of them will reply (they do collision
		// detection while sending the CID in reply). We assign a new RCA to
		// that first card, and repeat the process with the remaining ones
		// until no one answers to CMD2. Then we know all cards have an RCA
		// (and a matching published device on our side).
		uint32_t cid[4];
		uint32_t vendor;
		char name[7];
		uint32_t serial;
		uint16_t revision;
		uint8_t month;
		uint16_t year;
		uint16_t rca;
		bool cardFound = false;
		// This being an if statement as opposed to a while statement restricts
		// it to one device per bus.
		if ((cardType == CARD_TYPE_MMC) || (cardType == CARD_TYPE_MMC_EXTENDED_CAPACITY)) {
			if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
				// We currently support only a single card, so use a fixed RCA.
				rca = 1;
				status
					= bus->ExecuteCommand(0, MMC_SET_RELATIVE_ADDR, ((uint32)rca) << 16, &response);
				TRACE("MMC RCA: %x Status: %x\n", rca, response & 0xFFFF);
				if (status != B_OK || (response & kMmcR1ErrorMask) != 0) {
					TRACE("Failed to set RCA for MMC card\n");
				} else {
					MMCCid mmcCid(cid);
					vendor = mmcCid.VendorID();
					mmcCid.ProductName(name);
					serial = mmcCid.ProductSerial();
					revision = mmcCid.ProductRevision();
					month = mmcCid.ManufactureMonth();
					year = mmcCid.ManufactureYear(true);
					cardFound = true;
				}
			}
		} else if (bus->ExecuteCommand(0, ALL_SEND_CID, 0, cid) == B_OK) {
			status = bus->ExecuteCommand(0, SD_SEND_RELATIVE_ADDR, 0, &response);

			TRACE("RCA: %x Status: %x\n", response >> 16, response & 0xFFFF);

			if (status != B_OK || (response & 0xFF00) != 0x500) {
				TRACE("Card did not enter data state\n");
				// This probably means there are no more cards to scan on the
				// bus, so exit the loop.
				break;
			}

			// The card now has an RCA and it entered the data phase, which
			// means our initializing job is over, we can pass it on to the
			// mmc_disk driver.
			rca = response >> 16;
			SDCid sdCid(cid);
			vendor = sdCid.VendorID();
			sdCid.ProductName(name);
			serial = sdCid.ProductSerial();
			revision = sdCid.ProductRevision();
			month = sdCid.ManufactureMonth();
			year = sdCid.ManufactureYear();
			cardFound = true;
		}

		uint32_t sectorCount = 0;
		uint8_t cacheEnabled = 0;
		if (cardFound && is_mmc_card((card_type)cardType)) {
			uint8_t extended[512];
			status = bus->_ActivateDevice(rca);
			if (status == B_OK)
				status = bus->fController->read_extended_csd(bus->fCookie, extended);
			if (status == B_OK && (extended[192] < 2
					|| (extended[179] & 7) != 0 || extended[61] != 0))
				status = B_NOT_SUPPORTED;
			if (status == B_OK) {
				sectorCount = mmc_ext_csd_sector_count(extended);
				if (sectorCount == 0)
					status = B_BAD_DATA;
			}
			if (status != B_OK) {
				ERROR("MMC user-area discovery failed: %s\n", strerror(status));
				cardFound = false;
			} else {
				cacheEnabled = extended[33] & 1;
				year = MMCCid(cid).ManufactureYear(extended[192] > 4);
				TRACE_ALWAYS("MMC EXT_CSD: revision %u, sectors %" B_PRIu32
					", cache enabled %u\n", extended[192], sectorCount, cacheEnabled);
			}
		}

		// Do not publish a disk whose operational clock could not be set.
		clockStatus = bus->SetClock(25000);
		if (clockStatus != B_OK) {
			bus->_TerminateBus();
			bus->ReleaseBus();
			return clockStatus;
		}

		if (cardFound) {
			device_attr attrs[] = {
				{ B_DEVICE_BUS, B_STRING_TYPE, {.string = "mmc" }},
				{ B_DEVICE_PRETTY_NAME, B_STRING_TYPE, {.string = "mmc device" }},
				{ B_DEVICE_VENDOR_ID, B_UINT32_TYPE, {.ui32 = vendor}},
				{ B_DEVICE_ID, B_STRING_TYPE, {.string = name}},
				{ B_DEVICE_UNIQUE_ID, B_UINT32_TYPE, {.ui32 = serial}},
				{ "mmc/revision", B_UINT16_TYPE, {.ui16 = revision}},
				{ "mmc/month", B_UINT8_TYPE, {.ui8 = month}},
				{ "mmc/year", B_UINT16_TYPE, {.ui16 = year}},
				{ kMmcRcaAttribute, B_UINT16_TYPE, {.ui16 = rca}},
				{ kMmcTypeAttribute, B_UINT8_TYPE, {.ui8 = cardType}},
				{ kMmcSectorCountAttribute, B_UINT32_TYPE, {.ui32 = sectorCount}},
				{ kMmcCacheEnabledAttribute, B_UINT8_TYPE, {.ui8 = cacheEnabled}},
				{}
			};

			// publish child device for the card
			gDeviceManager->register_node(bus->fNode, MMC_BUS_MODULE_NAME,
				attrs, NULL, NULL);
		}

		// TODO if there is a single card active, check if it supports CMD6
		// (spec version 1.10 or later in SCR). If it does, check if CMD6 can
		// enable high speed mode, use that to go to 50MHz instead of 25.

		// FIXME we also need to unpublish devices that are gone. Probably need
		// to "ping" all RCAs somehow? Or is there an interrupt we can look for
		// to detect added/removed cards?

		// Wait for the next scan request
		// The thread will spend most of its time waiting here
		bus->_AcquireScanSemaphore();
	}

	bus->ReleaseBus();

	TRACE("poller thread terminating");
	return B_OK;
}
