/*
 * Copyright 2011, Gabriel Hartmann, gabriel.hartmann@gmail.com.
 * Copyright 2011, Jérôme Duval, korli@users.berlios.de.
 * Copyright 2009, Ithamar Adema, <ithamar.adema@team-embedded.nl>.
 * Copyright 2026, air/OS.
 * Distributed under the terms of the MIT License.
 */

/*!	USB Video Class cameras.

	The camera is asked for the frame size closest to what the consumer of
	the node wants, uncompressed (YUY2, UYVY, NV12, I420) or Motion-JPEG,
	whichever is faster at that size. What arrives is converted to B_RGB32
	and, should the camera not have the size asked for, cropped and scaled.

	Isochronous cameras are read through an isochronous stream of the USB
	Kit: a camera sends a packet every 125 µs, and one transfer at a time
	from userland would lose those in between.
*/


#include "UVCCamDevice.h"

#include <algorithm>
#include <new>
#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <Autolock.h>
#include <ParameterWeb.h>
#include <media/Buffer.h>
#include <usb/USB_video.h>

#ifdef HAVE_LIBJPEG
extern "C" {
#	include <jpeglib.h>
}
#endif


//#define TRACE_UVC
#ifdef TRACE_UVC
#	define TRACE(x...)	printf("UVC: " x)
#else
#	define TRACE(x...)	;
#endif
#define ERROR(x...)		fprintf(stderr, "UVC: " x)


usb_webcam_support_descriptor kSupportedDevices[] = {
	// Any device with a Video Control interface...
	{{ USB_VIDEO_DEVICE_CLASS, USB_VIDEO_INTERFACE_VIDEOCONTROL_SUBCLASS, 0,
		0, 0 }, "Generic UVC", "Video Class", "??" },
	// ...and those that follow the class without saying so: their interfaces
	// have the vendor specific class. (The list is the one of Linux.)
	{{ 0, 0, 0, 0x045e, 0x00f8, }, "Microsoft", "Lifecam NX-6000", "??" },
	{{ 0, 0, 0, 0x045e, 0x0723, }, "Microsoft", "Lifecam VX-7000", "??" },
	{{ 0, 0, 0, 0x046d, 0x08c1, }, "Logitech", "QuickCam Fusion", "??" },
	{{ 0, 0, 0, 0x046d, 0x08c2, }, "Logitech", "QuickCam Orbit MP", "??" },
	{{ 0, 0, 0, 0x046d, 0x08c3, }, "Logitech", "QuickCam Pro for Notebook",
		"??" },
	{{ 0, 0, 0, 0x046d, 0x08c5, }, "Logitech", "QuickCam Pro 5000", "??" },
	{{ 0, 0, 0, 0x046d, 0x08c6, }, "Logitech", "QuickCam OEM Dell Notebook",
		"??" },
	{{ 0, 0, 0, 0x046d, 0x08c7, }, "Logitech",
		"QuickCam OEM Cisco VT Camera II", "??" },
	{{ 0, 0, 0, 0x05ac, 0x8501, }, "Apple", "Built-In iSight", "??" },
	{{ 0, 0, 0, 0x05e3, 0x0505, }, "Genesys Logic", "USB 2.0 PC Camera",
		"??" },
	{{ 0, 0, 0, 0x0e8d, 0x0004, }, "N/A", "MT6227", "??" },
	{{ 0, 0, 0, 0x174f, 0x5212, }, "Syntek", "(HP Spartan)", "??" },
	{{ 0, 0, 0, 0x174f, 0x5931, }, "Syntek", "(Samsung Q310)", "??" },
	{{ 0, 0, 0, 0x174f, 0x8a31, }, "Syntek", "Asus F9SG", "??" },
	{{ 0, 0, 0, 0x174f, 0x8a33, }, "Syntek", "Asus U3S", "??" },
	{{ 0, 0, 0, 0x17ef, 0x480b, }, "N/A", "Lenovo Thinkpad SL500", "??" },
	{{ 0, 0, 0, 0x18cd, 0xcafe, }, "Ecamm", "Pico iMage", "??" },
	{{ 0, 0, 0, 0x19ab, 0x1000, }, "Bodelin", "ProScopeHR", "??" },
	{{ 0, 0, 0, 0x1c4f, 0x3000, }, "SiGma Micro", "USB Web Camera", "??" },
	{{ 0, 0, 0, 0, 0}, NULL, NULL, NULL }
};


// The GUID of an uncompressed format is its FOURCC followed by this.
static const uint8 kGuidTail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00,
	0xaa, 0x00, 0x38, 0x9b, 0x71};

static const uint32 kRequestTypeSet
	= USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE_OUT;
static const uint32 kRequestTypeGet
	= USB_REQTYPE_CLASS | USB_REQTYPE_INTERFACE_IN;

// payload header bits
static const uint8 kHeaderFrameID = 0x01;
static const uint8 kHeaderEndOfFrame = 0x02;
static const uint8 kHeaderError = 0x40;

// 30 frames per second, in units of 100 ns
static const uint32 kPreferredInterval = 333333;

static const bigtime_t kFrameTimeout = 500000;

enum {
	CONTROL_RANGE,
	CONTROL_BOOLEAN,
	CONTROL_POWER_LINE,
	CONTROL_AUTO_EXPOSURE
};


static inline uint16
get16(const uint8* data)
{
	return data[0] | (data[1] << 8);
}


static inline uint32
get32(const uint8* data)
{
	return data[0] | (data[1] << 8) | (data[2] << 16) | ((uint32)data[3] << 24);
}


static inline void
set32(uint8* data, uint32 value)
{
	data[0] = value;
	data[1] = value >> 8;
	data[2] = value >> 16;
	data[3] = value >> 24;
}


static const char*
format_name(uvc_pixel_format format)
{
	switch (format) {
		case UVC_FORMAT_YUY2:
			return "YUY2";
		case UVC_FORMAT_UYVY:
			return "UYVY";
		case UVC_FORMAT_NV12:
			return "NV12";
		case UVC_FORMAT_I420:
			return "I420";
		case UVC_FORMAT_MJPEG:
			return "MJPEG";
		default:
			return "unsupported";
	}
}


static size_t
uncompressed_frame_size(uvc_pixel_format format, uint32 width, uint32 height)
{
	switch (format) {
		case UVC_FORMAT_YUY2:
		case UVC_FORMAT_UYVY:
			return (size_t)width * height * 2;
		case UVC_FORMAT_NV12:
		case UVC_FORMAT_I420:
			return (size_t)width * height * 3 / 2;
		default:
			return 0;
	}
}


/*!	The part of a source picture that has the proportions of the destination,
	taken from its middle.
*/
static void
crop_for(uint32 sourceWidth, uint32 sourceHeight, uint32 width, uint32 height,
	uint32& left, uint32& top, uint32& cropWidth, uint32& cropHeight)
{
	if ((uint64)sourceWidth * height > (uint64)sourceHeight * width) {
		cropHeight = sourceHeight;
		cropWidth = (uint64)sourceHeight * width / height;
	} else {
		cropWidth = sourceWidth;
		cropHeight = (uint64)sourceWidth * height / width;
	}
	if (cropWidth == 0)
		cropWidth = 1;
	if (cropHeight == 0)
		cropHeight = 1;

	// even, so that the chroma samples stay where they are
	left = ((sourceWidth - cropWidth) / 2) & ~1;
	top = ((sourceHeight - cropHeight) / 2) & ~1;
}


static inline uint8
clamp8(int32 value)
{
	return value < 0 ? 0 : (value > 255 ? 255 : value);
}


static inline void
store_pixel(uint8* destination, int32 y, int32 u, int32 v)
{
	// ITU-R BT.601, video range
	const int32 c = 298 * (y - 16) + 128;
	const int32 d = u - 128;
	const int32 e = v - 128;
	destination[0] = clamp8((c + 516 * d) >> 8);
	destination[1] = clamp8((c - 100 * d - 208 * e) >> 8);
	destination[2] = clamp8((c + 409 * e) >> 8);
	destination[3] = 255;
}


