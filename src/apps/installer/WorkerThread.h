/*
 * Copyright 2009, Stephan Aßmus <superstippi@gmx.de>.
 * Copyright 2005, Jérôme DUVAL.
 * All rights reserved. Distributed under the terms of the MIT License.
 */
#ifndef WORKER_THREAD_H
#define WORKER_THREAD_H

#include <DiskDevice.h>
#include <DiskDeviceRoster.h>
#include <Looper.h>
#include <Messenger.h>
#include <Partition.h>
#include <Volume.h>

class BList;
class BMenu;
class ProgressReporter;

class WorkerThread : public BLooper {
public:
								WorkerThread(const BMessenger& owner);

	virtual	void				MessageReceived(BMessage* message);

			void 				InstallEFILoader(partition_id id, bool rename);

			void				ScanDisksPartitions(BMenu* srcMenu,
									BMenu* dstMenu, BMenu* EFIMenu);

			void				SetPackagesList(BList* list);
			void				SetSpaceRequired(off_t bytes)
									{ fSpaceRequired = bytes; };

			bool				Cancel();
			void				SetLock(sem_id cancelSemaphore)
									{ fCancelSemaphore = cancelSemaphore; }

			void				StartInstall(partition_id sourcePartitionID,
									partition_id targetPartitionID,
									bool wholeDisk = false);
			void				WriteBootSector(BMenu* dstMenu);

private:
			status_t			_WriteBootSector(BPath& path);
			status_t			_LaunchFinishScript(BPath& path);

			status_t			_PerformInstall(partition_id sourcePartitionID,
									partition_id targetPartitionID,
									bool wholeDisk);
			status_t			_PrepareWholeDisk(partition_id diskID,
									partition_id& _bootID,
									partition_id& _espID);
			status_t			_InitializePartition(partition_id diskID,
									partition_id partitionID,
									const char* diskSystem, const char* name,
									const char* parameters);
			status_t			_InstallEFILoader(partition_id espID);
			status_t			_PrepareCleanInstall(
									const BPath& targetDirectory) const;
			status_t			_InstallationError(status_t error);
			status_t			_MirrorIndices(const BPath& srcDirectory,
									const BPath& targetDirectory) const;
			status_t			_CreateDefaultIndices(
									const BPath& targetDirectory) const;
			status_t			_ProcessZipPackages(const char* sourcePath,
									const char* targetPath,
									ProgressReporter* reporter,
									BList& unzipEngines);

			void				_SetStatusMessage(const char* status);

private:
			class EntryFilter;

private:
			BMessenger			fOwner;
			BDiskDeviceRoster	fDDRoster;
			BList*				fPackages;
			off_t				fSpaceRequired;
			sem_id				fCancelSemaphore;
};

#endif // WORKER_THREAD_H
