/* Copyright 2026, air/OS. Distributed under the terms of the MIT License. */
#ifndef AMD_UVD_H264_OUTPUT_H
#define AMD_UVD_H264_OUTPUT_H
#include <stddef.h>
#include <stdint.h>
#include <vector>

struct H264Frame {
	std::vector<uint8_t> pixels;
	uint32_t sequence, pitch, codedHeight;
	int32_t poc;
	int64_t time;
	int width, height, cropLeft, cropTop, matrix;
	bool fullRange;
};

class H264Output {
public:
	enum Format { NV12, I420, YCbCr422, RGB32 };
	H264Output() { Reset(); }
	void Reset();
	bool Push(H264Frame&& frame, unsigned reorderLimit, bool discardPrior);
	bool Ready(bool drain) const;
	const H264Frame& Front() const { return fFrames.front(); }
	void Pop();
	static size_t Bytes(int width, int height, Format format);
	static bool Copy(const H264Frame& frame, Format format, uint8_t* output);
private:
	std::vector<H264Frame> fFrames;
	uint32_t fSequence, fLastSequence;
	int32_t fLastPoc;
	unsigned fReorderLimit;
	bool fHaveOutput;
};
#endif
