/*
 * Copyright 2026, air/OS. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef MMAL_PROTOCOL_H
#define MMAL_PROTOCOL_H


#include <SupportDefs.h>


/*	The messages of the VideoCore firmware's "mmal" service (multimedia
	components: the H.264 decoder here), as the firmware lays them out: it is
	a 32 bit program, pointers in its structures are 32 bit words that mean
	nothing to us. Names follow Broadcom's MMAL headers (interface/mmal in
	the userland sources, BSD licensed). */

#define MMAL_FOURCC(a, b, c, d) \
	((uint32)(a) | ((uint32)(b) << 8) | ((uint32)(c) << 16) \
		| ((uint32)(d) << 24))

#define MMAL_MAGIC					MMAL_FOURCC('m', 'm', 'a', 'l')
#define MMAL_SERVICE_VERSION		15
#define MMAL_SERVICE_VERSION_MIN	10

#define MMAL_ENCODING_H264			MMAL_FOURCC('H', '2', '6', '4')
#define MMAL_ENCODING_I420			MMAL_FOURCC('I', '4', '2', '0')
#define MMAL_ENCODING_NV12			MMAL_FOURCC('N', 'V', '1', '2')

#define MMAL_EVENT_ERROR			MMAL_FOURCC('E', 'R', 'R', 'O')
#define MMAL_EVENT_EOS				MMAL_FOURCC('E', 'E', 'O', 'S')
#define MMAL_EVENT_FORMAT_CHANGED	MMAL_FOURCC('E', 'F', 'C', 'H')

#define MMAL_TIME_UNKNOWN			((int64)1 << 63)

enum {
	MMAL_MSG_COMPONENT_CREATE = 4,
	MMAL_MSG_COMPONENT_DESTROY,
	MMAL_MSG_COMPONENT_ENABLE,
	MMAL_MSG_COMPONENT_DISABLE,
	MMAL_MSG_PORT_INFO_GET,
	MMAL_MSG_PORT_INFO_SET,
	MMAL_MSG_PORT_ACTION,
	MMAL_MSG_BUFFER_FROM_HOST,
	MMAL_MSG_BUFFER_TO_HOST,
	MMAL_MSG_GET_STATS,
	MMAL_MSG_PORT_PARAMETER_SET,
	MMAL_MSG_PORT_PARAMETER_GET,
	MMAL_MSG_EVENT_TO_HOST
};

enum {
	MMAL_PORT_ACTION_ENABLE = 1,
	MMAL_PORT_ACTION_DISABLE,
	MMAL_PORT_ACTION_FLUSH
};

enum {
	MMAL_PORT_TYPE_CONTROL = 1,
	MMAL_PORT_TYPE_INPUT,
	MMAL_PORT_TYPE_OUTPUT
};

enum {
	MMAL_ES_TYPE_VIDEO = 3
};

#define MMAL_ES_FORMAT_FLAG_FRAMED		0x1

#define MMAL_BUFFER_FLAG_EOS			(1 << 0)
#define MMAL_BUFFER_FLAG_FRAME_START	(1 << 1)
#define MMAL_BUFFER_FLAG_FRAME_END		(1 << 2)
#define MMAL_BUFFER_FLAG_KEYFRAME		(1 << 3)
#define MMAL_BUFFER_FLAG_DISCONTINUITY	(1 << 4)
#define MMAL_BUFFER_FLAG_CONFIG			(1 << 5)
#define MMAL_BUFFER_FLAG_CORRUPTED		(1 << 9)
#define MMAL_BUFFER_FLAG_DECODEONLY		(1 << 11)
	// decode, and give no picture for it

#define MMAL_PARAMETER_EXTRA_BUFFERS					0x20013
#define MMAL_PARAMETER_VIDEO_DECODE_ERROR_CONCEALMENT	0x20025
#define MMAL_PARAMETER_VIDEO_INTERPOLATE_TIMESTAMPS		0x2002f

#define MMAL_EXTRADATA_SIZE			128
#define MMAL_SHORT_DATA				128
#define MMAL_PARAMETER_SPACE		96
#define MMAL_EVENT_SPACE			256


struct mmal_header {
	uint32	magic;
	uint32	type;
	uint32	control_service;
	uint32	context;
	uint32	status;
	uint32	padding;
};

