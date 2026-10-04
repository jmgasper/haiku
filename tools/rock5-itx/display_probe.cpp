/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

// Read-only ROCK 5 ITX display observation. Prints the admitted resource
// description and three sequential register snapshots of VOP2, HDMI TX1,
// the HDPTX PHY GRF and the clock/power controllers. Nothing is written.

#include <OS.h>
#include <graphic_driver.h>

#include "DisplayEdid.h"
#include "DisplayScanout.h"
#include "DisplayAccelerant.h"
#include "DisplayModeSet.h"
#include "DisplayCursor.h"
#include "DisplayPort.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

using namespace RK3588Display;

static const char* kDevice = "/dev/graphics/rk3588_display/0";


static bool
ReportResources(const ResourceInfo& info)
{
	printf("ROCK5_DISPLAY_RESOURCES version=%" PRIu32 " flags=%" PRIu32
		" vop=%#" PRIx64 "/%#" PRIx64 " lut=%#" PRIx64 "/%#" PRIx64
		" hdmi=%#" PRIx64 "/%#" PRIx64 " hdptx=%#" PRIx64 "/%#" PRIx64
		" hdptx_grf=%#" PRIx64 "/%#" PRIx64 " sys_grf=%#" PRIx64 "/%#" PRIx64
		" vop_grf=%#" PRIx64 "/%#" PRIx64 " vo1_grf=%#" PRIx64 "/%#" PRIx64
		" pmu=%#" PRIx64 "/%#" PRIx64 " cru=%#" PRIx64 "/%#" PRIx64 " gic=%#" PRIx64
		" vop_irq=%" PRIu32 " hdmi_irqs=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		" vop_clocks=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		" hdmi_clocks=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		" vop_pd=%" PRIu32 " hdmi_pd=%" PRIu32 " phy_phandle=%" PRIu32 " vop_port=%" PRIu32
		" board=%s usbdp=%#" PRIx64 "/%#" PRIx64 " usbdp_grf=%#" PRIx64 "/%#" PRIx64
		" vo0_grf=%#" PRIx64 "/%#" PRIx64 " ioc=%#" PRIx64 "/%#" PRIx64 " gpio3=%#" PRIx64 "/%#"
		PRIx64 " dp=%#" PRIx64 "/%#" PRIx64 " dp_pd=%" PRIu32 " usbdp_clocks=%" PRIu32 ",%" PRIu32
		",%" PRIu32 " usbdp_resets=%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32 ",%" PRIu32
		" gpio3_clocks=%" PRIu32 ",%" PRIu32 "\n",
		info.version, info.flags, info.vopBase, info.vopSize, info.vopLutBase, info.vopLutSize,
		info.hdmiBase, info.hdmiSize, info.hdptxBase, info.hdptxSize,
		info.hdptxGrfBase, info.hdptxGrfSize, info.sysGrfBase, info.sysGrfSize,
		info.vopGrfBase, info.vopGrfSize, info.vo1GrfBase, info.vo1GrfSize,
		info.pmuBase, info.pmuSize, info.clockBase, info.clockSize, info.interruptBase,
		info.vopInterrupt, info.hdmiInterrupts[0], info.hdmiInterrupts[1],
		info.hdmiInterrupts[2], info.hdmiInterrupts[3], info.hdmiInterrupts[4],
		info.vopClockIds[0], info.vopClockIds[1], info.vopClockIds[2], info.vopClockIds[3],
		info.vopClockIds[4], info.vopClockIds[5], info.vopClockIds[6],
		info.hdmiClockIds[0], info.hdmiClockIds[1], info.hdmiClockIds[2],
		info.hdmiClockIds[3], info.hdmiClockIds[4], info.hdmiClockIds[5],
		info.vopPowerDomain, info.hdmiPowerDomain, info.hdmiPhyPhandle, info.vopPortIndex,
		info.boardCompatible, info.usbdpPhyBase, info.usbdpPhySize, info.usbdpGrfBase,
		info.usbdpGrfSize, info.vo0GrfBase, info.vo0GrfSize, info.iocBase, info.iocSize,
		info.gpio3Base, info.gpio3Size, info.dpBase, info.dpSize, info.dpPowerDomain,
		info.usbdpPhyClockIds[0], info.usbdpPhyClockIds[1], info.usbdpPhyClockIds[2],
		info.usbdpPhyResets[0], info.usbdpPhyResets[1], info.usbdpPhyResets[2],
		info.usbdpPhyResets[3], info.usbdpPhyResets[4], info.gpio3ClockIds[0],
		info.gpio3ClockIds[1]);
	if (!ResourcesMatch(info)) {
		fprintf(stderr, "Display resource description does not match the recorded board\n");
		return false;
	}
	return true;
}


static void
PrintWords(const char* label, unsigned index, const uint32_t* words, unsigned count)
{
	printf("ROCK5_DISPLAY_%s sample=%u", label, index);
	for (unsigned i = 0; i < count; i++)
		printf("%c%08" PRIx32, i == 0 ? ' ' : ',', words[i]);
	printf("\n");
}


