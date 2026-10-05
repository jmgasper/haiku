/*
 * Copyright 2026, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef _LE_LEGACY_PAIRING_CRYPTO_H_
#define _LE_LEGACY_PAIRING_CRYPTO_H_

#include <stdint.h>


namespace Bluetooth {

// Arrays are in Bluetooth wire order: least significant octet first.
// Pairing request and response include their SMP command octet.
bool LELegacyConfirm(const uint8_t tk[16], const uint8_t random[16],
	const uint8_t request[7], const uint8_t response[7],
	uint8_t initiatorAddressType, const uint8_t initiatorAddress[6],
	uint8_t responderAddressType, const uint8_t responderAddress[6],
	uint8_t confirm[16]);

bool LELegacyShortTermKey(const uint8_t tk[16],
	const uint8_t firstRandom[16], const uint8_t secondRandom[16],
	uint8_t key[16]);

// The IRK and address are in Bluetooth wire order (least significant octet
// first). Only resolvable private random addresses can match.
bool LEResolvePrivateAddress(const uint8_t irk[16],
	const uint8_t address[6]);

} // namespace Bluetooth

#endif
