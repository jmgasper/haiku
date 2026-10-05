/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_HID_SERVICE_H_
#define _LE_HID_SERVICE_H_

#include <LEAttributeClient.h>


namespace Bluetooth {

struct LEHIDInputReport {
	uint16 valueHandle;
	uint16 configurationHandle;
	uint8 reportID;
};


// Discovers the HID service and its report-mode input characteristics on an
// encrypted ATT connection. The caller retains the ATT client and LE link.
class LEHIDService {
public:
	status_t Discover(LEAttributeClient& client);
	status_t EnableInputReports(LEAttributeClient& client) const;

	const std::vector<uint8>& ReportMap() const { return fReportMap; }
	const std::vector<LEHIDInputReport>& InputReports() const
		{ return fInputReports; }
	const LEHIDInputReport* FindInputReport(uint16 valueHandle) const;

private:
	std::vector<uint8> fReportMap;
	std::vector<LEHIDInputReport> fInputReports;
};

} // namespace Bluetooth

#endif