//	#pragma mark - Motion-JPEG


#ifdef HAVE_LIBJPEG

struct jpeg_error_jump {
	jpeg_error_mgr	manager;
	jmp_buf			jump;
};


static void
jpeg_error_exit(j_common_ptr info)
{
	longjmp(((jpeg_error_jump*)info->err)->jump, 1);
}


static void
jpeg_output_nothing(j_common_ptr info)
{
}


static void
jpeg_add_huffman_table(j_decompress_ptr info, JHUFF_TBL** _table,
	const uint8* bits, const uint8* values, size_t valueCount)
{
	if (*_table != NULL)
		return;

	*_table = jpeg_alloc_huff_table((j_common_ptr)info);
	memcpy((*_table)->bits, bits, 17);
	memcpy((*_table)->huffval, values, valueCount);
}


/*!	Cameras leave the Huffman tables out of their pictures: every one of them
	uses those of the JPEG standard (K.3).
*/
static void
jpeg_add_standard_tables(j_decompress_ptr info)
{
	static const uint8 kLuminanceDCBits[17]
		= {0, 0, 1, 5, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0};
	static const uint8 kChrominanceDCBits[17]
		= {0, 0, 3, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 0, 0, 0};
	static const uint8 kDCValues[12]
		= {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11};

	static const uint8 kLuminanceACBits[17]
		= {0, 0, 2, 1, 3, 3, 2, 4, 3, 5, 5, 4, 4, 0, 0, 1, 0x7d};
	static const uint8 kLuminanceACValues[162] = {
		0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05, 0x12,
		0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07,
		0x22, 0x71, 0x14, 0x32, 0x81, 0x91, 0xa1, 0x08,
		0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1, 0xf0,
		0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16,
		0x17, 0x18, 0x19, 0x1a, 0x25, 0x26, 0x27, 0x28,
		0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39,
		0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49,
		0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59,
		0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68, 0x69,
		0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79,
		0x7a, 0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89,
		0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97, 0x98,
		0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7,
		0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6,
		0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4, 0xc5,
		0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4,
		0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe1, 0xe2,
		0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9, 0xea,
		0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
		0xf9, 0xfa
	};

	static const uint8 kChrominanceACBits[17]
		= {0, 0, 2, 1, 2, 4, 4, 3, 4, 7, 5, 4, 4, 0, 1, 2, 0x77};
	static const uint8 kChrominanceACValues[162] = {
		0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05, 0x21,
		0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71,
		0x13, 0x22, 0x32, 0x81, 0x08, 0x14, 0x42, 0x91,
		0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52, 0xf0,
		0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34,
		0xe1, 0x25, 0xf1, 0x17, 0x18, 0x19, 0x1a, 0x26,
		0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37, 0x38,
		0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48,
		0x49, 0x4a, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58,
		0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
		0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78,
		0x79, 0x7a, 0x82, 0x83, 0x84, 0x85, 0x86, 0x87,
		0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96,
		0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5,
		0xa6, 0xa7, 0xa8, 0xa9, 0xaa, 0xb2, 0xb3, 0xb4,
		0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3,
		0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2,
		0xd3, 0xd4, 0xd5, 0xd6, 0xd7, 0xd8, 0xd9, 0xda,
		0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
		0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8,
		0xf9, 0xfa
	};

	jpeg_add_huffman_table(info, &info->dc_huff_tbl_ptrs[0], kLuminanceDCBits,
		kDCValues, sizeof(kDCValues));
	jpeg_add_huffman_table(info, &info->dc_huff_tbl_ptrs[1],
		kChrominanceDCBits, kDCValues, sizeof(kDCValues));
	jpeg_add_huffman_table(info, &info->ac_huff_tbl_ptrs[0], kLuminanceACBits,
		kLuminanceACValues, sizeof(kLuminanceACValues));
	jpeg_add_huffman_table(info, &info->ac_huff_tbl_ptrs[1],
		kChrominanceACBits, kChrominanceACValues,
		sizeof(kChrominanceACValues));
}

#endif	// HAVE_LIBJPEG


//	#pragma mark - UVCCamDevice


