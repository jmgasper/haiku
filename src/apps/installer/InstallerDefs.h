/*
 * Copyright 2013, Haiku, Inc.
 * Distributed under the terms of the MIT License.
 */
#ifndef INSTALLER_DEFS_H
#define INSTALLER_DEFS_H


#include <SupportDefs.h>


static const uint32 MSG_STATUS_MESSAGE = 'iSTM';
static const uint32 MSG_INSTALL_FINISHED = 'iIFN';
static const uint32 MSG_RESET = 'iRSI';
static const uint32 MSG_WRITE_BOOT_SECTOR = 'iWBS';

extern const char* const kPackagesDirectoryPath;
extern const char* const kSourcesDirectoryPath;

// Installing onto a whole disk: the disk gets a GUID partition map with an
// EFI system partition of this size and one BFS partition with this name.
extern const char* const kWholeDiskVolumeName;
static const off_t kEFISystemPartitionSize = 256LL * 1024 * 1024;


#endif	// INSTALLER_DEFS_H
