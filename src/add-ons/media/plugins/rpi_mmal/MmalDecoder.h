/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MMAL_DECODER_H
#define MMAL_DECODER_H


#include <Locker.h>
#include <OS.h>
#include <String.h>

#include "MmalProtocol.h"


/*!	The firmware's H.264 decoder ("ril.video_decode") on the Raspberry Pi,
	through /dev/misc/vchiq. Input is the Annex B byte stream in pieces;
	pictures come out in display order as I420 (or NV12 should the firmware
	not give I420), in rows of Stride() bytes: SliceHeight() rows of luma,
	then the chroma, for I420 as two planes of half the size each way.

	One thread feeds and fetches; a thread of the object's own receives the
	firmware's messages.
*/
class MmalDecoder {
public:
	struct Frame {
		const uint8*	data;
		uint32			length;
		int64			pts;
		uint32			flags;
		int32			index;
	};

								MmalDecoder();
								~MmalDecoder();

			/*!	\a framed: the input comes in whole access units, the last
				piece of each flagged as the frame's end.
			*/
			status_t			Open(uint32 width, uint32 height, bool framed,
									BString* _error);
			void				Close();

			// the size of one piece of input at most
			uint32				InputSize() const { return fInputSize; }
			bool				CanSend() const;
			/*!	B_WOULD_BLOCK when the firmware holds all input buffers. */
			status_t			Send(const void* data, size_t size, int64 pts,
									uint32 flags);
			status_t			SendEndOfStream();

			/*!	B_OK with a frame (to be given back with ReleaseFrame()),
				B_TIMED_OUT, B_LAST_BUFFER_ERROR after the last picture of
				the stream, or what went wrong.
			*/
			status_t			NextFrame(Frame& frame, bigtime_t timeout);
			void				ReleaseFrame(const Frame& frame);

			/*!	Waits until input can be sent or a frame may be there. */
			void				Wait(bigtime_t timeout);

			/*!	Drops everything under way (a seek). */
			status_t			Flush();

			// the pictures; they can change before any frame
			uint32				Encoding() const { return fEncoding; }
			uint32				Width() const { return fWidth; }
			uint32				Height() const { return fHeight; }
			uint32				Stride() const { return fStride; }
			uint32				SliceHeight() const { return fSliceHeight; }

			const char*			Error() const { return fError.String(); }

private:
	enum {
		kMaxDecoders = 2,
		kMaxOutputBuffers = 8,
		kInputBuffers = 8,
			// pieces of input with the firmware at most
		kInputSpare = 4,
			// more than that in its pool: it says a piece is free a little
			// before it is
		kInputContext = 0x1000,
		kOutputContext = 0x2000
	};
	enum buffer_state {
		BUFFER_FREE,		// ours, to be sent
		BUFFER_SENT,		// the firmware has it
		BUFFER_READY,		// a picture in it, queued
		BUFFER_HELD			// the caller has it
	};
	struct OutputBuffer {
		uint8*			memory;
		buffer_state	state;
		uint32			length;
		uint32			flags;
		int64			pts;
	};
	struct Port {
		uint32				type;
		uint32				handle;
		mmal_port			port;
		mmal_es_format		format;
		mmal_video_format	es;
	};

			status_t			_Request(mmal_message& message, size_t size,
									size_t replySize);
			status_t			_Send(const void* message, size_t size);
			status_t			_GetPort(Port& port);
			status_t			_SetPort(Port& port);
			status_t			_PortAction(Port& port, uint32 action);
			status_t			_SetParameter(Port& port, uint32 id,
									uint32 value);
			status_t			_ConfigureOutput(
									const mmal_event_format_changed* event);
			status_t			_AllocateOutputBuffers();
			void				_FreeOutputBuffers();
			status_t			_SendOutputBuffer(int32 index);
			void				_SendFreeOutputBuffers();
			void				_UpdateGeometry();
			void				_Fail(const char* format, ...);

	static	int32				_ReaderEntry(void* self);
			void				_Reader();
			void				_BufferToHost(const mmal_message& message);
			void				_EventToHost(const mmal_message& message);

			port_id				fToken;
			int					fDevice;
			uint32				fService;
			uint32				fComponent;
			bool				fComponentEnabled;
			Port				fControl;
			Port				fInput;
			Port				fOutput;
			uint32				fInputSize;

			uint32				fEncoding;
			uint32				fWidth;
			uint32				fHeight;
			uint32				fStride;
			uint32				fSliceHeight;

			thread_id			fReader;
			volatile bool		fQuit;
			sem_id				fWakeSem;

			// one request at a time
			BLocker				fRequestLock;
			sem_id				fReplySem;
			uint32				fNextContext;
			volatile uint32		fRequestContext;
			mmal_message*		fReply;
			size_t				fReplySize;

			BLocker				fLock;
				// the buffers and what follows
			OutputBuffer		fBuffers[kMaxOutputBuffers];
			uint32				fBufferCount;
			uint32				fBufferSize;
			int32				fReady[kMaxOutputBuffers];
			uint32				fReadyCount;
			bool				fOutputEnabled;
			int32				fInputOutstanding;
			bool				fEndOfStream;
			bool				fFormatChanged;
			mmal_event_format_changed fNewFormat;
			status_t			fStatus;
			BString				fError;
			uint8*				fScratch;
			uint8*				fInputCopy;
};


#endif	// MMAL_DECODER_H