UVCCamDevice::UVCCamDevice(CamDeviceAddon& _addon, BUSBDevice* _device)
	: CamDevice(_addon, _device),
	fVendorClass(false),
	fVersion(0x0100),
	fControlInterface(0),
	fStreamingInterface(0),
	fStreamingIndex(-1),
	fIsBulk(false),
	fOutputWidth(320),
	fOutputHeight(240),
	fCaptureFormat(UVC_FORMAT_UNSUPPORTED),
	fCaptureWidth(0),
	fCaptureHeight(0),
	fCaptureInterval(kPreferredInterval),
	fMaxVideoFrameSize(0),
	fMaxPayloadTransferSize(0),
	fPacketSize(0),
	fAssembly(NULL),
	fAssemblyLength(0),
	fFrameCapacity(0),
	fHaveFrameID(false),
	fFrameID(0),
	fFrameDone(false),
	fFrameBad(false),
	fFrameLock("UVC frame"),
	fFrameSem(-1),
	fReady(NULL),
	fReadyLength(0),
	fReadyStamp(0),
	fReadySequence(0),
	fDeliveredSequence(0),
	fWork(NULL),
	fDroppedFrames(0),
	fProcessingUnit(0),
	fCameraTerminal(0)
{
	// The devices matched by their ID use the vendor specific class.
	fVendorClass = fSupportedDeviceIndex > 0;

	fFrameSem = create_sem(0, "UVC frame");

	// Only look at the active configuration: setting it again would take
	// the camera's microphone away from the audio driver.
	const BUSBConfiguration* config = _device->ActiveConfiguration();
	if (config == NULL)
		return;

	bool haveControl = false;
	uint8 buffer[1024];
	usb_descriptor* generic = (usb_descriptor*)buffer;

	for (uint32 i = 0; i < config->CountInterfaces(); i++) {
		const BUSBInterface* interface = config->InterfaceAt(i);
		if (interface == NULL)
			continue;

		if (!haveControl && _IsVideoInterface(interface,
				USB_VIDEO_INTERFACE_VIDEOCONTROL_SUBCLASS)) {
			haveControl = true;
			fControlInterface = interface->Descriptor()->interface_number;

			for (uint32 k = 0; interface->OtherDescriptorAt(k, generic,
					sizeof(buffer)) == B_OK; k++) {
				if (generic->generic.descriptor_type != USB_VIDEO_CS_INTERFACE
					|| generic->generic.length < 3) {
					continue;
				}
				_ParseVideoControl(buffer, generic->generic.length);
			}
		} else if (fStreamingIndex < 0 && _IsVideoInterface(interface,
				USB_VIDEO_INTERFACE_VIDEOSTREAMING_SUBCLASS)) {
			fStreamingIndex = i;
			fStreamingInterface = interface->Descriptor()->interface_number;

			// The class specific descriptors come with the first alternate.
			const BUSBInterface* first = interface->AlternateAt(0);
			if (first == NULL)
				first = interface;
			for (uint32 k = 0; first->OtherDescriptorAt(k, generic,
					sizeof(buffer)) == B_OK; k++) {
				if (generic->generic.descriptor_type != USB_VIDEO_CS_INTERFACE
					|| generic->generic.length < 3) {
					continue;
				}
				_ParseVideoStreaming(buffer, generic->generic.length);
			}

			// A camera with a bulk endpoint has it in the first alternate;
			// the isochronous ones have none there.
			for (uint32 k = 0; k < first->CountEndpoints(); k++) {
				const BUSBEndpoint* endpoint = first->EndpointAt(k);
				if (endpoint != NULL && endpoint->IsBulk()
					&& endpoint->IsInput()) {
					fIsBulk = true;
					break;
				}
			}
		}
	}

	// drop the formats that turned out to have no frames
	for (int32 i = fFormats.size() - 1; i >= 0; i--) {
		if (fFormats[i].pixelFormat == UVC_FORMAT_UNSUPPORTED
			|| fFormats[i].frames.empty()) {
			fFormats.erase(fFormats.begin() + i);
		}
	}

	_DumpFormats();

	if (!haveControl || fStreamingIndex < 0) {
		ERROR("no video control or streaming interface\n");
		return;
	}
	if (fFormats.empty()) {
		ERROR("the camera has no format we can use\n");
		return;
	}

	// call the camera what it calls itself
	BString product(_device->ProductString());
	product.Trim();
	if (product.Length() > 0) {
		BString manufacturer(_device->ManufacturerString());
		manufacturer.Trim();

		fFlavorInfoNameStr = product;
		fFlavorInfoNameStr.Truncate(B_MEDIA_NAME_LENGTH - 1);
		fFlavorInfoInfoStr = "";
		if (manufacturer.Length() > 0
			&& product.FindFirst(manufacturer) != 0) {
			fFlavorInfoInfoStr << manufacturer << " ";
		}
		fFlavorInfoInfoStr << product << " (USB Video Class)";
		fFlavorInfo.name = fFlavorInfoNameStr.String();
		fFlavorInfo.info = fFlavorInfoInfoStr.String();
	}

	// the controls of the picture...
	static const struct {
		uint8		bit;
		uint8		selector;
		uint8		size;
		uint8		kind;
		const char*	name;
	} kProcessingControls[] = {
		{0, USB_VIDEO_PU_BRIGHTNESS_CONTROL, 2, CONTROL_RANGE, "Brightness"},
		{1, USB_VIDEO_PU_CONTRAST_CONTROL, 2, CONTROL_RANGE, "Contrast"},
		{3, USB_VIDEO_PU_SATURATION_CONTROL, 2, CONTROL_RANGE, "Saturation"},
		{2, USB_VIDEO_PU_HUE_CONTROL, 2, CONTROL_RANGE, "Hue"},
		{11, USB_VIDEO_PU_HUE_AUTO_CONTROL, 1, CONTROL_BOOLEAN,
			"Automatic hue"},
		{4, USB_VIDEO_PU_SHARPNESS_CONTROL, 2, CONTROL_RANGE, "Sharpness"},
		{5, USB_VIDEO_PU_GAMMA_CONTROL, 2, CONTROL_RANGE, "Gamma"},
		{12, USB_VIDEO_PU_WHITE_BALANCE_TEMPERATURE_AUTO_CONTROL, 1,
			CONTROL_BOOLEAN, "Automatic white balance"},
		{6, USB_VIDEO_PU_WHITE_BALANCE_TEMPERATURE_CONTROL, 2, CONTROL_RANGE,
			"White balance temperature"},
		{8, USB_VIDEO_PU_BACKLIGHT_COMPENSATION_CONTROL, 2, CONTROL_RANGE,
			"Backlight compensation"},
		{9, USB_VIDEO_PU_GAIN_CONTROL, 2, CONTROL_RANGE, "Gain"},
		{10, USB_VIDEO_PU_POWER_LINE_FREQUENCY_CONTROL, 1, CONTROL_POWER_LINE,
			"Power line frequency"}
	};
	// ...and those of the camera itself
	static const struct {
		uint8		bit;
		uint8		selector;
		uint8		size;
		uint8		kind;
		const char*	name;
	} kCameraControls[] = {
		{1, USB_VIDEO_CT_AE_MODE_CONTROL, 1, CONTROL_AUTO_EXPOSURE,
			"Automatic exposure"},
		{3, USB_VIDEO_CT_EXPOSURE_TIME_ABSOLUTE_CONTROL, 4, CONTROL_RANGE,
			"Exposure time"},
		{17, USB_VIDEO_CT_FOCUS_AUTO_CONTROL, 1, CONTROL_BOOLEAN,
			"Automatic focus"},
		{5, USB_VIDEO_CT_FOCUS_ABSOLUTE_CONTROL, 2, CONTROL_RANGE, "Focus"},
		{9, USB_VIDEO_CT_ZOOM_ABSOLUTE_CONTROL, 2, CONTROL_RANGE, "Zoom"}
	};

	for (size_t i = 0; i < B_COUNT_OF(kProcessingControls); i++) {
		const uint8 bit = kProcessingControls[i].bit;
		if (bit / 8 >= fProcessingControls.size()
			|| (fProcessingControls[bit / 8] & (1 << (bit % 8))) == 0) {
			continue;
		}
		_AddControl(kProcessingControls[i].name, fProcessingUnit,
			kProcessingControls[i].selector, kProcessingControls[i].size,
			kProcessingControls[i].kind);
	}
	for (size_t i = 0; i < B_COUNT_OF(kCameraControls); i++) {
		const uint8 bit = kCameraControls[i].bit;
		if (bit / 8 >= fCameraControls.size()
			|| (fCameraControls[bit / 8] & (1 << (bit % 8))) == 0) {
			continue;
		}
		_AddControl(kCameraControls[i].name, fCameraTerminal,
			kCameraControls[i].selector, kCameraControls[i].size,
			kCameraControls[i].kind);
	}

	fInitStatus = B_OK;
}


UVCCamDevice::~UVCCamDevice()
{
	free(fAssembly);
	free(fReady);
	free(fWork);
	delete_sem(fFrameSem);
}


void
UVCCamDevice::Unplugged()
{
	// The endpoints go away with the device: make sure the pump thread is
	// done with them before that.
	fLocker.Lock();
	if (fTransferEnabled)
		CamDevice::StopTransfer();
	CamDevice::Unplugged();
	fLocker.Unlock();
}


bool
UVCCamDevice::SupportsBulk()
{
	return fIsBulk;
}


bool
UVCCamDevice::SupportsIsochronous()
{
	return !fIsBulk;
}


bool
UVCCamDevice::_IsVideoInterface(const BUSBInterface* interface,
	uint8 subclass) const
{
	if (interface->Subclass() != subclass)
		return false;

	return interface->Class() == USB_VIDEO_DEVICE_CLASS
		|| (fVendorClass && interface->Class() == 0xff);
}


BUSBInterface*
UVCCamDevice::_StreamingInterface() const
{
	if (fDevice == NULL || fStreamingIndex < 0)
		return NULL;

	const BUSBConfiguration* config = fDevice->ActiveConfiguration();
	if (config == NULL)
		return NULL;

	return const_cast<BUSBInterface*>(config->InterfaceAt(fStreamingIndex));
}


//	#pragma mark - descriptors


void
UVCCamDevice::_ParseVideoControl(const uint8* descriptor, size_t length)
{
	switch (descriptor[2]) {
		case USB_VIDEO_VC_HEADER:
			if (length >= 5)
				fVersion = get16(descriptor + 3);
			break;

		case USB_VIDEO_VC_INPUT_TERMINAL:
			// bTerminalID, wTerminalType, ..., then for a camera:
			// bControlSize at 14, bmControls after it
			if (length >= 15 && get16(descriptor + 4) == USB_VIDEO_CAMERA_IN
				&& fCameraTerminal == 0) {
				fCameraTerminal = descriptor[3];
				size_t size = descriptor[14];
				if (size > length - 15)
					size = length - 15;
				fCameraControls.assign(descriptor + 15, descriptor + 15 + size);
			}
			break;

		case USB_VIDEO_VC_PROCESSING_UNIT:
			// bUnitID, bSourceID, wMaxMultiplier, bControlSize, bmControls
			if (length >= 8 && fProcessingUnit == 0) {
				fProcessingUnit = descriptor[3];
				size_t size = descriptor[7];
				if (size > length - 8)
					size = length - 8;
				fProcessingControls.assign(descriptor + 8,
					descriptor + 8 + size);
			}
			break;
	}
}