static void
ReportSnapshot(unsigned index, const DisplaySnapshot& snapshot)
{
	printf("ROCK5_DISPLAY_SNAPSHOT sample=%u version=%" PRIu32 " flags=%#" PRIx32
		" start_us=%" PRId64 " end_us=%" PRId64 "\n", index, snapshot.version,
		snapshot.flags, snapshot.startedMicros, snapshot.finishedMicros);
	PrintWords("PMU", index, snapshot.pmu, kPmuCount);
	PrintWords("CRU_SELECT", index, snapshot.clockSelect, kClockSelectCount);
	PrintWords("CRU_GATE", index, snapshot.clockGate, kClockGateCount);
	PrintWords("SYS_GRF", index, snapshot.sysGrf, kSysGrfCount);
	PrintWords("VOP_GRF", index, &snapshot.vopGrf, 1);
	PrintWords("VO1_GRF", index, snapshot.vo1Grf, kVo1GrfCount);
	PrintWords("HDPTX1_GRF", index, snapshot.hdptxGrf, kHdptxGrfCount);
	PrintWords("USBDP1_GRF", index, snapshot.usbdpGrf, kUsbdpGrfCount);
	PrintWords("VO0_GRF", index, snapshot.vo0Grf, kVo0GrfCount);
	PrintWords("IOC", index, snapshot.ioc, kIocCount);
	if ((snapshot.flags & kSnapshotGpioRead) != 0)
		PrintWords("GPIO3", index, snapshot.gpio, kGpioCount);
	if ((snapshot.flags & kSnapshotDpRead) != 0)
		PrintWords("DP1", index, snapshot.dp, kDpCount);
	if ((snapshot.flags & kSnapshotDpAuxRead) != 0)
		PrintWords("DP1_AUX", index, snapshot.dpAux, kDpAuxCount);
	if ((snapshot.flags & kSnapshotVopRead) != 0) {
		PrintWords("VOP_SYS", index, snapshot.vopSystem, kVopSystemCount);
		PrintWords("VOP_OVL", index, snapshot.vopOverlay, kVopOverlayCount);
		for (unsigned port = 0; port < kVopPortCount; port++) {
			char label[16];
			snprintf(label, sizeof(label), "VOP_VP%u", port);
			PrintWords(label, index, snapshot.vopPort[port], kVopPortRegisterCount);
		}
		for (unsigned window = 0; window < kVopClusterCount; window++) {
			char label[24];
			snprintf(label, sizeof(label), "VOP_CLUSTER%u", window);
			PrintWords(label, index, snapshot.vopCluster[window], kVopClusterRegisterCount);
		}
		for (unsigned window = 0; window < kVopEsmartCount; window++) {
			char label[24];
			snprintf(label, sizeof(label), "VOP_ESMART%u", window);
			PrintWords(label, index, snapshot.vopEsmart[window], kVopEsmartRegisterCount);
		}
		// Decoded timing for each video port: totals and active ranges.
		for (unsigned port = 0; port < kVopPortCount; port++) {
			const uint32_t* vp = snapshot.vopPort[port];
			uint32_t htotal = vp[kVopPortHTotal] >> 16 & 0x1fff;
			uint32_t hsEnd = vp[kVopPortHTotal] & 0x1fff;
			uint32_t hactStart = vp[kVopPortHActive] >> 16 & 0x1fff;
			uint32_t hactEnd = vp[kVopPortHActive] & 0x1fff;
			uint32_t vtotal = vp[kVopPortVTotal] >> 16 & 0x1fff;
			uint32_t vsEnd = vp[kVopPortVTotal] & 0x1fff;
			uint32_t vactStart = vp[kVopPortVActive] >> 16 & 0x1fff;
			uint32_t vactEnd = vp[kVopPortVActive] & 0x1fff;
			printf("ROCK5_DISPLAY_VP_TIMING sample=%u port=%u standby=%u out_mode=%" PRIu32
				" htotal=%" PRIu32 " hsync_end=%" PRIu32 " hactive=%" PRIu32 "-%" PRIu32
				" vtotal=%" PRIu32 " vsync_end=%" PRIu32 " vactive=%" PRIu32 "-%" PRIu32
				" width=%" PRIu32 " height=%" PRIu32 "\n", index, port,
				vp[kVopPortDisplayControl] >> 31 & 1, vp[kVopPortDisplayControl] & 0xf,
				htotal, hsEnd, hactStart, hactEnd, vtotal, vsEnd, vactStart, vactEnd,
				hactEnd > hactStart ? hactEnd - hactStart : 0,
				vactEnd > vactStart ? vactEnd - vactStart : 0);
		}
		uint32_t interfaces = snapshot.vopSystem[kVopSystemInterfaceEnable];
		printf("ROCK5_DISPLAY_IF sample=%u dp0=%u dp1=%u edp0=%u hdmi0=%u edp1=%u hdmi1=%u"
			" mipi0=%u mipi1=%u rgb=%u dp0_mux=%u dp1_mux=%u hdmi_edp0_mux=%u hdmi_edp1_mux=%u"
			" version=%08" PRIx32 "\n", index, interfaces & 1, interfaces >> 1 & 1,
			interfaces >> 2 & 1, interfaces >> 3 & 1, interfaces >> 4 & 1, interfaces >> 5 & 1,
			interfaces >> 6 & 1, interfaces >> 7 & 1, interfaces >> 8 & 1,
			interfaces >> 12 & 3, interfaces >> 14 & 3, interfaces >> 16 & 3, interfaces >> 18 & 3,
			snapshot.vopSystem[kVopSystemVersion]);
	}
	if ((snapshot.flags & kSnapshotHdmiRead) != 0)
		PrintWords("HDMI1", index, snapshot.hdmi, kHdmiCount);
	// The DisplayPort TX1 path: domain, gates, the pin's mux and level, and
	// what the block itself says when readable (its version, whether it is
	// held in soft reset and its hot-plug status word on the AUX clock).
	uint32_t dpGates = snapshot.clockGate[kClockGateDp];
	printf("ROCK5_DISPLAY_DP1_PATH sample=%u vo0_on=%u pclk_dp1_gated=%u aux_gated=%u"
		" hdcp_gated=%u usbdp_pclk_gated=%u immortal_gated=%u gpio3_gated=%u pin_mux=%u"
		" pin_level=%u pin_direction=%u dp_read=%u aux_read=%u version=%08" PRIx32
		" soft_reset=%08" PRIx32 " phyif=%08" PRIx32 " pwrdown=%08" PRIx32 " hpd_status=%08"
		PRIx32 " lane_mux=%08" PRIx32 "\n", index,
		snapshot.pmu[kPmuRepairStatus] >> 17 & 1, (dpGates & kClockGateDpMask) != 0,
		(dpGates & kClockGateDpAuxMask) != 0, (dpGates & kClockGateDpHdcpMask) != 0,
		(snapshot.clockGate[kClockGateUsbdp] & kClockGateUsbdpMask) != 0,
		(snapshot.clockGate[kClockGateImmortal] & kClockGateImmortalMask) != 0,
		(snapshot.clockGate[kClockGateGpio] & kClockGateGpioMask) != 0,
		snapshot.ioc[1] >> 4 & 0xf, snapshot.gpio[kGpioExternalPort] >> kGpioHotPlugPin & 1,
		snapshot.gpio[3] >> (kGpioHotPlugPin - 16) & 1,
		(snapshot.flags & kSnapshotDpRead) != 0, (snapshot.flags & kSnapshotDpAuxRead) != 0,
		snapshot.dp[0], snapshot.dp[7], snapshot.dp[10], snapshot.dp[11], snapshot.dpAux[2],
		snapshot.vo0Grf[0]);
	uint32_t status1 = snapshot.sysGrf[2];
	printf("ROCK5_DISPLAY_HPD sample=%u hdmi0_level=%u hdmi0_int=%u hdmi1_level=%u hdmi1_int=%u"
		" vop_on=%u vo0_on=%u vo1_on=%u vop_gates=%#" PRIx32 " hdmi_gates=%#" PRIx32
		" hdptx1_status=%#" PRIx32 "\n", index,
		status1 >> 19 & 1, status1 >> 16 & 1, status1 >> 27 & 1, status1 >> 24 & 1,
		snapshot.pmu[kPmuRepairStatus] >> 16 & 1, snapshot.pmu[kPmuRepairStatus] >> 17 & 1,
		snapshot.pmu[kPmuRepairStatus] >> 18 & 1,
		snapshot.clockGate[kClockGateVop] & kClockGateVopMask,
		snapshot.clockGate[kClockGateHdmi] & kClockGateHdmiMask, snapshot.hdptxGrf[1]);
}


static bool
DecodeEdid(const uint8_t* block, unsigned* extensions)
{
	static const uint8_t kHeader[8] = {0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00};
	if (memcmp(block, kHeader, 8) != 0) {
		fprintf(stderr, "EDID header mismatch\n");
		return false;
	}
	unsigned sum = 0;
	for (unsigned i = 0; i < kEdidBlockBytes; i++)
		sum += block[i];
	if ((sum & 0xff) != 0) {
		fprintf(stderr, "EDID base block checksum %u\n", sum & 0xff);
		return false;
	}
	uint16_t vendor = (uint16_t)(block[8] << 8 | block[9]);
	char manufacturer[4] = {(char)('A' - 1 + ((vendor >> 10) & 0x1f)),
		(char)('A' - 1 + ((vendor >> 5) & 0x1f)), (char)('A' - 1 + (vendor & 0x1f)), 0};
	const uint8_t* dtd = block + 54;
	unsigned pixelClock = (dtd[0] | dtd[1] << 8) * 10;
	unsigned hActive = dtd[2] | (dtd[4] & 0xf0) << 4;
	unsigned hBlank = dtd[3] | (dtd[4] & 0x0f) << 8;
	unsigned vActive = dtd[5] | (dtd[7] & 0xf0) << 4;
	unsigned vBlank = dtd[6] | (dtd[7] & 0x0f) << 8;
	unsigned hSyncOffset = dtd[8] | (dtd[11] & 0xc0) << 2;
	unsigned hSyncWidth = dtd[9] | (dtd[11] & 0x30) << 4;
	unsigned vSyncOffset = dtd[10] >> 4 | (dtd[11] & 0x0c) << 2;
	unsigned vSyncWidth = (dtd[10] & 0x0f) | (dtd[11] & 0x03) << 4;
	unsigned widthMm = dtd[12] | (dtd[14] & 0xf0) << 4;
	unsigned heightMm = dtd[13] | (dtd[14] & 0x0f) << 8;
	*extensions = block[126];
	printf("ROCK5_DISPLAY_EDID_INFO manufacturer=%s product=%#x serial=%#x week=%u year=%u"
		" version=%u.%u extensions=%u digital=%u preferred=%ux%u pixel_khz=%u hblank=%u vblank=%u"
		" hsync_offset=%u hsync_width=%u vsync_offset=%u vsync_width=%u flags=%#x size_mm=%ux%u"
		" checksum=ok\n", manufacturer, block[10] | block[11] << 8,
		block[12] | block[13] << 8 | block[14] << 16 | block[15] << 24, block[16], 1990 + block[17],
		block[18], block[19], *extensions, block[20] >> 7, hActive, vActive, pixelClock, hBlank,
		vBlank, hSyncOffset, hSyncWidth, vSyncOffset, vSyncWidth, dtd[17], widthMm, heightMm);
	return pixelClock > 0 && hActive > 0 && vActive > 0;
}


