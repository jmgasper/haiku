/*
 * Copyright 2026 Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicode/ucal.h>
#include <unicode/uclean.h>
#include <unicode/uenum.h>
#include <unicode/unum.h>
#include <unicode/putil.h>


int
main(int argc, char** argv)
{
	bool expectMissing = argc == 2 && strcmp(argv[1], "--expect-missing") == 0;
	if (argc != 1 && !expectMissing) {
		fprintf(stderr, "Usage: %s [--expect-missing]\n", argv[0]);
		return 2;
	}
	printf("ROCK5_ICU_DATA directory=%s env=%s\n", u_getDataDirectory(),
		getenv("ICU_DATA") != NULL ? getenv("ICU_DATA") : "<unset>");
	UErrorCode initStatus = U_ZERO_ERROR;
	u_init(&initStatus);
	UErrorCode zoneStatus = U_ZERO_ERROR;
	UEnumeration* zones = ucal_openTimeZoneIDEnumeration(
		UCAL_ZONE_TYPE_CANONICAL, NULL, NULL, &zoneStatus);
	int32_t count = -1;
	if (zones != NULL && U_SUCCESS(zoneStatus))
		count = uenum_count(zones, &zoneStatus);
	uenum_close(zones);

	UErrorCode numberStatus = U_ZERO_ERROR;
	UNumberFormat* number = unum_open(UNUM_DECIMAL, NULL, 0, "en_US_POSIX",
		NULL, &numberStatus);
	UChar formatted[32] = {};
	int32_t length = -1;
	if (number != NULL && U_SUCCESS(numberStatus)) {
		unum_setAttribute(number, UNUM_GROUPING_USED, 0);
		length = unum_formatDouble(number, 1234.5, formatted, 32, NULL,
			&numberStatus);
	}
	unum_close(number);
	const UChar expected[] = {'1', '2', '3', '4', '.', '5'};
	bool numberMatches = U_SUCCESS(numberStatus) && length == 6
		&& memcmp(formatted, expected, sizeof(expected)) == 0;
	bool available = U_SUCCESS(initStatus) && U_SUCCESS(zoneStatus)
		&& count > 0 && numberMatches;
	bool missing = U_FAILURE(initStatus) && U_FAILURE(zoneStatus)
		&& count == -1 && !numberMatches;
	bool pass = expectMissing ? missing : available;
	printf("ROCK5_ICU_RESULT init=%s zones=%s count=%ld number=%s "
		"number_matches=%d expect_missing=%d status=%s\n",
		u_errorName(initStatus), u_errorName(zoneStatus), (long)count,
		u_errorName(numberStatus), numberMatches, expectMissing,
		pass ? "pass" : "fail");
	u_cleanup();
	return pass ? 0 : 1;
}