void
UVCCamDevice::_ParseVideoStreaming(const uint8* descriptor, size_t length)
{
	switch (descriptor[2]) {
		case USB_VIDEO_VS_FORMAT_UNCOMPRESSED:
		{
			// bFormatIndex, bNumFrameDescriptors, guidFormat, ...
			if (length < 21)
				break;

			uvc_format format;
			format.index = descriptor[3];
			format.pixelFormat = UVC_FORMAT_UNSUPPORTED;

			const uint8* guid = descriptor + 5;
			if (memcmp(guid + 4, kGuidTail, sizeof(kGuidTail)) == 0) {
				if (memcmp(guid, "YUY2", 4) == 0)
					format.pixelFormat = UVC_FORMAT_YUY2;
				else if (memcmp(guid, "UYVY", 4) == 0)
					format.pixelFormat = UVC_FORMAT_UYVY;
				else if (memcmp(guid, "NV12", 4) == 0)
					format.pixelFormat = UVC_FORMAT_NV12;
				else if (memcmp(guid, "I420", 4) == 0)
					format.pixelFormat = UVC_FORMAT_I420;
			}
			fFormats.push_back(format);
			break;
		}

		case USB_VIDEO_VS_FORMAT_MJPEG:
		{
			if (length < 5)
				break;

			uvc_format format;
			format.index = descriptor[3];
#ifdef HAVE_LIBJPEG
			format.pixelFormat = UVC_FORMAT_MJPEG;
#else
			format.pixelFormat = UVC_FORMAT_UNSUPPORTED;
#endif
			fFormats.push_back(format);
			break;
		}

		case USB_VIDEO_VS_FORMAT_MPEG2TS:
		case USB_VIDEO_VS_FORMAT_DV:
		case USB_VIDEO_VS_FORMAT_FRAME_BASED:
		case USB_VIDEO_VS_FORMAT_STREAM_BASED:
		case USB_VIDEO_VS_FORMAT_H264:
		case USB_VIDEO_VS_FORMAT_H264_SIMULCAST:
		case USB_VIDEO_VS_FORMAT_VP8:
		case USB_VIDEO_VS_FORMAT_VP8_SIMULCAST:
		{
			// Nothing we can use, but its frames must not be taken for those
			// of the format before it.
			uvc_format format;
			format.index = length > 3 ? descriptor[3] : 0;
			format.pixelFormat = UVC_FORMAT_UNSUPPORTED;
			fFormats.push_back(format);
			break;
		}

		case USB_VIDEO_VS_FRAME_UNCOMPRESSED:
		case USB_VIDEO_VS_FRAME_MJPEG:
		{
			// bFrameIndex, bmCapabilities, wWidth, wHeight, dwMinBitRate,
			// dwMaxBitRate, dwMaxVideoFrameBufferSize,
			// dwDefaultFrameInterval, bFrameIntervalType, intervals
			if (length < 26 || fFormats.empty())
				break;

			uvc_frame frame;
			frame.index = descriptor[3];
			frame.width = get16(descriptor + 5);
			frame.height = get16(descriptor + 7);
			frame.maxFrameSize = get32(descriptor + 17);
			frame.defaultInterval = get32(descriptor + 21);
			frame.minInterval = frame.maxInterval = frame.defaultInterval;
			frame.intervalStep = 0;

			const uint8 type = descriptor[25];
			if (type == 0) {
				if (length >= 38) {
					frame.minInterval = get32(descriptor + 26);
					frame.maxInterval = get32(descriptor + 30);
					frame.intervalStep = get32(descriptor + 34);
				}
			} else {
				for (uint8 i = 0; i < type && 26 + (i + 1) * 4u <= length;
						i++) {
					const uint32 interval = get32(descriptor + 26 + i * 4);
					if (interval != 0)
						frame.intervals.push_back(interval);
				}
				if (frame.intervals.empty())
					frame.intervals.push_back(frame.defaultInterval);
			}

			if (frame.width == 0 || frame.height == 0)
				break;
			if (frame.defaultInterval == 0)
				frame.defaultInterval = kPreferredInterval;

			fFormats.back().frames.push_back(frame);
			break;
		}
	}
}


void
UVCCamDevice::_DumpFormats() const
{
	printf("UVC: \"%s\", version %x.%02x, %s\n", fDevice->ProductString(),
		fVersion >> 8, fVersion & 0xff, fIsBulk ? "bulk" : "isochronous");
	for (size_t i = 0; i < fFormats.size(); i++) {
		const uvc_format& format = fFormats[i];
		printf("UVC:   format %d, %s:", format.index,
			format_name(format.pixelFormat));
		for (size_t j = 0; j < format.frames.size(); j++) {
			const uvc_frame& frame = format.frames[j];
			uint32 fastest = frame.minInterval;
			if (!frame.intervals.empty()) {
				fastest = *std::min_element(frame.intervals.begin(),
					frame.intervals.end());
			}
			printf(" %ux%u@%g", frame.width, frame.height,
				fastest != 0 ? 10000000.0 / fastest : 0.0);
		}
		printf("\n");
	}
}


//	#pragma mark - negotiation


/*!	Lists what the camera could be asked for to end up with pictures of the
	given size, the best choice first: the size itself before larger sizes
	(to be scaled down) before smaller ones, then the frame rate up to 30 per
	second, then uncompressed before compressed.
*/
void
UVCCamDevice::_CollectModes(uint32 width, uint32 height,
	std::vector<uvc_mode>& modes) const
{
	const int64 wanted = (int64)width * height;

	for (size_t i = 0; i < fFormats.size(); i++) {
		const uvc_format& format = fFormats[i];

		for (size_t j = 0; j < format.frames.size(); j++) {
			const uvc_frame& frame = format.frames[j];
			const int64 area = (int64)frame.width * frame.height;

			int64 sizeScore;
			if (frame.width == width && frame.height == height)
				sizeScore = (int64)3000000000LL;
			else if (frame.width >= width && frame.height >= height)
				sizeScore = 2000000000LL - std::min(area - wanted, (int64)900000000);
			else {
				sizeScore = 1000000000LL
					- std::min(std::max(wanted - area, (int64)0), (int64)900000000);
			}

			// The rates to try: the fastest one up to 30 per second, about
			// half that, and the slowest, for when the bus has no room.
			uint32 intervals[3];
			if (frame.intervals.empty()) {
				intervals[0] = std::min(std::max(kPreferredInterval,
					frame.minInterval), frame.maxInterval);
				intervals[1] = std::min(std::max(2 * kPreferredInterval,
					frame.minInterval), frame.maxInterval);
				intervals[2] = frame.maxInterval;
			} else {
				uint32 slowest = 0;
				uint32 best = 0;
				uint32 half = 0;
				for (size_t k = 0; k < frame.intervals.size(); k++) {
					const uint32 interval = frame.intervals[k];
					slowest = std::max(slowest, interval);
					// (a little slack for 29.97 written as 333334)
					if (interval + 100 >= kPreferredInterval
						&& (best == 0 || interval < best)) {
						best = interval;
					}
					if (interval + 100 >= 2 * kPreferredInterval
						&& (half == 0 || interval < half)) {
						half = interval;
					}
				}
				if (best == 0) {
					// all faster than 30 per second: the slowest is closest
					best = slowest;
				}
				intervals[0] = best;
				intervals[1] = half != 0 ? half : slowest;
				intervals[2] = slowest;
			}

			for (int32 k = 0; k < 3; k++) {
				if (intervals[k] == 0
					|| (k > 0 && intervals[k] == intervals[k - 1])
					|| (k == 2 && intervals[k] == intervals[0])) {
					continue;
				}

				uvc_mode mode;
				mode.format = &format;
				mode.frame = &frame;
				mode.interval = intervals[k];

				int64 rate = 10000000 / intervals[k];
				if (rate > 30)
					rate = 30;
				mode.score = sizeScore * 1000 + rate * 10
					+ (format.pixelFormat != UVC_FORMAT_MJPEG ? 1 : 0);
				modes.push_back(mode);
			}
		}
	}

	struct Compare {
		static bool Better(const uvc_mode& a, const uvc_mode& b)
		{
			return a.score > b.score;
		}
	};
	std::stable_sort(modes.begin(), modes.end(), Compare::Better);
}


