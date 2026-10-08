/*
 * Copyright 2005, Stephan Aßmus <superstippi@gmx.de>. All rights reserved.
 * Distributed under the terms of the MIT License.
 *
 * DrawingMode implementing B_OP_COPY ignoring the pattern (solid) on B_RGBA32.
 *
 */

#ifndef DRAWING_MODE_COPY_SOLID_H
#define DRAWING_MODE_COPY_SOLID_H

#include <ByteOrder.h>

#include "DrawingModeOver.h"

// blend_pixel_copy_solid
void
blend_pixel_copy_solid(int x, int y, const color_type& c, uint8 cover,
					   agg_buffer* buffer, const PatternHandler* pattern)
{
	uint8* p = buffer->row_ptr(y) + (x << 2);
	if (cover == 255) {
		ASSIGN_OVER(p, c.r, c.g, c.b);
	} else {
		BLEND_OVER(p, c.r, c.g, c.b, cover);
	}
}

// blend_hline_copy_solid
void
blend_hline_copy_solid(int x, int y, unsigned len, 
					   const color_type& c, uint8 cover,
					   agg_buffer* buffer, const PatternHandler* pattern)
{
	if (cover == 255) {
		uint32 v;
		uint8* p8 = (uint8*)&v;
		p8[0] = (uint8)c.b;
		p8[1] = (uint8)c.g;
		p8[2] = (uint8)c.r;
		p8[3] = 255;
		uint32* p32 = (uint32*)(buffer->row_ptr(y)) + x;
		if (len >= 8) {
			do {
				p32[0] = v;
				p32[1] = v;
				p32[2] = v;
				p32[3] = v;
				p32[4] = v;
				p32[5] = v;
				p32[6] = v;
				p32[7] = v;
				p32 += 8;
				len -= 8;
			} while (len >= 8);
			if (len == 0)
				return;
		}
		do {
			*p32++ = v;
		} while(--len);
	} else {
		uint32* p = (uint32*)(buffer->row_ptr(y)) + x;
		const uint32 sourceRB = ((uint32(c.r) << 16) | c.b) * cover;
		const uint32 sourceG = uint32(c.g) * cover;
		const uint32 inverse = 256 - cover;
		// Red and blue use separate 16-bit lanes. Each weighted sum is at
		// most 255 * 256, so neither lane can carry into the other. Keep
		// BLEND_OVER's division by 256 and its opaque destination alpha.
		do {
			const uint32 destination = B_LENDIAN_TO_HOST_INT32(*p);
			const uint32 rb = (((destination & 0x00ff00ff) * inverse
				+ sourceRB) >> 8) & 0x00ff00ff;
			const uint32 green = (((destination >> 8) & 255) * inverse
				+ sourceG) & 0x0000ff00;
			*p++ = B_HOST_TO_LENDIAN_INT32(0xff000000 | rb | green);
		} while(--len);
	}
}

// blend_solid_hspan_copy_solid
void
blend_solid_hspan_copy_solid(int x, int y, unsigned len, 
							 const color_type& c, const uint8* covers,
							 agg_buffer* buffer,
							 const PatternHandler* pattern)
{
	uint8* p = buffer->row_ptr(y) + (x << 2);
	do {
		if (*covers) {
			if (*covers == 255) {
				ASSIGN_OVER(p, c.r, c.g, c.b);
			} else {
				BLEND_OVER(p, c.r, c.g, c.b, *covers);
			}
		}
		covers++;
		p += 4;
		x++;
	} while(--len);
}



// blend_solid_vspan_copy_solid
void
blend_solid_vspan_copy_solid(int x, int y, unsigned len, 
							 const color_type& c, const uint8* covers,
							 agg_buffer* buffer,
							 const PatternHandler* pattern)
{
	uint8* p = buffer->row_ptr(y) + (x << 2);
	do {
		if (*covers) {
			if (*covers == 255) {
				ASSIGN_OVER(p, c.r, c.g, c.b);
			} else {
				BLEND_OVER(p, c.r, c.g, c.b, *covers);
			}
		}
		covers++;
		p += buffer->stride();
		y++;
	} while(--len);
}


// blend_color_hspan_copy_solid
void
blend_color_hspan_copy_solid(int x, int y, unsigned len, 
							 const color_type* colors, const uint8* covers,
							 uint8 cover,
							 agg_buffer* buffer,
							 const PatternHandler* pattern)
{
	uint8* p = buffer->row_ptr(y) + (x << 2);
	if (covers) {
		// non-solid opacity
		do {
				if (*covers) {
				if (*covers == 255) {
					ASSIGN_OVER(p, colors->r, colors->g, colors->b);
				} else {
					BLEND_OVER(p, colors->r, colors->g, colors->b, *covers);
				}
			}
			covers++;
			p += 4;
			++colors;
		} while(--len);
	} else {
		// solid full opcacity
		if (cover == 255) {
			do {
				ASSIGN_OVER(p, colors->r, colors->g, colors->b);
				p += 4;
				++colors;
			} while(--len);
		// solid partial opacity
		} else if (cover) {
			do {
				BLEND_OVER(p, colors->r, colors->g, colors->b, cover);
				p += 4;
				++colors;
			} while(--len);
		}
	}
}

#endif // DRAWING_MODE_COPY_SOLID_H