static bool
ReadEdid(int fd)
{
	unsigned blocks = 1;
	unsigned extensions = 0;
	unsigned totalPolls = 0;
	for (unsigned block = 0; block < blocks; block++) {
		EdidRequest request = {};
		request.version = kEdidVersion;
		request.block = block;
		if (block == 0) {
			EdidRequest invalid = request;
			invalid.version = 99;
			if (ioctl(fd, kReadEdid, &invalid, sizeof(invalid)) == 0 || errno != EINVAL) {
				fprintf(stderr, "Invalid EDID request version was not rejected\n");
				return false;
			}
			invalid = request;
			invalid.block = kEdidMaxBlocks;
			if (ioctl(fd, kReadEdid, &invalid, sizeof(invalid)) == 0 || errno != EINVAL) {
				fprintf(stderr, "Invalid EDID block was not rejected\n");
				return false;
			}
			if (ioctl(fd, kReadEdid, &request, sizeof(request) - 1) == 0 || errno != EINVAL) {
				fprintf(stderr, "Malformed EDID request was not rejected\n");
				return false;
			}
			if (ioctl(fd, kReadEdid, NULL, sizeof(request)) == 0 || errno != EFAULT) {
				fprintf(stderr, "Null EDID request was not rejected\n");
				return false;
			}
			printf("ROCK5_DISPLAY_EDID_REQUEST_CHECKS_PASS\n");
		}
		if (ioctl(fd, kReadEdid, &request, sizeof(request)) != 0) {
			perror("display EDID");
			return false;
		}
		printf("ROCK5_DISPLAY_EDID block=%u result=%" PRIu32 " flags=%#" PRIx32 " bytes=%" PRIu32
			" polls=%" PRIu32 " control=%08" PRIx32 "/%08" PRIx32 " status=%08" PRIx32 "/%08" PRIx32
			" hpd=%08" PRIx32 " start_us=%" PRId64 " end_us=%" PRId64 " hex=", block, request.result,
			request.flags, request.bytesRead, request.polls, request.controlBefore,
			request.controlAfter, request.statusBefore, request.statusAfter, request.hotPlug,
			request.startedMicros, request.finishedMicros);
		for (unsigned i = 0; i < kEdidBlockBytes; i++)
			printf("%02x", request.data[i]);
		printf("\n");
		if (request.result != kEdidOK || request.bytesRead != kEdidBlockBytes) {
			fprintf(stderr, "EDID block %u result %" PRIu32 " after %" PRIu32 " bytes\n", block,
				request.result, request.bytesRead);
			return false;
		}
		if ((request.controlAfter & kI2cmWriteMask) != 0 || (request.statusAfter & (kI2cmOperationDone | kI2cmNack)) != 0) {
			fprintf(stderr, "I2C master left busy or with pending status\n");
			return false;
		}
		totalPolls += request.polls;
		unsigned sum = 0;
		for (unsigned i = 0; i < kEdidBlockBytes; i++)
			sum += request.data[i];
		if ((sum & 0xff) != 0) {
			fprintf(stderr, "EDID block %u checksum %u\n", block, sum & 0xff);
			return false;
		}
		if (block == 0) {
			if (!DecodeEdid(request.data, &extensions))
				return false;
			blocks = 1 + (extensions < kEdidMaxBlocks - 1 ? extensions : kEdidMaxBlocks - 1);
		} else {
			printf("ROCK5_DISPLAY_EDID_EXTENSION block=%u tag=%#x revision=%u checksum=ok\n",
				block, request.data[0], request.data[1]);
		}
	}
	printf("ROCK5_DISPLAY_EDID_PASS blocks=%u extensions=%u polls=%u register_writes=i2c_master_only\n",
		blocks, extensions, totalPolls);
	return true;
}


static void
PrintScanout(const char* action, const ScanoutRequest& r)
{
	printf("ROCK5_DISPLAY_SCANOUT action=%s result=%" PRIu32 " flags=%" PRIu32 " port=%" PRIu32
		" window=%" PRIu32 " before=%08" PRIx32 " after=%08" PRIx32 " firmware=%08" PRIx32
		" pattern=%08" PRIx32 " region_control=%08" PRIx32 " virtual=%" PRIu32 " active=%08" PRIx32
		" display=%08" PRIx32 " start=%08" PRIx32 " if_en=%08" PRIx32 " cfg_done=%08" PRIx32
		" polls=%" PRIu32 " start_us=%" PRId64 " end_us=%" PRId64 "\n", action, r.result, r.flags,
		r.port, r.window, r.addressBefore, r.addressAfter, r.firmwareAddress, r.patternAddress,
		r.regionControl, r.virtualWidth, r.activeInfo, r.displayInfo, r.displayStart,
		r.interfaceEnable, r.configDone, r.polls, r.startedMicros, r.finishedMicros);
	fflush(stdout);
}