/*!	Proposes \a mode to the camera and fetches what it makes of it.
*/
status_t
UVCCamDevice::_Probe(const uvc_mode& mode, uint8* probe, size_t& probeLength)
{
	// The size of the probe and commit controls grew with the versions of
	// the class. Some cameras do not go by the version they claim, so try
	// the other sizes as well.
	size_t lengths[3] = {26, 34, 48};
	if (fVersion >= 0x0150)
		std::swap(lengths[0], lengths[2]);
	else if (fVersion >= 0x0110)
		std::swap(lengths[0], lengths[1]);

	for (int32 i = 0; i < 3; i++) {
		const size_t length = lengths[i];

		memset(probe, 0, 48);
		probe[0] = 0x01;
			// bmHint: keep the frame interval
		probe[2] = mode.format->index;
		probe[3] = mode.frame->index;
		set32(probe + 4, mode.interval);

		ssize_t result = fDevice->ControlTransfer(kRequestTypeSet,
			USB_VIDEO_RC_SET_CUR, USB_VIDEO_VS_PROBE_CONTROL << 8,
			fStreamingInterface, length, probe);
		if (result != (ssize_t)length)
			continue;

		uint8 reply[48];
		memset(reply, 0, sizeof(reply));
		result = fDevice->ControlTransfer(kRequestTypeGet,
			USB_VIDEO_RC_GET_CUR, USB_VIDEO_VS_PROBE_CONTROL << 8,
			fStreamingInterface, length, reply);
		if (result < 26)
			continue;

		memcpy(probe, reply, length);
		probeLength = length;
		return B_OK;
	}

	return B_ERROR;
}


/*!	Finds the alternate of the streaming interface with the smallest
	isochronous endpoint that can carry payloads of the given size.
*/
int32
UVCCamDevice::_FindAlternate(uint32 payloadSize, uint32& endpointIndex) const
{
	const BUSBInterface* streaming = _StreamingInterface();
	if (streaming == NULL)
		return -1;

	int32 best = -1;
	uint32 bestSize = 0;
	for (uint32 i = 0; i < streaming->CountAlternates(); i++) {
		const BUSBInterface* alternate = streaming->AlternateAt(i);
		if (alternate == NULL)
			continue;

		for (uint32 j = 0; j < alternate->CountEndpoints(); j++) {
			const BUSBEndpoint* endpoint = alternate->EndpointAt(j);
			if (endpoint == NULL || !endpoint->IsIsochronous()
				|| !endpoint->IsInput()) {
				continue;
			}

			// bits 11 and 12: additional transactions per microframe
			const uint16 packetSize = endpoint->MaxPacketSize();
			const uint32 size = (packetSize & 0x7ff)
				* (1 + ((packetSize >> 11) & 3));
			if (size < payloadSize)
				continue;
			if (best < 0 || size < bestSize) {
				best = i;
				bestSize = size;
				endpointIndex = j;
			}
		}
	}

	return best;
}


/*!	Agrees with the camera on what to send, and has it ready to send it:
	for an isochronous camera, the alternate with the bandwidth for it is
	selected.
*/
status_t
UVCCamDevice::_Negotiate()
{
	std::vector<uvc_mode> modes;
	_CollectModes(fOutputWidth, fOutputHeight, modes);

	BUSBInterface* streaming = _StreamingInterface();
	if (streaming == NULL)
		return B_DEV_NOT_READY;

	for (size_t i = 0; i < modes.size(); i++) {
		const uvc_mode& mode = modes[i];
		TRACE("trying %s %ux%u, interval %" B_PRIu32 "\n",
			format_name(mode.format->pixelFormat), mode.frame->width,
			mode.frame->height, mode.interval);

		uint8 probe[48];
		size_t probeLength = 0;
		if (_Probe(mode, probe, probeLength) != B_OK) {
			TRACE("  probe failed\n");
			continue;
		}
		if (probe[2] != mode.format->index || probe[3] != mode.frame->index) {
			TRACE("  the camera wants something else\n");
			continue;
		}

		const uint32 payloadSize = get32(probe + 22);
		int32 alternate = -1;
		uint32 endpointIndex = 0;
		if (fIsBulk) {
			const BUSBInterface* first = streaming->AlternateAt(0);
			for (uint32 k = 0; first != NULL && k < first->CountEndpoints();
					k++) {
				const BUSBEndpoint* endpoint = first->EndpointAt(k);
				if (endpoint != NULL && endpoint->IsBulk()
					&& endpoint->IsInput()) {
					endpointIndex = k;
					alternate = 0;
					break;
				}
			}
		} else
			alternate = _FindAlternate(payloadSize, endpointIndex);

		if (alternate < 0) {
			TRACE("  no alternate for payloads of %" B_PRIu32 " bytes\n",
				payloadSize);
			continue;
		}

		ssize_t result = fDevice->ControlTransfer(kRequestTypeSet,
			USB_VIDEO_RC_SET_CUR, USB_VIDEO_VS_COMMIT_CONTROL << 8,
			fStreamingInterface, probeLength, probe);
		if (result != (ssize_t)probeLength) {
			TRACE("  commit failed\n");
			continue;
		}

		if (!fIsBulk || streaming->AlternateIndex() != 0) {
			if (streaming->SetAlternate(alternate) != B_OK) {
				ERROR("selecting alternate %" B_PRId32 " failed\n", alternate);
				continue;
			}
		}

		const BUSBEndpoint* endpoint = streaming->EndpointAt(endpointIndex);
		if (endpoint == NULL) {
			streaming->SetAlternate(0);
			continue;
		}

		fBulkIn = fIsBulk ? endpoint : NULL;
		fIsoIn = fIsBulk ? NULL : endpoint;
		const uint16 packetSize = endpoint->MaxPacketSize();
		fPacketSize = (packetSize & 0x7ff) * (1 + ((packetSize >> 11) & 3));

		fCaptureFormat = mode.format->pixelFormat;
		fCaptureWidth = mode.frame->width;
		fCaptureHeight = mode.frame->height;
		fCaptureInterval = get32(probe + 4);
		if (fCaptureInterval == 0)
			fCaptureInterval = mode.interval;
		fMaxVideoFrameSize = get32(probe + 18);
		fMaxPayloadTransferSize = payloadSize;

		printf("UVC: capturing %s %" B_PRIu32 "x%" B_PRIu32 " at %g fps for "
			"%" B_PRIu32 "x%" B_PRIu32 " (alternate %" B_PRId32 ", %"
			B_PRIuSIZE " bytes per packet)\n", format_name(fCaptureFormat),
			fCaptureWidth, fCaptureHeight, 10000000.0 / fCaptureInterval,
			fOutputWidth, fOutputHeight, alternate, fPacketSize);
		return B_OK;
	}

	ERROR("the camera accepted none of %" B_PRIuSIZE " modes\n", modes.size());
	return B_ERROR;
}