struct mmal_port {
	uint32	priv;
	uint32	name;
	uint32	type;
	uint16	index;
	uint16	index_all;
	uint32	is_enabled;
	uint32	format;
	uint32	buffer_num_min;
	uint32	buffer_size_min;
	uint32	buffer_alignment_min;
	uint32	buffer_num_recommended;
	uint32	buffer_size_recommended;
	uint32	buffer_num;
	uint32	buffer_size;
	uint32	component;
	uint32	userdata;
	uint32	capabilities;
};

struct mmal_video_format {
	uint32	width;
	uint32	height;
	struct {
		int32	x, y, width, height;
	} crop;
	struct {
		int32	numerator, denominator;
	} frame_rate, par;
	uint32	color_space;
};

struct mmal_es_format {
	uint32	type;
	uint32	encoding;
	uint32	encoding_variant;
	uint32	es;
	uint32	bitrate;
	uint32	flags;
	uint32	extradata_size;
	uint32	extradata;
};

struct mmal_component_create {
	uint32	client_component;
	char	name[128];
	uint32	pid;
};

struct mmal_component_create_reply {
	uint32	status;
	uint32	component_handle;
	uint32	input_num;
	uint32	output_num;
	uint32	clock_num;
};

struct mmal_component_request {
	uint32	component_handle;
};

struct mmal_status_reply {
	uint32	status;
};

struct mmal_port_info_get {
	uint32	component_handle;
	uint32	port_type;
	uint32	index;
};

// also the reply to mmal_port_info_set
struct mmal_port_info_get_reply {
	uint32	status;
	uint32	component_handle;
	uint32	port_type;
	uint32	port_index;
	int32	found;
	uint32	port_handle;
	mmal_port			port;
	mmal_es_format		format;
	mmal_video_format	es;
	uint8	extradata[MMAL_EXTRADATA_SIZE];
};

struct mmal_port_info_set {
	uint32	component_handle;
	uint32	port_type;
	uint32	port_index;
	mmal_port			port;
	mmal_es_format		format;
	mmal_video_format	es;
	uint8	extradata[MMAL_EXTRADATA_SIZE];
};

struct mmal_port_action {
	uint32	component_handle;
	uint32	port_handle;
	uint32	action;
	mmal_port	port;
};

struct mmal_driver_buffer {
	uint32	magic;
	uint32	component_handle;
	uint32	port_handle;
	uint32	client_context;
};

struct mmal_buffer_header {
	uint32	next;
	uint32	priv;
	uint32	cmd;
	uint32	data;
	uint32	alloc_size;
	uint32	length;
	uint32	offset;
	uint32	flags;
	int64	pts;
	int64	dts;
	uint32	type;
	uint32	user_data;
};

struct mmal_buffer_from_host {
	mmal_driver_buffer	drvbuf;
	mmal_driver_buffer	drvbuf_ref;
	mmal_buffer_header	buffer_header;
	struct {
		uint32	planes;
		uint32	offset[4];
		uint32	pitch[4];
		uint32	flags;
	} video;
	int32	is_zero_copy;
	int32	has_reference;
	uint32	payload_in_message;
	uint8	short_data[MMAL_SHORT_DATA];
};

struct mmal_port_parameter_set {
	uint32	component_handle;
	uint32	port_handle;
	uint32	id;
	uint32	size;		// of the value, plus 8 for id and size
	uint32	value[MMAL_PARAMETER_SPACE];
};

struct mmal_event_to_host {
	uint32	client_component;
	uint32	port_type;
	uint32	port_num;
	uint32	cmd;
	uint32	length;
	uint8	data[MMAL_EVENT_SPACE];
	uint32	delayed_buffer;
};

// the data of MMAL_EVENT_FORMAT_CHANGED
struct mmal_event_format_changed {
	uint32	buffer_size_min;
	uint32	buffer_num_min;
	uint32	buffer_size_recommended;
	uint32	buffer_num_recommended;
	uint32	format;
	mmal_es_format		es_format;
	mmal_video_format	es;
};

struct mmal_message {
	mmal_header	h;
	union {
		mmal_component_create		component_create;
		mmal_component_create_reply	component_create_reply;
		mmal_component_request		component;
		mmal_status_reply			status;
		mmal_port_info_get			port_info_get;
		mmal_port_info_get_reply	port_info_get_reply;
		mmal_port_info_set			port_info_set;
		mmal_port_action			port_action;
		mmal_buffer_from_host		buffer_from_host;
		mmal_port_parameter_set		port_parameter_set;
		mmal_event_to_host			event_to_host;
		uint8						payload[488];
	} u;
};


#endif	// MMAL_PROTOCOL_H
