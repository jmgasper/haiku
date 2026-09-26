/* Bounded LE advertising-data and mouse-identification fixture. */
#include <LEAdvertisingData.h>

#include <stdio.h>


int
main()
{
	const uint8_t mouse[] = {
		3, 0x08, 'M', 'X',
		3, 0x19, 0xc2, 0x03,
		5, 0x09, 'M', 'a', 's', 't',
		3, 0x03, 0x12, 0x18
	};
	LEAdvertisingData parsed = ParseLEAdvertisingData(mouse, sizeof(mouse));
	bool namedMouse = parsed.name == "Mast" && parsed.completeName
		&& parsed.appearance == 0x03c2 && parsed.advertisesHID;

	const uint8_t serviceData[] = { 4, 0x16, 0x12, 0x18, 0x01 };
	parsed = ParseLEAdvertisingData(serviceData, sizeof(serviceData));
	bool hidServiceData = parsed.advertisesHID && parsed.name.empty();

	const uint8_t malformed[] = {
		3, 0x08, 'M', 'X',
		10, 0x19, 0xc2
	};
	parsed = ParseLEAdvertisingData(malformed, sizeof(malformed));
	bool bounded = parsed.name == "MX" && !parsed.completeName
		&& parsed.appearance == 0 && !parsed.advertisesHID;
	parsed = ParseLEAdvertisingData(NULL, 10);
	bool nullData = parsed.name.empty() && parsed.appearance == 0
		&& !parsed.advertisesHID;

	printf("named_mouse=%s hid_service_data=%s bounded=%s null_data=%s\n",
		namedMouse ? "pass" : "fail", hidServiceData ? "pass" : "fail",
		bounded ? "pass" : "fail", nullData ? "pass" : "fail");
	return namedMouse && hidServiceData && bounded && nullData ? 0 : 1;
}
