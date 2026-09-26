/* Round-trip and corruption test using only synthetic Bluetooth keys. */
#include <LEBondStore.h>

#include <dirent.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


int
main()
{
	const uint8 local[6] = { 1, 2, 3, 4, 5, 6 };
	const uint8 peer[6] = { 7, 8, 9, 10, 11, 0xc0 };
#ifdef __HAIKU__
	char defaultDirectory[PATH_MAX];
	bool defaultPath = Bluetooth::DefaultLEBondDirectory(defaultDirectory,
		sizeof(defaultDirectory)) == B_OK
		&& strstr(defaultDirectory, "Bluetooth_LE_Bonds") != NULL;
#else
	bool defaultPath = true;
#endif
	Bluetooth::LELegacyBondKey original = {};
	original.hasLongTermKey = true;
	original.keySize = 12;
	original.encryptedDiversifier = 0x1234;
	original.hasIdentity = true;
	original.identityAddressType = 1;
	for (uint8 i = 0; i < 16; i++) {
		original.longTermKey[i] = i + 1;
		original.identityResolvingKey[i] = i + 0x80;
	}
	for (uint8 i = 0; i < 8; i++)
		original.randomNumber[i] = i + 0x20;
	for (uint8 i = 0; i < 6; i++)
		original.identityAddress[i] = i + 0x30;
	const char* root = getenv("TMPDIR");
	if (root == NULL || root[0] == '\0')
		root = "/tmp";
	char directory[PATH_MAX];
	if (snprintf(directory, sizeof(directory), "%s/le-bond-test-XXXXXX",
		root) >= (int)sizeof(directory) || mkdtemp(directory) == NULL) {
		printf("bond_fixture=fail\n");
		return 1;
	}
	bool saved = Bluetooth::SaveLEBond(directory, local, 0, peer, 1,
		original) == B_OK;
	char file[PATH_MAX] = {};
	DIR* listing = opendir(directory);
	if (listing != NULL) {
		struct dirent* entry;
		while ((entry = readdir(listing)) != NULL) {
			if (strstr(entry->d_name, ".bond") == NULL)
				continue;
			size_t directoryLength = strlen(directory);
			size_t nameLength = strlen(entry->d_name);
			if (directoryLength + nameLength + 2 >= sizeof(file))
				break;
			memcpy(file, directory, directoryLength);
			file[directoryLength] = '/';
			memcpy(file + directoryLength + 1, entry->d_name,
				nameLength + 1);
			break;
		}
		closedir(listing);
	}
	struct stat info;
	saved = saved && file[0] != '\0' && stat(file, &info) == 0
		&& (info.st_mode & 0777) == 0600;
	Bluetooth::LELegacyBondKey restored = {};
	bool loaded = Bluetooth::LoadLEBond(directory, local, 0, peer, 1,
		restored) == B_OK && restored.hasLongTermKey && restored.hasIdentity
		&& restored.keySize == 12
		&& restored.encryptedDiversifier == 0x1234
		&& restored.identityAddressType == 1
		&& memcmp(restored.longTermKey, original.longTermKey, 12) == 0
		&& restored.longTermKey[12] == 0 && restored.longTermKey[15] == 0
		&& memcmp(restored.randomNumber, original.randomNumber, 8) == 0
		&& memcmp(restored.identityResolvingKey,
			original.identityResolvingKey, 16) == 0
		&& memcmp(restored.identityAddress,
			original.identityAddress, 6) == 0;
	std::vector<Bluetooth::LEHIDMouseDevice> mice;
	bool mouseMarker = Bluetooth::SaveLEHIDMouse(directory, local, peer, 1)
		== B_OK && Bluetooth::ListLEHIDMice(directory, mice) == B_OK
		&& mice.size() == 1
		&& memcmp(mice[0].localAddress, local, 6) == 0
		&& memcmp(mice[0].peerAddress, peer, 6) == 0
		&& mice[0].peerAddressType == 1;
	bool permissions = chmod(file, 0644) == 0
		&& Bluetooth::LoadLEBond(directory, local, 0, peer, 1,
			restored) == B_PERMISSION_DENIED
		&& !restored.hasLongTermKey;
	if (file[0] != '\0')
		chmod(file, 0600);
	bool corruption = false;
	int descriptor = open(file, O_RDWR);
	if (descriptor >= 0) {
		uint8 byte;
		if (lseek(descriptor, 31, SEEK_SET) == 31
			&& read(descriptor, &byte, 1) == 1
			&& lseek(descriptor, 31, SEEK_SET) == 31) {
			byte ^= 1;
			corruption = write(descriptor, &byte, 1) == 1;
		}
		close(descriptor);
	}
	corruption = corruption
		&& Bluetooth::LoadLEBond(directory, local, 0, peer, 1,
			restored) == B_BAD_DATA
		&& !restored.hasLongTermKey;
	bool removed = Bluetooth::RemoveLEBond(directory, local, 0, peer, 1)
		== B_OK
		&& Bluetooth::LoadLEBond(directory, local, 0, peer, 1,
			restored) == B_ENTRY_NOT_FOUND
		&& Bluetooth::ListLEHIDMice(directory, mice) == B_OK
		&& mice.empty();
	if (file[0] != '\0')
		unlink(file);
	rmdir(directory);
	printf("bond_path=%s bond_save=%s bond_load=%s mouse_marker=%s bond_permissions=%s "
		"bond_corruption=%s bond_remove=%s\n",
		defaultPath ? "pass" : "fail",
		saved ? "pass" : "fail", loaded ? "pass" : "fail",
		mouseMarker ? "pass" : "fail",
		permissions ? "pass" : "fail",
		corruption ? "pass" : "fail", removed ? "pass" : "fail");
	return defaultPath && saved && loaded && mouseMarker && permissions && corruption
		&& removed ? 0 : 1;
}