// Opt-in scanout swap: query, show the driver pattern for `hold` seconds so
// the capture side can look at it, query again, restore and query once more.
// Closing the device also restores, so an aborted run leaves the desktop.
static bool
SwapScanout(int fd, unsigned hold)
{
	ScanoutRequest request = {};
	request.version = kScanoutVersion;
	if (ioctl(fd, kSwapScanout, &request, sizeof(request) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed scanout request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kSwapScanout, NULL, sizeof(request)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null scanout request was not rejected\n");
		return false;
	}
	request.version = kScanoutVersion + 1;
	if (ioctl(fd, kSwapScanout, &request, sizeof(request)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid scanout request version was not rejected\n");
		return false;
	}
	request.version = kScanoutVersion;
	request.action = kScanoutRestore + 1;
	if (ioctl(fd, kSwapScanout, &request, sizeof(request)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid scanout action was not rejected\n");
		return false;
	}
	printf("ROCK5_DISPLAY_SCANOUT_REQUEST_CHECKS_PASS\n");
	struct Step { uint32_t action; const char* name; uint32_t flags; };
	static const Step kSteps[] = {
		{kScanoutQuery, "query", 0}, {kScanoutShowPattern, "show", kScanoutSwapped},
		{kScanoutQuery, "query", kScanoutSwapped}, {kScanoutRestore, "restore", 0},
		{kScanoutQuery, "query", 0}};
	uint32_t firmware = 0, pattern = 0, port = 0, window = 0;
	for (unsigned i = 0; i < sizeof(kSteps) / sizeof(kSteps[0]); i++) {
		memset(&request, 0, sizeof(request));
		request.version = kScanoutVersion;
		request.action = kSteps[i].action;
		if (ioctl(fd, kSwapScanout, &request, sizeof(request)) != 0) {
			perror("display scanout");
			printf("ROCK5_DISPLAY_SCANOUT_ABORT step=%s\n", kSteps[i].name);
			return false;
		}
		PrintScanout(kSteps[i].name, request);
		bool ok = request.result == kScanoutOK && request.flags == kSteps[i].flags
			&& request.version == kScanoutVersion && request.action == kSteps[i].action
			&& request.finishedMicros >= request.startedMicros;
		if (i == 0) {
			firmware = request.addressBefore;
			port = request.port;
			window = request.window;
		} else {
			ok = ok && request.port == port && request.window == window
				&& request.firmwareAddress == firmware;
		}
		if (kSteps[i].action == kScanoutShowPattern) {
			pattern = request.patternAddress;
			ok = ok && request.addressBefore == firmware && request.addressAfter == pattern
				&& pattern != firmware && pattern != 0 && (pattern & (B_PAGE_SIZE - 1)) == 0;
		} else if (kSteps[i].action == kScanoutRestore) {
			ok = ok && request.addressBefore == pattern && request.addressAfter == firmware
				&& request.patternAddress == pattern;
		} else if (i > 0) {
			ok = ok && request.addressBefore == (kSteps[i].flags != 0 ? pattern : firmware);
		}
		if (!ok) {
			fprintf(stderr, "Scanout step %s failed\n", kSteps[i].name);
			printf("ROCK5_DISPLAY_SCANOUT_ABORT step=%s\n", kSteps[i].name);
			return false;
		}
		if (kSteps[i].action == kScanoutShowPattern) {
			printf("ROCK5_DISPLAY_SCANOUT_HOLD seconds=%u\n", hold);
			fflush(stdout);
			sleep(hold);
		}
	}
	printf("ROCK5_DISPLAY_SCANOUT_PASS port=%" PRIu32 " window=%" PRIu32 " firmware=%08" PRIx32
		" pattern=%08" PRIx32 " hold_seconds=%u register_writes=window_address_and_cfg_done\n",
		port, window, firmware, pattern, hold);
	fflush(stdout);
	return true;
}


// Accelerant profile: app_server's primary accelerant owns the frame buffer.
// Through a second writable handle, read the driver's description and the
// shared information, map the live frame buffer once, and confirm that a
// second acquisition is refused. Nothing here changes the display.
static bool
CheckAccelerant()
{
	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		perror(kDevice);
		return false;
	}
	AccelerantInfo info = {};
	info.version = kAccelerantVersion;
	if (ioctl(fd, kGetAccelerantInfo, &info, sizeof(info) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed accelerant request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kGetAccelerantInfo, NULL, sizeof(info)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null accelerant request was not rejected\n");
		return false;
	}
	info.version = kAccelerantVersion + 1;
	if (ioctl(fd, kGetAccelerantInfo, &info, sizeof(info)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid accelerant request version was not rejected\n");
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT_REQUEST_CHECKS_PASS\n");
	info.version = kAccelerantVersion;
	if (ioctl(fd, kGetAccelerantInfo, &info, sizeof(info)) != 0) {
		printf("ROCK5_DISPLAY_ACCELERANT_NOT_ACQUIRED errno=%d\n", errno);
		fprintf(stderr, "No accelerant owns the frame buffer (errno %d)\n", errno);
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT flags=%" PRIu32 " shared_area=%" PRId32
		" framebuffer=%08" PRIx32 " firmware=%08" PRIx32 " port=%" PRIu32 " window=%" PRIu32
		" polls=%" PRIu32 " width=%" PRIu32 " height=%" PRIu32 " bytes_per_row=%" PRIu32
		" retrace_sem=%" PRId32 " retraces=%" PRIu32 " calls=%" PRIu32 " spurious=%" PRIu32
		" first_us=%" PRId64 " last_us=%" PRId64 " now_us=%" PRId64 "\n", info.flags,
		info.sharedArea, info.frameBufferPhysical, info.firmwareAddress, info.port, info.window,
		info.polls, info.width, info.height, info.bytesPerRow, info.retraceSemaphore,
		info.retraces, info.interruptCalls, info.interruptSpurious, info.firstRetraceMicros,
		info.lastRetraceMicros, system_time());
	if ((info.flags & kAccelerantRetrace) != 0) {
		// Wait for a series of frame starts and measure their spacing.
		const unsigned kWaits = 24;
		unsigned timeouts = 0, errors = 0;
		status_t lastError = B_OK;
		bigtime_t first = 0, last = 0;
		bigtime_t started = system_time();
		for (unsigned i = 0; i < kWaits; i++) {
			status_t status = acquire_sem_etc(info.retraceSemaphore, 1, B_RELATIVE_TIMEOUT, 200000);
			bigtime_t now = system_time();
			if (status == B_TIMED_OUT) {
				timeouts++;
				continue;
			}
			if (status != B_OK) {
				errors++;
				lastError = status;
				continue;
			}
			if (first == 0)
				first = now;
			last = now;
		}
		AccelerantInfo after = {};
		after.version = kAccelerantVersion;
		if (ioctl(fd, kGetAccelerantInfo, &after, sizeof(after)) != 0) {
			perror("accelerant info after retrace");
			return false;
		}
		unsigned waited = kWaits - timeouts - errors;
		printf("ROCK5_DISPLAY_RETRACE waits=%u timeouts=%u first_us=%" PRId64 " last_us=%" PRId64
			" period_us=%" PRId64 " retraces_before=%" PRIu32 " retraces_after=%" PRIu32
			" elapsed_us=%" PRId64 " errors=%u status=%s\n", kWaits, timeouts, first, last,
			waited > 1 ? (last - first) / (bigtime_t)(waited - 1) : 0, info.retraces,
			after.retraces, system_time() - started, errors,
			errors != 0 ? strerror(lastError) : "ok");
		if (waited == 0) {
			// Diagnostic: re-arm the interrupt and look again.
			RetraceRearm rearm = {};
			rearm.version = kAccelerantVersion;
			if (ioctl(fd, kRearmRetrace, &rearm, sizeof(rearm)) != 0) {
				perror("retrace re-arm");
			} else {
				unsigned again = 0;
				for (unsigned i = 0; i < 12; i++) {
					if (acquire_sem_etc(info.retraceSemaphore, 1, B_RELATIVE_TIMEOUT, 200000) == B_OK)
						again++;
				}
				AccelerantInfo later = {};
				later.version = kAccelerantVersion;
				ioctl(fd, kGetAccelerantInfo, &later, sizeof(later));
				printf("ROCK5_DISPLAY_RETRACE_REARM enable=%08" PRIx32 "/%08" PRIx32 " status=%08"
					PRIx32 "/%08" PRIx32 " reinstall=%" PRId32 " waits=12 acquired=%u retraces=%"
					PRIu32 "/%" PRIu32 " calls=%" PRIu32 " spurious=%" PRIu32 "\n",
					rearm.enableBefore, rearm.enableAfter, rearm.statusBefore, rearm.statusAfter,
					rearm.reinstall, again, rearm.retraces, later.retraces, later.interruptCalls,
					later.interruptSpurious);
			}
		}
	}
	char text[B_PATH_NAME_LENGTH];
	if (ioctl(fd, B_GET_ACCELERANT_SIGNATURE, text, sizeof(text)) != 0) {
		perror("accelerant signature");
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT_SIGNATURE %s\n", text);
	if (ioctl(fd, kGetDeviceName, text, sizeof(text)) != 0) {
		perror("accelerant device name");
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT_DEVICE %s\n", text);
	const SharedInfo* shared = NULL;
	area_id sharedArea = clone_area("probe shared info", (void**)&shared, B_ANY_ADDRESS,
		B_READ_AREA, info.sharedArea);
	if (sharedArea < 0) {
		fprintf(stderr, "Cloning the shared area failed: %s\n", strerror(sharedArea));
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT_SHARED version=%" PRIu32 " flags=%" PRIu32
		" mode_list_area=%" PRId32 " modes=%" PRIu32 " size=%" PRIu32 "x%" PRIu32
		" bytes_per_row=%" PRIu32 " pixel_khz=%" PRIu32 " h=%" PRIu32 "/%" PRIu32 "/%" PRIu32
		" v=%" PRIu32 "/%" PRIu32 "/%" PRIu32 " port_timing=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
		",%08" PRIx32 " edid_result=%" PRIu32 " power=%" PRIu32 " name=%.31s edid=", shared->version,
		shared->flags, shared->modeListArea, shared->modeCount, shared->width, shared->height,
		shared->bytesPerRow, shared->pixelClockKHz, shared->hSyncStart, shared->hSyncEnd,
		shared->hTotal, shared->vSyncStart, shared->vSyncEnd, shared->vTotal,
		shared->portTiming[0], shared->portTiming[1], shared->portTiming[2],
		shared->portTiming[3], shared->edidResult, shared->powerMode, shared->name);
	for (unsigned i = 0; i < sizeof(shared->edid); i++)
		printf("%02x", shared->edid[i]);
	printf("\n");
	delete_area(sharedArea);
	area_info clone = {};
	if (ioctl(fd, kCloneFrameBuffer, &clone, sizeof(clone)) != 0) {
		perror("frame buffer clone");
		return false;
	}
	const uint32_t* pixels = (const uint32_t*)clone.address;
	printf("ROCK5_DISPLAY_ACCELERANT_CLONE area=%" PRId32 " size=%zu samples=%08" PRIx32
		",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 "\n", clone.area, clone.size,
		pixels[0], pixels[540 * info.width + 960], pixels[1079 * info.width + 1919],
		pixels[20 * info.width + 1850]);
	delete_area(clone.area);
	if (ioctl(fd, kAcquireFrameBuffer, NULL, 0) == 0 || errno != EBUSY) {
		fprintf(stderr, "A second acquisition was not refused (errno %d)\n", errno);
		return false;
	}
	printf("ROCK5_DISPLAY_ACCELERANT_ACQUIRE_BUSY\n");
	close(fd);
	printf("ROCK5_DISPLAY_ACCELERANT_PASS acquired=%u edid=%u framebuffer=%08" PRIx32
		" firmware=%08" PRIx32 " register_writes=owner_only\n",
		(info.flags & kAccelerantAcquired) != 0, (info.flags & kAccelerantEdid) != 0,
		info.frameBufferPhysical, info.firmwareAddress);
	return true;
}


// Mode-set profile: ask the driver for one of the CEA modes and report what
// the hardware did. The frame buffer stays; the port shows its top-left part.
static bool
ChangeMode(unsigned width, unsigned height)
{
	struct Timing { unsigned w, h, clock, hss, hse, ht, vss, vse, vt, vic; };
	static const Timing kTimings[] = {
		{1920, 1080, 148500, 2008, 2052, 2200, 1084, 1089, 1125, 16},
		{1280, 720, 74250, 1390, 1430, 1650, 725, 730, 750, 4},
		{720, 480, 27000, 736, 798, 858, 489, 495, 525, 2},
		{640, 480, 25175, 656, 752, 800, 490, 492, 525, 1},
	};
	const Timing* timing = NULL;
	for (unsigned i = 0; i < sizeof(kTimings) / sizeof(kTimings[0]); i++) {
		if (kTimings[i].w == width && kTimings[i].h == height)
			timing = &kTimings[i];
	}
	if (timing == NULL) {
		fprintf(stderr, "No CEA timing for %ux%u\n", width, height);
		return false;
	}
	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		perror(kDevice);
		return false;
	}
	ModeRequest request = {};
	request.version = kModeVersion;
	if (ioctl(fd, kSetDisplayMode, &request, sizeof(request) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed mode request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kSetDisplayMode, NULL, sizeof(request)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null mode request was not rejected\n");
		return false;
	}
	request.version = kModeVersion + 1;
	if (ioctl(fd, kSetDisplayMode, &request, sizeof(request)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid mode request version was not rejected\n");
		return false;
	}
	printf("ROCK5_DISPLAY_MODE_REQUEST_CHECKS_PASS\n");
	memset(&request, 0, sizeof(request));
	request.version = kModeVersion;
	request.flags = kModePositiveHSync | kModePositiveVSync;
	request.pixelClockKHz = timing->clock;
	request.hDisplay = timing->w;
	request.hSyncStart = timing->hss;
	request.hSyncEnd = timing->hse;
	request.hTotal = timing->ht;
	request.vDisplay = timing->h;
	request.vSyncStart = timing->vss;
	request.vSyncEnd = timing->vse;
	request.vTotal = timing->vt;
	request.vic = timing->vic;
	if (ioctl(fd, kSetDisplayMode, &request, sizeof(request)) != 0) {
		perror("display mode");
		close(fd);
		return false;
	}
	printf("ROCK5_DISPLAY_MODE width=%u height=%u clock=%" PRIu32 " vic=%" PRIu32 " result=%" PRIu32
		" phase=%" PRIu32 " hold_polls=%" PRIu32 " clock_polls=%" PRIu32 " lock_polls=%" PRIu32
		" phy_status=%08" PRIx32 " timing=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
		" if_en=%08" PRIx32 " micros=%" PRId64 "\n", timing->w, timing->h, request.pixelClockKHz,
		request.vic, request.result, request.phase, request.holdPolls, request.clockPolls,
		request.lockPolls, request.phyStatus, request.timing[0], request.timing[1],
		request.timing[2], request.timing[3], request.interfaceEnable,
		request.finishedMicros - request.startedMicros);
	AccelerantInfo info = {};
	info.version = kAccelerantVersion;
	if (ioctl(fd, kGetAccelerantInfo, &info, sizeof(info)) == 0) {
		printf("ROCK5_DISPLAY_MODE_ACCELERANT width=%" PRIu32 " height=%" PRIu32 " flags=%" PRIu32
			" retraces=%" PRIu32 "\n", info.width, info.height, info.flags, info.retraces);
	}
	close(fd);
	if (request.result != kModeOK) {
		fprintf(stderr, "Mode change result %" PRIu32 " at phase %" PRIu32 "\n", request.result,
			request.phase);
		return false;
	}
	printf("ROCK5_DISPLAY_MODE_PASS width=%u height=%u clock=%u vic=%u\n", timing->w, timing->h,
		timing->clock, timing->vic);
	return true;
}


// DPMS through the driver: off stops the port and powers the PHY down, on
// repeats the current mode set. The retrace count read twice half a second
// apart shows whether frames still start.
static bool
ChangePower(bool off)
{
	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		perror(kDevice);
		return false;
	}
	PowerRequest request = {};
	request.version = kPowerVersion;
	request.mode = off ? kPowerOff : kPowerOn;
	if (ioctl(fd, kSetPowerMode, &request, sizeof(request) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed power request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kSetPowerMode, NULL, sizeof(request)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null power request was not rejected\n");
		return false;
	}
	request.version = kPowerVersion + 1;
	if (ioctl(fd, kSetPowerMode, &request, sizeof(request)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid power request version was not rejected\n");
		return false;
	}
	request.version = kPowerVersion;
	request.mode = 2;
	if (ioctl(fd, kSetPowerMode, &request, sizeof(request)) != 0 || request.result != kModeUnsupported) {
		fprintf(stderr, "Unknown power mode was not refused\n");
		return false;
	}
	printf("ROCK5_DISPLAY_POWER_REQUEST_CHECKS_PASS\n");
	memset(&request, 0, sizeof(request));
	request.version = kPowerVersion;
	request.mode = off ? kPowerOff : kPowerOn;
	if (ioctl(fd, kSetPowerMode, &request, sizeof(request)) != 0) {
		perror("display power");
		close(fd);
		return false;
	}
	printf("ROCK5_DISPLAY_POWER mode=%s result=%" PRIu32 " phase=%" PRIu32 " hold_polls=%" PRIu32
		" clock_polls=%" PRIu32 " lock_polls=%" PRIu32 " phy_status=%08" PRIx32
		" control=%08" PRIx32 " previous=%" PRIu32 " micros=%" PRId64 "\n", off ? "off" : "on",
		request.result, request.phase, request.holdPolls, request.clockPolls, request.lockPolls,
		request.phyStatus, request.portControl, request.previous,
		request.finishedMicros - request.startedMicros);
	AccelerantInfo before = {}, after = {};
	before.version = after.version = kAccelerantVersion;
	if (ioctl(fd, kGetAccelerantInfo, &before, sizeof(before)) == 0) {
		snooze(500000);
		if (ioctl(fd, kGetAccelerantInfo, &after, sizeof(after)) == 0) {
			uint32_t power = 99;
			SharedInfo* shared = NULL;
			area_id area = clone_area("rock5 display power shared", (void**)&shared, B_ANY_ADDRESS,
				B_READ_AREA, after.sharedArea);
			if (area >= 0) {
				power = shared->powerMode;
				delete_area(area);
			}
			printf("ROCK5_DISPLAY_POWER_ACCELERANT width=%" PRIu32 " height=%" PRIu32 " flags=%"
				PRIu32 " retraces_before=%" PRIu32 " retraces_after=%" PRIu32 " shared_power=%"
				PRIu32 "\n", after.width, after.height, after.flags, before.retraces,
				after.retraces, power);
		}
	}
	close(fd);
	if (request.result != kModeOK) {
		fprintf(stderr, "Power change result %" PRIu32 " at phase %" PRIu32 "\n", request.result,
			request.phase);
		return false;
	}
	printf("ROCK5_DISPLAY_POWER_PASS mode=%s\n", off ? "off" : "on");
	return true;
}


// Hardware cursor through the driver, while app_server keeps its own
// pointer state: the probe saves that state to a file, shows its own
// 64x64 quadrant bitmap (white, black, transparent red, half-transparent
// white) at a position for the capture, hides it, and restores the saved
// state afterwards.
static const char* kCursorSaved = "/tmp/rock5-cursor-saved.bin";


static void
ReportCursor(const char* label, const CursorState& state, const char* saved)
{
	printf("ROCK5_DISPLAY_CURSOR_%s width=%" PRIu32 " height=%" PRIu32 " hot=%" PRIu32 ",%" PRIu32
		" x=%" PRId32 " y=%" PRId32 " visible=%" PRIu32 " window=%" PRIu32 " mixer=%" PRIu32
		" control=%08" PRIx32 " start=%08" PRIx32 " address=%08" PRIx32 " mix=%08" PRIx32 ",%08"
		PRIx32 ",%08" PRIx32 ",%08" PRIx32 " saved=%s\n", label, state.width, state.height,
		state.hotX, state.hotY, state.x, state.y, state.visible, state.window, state.mixer,
		state.regionControl, state.displayStart, state.address, state.mixWords[0],
		state.mixWords[1], state.mixWords[2], state.mixWords[3], saved);
}


static bool
CursorChecks(int fd)
{
	CursorShow show = {};
	show.version = kCursorVersion;
	if (ioctl(fd, kShowCursor, &show, sizeof(show) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed cursor request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kShowCursor, NULL, sizeof(show)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null cursor request was not rejected\n");
		return false;
	}
	show.version = kCursorVersion + 1;
	if (ioctl(fd, kShowCursor, &show, sizeof(show)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid cursor request version was not rejected\n");
		return false;
	}
	CursorMove move = {};
	move.version = kCursorVersion;
	move.x = 40000;
	if (ioctl(fd, kMoveCursor, &move, sizeof(move)) != 0 || move.result != kCursorUnsupported) {
		fprintf(stderr, "Out-of-range cursor position was not refused\n");
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_REQUEST_CHECKS_PASS\n");
	AccelerantInfo info = {};
	info.version = kAccelerantVersion;
	if (ioctl(fd, kGetAccelerantInfo, &info, sizeof(info)) != 0) {
		perror("accelerant info");
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_ACCELERANT flags=%" PRIu32 " width=%" PRIu32 " height=%" PRIu32
		"\n", info.flags, info.width, info.height);
	if ((info.flags & kAccelerantCursor) == 0) {
		fprintf(stderr, "The accelerant has no hardware cursor\n");
		return false;
	}
	return true;
}


static bool
MoveAndShow(int fd, int x, int y, bool visible, uint32_t& polls)
{
	CursorMove move = {};
	move.version = kCursorVersion;
	move.x = x;
	move.y = y;
	if (ioctl(fd, kMoveCursor, &move, sizeof(move)) != 0) {
		perror("cursor move");
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_MOVE x=%d y=%d result=%" PRIu32 " polls=%" PRIu32 " start=%08"
		PRIx32 " address=%08" PRIx32 "\n", x, y, move.result, move.polls, move.displayStart,
		move.address);
	CursorShow show = {};
	show.version = kCursorVersion;
	show.visible = visible ? 1 : 0;
	if (ioctl(fd, kShowCursor, &show, sizeof(show)) != 0) {
		perror("cursor show");
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_SHOW visible=%u result=%" PRIu32 " polls=%" PRIu32
		" control=%08" PRIx32 "\n", visible ? 1 : 0, show.result, show.polls, show.regionControl);
	polls = move.polls + show.polls;
	return move.result == kCursorOK && show.result == kCursorOK;
}


static bool
SetBitmap(int fd, CursorBitmap& bitmap)
{
	if (ioctl(fd, kSetCursorBitmap, &bitmap, sizeof(bitmap)) != 0) {
		perror("cursor bitmap");
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_BITMAP width=%" PRIu32 " height=%" PRIu32 " hot=%" PRIu32 ",%"
		PRIu32 " result=%" PRIu32 " polls=%" PRIu32 "\n", bitmap.width, bitmap.height,
		bitmap.hotX, bitmap.hotY, bitmap.result, bitmap.polls);
	return bitmap.result == kCursorOK;
}


// --cursor X Y: the quadrant bitmap at X,Y; --cursor-hide: hidden where it is;
// --cursor-restore: app_server's saved state back.
static bool
ControlCursor(const char* action, int x, int y)
{
	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		perror(kDevice);
		return false;
	}
	if (!CursorChecks(fd)) {
		close(fd);
		return false;
	}
	static CursorState before, after, saved;
	before.version = kCursorVersion;
	if (ioctl(fd, kGetCursor, &before, sizeof(before)) != 0) {
		perror("cursor state");
		close(fd);
		return false;
	}
	bool restoring = strcmp(action, "restore") == 0;
	bool hiding = strcmp(action, "hide") == 0;
	FILE* file = fopen(kCursorSaved, restoring ? "rb" : "rb");
	bool haveSaved = file != NULL && fread(&saved, sizeof(saved), 1, file) == 1;
	if (file != NULL)
		fclose(file);
	if (!restoring && !haveSaved) {
		// The first placement keeps app_server's pointer for the restore.
		file = fopen(kCursorSaved, "wb");
		if (file == NULL || fwrite(&before, sizeof(before), 1, file) != 1) {
			perror(kCursorSaved);
			close(fd);
			return false;
		}
		fclose(file);
		saved = before;
		haveSaved = true;
	}
	ReportCursor("BEFORE", before, haveSaved ? "yes" : "no");
	bool passed = false;
	uint32_t polls = 0;
	if (restoring) {
		if (!haveSaved) {
			fprintf(stderr, "No saved cursor state at %s\n", kCursorSaved);
			close(fd);
			return false;
		}
		// The saved bitmap comes back; a state without one (app_server on its
		// software pointer) clears the window's bitmap.
		static CursorBitmap bitmap;
		memset(&bitmap, 0, sizeof(bitmap));
		bitmap.version = kCursorVersion;
		bitmap.width = saved.width;
		bitmap.height = saved.height;
		bitmap.hotX = saved.hotX;
		bitmap.hotY = saved.hotY;
		bitmap.bytesPerRow = kCursorBytesPerRow;
		memcpy(bitmap.data, saved.data, sizeof(bitmap.data));
		passed = SetBitmap(fd, bitmap);
		polls += bitmap.polls;
		uint32_t movePolls = 0;
		passed = MoveAndShow(fd, saved.x, saved.y, saved.visible != 0, movePolls) && passed;
		polls += movePolls;
		unlink(kCursorSaved);
	} else if (hiding) {
		passed = MoveAndShow(fd, before.x, before.y, false, polls);
	} else {
		static CursorBitmap bitmap;
		memset(&bitmap, 0, sizeof(bitmap));
		bitmap.version = kCursorVersion;
		bitmap.width = bitmap.height = kCursorMaxSize;
		bitmap.bytesPerRow = kCursorBytesPerRow;
		for (unsigned row = 0; row < kCursorMaxSize; row++) {
			for (unsigned column = 0; column < kCursorMaxSize; column++) {
				uint8_t* pixel = bitmap.data + row * kCursorBytesPerRow + column * 4;
				bool right = column >= kCursorMaxSize / 2, bottom = row >= kCursorMaxSize / 2;
				// B_RGBA32 little-endian: blue, green, red, alpha.
				if (!bottom && !right) {
					pixel[0] = pixel[1] = pixel[2] = 0xff; pixel[3] = 0xff; // white
				} else if (!bottom) {
					pixel[0] = pixel[1] = pixel[2] = 0x00; pixel[3] = 0xff; // black
				} else if (!right) {
					pixel[0] = pixel[1] = 0x00; pixel[2] = 0xff; pixel[3] = 0x00; // transparent red
				} else {
					pixel[0] = pixel[1] = pixel[2] = 0xff; pixel[3] = 0x80; // half white
				}
			}
		}
		passed = SetBitmap(fd, bitmap);
		polls += bitmap.polls;
		uint32_t movePolls = 0;
		passed = MoveAndShow(fd, x, y, true, movePolls) && passed;
		polls += movePolls;
	}
	after.version = kCursorVersion;
	if (ioctl(fd, kGetCursor, &after, sizeof(after)) != 0) {
		perror("cursor state after");
		close(fd);
		return false;
	}
	ReportCursor("AFTER", after, restoring ? "removed" : "kept");
	close(fd);
	if (!passed) {
		fprintf(stderr, "Cursor %s failed\n", action);
		return false;
	}
	printf("ROCK5_DISPLAY_CURSOR_PASS action=%s x=%" PRId32 " y=%" PRId32 " visible=%" PRIu32
		" width=%" PRIu32 " height=%" PRIu32 " polls=%" PRIu32 "\n", action, after.x, after.y,
		after.visible, after.width, after.height, polls);
	return true;
}


// --dp [edid]: bring the second connector's path up to its AUX channel and
// read the sink's DPCD (and EDID block 0), reporting every word it touched.
static void
PrintBytes(const char* label, const uint8_t* bytes, unsigned count)
{
	printf("%s", label);
	for (unsigned i = 0; i < count; i++)
		printf("%02x", bytes[i]);
	printf("\n");
}


static bool
ProbeDisplayPort(bool edid, bool train, bool video, bool window)
{
	int fd = open(kDevice, O_RDWR);
	if (fd < 0) {
		perror(kDevice);
		return false;
	}
	DpProbeRequest request = {};
	request.version = kDpVersion;
	if (ioctl(fd, kDpProbe, &request, sizeof(request) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed DP request was not rejected\n");
		return false;
	}
	if (ioctl(fd, kDpProbe, NULL, sizeof(request)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null DP request was not rejected\n");
		return false;
	}
	request.version = kDpVersion + 1;
	if (ioctl(fd, kDpProbe, &request, sizeof(request)) == 0 || errno != EINVAL) {
		fprintf(stderr, "Invalid DP request version was not rejected\n");
		return false;
	}
	printf("ROCK5_DISPLAY_DP_REQUEST_CHECKS_PASS\n");
	memset(&request, 0, sizeof(request));
	request.version = kDpVersion;
	request.flags = (edid ? kDpProbeEdid : 0) | (train ? kDpProbeTrain : 0) | (video ? kDpProbeVideo : 0)
		| (window ? kDpProbeWindow : 0);
	if (ioctl(fd, kDpProbe, &request, sizeof(request)) != 0) {
		perror("dp probe");
		close(fd);
		return false;
	}
	close(fd);
	printf("ROCK5_DISPLAY_DP result=%" PRIu32 " phase=%" PRIu32 " pin=%08" PRIx32 ",%08" PRIx32
		" level=%" PRIu32 " hpd=%08" PRIx32 ",%08" PRIx32 " hpd_polls=%" PRIu32 " refclk=%08" PRIx32
		" lcpll_polls=%" PRIu32 " aux=%" PRIu32 ",%" PRIu32 ",%" PRIu32 " aux_status=%08" PRIx32
		" dpcd_count=%" PRIu32 " sinks=%02" PRIx32 " edid_bytes=%" PRIu32 " micros=%" PRId64 "\n",
		request.result, request.phase, request.pinMuxBefore, request.pinMuxAfter, request.gpioLevel,
		request.hpdStatusBefore, request.hpdStatusAfter, request.hpdPolls, request.refclkSelect,
		request.lcpllPolls, request.auxTransfers, request.auxRetries, request.auxPolls,
		request.auxStatus, request.dpcdCount, request.sinkCount, request.edidBytes,
		request.finishedMicros - request.startedMicros);
	printf("ROCK5_DISPLAY_DP_WORDS resets=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32
		"/%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 " usbdp_grf=%08" PRIx32 ",%08" PRIx32
		" vo0_grf=%08" PRIx32 ",%08" PRIx32 " cctl=%08" PRIx32 ",%08" PRIx32 " aux_clock=%08" PRIx32
		",%08" PRIx32 " pma_before=",
		request.resetsBefore[0], request.resetsBefore[1], request.resetsBefore[2],
		request.resetsBefore[3], request.resetsAfter[0], request.resetsAfter[1],
		request.resetsAfter[2], request.resetsAfter[3], request.usbdpGrfBefore,
		request.usbdpGrfAfter, request.vo0GrfBefore, request.vo0GrfAfter, request.cctlBefore,
		request.cctlAfter, request.auxClockBefore, request.auxClockAfter);
	for (unsigned i = 0; i < kDpPmaWordCount; i++)
		printf("%s%08" PRIx32, i == 0 ? "" : ",", request.pmaBefore[i]);
	printf(" pma_after=");
	for (unsigned i = 0; i < kDpPmaWordCount; i++)
		printf("%s%08" PRIx32, i == 0 ? "" : ",", request.pmaAfter[i]);
	printf("\n");
	PrintBytes("ROCK5_DISPLAY_DP_DPCD ", request.dpcd, 16);
	if (request.edidBytes > 0)
		PrintBytes("ROCK5_DISPLAY_DP_EDID ", request.edid, 128);
	if (train) {
		printf("ROCK5_DISPLAY_DP_TRAIN rate=%02" PRIx32 " lanes=%" PRIu32 " enhanced=%" PRIu32 " ssc=%" PRIu32
			" pattern=%" PRIu32 " attempts=%" PRIu32 " cr_loops=%" PRIu32 " eq_loops=%" PRIu32
			" ropll_polls=%" PRIu32 " swing=%" PRIu32 ",%" PRIu32 " pre=%" PRIu32 ",%" PRIu32
			" phyif=%08" PRIx32 " cctl=%08" PRIx32 " status=", request.linkRate, request.laneCount,
			request.enhancedFraming, request.spreadSpectrum, request.trainingPattern, request.attempts,
			request.clockRecoveryLoops, request.equalizationLoops, request.ropllPolls, request.swing[0],
			request.swing[1], request.preEmphasis[0], request.preEmphasis[1], request.phyifAfter,
			request.cctlTrained);
		for (unsigned i = 0; i < 6; i++)
			printf("%02x", request.linkStatus[i]);
		printf("\n");
	}
	if (video) {
		printf("ROCK5_DISPLAY_DP_VIDEO gpll=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 " dclk=%08" PRIx32 ",%08" PRIx32
			" mux=%08" PRIx32 ",%08" PRIx32 " gates=%08" PRIx32 ",%08" PRIx32 "/%08" PRIx32 ",%08" PRIx32
			" port=%08" PRIx32 ",%08" PRIx32 " if_en=%08" PRIx32 ",%08" PRIx32 " if_pol=%08" PRIx32 ",%08" PRIx32
			" clk=%08" PRIx32 " background=%08" PRIx32 " commit_polls=%" PRIu32 " config=",
			request.gpll[0], request.gpll[1], request.gpll[2], request.dclkSelectBefore, request.dclkSelectAfter,
			request.dclkMuxBefore, request.dclkMuxAfter, request.dclkGatesBefore[0], request.dclkGatesBefore[1],
			request.dclkGatesAfter[0], request.dclkGatesAfter[1], request.portControlBefore,
			request.portControlAfter, request.interfaceEnableBefore, request.interfaceEnableAfter,
			request.interfacePolarityBefore, request.interfacePolarityAfter, request.portClock,
			request.background, request.commitPolls);
		for (unsigned i = 0; i < 5; i++)
			printf("%s%08" PRIx32, i == 0 ? "" : ",", request.videoConfig[i]);
		printf(" msa=%08" PRIx32 ",%08" PRIx32 ",%08" PRIx32 " hblank=%08" PRIx32 " vsample=%08" PRIx32 "\n",
			request.msa[0], request.msa[1], request.msa[2], request.hblankInterval, request.vsampleAfter);
	}
	if (window) {
		printf("ROCK5_DISPLAY_DP_WINDOW desktop=%" PRIu32 " region=%08" PRIx32 " address=%08" PRIx32
			" virtual=%" PRIu32 " active=%08" PRIx32 " control1=%08" PRIx32 " axi=%08" PRIx32
			" delay=%08" PRIx32 ",%08" PRIx32 " gating=%08" PRIx32 ",%08" PRIx32 " polls=%" PRIu32 " mixers=",
			request.desktopWindow, request.windowRegionControl, request.windowAddress, request.windowVirtual,
			request.windowActive, request.windowControl1, request.windowAxi, request.smartDelay[0],
			request.smartDelay[1], request.autoGating[0], request.autoGating[1], request.windowPolls);
		for (unsigned i = 0; i < 4; i++) {
			for (unsigned w = 0; w < 4; w++)
				printf("%s%08" PRIx32, i == 0 && w == 0 ? "" : ",", request.mixers[i][w]);
		}
		printf("\n");
	}
	if (request.result != kDpOK) {
		fprintf(stderr, "DP probe result %" PRIu32 " at phase %" PRIu32 "\n", request.result,
			request.phase);
		return false;
	}
	printf("ROCK5_DISPLAY_DP_PASS edid=%u train=%u video=%u%s\n", edid ? 1 : 0, train ? 1 : 0, video ? 1 : 0,
		window ? " window=1" : "");
	return true;
}


int
main(int argc, char** argv)
{
	unsigned samples = 3;
	if (argc == 2 && strcmp(argv[1], "--absent-device") == 0) {
		// Emulator fixture: the device tree has no RK3588 VOP2, so the driver
		// must not publish a device. This proves packaging, not observation.
		int fd = open(kDevice, O_RDONLY);
		if (fd >= 0 || errno != ENOENT) {
			if (fd >= 0)
				close(fd);
			fprintf(stderr, "Unexpected display device presence (errno %d)\n", errno);
			return 1;
		}
		printf("ROCK5_DISPLAY_ABSENT_PASS device=%s errno=ENOENT\n", kDevice);
		return 0;
	}
	bool edid = argc == 2 && strcmp(argv[1], "--edid") == 0;
	bool accelerant = argc == 2 && strcmp(argv[1], "--accelerant") == 0;
	bool scanout = argc >= 2 && strcmp(argv[1], "--scanout") == 0;
	if (argc == 3 && strcmp(argv[1], "--mode") == 0) {
		unsigned width = 0, height = 0;
		if (sscanf(argv[2], "%ux%u", &width, &height) != 2) {
			fprintf(stderr, "usage: %s --mode WIDTHxHEIGHT\n", argv[0]);
			return 2;
		}
		return ChangeMode(width, height) ? 0 : 1;
	}
	if (argc == 4 && strcmp(argv[1], "--cursor") == 0) {
		char* end = NULL;
		long x = strtol(argv[2], &end, 10);
		bool valid = end != NULL && *end == '\0';
		long y = strtol(argv[3], &end, 10);
		valid = valid && end != NULL && *end == '\0' && x >= -32768 && x <= 32767 && y >= -32768
			&& y <= 32767;
		if (!valid) {
			fprintf(stderr, "usage: %s --cursor X Y\n", argv[0]);
			return 2;
		}
		return ControlCursor("place", (int)x, (int)y) ? 0 : 1;
	}
	if (argc == 2 && (strcmp(argv[1], "--cursor-hide") == 0 || strcmp(argv[1], "--cursor-restore") == 0))
		return ControlCursor(argv[1] + strlen("--cursor-"), 0, 0) ? 0 : 1;
	if (argc >= 2 && argc <= 6 && strcmp(argv[1], "--dp") == 0) {
		bool edid = false, train = false, video = false, window = false, valid = true;
		for (int i = 2; i < argc; i++) {
			if (strcmp(argv[i], "edid") == 0 && !edid)
				edid = true;
			else if (strcmp(argv[i], "train") == 0 && !train)
				train = true;
			else if (strcmp(argv[i], "video") == 0 && !video)
				video = true;
			else if (strcmp(argv[i], "window") == 0 && !window)
				window = true;
			else
				valid = false;
		}
		if (!valid || (video && !train) || (window && !video)) {
			fprintf(stderr, "usage: %s --dp [edid] [train [video [window]]]\n", argv[0]);
			return 2;
		}
		return ProbeDisplayPort(edid, train, video, window) ? 0 : 1;
	}
	if (argc == 3 && strcmp(argv[1], "--power") == 0) {
		if (strcmp(argv[2], "off") != 0 && strcmp(argv[2], "on") != 0) {
			fprintf(stderr, "usage: %s --power off|on\n", argv[0]);
			return 2;
		}
		return ChangePower(strcmp(argv[2], "off") == 0) ? 0 : 1;
	}
	unsigned hold = 10;
	if (scanout && argc == 3)
		hold = (unsigned)atoi(argv[2]);
	if (argc == 2 && !edid && !scanout && !accelerant)
		samples = (unsigned)atoi(argv[1]);
	if ((scanout ? argc > 3 || hold < 1 || hold > 120 : argc > 2) || samples < 1 || samples > 16) {
		fprintf(stderr, "usage: %s [samples 1-16 | --absent-device | --edid | --accelerant"
			" | --scanout [hold-seconds 1-120] | --mode WIDTHxHEIGHT | --power off|on"
			" | --cursor X Y | --cursor-hide | --cursor-restore | --dp [edid] [train]]\n", argv[0]);
		return 2;
	}
	// Only the accelerant profile admits writable handles; opening and
	// closing one has no side effect on the display.
	int writable = open(kDevice, O_RDWR);
	if (writable >= 0) {
		close(writable);
		printf("ROCK5_DISPLAY_WRITE_OPEN_ALLOWED\n");
	} else if (errno == EPERM) {
		printf("ROCK5_DISPLAY_WRITE_OPEN_REJECTED\n");
	} else {
		fprintf(stderr, "Writable open of %s failed unexpectedly (errno %d)\n", kDevice, errno);
		return 1;
	}
	if (accelerant) {
		if (writable < 0) {
			fprintf(stderr, "The accelerant profile must admit a writable handle\n");
			return 1;
		}
		bool passed = CheckAccelerant();
		return passed ? 0 : 1;
	}
	int fd = open(kDevice, O_RDONLY);
	if (fd < 0) {
		perror(kDevice);
		return 1;
	}
	ResourceInfo info = {};
	if (ioctl(fd, kGetResources, &info, sizeof(info) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed resource request was not rejected\n");
		return 1;
	}
	if (ioctl(fd, kGetResources, NULL, sizeof(info)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null resource output was not rejected\n");
		return 1;
	}
	if (ioctl(fd, kGetResources, &info, sizeof(info)) != 0) {
		perror("display resources");
		return 1;
	}
	if (!ReportResources(info))
		return 1;
	printf("ROCK5_DISPLAY_RESOURCE_DESCRIPTION_PASS\n");
	if (edid || scanout) {
		bool passed = edid ? ReadEdid(fd) : SwapScanout(fd, hold);
		close(fd);
		return passed ? 0 : 1;
	}
	DisplaySnapshot snapshot = {};
	if (ioctl(fd, kGetSnapshot, &snapshot, sizeof(snapshot) - 1) == 0 || errno != EINVAL) {
		fprintf(stderr, "Malformed snapshot request was not rejected\n");
		return 1;
	}
	if (ioctl(fd, kGetSnapshot, NULL, sizeof(snapshot)) == 0 || errno != EFAULT) {
		fprintf(stderr, "Null snapshot output was not rejected\n");
		return 1;
	}
	bool consistent = true;
	DisplaySnapshot first = {};
	for (unsigned i = 0; i < samples; i++) {
		memset(&snapshot, 0, sizeof(snapshot));
		if (ioctl(fd, kGetSnapshot, &snapshot, sizeof(snapshot)) != 0) {
			perror("display snapshot");
			return 1;
		}
		if (snapshot.version != kSnapshotVersion
			|| (snapshot.flags & kSnapshotReadOnly) == 0
			|| snapshot.finishedMicros < snapshot.startedMicros) {
			fprintf(stderr, "Invalid snapshot header\n");
			return 1;
		}
		ReportSnapshot(i, snapshot);
		if (i == 0)
			first = snapshot;
		else if (snapshot.flags != first.flags
			|| memcmp(snapshot.vopPort, first.vopPort, sizeof(first.vopPort)) != 0
			|| memcmp(snapshot.vopSystem, first.vopSystem, sizeof(first.vopSystem)) != 0
			|| memcmp(snapshot.pmu, first.pmu, sizeof(first.pmu)) != 0) {
			consistent = false;
		}
	}
	close(fd);
	printf("ROCK5_DISPLAY_OBSERVATION_PASS samples=%u register_writes=0 consistent=%u"
		" vop_read=%u hdmi_read=%u dp_read=%u gpio_read=%u\n", samples, consistent ? 1 : 0,
		(first.flags & kSnapshotVopRead) != 0, (first.flags & kSnapshotHdmiRead) != 0,
		(first.flags & kSnapshotDpRead) != 0, (first.flags & kSnapshotGpioRead) != 0);
	return 0;
}
