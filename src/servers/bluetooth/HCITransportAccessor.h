/*
 * Copyright 2007-2008 Oliver Ruiz Dorantes, oliver.ruiz.dorantes_at_gmail.com
 *
 * All rights reserved. Distributed under the terms of the MIT License.
 *
 */


#ifndef _HCITRANSPORT_ACCESSOR_H_
#define _HCITRANSPORT_ACCESSOR_H_

#include "HCIDelegate.h"

#include <Locker.h>
#include <OS.h>

#include <deque>
#include <vector>


class HCITransportAccessor : public HCIDelegate {

	public:
		HCITransportAccessor(BPath* path);
		~HCITransportAccessor();
		status_t IssueCommand(raw_command rc, size_t size);
		status_t Launch();
		void CommandCredits(uint8 credits);
		void Pulse();
	private:
		status_t _Send(const uint8* command, size_t size);
		void _Drain();

		int fDescriptor;
		// Commands wait here while the controller has no command credits,
		// so concurrent clients cannot overrun it (Core Vol 4 Part E 4.4).
		BLocker fQueueLock;
		std::deque<std::vector<uint8> > fQueue;
		int32 fCredits;
		int32 fStalls;
		bigtime_t fLastSent;
};

#endif
