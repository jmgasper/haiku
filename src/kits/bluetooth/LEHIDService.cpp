/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */

#include <LEHIDService.h>

#include <Errors.h>
#include <LELog.h>

#include <string.h>


namespace Bluetooth {

static const uint16 kHIDService = 0x1812;
static const uint16 kReportMapCharacteristic = 0x2a4b;
static const uint16 kReportCharacteristic = 0x2a4d;
static const uint16 kReportReferenceDescriptor = 0x2908;
static const uint16 kClientConfigurationDescriptor = 0x2902;
static const uint8 kInputReport = 1;
static const uint8 kNotifyProperty = 0x10;
static const size_t kMaximumReports = 32;

#define LOG(level, format, args...) LELog(level, "hid", format, ##args)


status_t
LEHIDService::Discover(LEAttributeClient& client)
{
	fReportMap.clear();
	fInputReports.clear();
	std::vector<LEPrimaryService> services;
	status_t status = client.DiscoverPrimaryServices(services);
	if (status != B_OK) {
		LOG(LE_LOG_ERROR, "service discovery failed: %s", strerror(status));
		return status;
	}
	for (const LEPrimaryService& service : services) {
		LOG(LE_LOG_DEBUG, "service %#06x handles %#x-%#x", service.uuid16,
			service.startHandle, service.endHandle);
	}
	for (const LEPrimaryService& service : services) {
		if (service.uuid16 != kHIDService)
			continue;
		std::vector<LECharacteristic> characteristics;
		status = client.DiscoverCharacteristics(service.startHandle,
			service.endHandle, characteristics);
		if (status != B_OK)
			return status;
		for (size_t i = 0; i < characteristics.size(); i++) {
			const LECharacteristic& characteristic = characteristics[i];
			if (characteristic.uuid16 == kReportMapCharacteristic) {
				if (!fReportMap.empty())
					return B_BAD_DATA;
				status = client.ReadAttribute(characteristic.valueHandle,
					fReportMap);
				if (status != B_OK) {
					LOG(LE_LOG_ERROR, "reading the report map failed: %s "
						"(ATT error %#x)", strerror(status),
						client.LastATTError());
					return status;
				}
				LELogHex(LE_LOG_DEBUG, "hid", "report map",
					fReportMap.data(), fReportMap.size());
				if (fReportMap.empty())
					return B_BAD_DATA;
			} else if (characteristic.uuid16 == kReportCharacteristic
				&& (characteristic.properties & kNotifyProperty) != 0) {
				if (fInputReports.size() >= kMaximumReports)
					return B_BAD_DATA;
				uint16 end = i + 1 < characteristics.size()
					? characteristics[i + 1].declarationHandle - 1
					: service.endHandle;
				if (characteristic.valueHandle >= end)
					return B_BAD_DATA;
				// Skip a malformed report rather than lose the whole device.
				uint16 reference = 0;
				status = client.FindDescriptor(characteristic.valueHandle + 1,
					end, kReportReferenceDescriptor, reference);
				if (status != B_OK) {
					LOG(LE_LOG_INFO, "report %#x has no reference descriptor "
						"(%s); skipped", characteristic.valueHandle,
						strerror(status));
					continue;
				}
				std::vector<uint8> value;
				status = client.ReadAttribute(reference, value);
				if (status != B_OK) {
					LOG(LE_LOG_ERROR, "reading report reference %#x failed: "
						"%s (ATT error %#x)", reference, strerror(status),
						client.LastATTError());
					if (status == B_DEV_NOT_READY)
						return status;
					continue;
				}
				if (value.size() != 2 || value[1] < 1 || value[1] > 3) {
					LOG(LE_LOG_INFO, "report %#x has a malformed reference; "
						"skipped", characteristic.valueHandle);
					continue;
				}
				LOG(LE_LOG_DEBUG, "report %#x: id %u type %u",
					characteristic.valueHandle, value[0], value[1]);
				if (value[1] != kInputReport)
					continue;
				uint16 configuration = 0;
				status = client.FindDescriptor(characteristic.valueHandle + 1,
					end, kClientConfigurationDescriptor, configuration);
				if (status != B_OK) {
					LOG(LE_LOG_INFO, "input report %u has no notification "
						"descriptor; skipped", value[0]);
					continue;
				}
				bool duplicate = false;
				for (const LEHIDInputReport& report : fInputReports) {
					if (report.reportID == value[0])
						duplicate = true;
				}
				if (duplicate) {
					LOG(LE_LOG_INFO, "duplicate input report id %u; skipped",
						value[0]);
					continue;
				}
				fInputReports.push_back({characteristic.valueHandle,
					configuration, value[0]});
			}
		}
		break;
	}
	return fReportMap.empty() || fInputReports.empty()
		? B_ENTRY_NOT_FOUND : B_OK;
}


status_t
LEHIDService::EnableInputReports(LEAttributeClient& client) const
{
	if (fInputReports.empty())
		return B_NO_INIT;
	const uint8 enabled[2] = { 1, 0 };
	for (const LEHIDInputReport& report : fInputReports) {
		status_t status = client.WriteAttribute(report.configurationHandle,
			enabled, sizeof(enabled));
		if (status != B_OK)
			return status;
	}
	return B_OK;
}


const LEHIDInputReport*
LEHIDService::FindInputReport(uint16 valueHandle) const
{
	for (const LEHIDInputReport& report : fInputReports) {
		if (report.valueHandle == valueHandle)
			return &report;
	}
	return NULL;
}

} // namespace Bluetooth