status_t
UVCCamDevice::StartTransfer()
{
	// (called with the device locked)
	if (fDevice == NULL)
		return B_DEV_NOT_READY;
	if (fTransferEnabled)
		return EALREADY;

	status_t status = _Negotiate();
	if (status != B_OK)
		return status;

	// room for one frame
	size_t capacity = uncompressed_frame_size(fCaptureFormat, fCaptureWidth,
		fCaptureHeight);
	if (capacity == 0) {
		capacity = fMaxVideoFrameSize;
		const size_t raw = (size_t)fCaptureWidth * fCaptureHeight * 2;
		if (capacity < 4096 || capacity > 4 * raw)
			capacity = raw;
	}

	{
		BAutolock _(fFrameLock);
		free(fAssembly);
		free(fReady);
		free(fWork);
		fAssembly = (uint8*)malloc(capacity);
		fReady = (uint8*)malloc(capacity);
		fWork = (uint8*)malloc(capacity);
		fFrameCapacity = capacity;
		fAssemblyLength = 0;
		fReadyLength = 0;
		fDeliveredSequence = fReadySequence;
		fHaveFrameID = false;
		fFrameDone = false;
		fFrameBad = false;
		fDroppedFrames = 0;
	}

	if (fAssembly == NULL || fReady == NULL || fWork == NULL)
		status = B_NO_MEMORY;

	if (status == B_OK && !fIsBulk) {
		// Transfers of 4 ms at high speed (a packet per 125 µs), of 8 ms at
		// full speed (a packet per ms); four of them queued.
		const uint32 packets = fDevice->USBVersion() >= 0x0200 ? 32 : 8;
		status = fIsoIn->StartIsochronousStream(fPacketSize, packets, 4,
			4 * 1024 * 1024);
		if (status != B_OK)
			ERROR("starting the stream failed: %s\n", strerror(status));
	}

	if (status == B_OK)
		status = CamDevice::StartTransfer();

	if (status != B_OK) {
		BUSBInterface* streaming = _StreamingInterface();
		if (!fIsBulk && streaming != NULL)
			streaming->SetAlternate(0);
		fIsoIn = NULL;
		fBulkIn = NULL;
	}

	return status;
}


status_t
UVCCamDevice::StopTransfer()
{
	// (called with the device locked)
	if (!fTransferEnabled)
		return EALREADY;

	// waits for the pump thread
	CamDevice::StopTransfer();

	if (fDevice != NULL) {
		if (fIsBulk) {
			// that is how a bulk camera is told to stop
			if (fBulkIn != NULL)
				fBulkIn->ClearStall();
		} else {
			if (fIsoIn != NULL)
				fIsoIn->StopIsochronousStream();
			BUSBInterface* streaming = _StreamingInterface();
			if (streaming != NULL)
				streaming->SetAlternate(0);
		}
	}

	fIsoIn = NULL;
	fBulkIn = NULL;

	if (fDroppedFrames > 0) {
		printf("UVC: %" B_PRIu32 " incomplete frames dropped\n",
			fDroppedFrames);
	}
	return B_OK;
}


status_t
UVCCamDevice::SuggestVideoFrame(uint32& width, uint32& height)
{
	// what applications have come to expect
	width = 320;
	height = 240;
	return AcceptVideoFrame(width, height);
}


status_t
UVCCamDevice::AcceptVideoFrame(uint32& width, uint32& height)
{
	if (width == 0 || height == 0) {
		width = 320;
		height = 240;
	}
	if (width > 4096 || height > 4096)
		return B_BAD_VALUE;

	// Any size will do: what the camera does not have is made by scaling.
	fOutputWidth = width;
	fOutputHeight = height;
	SetVideoFrame(BRect(0, 0, width - 1, height - 1));
	return B_OK;
}


float
UVCCamDevice::FrameRate()
{
	if (fTransferEnabled && fCaptureInterval != 0)
		return 10000000.0f / fCaptureInterval;

	std::vector<uvc_mode> modes;
	_CollectModes(fOutputWidth, fOutputHeight, modes);
	if (modes.empty() || modes[0].interval == 0)
		return 30.0f;

	return 10000000.0f / modes[0].interval;
}


//	#pragma mark - stream


status_t
UVCCamDevice::DataPumpThread()
{
	if (fIsBulk) {
		// One transfer is one payload: a header and a piece of the frame.
		size_t size = fMaxPayloadTransferSize;
		if (size < 16 * 1024)
			size = 16 * 1024;
		if (size > 1024 * 1024)
			size = 1024 * 1024;

		uint8* buffer = (uint8*)malloc(size);
		if (buffer == NULL)
			return B_NO_MEMORY;

		int32 errors = 0;
		while (fTransferEnabled) {
			ssize_t length = fBulkIn->BulkTransfer(buffer, size);
			if (length < 0) {
				if (++errors > 20)
					break;
				snooze(10000);
				continue;
			}
			errors = 0;
			_HandlePayload(buffer, length, false);
		}

		free(buffer);
		return B_OK;
	}

	const size_t size = 256 * 1024;
	uint8* buffer = (uint8*)malloc(size);
	if (buffer == NULL)
		return B_NO_MEMORY;

	while (fTransferEnabled) {
		ssize_t length = fIsoIn->ReadIsochronousStream(buffer, size, 100000);
		if (length == B_TIMED_OUT || length == B_INTERRUPTED)
			continue;
		if (length < 0) {
			ERROR("the stream ended: %s\n", strerror(length));
			break;
		}

		// every packet is a payload
		size_t offset = 0;
		while (offset + sizeof(usb_stream_packet_header) <= (size_t)length) {
			usb_stream_packet_header header;
			memcpy(&header, buffer + offset, sizeof(header));
			offset += sizeof(header);
			if (offset + header.length > (size_t)length)
				break;

			if ((header.flags & B_USB_STREAM_PACKET_GAP) != 0)
				fFrameBad = true;
			_HandlePayload(buffer + offset, header.length,
				(header.flags & B_USB_STREAM_PACKET_ERROR) != 0);
			offset += header.length;
		}
	}

	free(buffer);
	return B_OK;
}


/*!	Adds the data of a payload to the frame being put together. A frame ends
	with the payload that says so, or where the frame ID changes.
*/
void
UVCCamDevice::_HandlePayload(const uint8* data, size_t length, bool error)
{
	if (length < 2)
		return;

	const size_t headerLength = data[0];
	if (headerLength < 2 || headerLength > length)
		return;

	const uint8 info = data[1];
	const uint8 frameID = info & kHeaderFrameID;
	const size_t payloadLength = length - headerLength;
	if ((info & kHeaderError) != 0)
		error = true;

	if (!fHaveFrameID || frameID != fFrameID) {
		// the first payload of the next frame
		if (fHaveFrameID && !fFrameDone && fAssemblyLength > 0)
			_FinishFrame();

		fHaveFrameID = true;
		fFrameID = frameID;
		fAssemblyLength = 0;
		fFrameDone = false;
		fFrameBad = false;
	} else if (fFrameDone) {
		// Past the end of a frame, and the frame ID is still the same. There
		// are cameras that never change it: take data as the next frame.
		if (payloadLength == 0)
			return;

		fAssemblyLength = 0;
		fFrameDone = false;
		fFrameBad = false;
	}

	if (error)
		fFrameBad = true;

	size_t copy = payloadLength;
	if (copy > fFrameCapacity - fAssemblyLength) {
		copy = fFrameCapacity - fAssemblyLength;
		if (fCaptureFormat == UVC_FORMAT_MJPEG)
			fFrameBad = true;
	}
	memcpy(fAssembly + fAssemblyLength, data + headerLength, copy);
	fAssemblyLength += copy;

	if ((info & kHeaderEndOfFrame) != 0 && fAssemblyLength > 0)
		_FinishFrame();
}


/*!	Hands the frame that was put together to whoever waits for one, if it is
	whole.
*/
void
UVCCamDevice::_FinishFrame()
{
	fFrameDone = true;

	bool good;
	if (fCaptureFormat == UVC_FORMAT_MJPEG) {
		good = !fFrameBad && fAssemblyLength > 4 && fAssembly[0] == 0xff
			&& fAssembly[1] == 0xd8;
	} else {
		// A frame with a piece missing would show shifted.
		good = fAssemblyLength == uncompressed_frame_size(fCaptureFormat,
			fCaptureWidth, fCaptureHeight);
	}

	if (!good) {
		fDroppedFrames++;
		return;
	}

	BAutolock _(fFrameLock);
	std::swap(fAssembly, fReady);
	fReadyLength = fAssemblyLength;
	fReadyStamp = system_time();
	fReadySequence++;
	fAssemblyLength = 0;
	release_sem(fFrameSem);
}


