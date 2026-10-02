/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef NETWORK_SHARES_H
#define NETWORK_SHARES_H


#include <Locker.h>
#include <Message.h>
#include <ObjectList.h>
#include <String.h>


/*!	The shares of other machines that are mounted here: which there are, and
	mounting them, in particular when the system starts.

	Mounting means waiting for a server, which may not be there, and so does
	everything else that ends up asking the network. All of that is done by a
	thread of its own, and those who asked get their reply from there.
*/
class NetworkShares {
public:
								NetworkShares();
								~NetworkShares();

			status_t			Init();

			void				MountAtStartup();
			void				NetworkChanged();
			void				VolumeUnmounted(dev_t device);

			bool				HandleMessage(BMessage* message);

private:
			struct Share;
			struct Job;

			void				_GetShares(BMessage& reply);
			status_t			_SetShare(const BMessage& message, int32& _id);
			status_t			_RemoveShare(int32 id);

			void				_AddJob(uint32 what, int32 id,
									BMessage* request);
			void				_Reply(BMessage* request, status_t error,
									int32 id = -1);

	static	status_t			_WorkerEntry(void* data);
			void				_Worker();
			void				_Retry();
			void				_CheckPasswords();
			status_t			_Mount(int32 id, bool byUser);
			status_t			_Unmount(int32 id);

			Share*				_ShareFor(int32 id) const;
			void				_ToMessage(const Share& share,
									BMessage& message) const;

			status_t			_ReadSettings();
			status_t			_WriteSettings();

	static	bool				_FindVolume(const Share& share, dev_t& _device,
									BString& _mountPoint);
	static	status_t			_CreateMountPoint(const Share& share,
									BString& _path);
	static	BString				_KeyIdentifier(const Share& share);
	static	status_t			_GetPassword(const Share& share,
									BString& _password);
	static	status_t			_HasPassword(const Share& share);
	static	status_t			_SetPassword(const Share& share,
									const char* password);
	static	bool				_IsWorthTryingAgain(status_t error);
	static	void				_Notify(const Share& share, status_t error);

private:
	mutable	BLocker				fLock;
			BObjectList<Share, true> fShares;
			BObjectList<Job, true> fJobs;
			int32				fNextID;
			thread_id			fWorker;
			sem_id				fWakeUp;
			bigtime_t			fRetryUntil;
};


#endif	// NETWORK_SHARES_H
