#pragma once

#include <unistd.h>


class FileDesc {
private:
	int fFd;

public:
	inline FileDesc();
	inline FileDesc(int fd);
	inline FileDesc(FileDesc &&other);
	inline ~FileDesc();

	inline FileDesc &operator=(FileDesc &&other);

	inline FileDesc(const FileDesc &other) = delete;
	inline FileDesc &operator=(const FileDesc &other) = delete;

	inline bool IsSet() const;
	inline int Get() const;
	inline int Detach();
};


FileDesc::FileDesc(): fFd(-1)
{
}

FileDesc::FileDesc(int fd): fFd(fd)
{
}

FileDesc::FileDesc(FileDesc &&other): fFd(other.fFd)
{
	other.fFd = -1;
}

FileDesc::~FileDesc()
{
	if (IsSet()) {
		close(fFd);
	}
}

FileDesc &FileDesc::operator=(FileDesc &&other)
{
	if (IsSet()) {
		close(fFd);
	}
	fFd = other.fFd;
	other.fFd = -1;
	return *this;
}

bool FileDesc::IsSet() const
{
	return fFd >= 0;
}

int FileDesc::Get() const
{
	return fFd;
}

int FileDesc::Detach()
{
	int res = fFd;
	fFd = -1;
	return res;
}