status_t
UVCCamDevice::FillFrameBuffer(BBuffer* buffer, bigtime_t* stamp)
{
	const bigtime_t end = system_time() + kFrameTimeout;

	size_t length = 0;
	while (true) {
		{
			BAutolock _(fFrameLock);
			if (fReadySequence != fDeliveredSequence && fReady != NULL) {
				std::swap(fReady, fWork);
				length = fReadyLength;
				fDeliveredSequence = fReadySequence;
				if (stamp != NULL)
					*stamp = fReadyStamp;
				break;
			}
		}

		status_t status = acquire_sem_etc(fFrameSem, 1, B_ABSOLUTE_TIMEOUT,
			end);
		if (status != B_OK)
			return status;
	}

	const uint32 width = fOutputWidth;
	const uint32 height = fOutputHeight;
	if (buffer->SizeAvailable() < (size_t)width * height * 4)
		return B_BUFFER_OVERFLOW;

	// Only this thread uses fWork until its next call, and the buffers are
	// only replaced while the node is stopped.
	if (!_Convert(fWork, length, (uint8*)buffer->Data(), width * 4, width,
			height)) {
		return B_BAD_DATA;
	}

	return B_OK;
}


//	#pragma mark - conversion


bool
UVCCamDevice::_Convert(const uint8* source, size_t length, uint8* destination,
	uint32 bytesPerRow, uint32 width, uint32 height)
{
	if (fCaptureFormat == UVC_FORMAT_MJPEG) {
		return _ConvertMJPEG(source, length, destination, bytesPerRow, width,
			height);
	}

	if (length < uncompressed_frame_size(fCaptureFormat, fCaptureWidth,
			fCaptureHeight)) {
		return false;
	}

	_ConvertYUV(source, destination, bytesPerRow, width, height);
	return true;
}


void
UVCCamDevice::_ConvertYUV(const uint8* source, uint8* destination,
	uint32 bytesPerRow, uint32 width, uint32 height)
{
	const uint32 sourceWidth = fCaptureWidth;
	const uint32 sourceHeight = fCaptureHeight;

	uint32 left, top, cropWidth, cropHeight;
	crop_for(sourceWidth, sourceHeight, width, height, left, top, cropWidth,
		cropHeight);

	// which column of the source each column of the destination comes from
	std::vector<uint32> columns(width);
	for (uint32 x = 0; x < width; x++)
		columns[x] = left + (uint64)x * cropWidth / width;

	const uint8* const chroma = source + (size_t)sourceWidth * sourceHeight;
	const uint32 chromaWidth = sourceWidth / 2;

	for (uint32 y = 0; y < height; y++) {
		const uint32 sourceY = top + (uint64)y * cropHeight / height;
		uint8* out = destination + (size_t)y * bytesPerRow;

		switch (fCaptureFormat) {
			case UVC_FORMAT_YUY2:
			{
				// Y0 U Y1 V
				const uint8* line = source + (size_t)sourceY * sourceWidth * 2;
				for (uint32 x = 0; x < width; x++, out += 4) {
					const uint32 column = columns[x];
					const uint8* pair = line + (column & ~1) * 2;
					store_pixel(out, line[column * 2], pair[1], pair[3]);
				}
				break;
			}

			case UVC_FORMAT_UYVY:
			{
				// U Y0 V Y1
				const uint8* line = source + (size_t)sourceY * sourceWidth * 2;
				for (uint32 x = 0; x < width; x++, out += 4) {
					const uint32 column = columns[x];
					const uint8* pair = line + (column & ~1) * 2;
					store_pixel(out, line[column * 2 + 1], pair[0], pair[2]);
				}
				break;
			}

			case UVC_FORMAT_NV12:
			{
				// a plane of Y, then one of U and V in turns at half the size
				const uint8* line = source + (size_t)sourceY * sourceWidth;
				const uint8* chromaLine = chroma
					+ (size_t)(sourceY / 2) * sourceWidth;
				for (uint32 x = 0; x < width; x++, out += 4) {
					const uint32 column = columns[x];
					const uint8* pair = chromaLine + (column & ~1);
					store_pixel(out, line[column], pair[0], pair[1]);
				}
				break;
			}

			case UVC_FORMAT_I420:
			{
				// a plane of Y, then one of U and one of V at half the size
				const uint8* line = source + (size_t)sourceY * sourceWidth;
				const uint8* uLine = chroma
					+ (size_t)(sourceY / 2) * chromaWidth;
				const uint8* vLine = uLine
					+ (size_t)chromaWidth * (sourceHeight / 2);
				for (uint32 x = 0; x < width; x++, out += 4) {
					const uint32 column = columns[x];
					store_pixel(out, line[column], uLine[column / 2],
						vLine[column / 2]);
				}
				break;
			}

			default:
				return;
		}
	}
}


bool
UVCCamDevice::_ConvertMJPEG(const uint8* source, size_t length,
	uint8* destination, uint32 bytesPerRow, uint32 width, uint32 height)
{
#ifdef HAVE_LIBJPEG
	jpeg_decompress_struct info;
	jpeg_error_jump error;
	uint8* volatile row = NULL;

	info.err = jpeg_std_error(&error.manager);
	error.manager.error_exit = jpeg_error_exit;
	error.manager.output_message = jpeg_output_nothing;
	if (setjmp(error.jump) != 0) {
		// a damaged picture
		jpeg_destroy_decompress(&info);
		free(row);
		return false;
	}

	jpeg_create_decompress(&info);
	jpeg_mem_src(&info, (unsigned char*)source, length);
	if (jpeg_read_header(&info, TRUE) != JPEG_HEADER_OK) {
		jpeg_destroy_decompress(&info);
		return false;
	}
	jpeg_add_standard_tables(&info);

#ifdef JCS_ALPHA_EXTENSIONS
	info.out_color_space = JCS_EXT_BGRA;
#else
	info.out_color_space = JCS_EXT_BGRX;
#endif
	info.dct_method = JDCT_IFAST;
	info.do_fancy_upsampling = FALSE;

	// let the decoder do as much of the scaling down as it can
	info.scale_num = 1;
	info.scale_denom = 1;
	for (uint32 denominator = 8; denominator > 1; denominator /= 2) {
		if (info.image_width / denominator >= width
			&& info.image_height / denominator >= height) {
			info.scale_denom = denominator;
			break;
		}
	}

	jpeg_start_decompress(&info);
	if (info.output_components != 4) {
		jpeg_destroy_decompress(&info);
		return false;
	}

	const uint32 sourceWidth = info.output_width;
	const uint32 sourceHeight = info.output_height;

	if (sourceWidth == width && sourceHeight == height) {
		while (info.output_scanline < sourceHeight) {
			JSAMPROW line = destination
				+ (size_t)info.output_scanline * bytesPerRow;
			jpeg_read_scanlines(&info, &line, 1);
		}
		jpeg_finish_decompress(&info);
		jpeg_destroy_decompress(&info);
		return true;
	}

	uint32 left, top, cropWidth, cropHeight;
	crop_for(sourceWidth, sourceHeight, width, height, left, top, cropWidth,
		cropHeight);

	std::vector<uint32> columns(width);
	for (uint32 x = 0; x < width; x++)
		columns[x] = left + (uint64)x * cropWidth / width;

	row = (uint8*)malloc((size_t)sourceWidth * 4);
	if (row == NULL) {
		jpeg_destroy_decompress(&info);
		return false;
	}

	// The lines of the destination come from lines of the source further
	// and further down, so one pass over the source will do.
	uint32 y = 0;
	while (y < height && info.output_scanline < sourceHeight) {
		const uint32 sourceY = info.output_scanline;
		JSAMPROW line = row;
		jpeg_read_scanlines(&info, &line, 1);

		while (y < height
			&& top + (uint64)y * cropHeight / height == sourceY) {
			uint32* out = (uint32*)(destination + (size_t)y * bytesPerRow);
			const uint32* in = (const uint32*)(uint8*)row;
			for (uint32 x = 0; x < width; x++)
				out[x] = in[columns[x]];
			y++;
		}
	}

	jpeg_abort_decompress(&info);
	jpeg_destroy_decompress(&info);
	free(row);
	return y == height;
#else
	return false;
#endif
}


