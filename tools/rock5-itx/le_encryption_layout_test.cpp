/* HCI LE Start Encryption fields from Bluetooth Core sample data. */
#include <bluetooth/HCI/btHCI_command.h>
#include <bluetooth/HCI/btHCI_event.h>

#include <ByteOrder.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>


int
main()
{
	const uint8 key[16] = {
		0xbf, 0x01, 0xfb, 0x9d, 0x4e, 0xf3, 0xbc, 0x36,
		0xd8, 0x74, 0xf5, 0x39, 0x41, 0x38, 0x68, 0x4c
	};
	const uint8 expected[31] = {
		0x19, 0x20, 0x1c, 0x00, 0x08,
		0x90, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab,
		0x74, 0x24,
		0xbf, 0x01, 0xfb, 0x9d, 0x4e, 0xf3, 0xbc, 0x36,
		0xd8, 0x74, 0xf5, 0x39, 0x41, 0x38, 0x68, 0x4c
	};
	hci_cp_le_start_encryption parameters = {};
	parameters.connection_handle = B_HOST_TO_LENDIAN_INT16(0x0800);
	const uint8 randomNumber[8] = {
		0x90, 0x78, 0x56, 0x34, 0x12, 0xef, 0xcd, 0xab
	};
	memcpy(parameters.random_number, randomNumber, sizeof(randomNumber));
	parameters.encrypted_diversifier = B_HOST_TO_LENDIAN_INT16(0x2474);
	memcpy(parameters.long_term_key, key, sizeof(key));
	uint16 opcode = PACK_OPCODE(OGF_LE_CONTROL, OCF_LE_START_ENCRYPTION);
	uint8 command[HCI_COMMAND_HDR_SIZE + sizeof(parameters)] = {
		(uint8)opcode, (uint8)(opcode >> 8), (uint8)sizeof(parameters)
	};
	memcpy(command + HCI_COMMAND_HDR_SIZE, &parameters, sizeof(parameters));
	bool layout = sizeof(parameters) == 28
		&& offsetof(hci_cp_le_start_encryption, random_number) == 2
		&& offsetof(hci_cp_le_start_encryption, encrypted_diversifier) == 10
		&& offsetof(hci_cp_le_start_encryption, long_term_key) == 12
		&& sizeof(hci_ev_encrypt_change) == 4
		&& memcmp(command, expected, sizeof(expected)) == 0;
	printf("le_start_encryption_layout=%s\n", layout ? "pass" : "fail");
	return layout ? 0 : 1;
}
