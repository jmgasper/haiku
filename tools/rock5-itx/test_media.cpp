/* Actual compatibility baud-rate lookup and ifconfig display regression. */
#include <cassert>
#include <cstdint>
#include <cstring>

#include <freebsd_network/compat/net/if_media.h>
#include <MediaTypes.h>
#include "media_under_test.inc"

int
main()
{
	// The native RTL8125 value was incorrectly truncated to 10Base5.
	assert((IFM_ETHER | IFM_2500_T | IFM_FDX | IFM_ACTIVE) == 0x900825);
	assert(ifmedia_baudrate(0x900825) == 2500000000ULL);
	assert(ifmedia_baudrate(IFM_ETHER | IFM_5000_T | IFM_FDX) == 5000000000ULL);
	assert(ifmedia_baudrate(IFM_ETHER | IFM_1000_T | IFM_FDX) == 1000000000ULL);
	assert(ifmedia_baudrate(IFM_ETHER | IFM_10_5) == 10000000ULL);
	assert(ifmedia_baudrate(IFM_ETHER | IFM_X(510)) == 0);

	// Every declared rate retains its subtype with unrelated global flags.
	for (const auto& entry : ifmedia_baudrate_descriptions) {
		if (entry.ifmb_word == 0)
			break;
		assert(ifmedia_baudrate(entry.ifmb_word | IFM_FDX | IFM_LOOP)
			== entry.ifmb_baudrate);
	}

	assert(strcmp(media_type_to_string(0x900825), "2.5 GBit, 2500BASE-T") == 0);
	assert(strcmp(media_type_to_string(IFM_ETHER | IFM_5000_T | IFM_FDX),
		"5 GBit, 5000BASE-T") == 0);
	assert(strcmp(media_type_to_string(IFM_ETHER | IFM_1000_T | IFM_ACTIVE),
		"1 GBit, 1000BASE-T") == 0);
	assert(strcmp(media_type_to_string(IFM_ETHER | IFM_AUTO), "Auto-select") == 0);
	// An unlisted extended subtype must not alias the generic auto subtype.
	assert(media_type_to_string(IFM_ETHER | IFM_X(32)) == nullptr);
	assert(strcmp(media_type_to_string(IFM_IEEE80211 | IFM_IEEE80211_11NG),
		"802.11n(g)") == 0);
	assert(media_type_to_string(0) == nullptr);
	int subtype = -1;
	assert(media_parse_subtype("2500baseT", IFM_ETHER, &subtype));
	assert(subtype == IFM_2500_T);
	assert(media_parse_subtype("5000baseT", IFM_ETHER, &subtype));
	assert(subtype == IFM_5000_T);
	assert(!media_parse_subtype("2500baseT", IFM_IEEE80211, &subtype));
	return 0;
}