//	#pragma mark - controls


status_t
UVCCamDevice::_ControlRequest(uint8 request, const uvc_control& control,
	int32& value)
{
	if (fDevice == NULL)
		return B_DEV_NOT_READY;

	uint8 data[4] = {0, 0, 0, 0};
	const bool set = request == USB_VIDEO_RC_SET_CUR;
	if (set)
		set32(data, (uint32)value);

	ssize_t result = fDevice->ControlTransfer(
		set ? kRequestTypeSet : kRequestTypeGet, request,
		control.selector << 8, (control.entity << 8) | fControlInterface,
		control.size, data);
	if (result != control.size)
		return result < 0 ? (status_t)result : B_ERROR;

	if (!set) {
		switch (control.size) {
			case 1:
				value = data[0];
				break;
			case 2:
				value = (int16)get16(data);
				break;
			default:
				value = (int32)get32(data);
				break;
		}
	}
	return B_OK;
}


/*!	Adds a control the camera says it has, if it answers for it.
*/
void
UVCCamDevice::_AddControl(const char* name, uint8 entity, uint8 selector,
	uint8 size, uint8 kind)
{
	uvc_control control;
	control.name = name;
	control.entity = entity;
	control.selector = selector;
	control.size = size;
	control.kind = kind;
	control.minimum = 0;
	control.maximum = 1;
	control.onValue = 1;

	int32 value;
	if (_ControlRequest(USB_VIDEO_RC_GET_CUR, control, value) != B_OK)
		return;

	if (kind == CONTROL_RANGE) {
		if (_ControlRequest(USB_VIDEO_RC_GET_MIN, control, control.minimum)
				!= B_OK
			|| _ControlRequest(USB_VIDEO_RC_GET_MAX, control, control.maximum)
				!= B_OK
			|| control.maximum <= control.minimum) {
			return;
		}
	} else if (kind == CONTROL_AUTO_EXPOSURE) {
		// A bit mask of modes: manual (1), automatic (2), shutter priority
		// (4) and aperture priority (8). GET_RES tells which there are.
		int32 modes = 0;
		if (_ControlRequest(USB_VIDEO_RC_GET_RES, control, modes) != B_OK)
			modes = value | 1;
		if ((modes & 1) == 0)
			return;
		if ((modes & 2) != 0)
			control.onValue = 2;
		else if ((modes & 8) != 0)
			control.onValue = 8;
		else if ((modes & 4) != 0)
			control.onValue = 4;
		else
			return;
	}

	fControls.push_back(control);
}


void
UVCCamDevice::AddParameters(BParameterGroup* group, int32& index)
{
	CamDevice::AddParameters(group, index);
		// sets fFirstParameterID

	BParameterGroup* picture = NULL;
	BParameterGroup* camera = NULL;

	for (size_t i = 0; i < fControls.size(); i++) {
		const uvc_control& control = fControls[i];
		const int32 id = fFirstParameterID + i;

		BParameterGroup* parent;
		if (control.entity == fCameraTerminal) {
			if (camera == NULL)
				camera = group->MakeGroup("Camera");
			parent = camera;
		} else {
			if (picture == NULL)
				picture = group->MakeGroup("Picture");
			parent = picture;
		}

		switch (control.kind) {
			case CONTROL_RANGE:
				parent->MakeContinuousParameter(id, B_MEDIA_RAW_VIDEO,
					control.name, B_GAIN, "", control.minimum, control.maximum,
					1.0);
				break;

			case CONTROL_POWER_LINE:
			{
				BDiscreteParameter* parameter = parent->MakeDiscreteParameter(
					id, B_MEDIA_RAW_VIDEO, control.name, B_INPUT_MUX);
				parameter->AddItem(0, "Off");
				parameter->AddItem(1, "50 Hz");
				parameter->AddItem(2, "60 Hz");
				break;
			}

			default:
				parent->MakeDiscreteParameter(id, B_MEDIA_RAW_VIDEO,
					control.name, B_ENABLE);
				break;
		}
	}

	index += fControls.size();
}


status_t
UVCCamDevice::GetParameterValue(int32 id, bigtime_t* lastChange, void* value,
	size_t* size)
{
	const int32 which = id - fFirstParameterID;
	if (which < 0 || which >= (int32)fControls.size())
		return B_BAD_VALUE;

	const uvc_control& control = fControls[which];
	int32 current = 0;
	status_t status = _ControlRequest(USB_VIDEO_RC_GET_CUR, control, current);
	if (status != B_OK)
		return status;

	*lastChange = fLastParameterChanges;

	if (control.kind == CONTROL_RANGE) {
		if (*size < sizeof(float))
			return B_BAD_VALUE;
		*(float*)value = current;
		*size = sizeof(float);
		return B_OK;
	}

	if (*size < sizeof(int32))
		return B_BAD_VALUE;
	if (control.kind == CONTROL_AUTO_EXPOSURE)
		current = current != 1;
	else if (control.kind == CONTROL_BOOLEAN)
		current = current != 0;
	*(int32*)value = current;
	*size = sizeof(int32);
	return B_OK;
}


status_t
UVCCamDevice::SetParameterValue(int32 id, bigtime_t when, const void* value,
	size_t size)
{
	const int32 which = id - fFirstParameterID;
	if (which < 0 || which >= (int32)fControls.size() || value == NULL)
		return B_BAD_VALUE;

	const uvc_control& control = fControls[which];
	int32 set;
	switch (control.kind) {
		case CONTROL_RANGE:
			if (size < sizeof(float))
				return B_BAD_VALUE;
			set = (int32)(*(const float*)value
				+ (*(const float*)value < 0 ? -0.5f : 0.5f));
			set = std::min(std::max(set, control.minimum), control.maximum);
			break;

		case CONTROL_AUTO_EXPOSURE:
			if (size < sizeof(int32))
				return B_BAD_VALUE;
			set = *(const int32*)value != 0 ? control.onValue : 1;
			break;

		case CONTROL_BOOLEAN:
			if (size < sizeof(int32))
				return B_BAD_VALUE;
			set = *(const int32*)value != 0 ? 1 : 0;
			break;

		default:
			if (size < sizeof(int32))
				return B_BAD_VALUE;
			set = *(const int32*)value;
			break;
	}

	// (A camera refuses a setting that one of its automatisms is in
	// charge of.)
	status_t status = _ControlRequest(USB_VIDEO_RC_SET_CUR, control, set);
	if (status == B_OK)
		fLastParameterChanges = when;
	return status;
}


//	#pragma mark - UVCCamDeviceAddon


UVCCamDeviceAddon::UVCCamDeviceAddon(WebCamMediaAddOn* webcam)
	: CamDeviceAddon(webcam)
{
	SetSupportedDevices(kSupportedDevices);
}


UVCCamDeviceAddon::~UVCCamDeviceAddon()
{
}


const char *
UVCCamDeviceAddon::BrandName()
{
	return "USB Video Class";
}


UVCCamDevice *
UVCCamDeviceAddon::Instantiate(CamRoster& roster, BUSBDevice* from)
{
	return new UVCCamDevice(*this, from);
}


extern "C" status_t
B_WEBCAM_MKINTFUNC(uvccam)
(WebCamMediaAddOn* webcam, CamDeviceAddon **addon)
{
	*addon = new UVCCamDeviceAddon(webcam);
	return B_OK;
}
