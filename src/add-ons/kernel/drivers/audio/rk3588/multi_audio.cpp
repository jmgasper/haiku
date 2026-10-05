/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */

#include "driver.h"

#include <kernel.h>
#include <string.h>


namespace {

static const multi_channel_info kChannelDescriptions[] = {
	{0, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS, 0},
	{1, B_MULTI_OUTPUT_CHANNEL, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS, 0},
	{2, B_MULTI_OUTPUT_BUS, B_CHANNEL_LEFT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO},
	{3, B_MULTI_OUTPUT_BUS, B_CHANNEL_RIGHT | B_CHANNEL_STEREO_BUS,
		B_CHANNEL_MINI_JACK_STEREO}
};


status_t
GetDescription(multi_description* userData)
{
	multi_description description;
	if (user_memcpy(&description, userData, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	description.interface_version = B_CURRENT_INTERFACE_VERSION;
	description.interface_minimum = B_CURRENT_INTERFACE_VERSION;
	strlcpy(description.friendly_name, "ROCK 5 ITX analog audio",
		sizeof(description.friendly_name));
	strlcpy(description.vendor_info, "Rockchip RK3588 / Everest ES8316",
		sizeof(description.vendor_info));
	description.output_channel_count = 2;
	description.input_channel_count = 0;
	description.output_bus_channel_count = 2;
	description.input_bus_channel_count = 0;
	description.aux_bus_channel_count = 0;
	description.output_rates = B_SR_48000;
	description.input_rates = 0;
	description.max_cvsr_rate = 0;
	description.min_cvsr_rate = 0;
	description.output_formats = B_FMT_16BIT;
	description.input_formats = 0;
	description.lock_sources = B_MULTI_LOCK_INTERNAL;
	description.timecode_sources = 0;
	description.interface_flags = B_MULTI_INTERFACE_PLAYBACK;
	description.start_latency = 40000;
	description.control_panel[0] = 0;
	if (user_memcpy(userData, &description, sizeof(description)) != B_OK)
		return B_BAD_ADDRESS;
	if ((size_t)description.request_channel_count
			>= B_COUNT_OF(kChannelDescriptions)
		&& user_memcpy(description.channels, kChannelDescriptions,
			sizeof(kChannelDescriptions)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


status_t
GetEnabledChannels(multi_channel_enable* userData)
{
	multi_channel_enable data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	B_SET_CHANNEL(data.enable_bits, 0, true);
	B_SET_CHANNEL(data.enable_bits, 1, true);
	data.lock_source = B_MULTI_LOCK_INTERNAL;
	return user_memcpy(userData, &data, sizeof(data)) == B_OK
		? B_OK : B_BAD_ADDRESS;
}


status_t
GetGlobalFormat(multi_format_info* userData)
{
	multi_format_info data;
	memset(&data, 0, sizeof(data));
	data.output.rate = B_SR_48000;
	data.output.format = B_FMT_16BIT;
	data.output_latency = 0;
	return user_memcpy(userData, &data, sizeof(data)) == B_OK
		? B_OK : B_BAD_ADDRESS;
}


status_t
SetGlobalFormat(const multi_format_info* userData)
{
	multi_format_info data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	return data.output.rate == B_SR_48000
		&& data.output.format == B_FMT_16BIT ? B_OK : B_NOT_SUPPORTED;
}


status_t
AllocateBuffers(AudioController* controller, multi_buffer_list* userData)
{
	multi_buffer_list data;
	if (user_memcpy(&data, userData, sizeof(data)) != B_OK)
		return B_BAD_ADDRESS;
	if (data.request_playback_buffers < (int32)kBufferCount
		|| data.request_playback_channels < (int32)kChannels
		|| data.playback_buffers == NULL) {
		return B_BAD_VALUE;
	}
	buffer_desc* playbackBuffers[kBufferCount];
	if (!IS_USER_ADDRESS(data.playback_buffers)
		|| user_memcpy(playbackBuffers, data.playback_buffers,
			sizeof(playbackBuffers)) != B_OK) {
		return B_BAD_ADDRESS;
	}
	rk3588_audio_stop(controller);
	rk3588_audio_release_buffers(controller);
	AudioStream& stream = controller->stream;
	stream.bufferLength = kFramesPerBuffer;
	size_t bytesPerBuffer = kFramesPerBuffer * kChannels * kSampleBytes;
	size_t bytes = bytesPerBuffer * kBufferCount;
	void* address = NULL;
	stream.area = create_area("RK3588 audio buffers", &address,
		B_ANY_KERNEL_ADDRESS, bytes, B_FULL_LOCK,
		B_READ_AREA | B_WRITE_AREA);
	if (stream.area < B_OK)
		return stream.area;
	memset(address, 0, bytes);
	for (uint32 index = 0; index < kBufferCount; index++)
		stream.buffers[index] = (uint8*)address + index * bytesPerBuffer;
	stream.readySem = create_sem(0, "RK3588 audio buffer ready");
	if (stream.readySem < B_OK) {
		status_t status = stream.readySem;
		rk3588_audio_release_buffers(controller);
		return status;
	}
	data.flags = B_MULTI_BUFFER_PLAYBACK;
	data.return_playback_buffers = kBufferCount;
	data.return_playback_channels = kChannels;
	data.return_playback_buffer_size = kFramesPerBuffer;
	data.return_record_buffers = 0;
	data.return_record_channels = 0;
	data.return_record_buffer_size = 0;
	for (uint32 index = 0; index < kBufferCount; index++) {
		buffer_desc descriptors[kChannels];
		for (uint32 channel = 0; channel < kChannels; channel++) {
			descriptors[channel].base = (char*)stream.buffers[index]
				+ channel * kSampleBytes;
			descriptors[channel].stride = kChannels * kSampleBytes;
		}
		if (!IS_USER_ADDRESS(playbackBuffers[index])
			|| user_memcpy(playbackBuffers[index], descriptors,
				sizeof(descriptors)) != B_OK) {
			rk3588_audio_release_buffers(controller);
			return B_BAD_ADDRESS;
		}
	}
	if (user_memcpy(userData, &data, sizeof(data)) != B_OK) {
		rk3588_audio_release_buffers(controller);
		return B_BAD_ADDRESS;
	}
	return B_OK;
}


status_t
BufferExchange(AudioController* controller, multi_buffer_info* userInfo)
{
	status_t status = rk3588_audio_start(controller);
	if (status != B_OK)
		return status;
	status = acquire_sem_etc(controller->stream.readySem, 1, B_CAN_INTERRUPT, 0);
	if (status != B_OK)
		return status;
	multi_buffer_info info;
	if (user_memcpy(&info, userInfo, sizeof(info)) != B_OK)
		return B_BAD_ADDRESS;
	cpu_status interrupts = disable_interrupts();
	acquire_spinlock(&controller->stream.lock);
	info.playback_buffer_cycle = controller->stream.completedBuffer;
	info.played_real_time = controller->stream.realTime;
	info.played_frames_count = controller->stream.framesCount;
	release_spinlock(&controller->stream.lock);
	restore_interrupts(interrupts);
	return user_memcpy(userInfo, &info, sizeof(info));
}

}


void
rk3588_audio_release_buffers(AudioController* controller)
{
	AudioStream& stream = controller->stream;
	if (stream.readySem >= B_OK) {
		delete_sem(stream.readySem);
		stream.readySem = -1;
	}
	if (stream.area >= B_OK) {
		delete_area(stream.area);
		stream.area = -1;
	}
	for (uint32 index = 0; index < kBufferCount; index++)
		stream.buffers[index] = NULL;
}


status_t
rk3588_audio_control(AudioController* controller, uint32 operation,
	void* buffer, size_t)
{
	switch (operation) {
		case B_MULTI_GET_DESCRIPTION:
			return GetDescription((multi_description*)buffer);
		case B_MULTI_GET_ENABLED_CHANNELS:
			return GetEnabledChannels((multi_channel_enable*)buffer);
		case B_MULTI_SET_ENABLED_CHANNELS:
		{
			multi_channel_enable data;
			return user_memcpy(&data, buffer, sizeof(data)) == B_OK
				? B_OK : B_BAD_ADDRESS;
		}
		case B_MULTI_GET_GLOBAL_FORMAT:
			return GetGlobalFormat((multi_format_info*)buffer);
		case B_MULTI_SET_GLOBAL_FORMAT:
			return SetGlobalFormat((const multi_format_info*)buffer);
		case B_MULTI_GET_BUFFERS:
			return AllocateBuffers(controller, (multi_buffer_list*)buffer);
		case B_MULTI_BUFFER_EXCHANGE:
			return BufferExchange(controller, (multi_buffer_info*)buffer);
		case B_MULTI_BUFFER_FORCE_STOP:
			rk3588_audio_stop(controller);
			return B_OK;
		case B_MULTI_LIST_MIX_CONTROLS: {
			multi_mix_control_info info;
			if (user_memcpy(&info, buffer, sizeof(info)) != B_OK)
				return B_BAD_ADDRESS;
			info.control_count = 0;
			return user_memcpy(buffer, &info, sizeof(info)) == B_OK
				? B_OK : B_BAD_ADDRESS;
		}
		case B_MULTI_GET_EVENT_INFO:
		case B_MULTI_SET_EVENT_INFO:
		case B_MULTI_GET_EVENT:
		case B_MULTI_GET_CHANNEL_FORMATS:
		case B_MULTI_SET_CHANNEL_FORMATS:
		case B_MULTI_GET_MIX:
		case B_MULTI_SET_MIX:
		case B_MULTI_LIST_MIX_CHANNELS:
		case B_MULTI_LIST_MIX_CONNECTIONS:
		case B_MULTI_SET_BUFFERS:
		case B_MULTI_SET_START_TIME:
			return B_NOT_SUPPORTED;
	}
	return B_DEV_INVALID_IOCTL;
}
