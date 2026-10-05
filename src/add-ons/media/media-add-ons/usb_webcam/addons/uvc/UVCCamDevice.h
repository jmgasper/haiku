/*
 * Copyright 2011, Gabriel Hartmann, gabriel.hartmann@gmail.com.
 * Copyright 2011, Jérôme Duval, korli@users.berlios.de.
 * Copyright 2009, Ithamar Adema, <ithamar.adema@team-embedded.nl>.
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */
#ifndef _UVC_CAM_DEVICE_H
#define _UVC_CAM_DEVICE_H


#include "CamDevice.h"

#include <vector>


enum uvc_pixel_format {
	UVC_FORMAT_UNSUPPORTED = 0,
	UVC_FORMAT_YUY2,
	UVC_FORMAT_UYVY,
	UVC_FORMAT_NV12,
	UVC_FORMAT_I420,
	UVC_FORMAT_MJPEG
};


struct uvc_frame {
	uint8					index;
	uint16					width;
	uint16					height;
	uint32					maxFrameSize;
	uint32					defaultInterval;
		// frame intervals are in units of 100 ns
	uint32					minInterval;
	uint32					maxInterval;
	uint32					intervalStep;
		// those three for a continuous range, in which case intervals is empty
	std::vector<uint32>		intervals;
};


struct uvc_format {
	uint8					index;
	uvc_pixel_format		pixelFormat;
	std::vector<uvc_frame>	frames;
};


// What to ask the device for: one frame size of one format at one rate.
struct uvc_mode {
	const uvc_format*		format;
	const uvc_frame*		frame;
	uint32					interval;
	int64					score;
};


struct uvc_control {
	const char*				name;
	uint8					entity;
		// ID of the unit or terminal
	uint8					selector;
	uint8					size;
	uint8					kind;
	int32					minimum;
	int32					maximum;
	int32					onValue;
};


class UVCCamDevice : public CamDevice {
public:
								UVCCamDevice(CamDeviceAddon &_addon,
									BUSBDevice* _device);
	virtual						~UVCCamDevice();

	virtual void				Unplugged();

	virtual bool				SupportsBulk();
	virtual bool				SupportsIsochronous();
	virtual status_t			StartTransfer();
	virtual status_t			StopTransfer();
	virtual status_t			SuggestVideoFrame(uint32 &width,
									uint32 &height);
	virtual status_t			AcceptVideoFrame(uint32 &width,
									uint32 &height);
	virtual float				FrameRate();
	virtual void				AddParameters(BParameterGroup *group,
									int32 &index);
	virtual status_t			GetParameterValue(int32 id,
									bigtime_t *last_change, void *value,
									size_t *size);
	virtual status_t			SetParameterValue(int32 id, bigtime_t when,
									const void *value, size_t size);
	virtual status_t			FillFrameBuffer(BBuffer *buffer,
									bigtime_t *stamp = NULL);

	virtual status_t			DataPumpThread();

private:
			bool				_IsVideoInterface(
									const BUSBInterface* interface,
									uint8 subclass) const;
			void				_ParseVideoControl(const uint8* descriptor,
									size_t length);
			void				_ParseVideoStreaming(const uint8* descriptor,
									size_t length);
			void				_DumpFormats() const;

			void				_CollectModes(uint32 width, uint32 height,
									std::vector<uvc_mode>& modes) const;
			status_t			_Negotiate();
			status_t			_Probe(const uvc_mode& mode, uint8* probe,
									size_t& probeLength);
			int32				_FindAlternate(uint32 payloadSize,
									uint32& endpointIndex) const;
			BUSBInterface*		_StreamingInterface() const;

			void				_HandlePayload(const uint8* data,
									size_t length, bool error);
			void				_FinishFrame();

			bool				_Convert(const uint8* source, size_t length,
									uint8* destination, uint32 bytesPerRow,
									uint32 width, uint32 height);
			void				_ConvertYUV(const uint8* source,
									uint8* destination, uint32 bytesPerRow,
									uint32 width, uint32 height);
			bool				_ConvertMJPEG(const uint8* source,
									size_t length, uint8* destination,
									uint32 bytesPerRow, uint32 width,
									uint32 height);

			void				_AddControl(const char* name, uint8 entity,
									uint8 selector, uint8 size, uint8 kind);
			status_t			_ControlRequest(uint8 request,
									const uvc_control& control, int32& value);

private:
			bool				fVendorClass;
			uint16				fVersion;
			uint8				fControlInterface;
			uint8				fStreamingInterface;
			int32				fStreamingIndex;
			bool				fIsBulk;

			std::vector<uvc_format> fFormats;

			// what the consumer gets
			uint32				fOutputWidth;
			uint32				fOutputHeight;

			// what the device was told to send
			uvc_pixel_format	fCaptureFormat;
			uint32				fCaptureWidth;
			uint32				fCaptureHeight;
			uint32				fCaptureInterval;
			uint32				fMaxVideoFrameSize;
			uint32				fMaxPayloadTransferSize;
			size_t				fPacketSize;

			// the frame being put together by the pump thread
			uint8*				fAssembly;
			size_t				fAssemblyLength;
			size_t				fFrameCapacity;
			bool				fHaveFrameID;
			uint8				fFrameID;
			bool				fFrameDone;
			bool				fFrameBad;

			// the last complete frame, and the one being converted
			BLocker				fFrameLock;
			sem_id				fFrameSem;
			uint8*				fReady;
			size_t				fReadyLength;
			bigtime_t			fReadyStamp;
			uint32				fReadySequence;
			uint32				fDeliveredSequence;
			uint8*				fWork;

			uint32				fDroppedFrames;

			uint8				fProcessingUnit;
			uint8				fCameraTerminal;
			std::vector<uint8>	fProcessingControls;
			std::vector<uint8>	fCameraControls;
			std::vector<uvc_control> fControls;
};


class UVCCamDeviceAddon : public CamDeviceAddon {
public:
								UVCCamDeviceAddon(WebCamMediaAddOn* webcam);
	virtual 					~UVCCamDeviceAddon();

	virtual const char*			BrandName();
	virtual UVCCamDevice*		Instantiate(CamRoster &roster,
									BUSBDevice *from);
};

#endif /* _UVC_CAM_DEVICE_H */
