/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>
#include <syscalls.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>


static void
require(bool condition, const char* message)
{
	if (!condition) {
		fprintf(stderr, "UNRESERVE_FAIL %s\n", message);
		exit(1);
	}
}


static addr_t
reserve(size_t size)
{
	addr_t address = 0;
	require(_kern_reserve_address_range(&address, B_ANY_ADDRESS, size) == B_OK,
		"reserve range");
	return address;
}


static void
release(addr_t address, size_t size)
{
	require(_kern_unreserve_address_range(address, size) == B_OK,
		"unreserve range");
}


static bool
reserve_exact(addr_t address, size_t size)
{
	addr_t actual = address;
	status_t status = _kern_reserve_address_range(&actual, B_EXACT_ADDRESS, size);
	if (status == B_OK)
		require(actual == address, "exact reservation address");
	return status == B_OK;
}


static area_id
map(addr_t address, size_t size)
{
	void* base = (void*)address;
	area_id area = create_area("unreserve test live area", &base, B_EXACT_ADDRESS,
		size, B_NO_LOCK, B_READ_AREA | B_WRITE_AREA);
	require(area >= B_OK && (addr_t)base == address, "map reserved range");
	memset(base, 0x69, size);
	return area;
}


static void
verify(area_id area, addr_t address, size_t size)
{
	area_info info = {};
	require(get_area_info(area, &info) == B_OK && info.address == (void*)address
		&& info.size == size, "live area survives unreserve");
	const uint8* bytes = (const uint8*)address;
	for (size_t i = 0; i < size; i++)
		require(bytes[i] == 0x69, "live bytes survive unreserve");
}


int
main()
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	const size_t page = B_PAGE_SIZE;

	// With no real area before it, the first reservation must be visited.
	addr_t base = reserve(8 * page);
	release(base, 8 * page);
	require(reserve_exact(base, 8 * page), "single reservation released");
	release(base, 8 * page);
	puts("UNRESERVE single PASS");

	// This is the layout left by deleting a cloned bitmap area.
	base = reserve(8 * page);
	area_id area = map(base, 2 * page);
	require(delete_area(area) == B_OK, "delete prefix area");
	release(base, 8 * page);
	require(reserve_exact(base, 8 * page), "reservation after hole released");
	release(base, 8 * page);
	puts("UNRESERVE hole PASS");

	// Do not remove real areas while removing reservations on both sides.
	base = reserve(12 * page);
	area = map(base + 4 * page, 4 * page);
	release(base, 12 * page);
	verify(area, base + 4 * page, 4 * page);
	require(reserve_exact(base, 4 * page), "prefix reservation released");
	require(reserve_exact(base + 8 * page, 4 * page), "suffix reservation released");
	release(base, 12 * page);
	verify(area, base + 4 * page, 4 * page);
	require(delete_area(area) == B_OK, "delete middle area");
	puts("UNRESERVE live_middle PASS");

	// Only whole reservations contained in the requested range are removed.
	base = reserve(12 * page);
	area = map(base + 4 * page, 4 * page);
	release(base + page, 2 * page);
	require(!reserve_exact(base, 4 * page), "partly covered reservation retained");
	release(base, 4 * page);
	require(reserve_exact(base, 4 * page), "requested prefix released");
	require(!reserve_exact(base + 8 * page, 4 * page), "outside suffix retained");
	verify(area, base + 4 * page, 4 * page);
	release(base, 12 * page);
	require(delete_area(area) == B_OK, "delete boundary area");
	puts("UNRESERVE boundaries PASS");

	// Advancing after a removal must remain valid when the tree is rebalanced.
	base = reserve(12 * page);
	release(base, 12 * page);
	for (int i = 0; i < 3; i++)
		require(reserve_exact(base + i * 4 * page, 4 * page), "adjacent reservation");
	release(base, 12 * page);
	require(reserve_exact(base, 12 * page), "all adjacent reservations released");
	release(base, 12 * page);
	puts("UNRESERVE adjacent PASS");

	for (int i = 0; i < 1000; i++) {
		require(reserve_exact(base, 12 * page), "repeated reservation reuse");
		release(base, 12 * page);
	}
	puts("UNRESERVE_PASS cases=6 cycles=1000");
	return 0;
}
