#pragma once

#include <NvRmDevice.h>
#include <NvKmsDevice.h>
#include <NvKmsSurface.h>

#include <GraphicsDefs.h>


class NvKmsBitmap {
private:
	NvRmObject fMemory;
	NvKmsSurface fSurface;
	NvRmMemoryMapping fMapping;

	uint32 fWidth;
	uint32 fHeight;
	uint32 fBytesPerRow;
	color_space fColorSpace;

public:
	inline NvKmsBitmap();
	inline NvKmsBitmap(NvKmsBitmap &&other);
	NvKmsBitmap(NvRmDevice &rmDev, NvKmsDevice &kmsDev, uint32 width, uint32 height, color_space colorSpace);

	inline NvKmsBitmap &operator=(NvKmsBitmap &&other);

	inline NvKmsBitmap(const NvKmsBitmap &other) = delete;
	inline NvKmsBitmap &operator=(const NvKmsBitmap &other) = delete;

	inline bool IsSet() const;

	inline uint32 Width() const;
	inline uint32 Height() const;
	inline void *Bits() const;
	inline int32 BitsLength() const;
	inline int32 BytesPerRow() const;
	inline color_space ColorSpace() const;

	inline const NvRmObject &Memory() const;
	inline const NvKmsSurface &Surface() const;
};


NvKmsBitmap::NvKmsBitmap():
	fWidth(0),
	fHeight(0),
	fBytesPerRow(0),
	fColorSpace(B_NO_COLOR_SPACE)
{
}

NvKmsBitmap::NvKmsBitmap(NvKmsBitmap &&other):
	fWidth(other.fWidth),
	fHeight(other.fHeight),
	fBytesPerRow(other.fBytesPerRow),
	fColorSpace(other.fColorSpace)
{
	fMapping = std::move(other.fMapping);
	fSurface = std::move(other.fSurface);
	fMemory = std::move(other.fMemory);

	other.fWidth = 0;
	other.fHeight = 0;
	other.fBytesPerRow = 0;
	other.fColorSpace = B_NO_COLOR_SPACE;
}

NvKmsBitmap &NvKmsBitmap::operator=(NvKmsBitmap &&other)
{
	fMapping = std::move(other.fMapping);
	fSurface = std::move(other.fSurface);
	fMemory = std::move(other.fMemory);

	fWidth = other.fWidth;
	fHeight = other.fHeight;
	fBytesPerRow = other.fBytesPerRow;
	fColorSpace = other.fColorSpace;

	other.fWidth = 0;
	other.fHeight = 0;
	other.fBytesPerRow = 0;
	other.fColorSpace = B_NO_COLOR_SPACE;

	return *this;
}

bool NvKmsBitmap::IsSet() const
{
	return fSurface.IsSet();
}


uint32 NvKmsBitmap::Width() const
{
	return fWidth;
}

uint32 NvKmsBitmap::Height() const
{
	return fHeight;
}

void *NvKmsBitmap::Bits() const
{
	return fMapping.Address();
}

int32 NvKmsBitmap::BitsLength() const
{
	return fBytesPerRow*fHeight;
}

int32 NvKmsBitmap::BytesPerRow() const
{
	return fBytesPerRow;
}

color_space NvKmsBitmap::ColorSpace() const
{
	return fColorSpace;
}

const NvRmObject &NvKmsBitmap::Memory() const
{
	return fMemory;
}

const NvKmsSurface &NvKmsBitmap::Surface() const
{
	return fSurface;
}
