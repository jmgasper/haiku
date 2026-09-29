/* Check that a network share that is mounted does what a disk does.
 *
 * A share that lists its folders has shown that the server can be talked to,
 * and little else. What goes wrong is further in: writing at the end of a file
 * that ends up at its start, a rename that will not replace what is there, a
 * file that reads differently the second time. So this reads a file twice, in
 * pieces of different sizes, and compares; and it writes, appends, renames,
 * truncates and dates files and reads back what became of them.
 *
 * Everything it writes is in a folder of its own that it makes in the share
 * and removes again. Nothing else in the share is touched.
 *
 * usage: sharetest <mount point> [--reading | --read-only]
 *
 * --reading leaves the writing out, for checking a share that is someone's
 * without putting anything into it. --read-only is for a share that was
 * mounted that way, and has it that writing is refused.
 *
 * Build on the machine: g++ -o sharetest sharetest.cpp -lbe
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <utime.h>

#include <string>

#include <fs_info.h>
#include <OS.h>


static int sPassed = 0;
static int sFailed = 0;


static void
say(bool ok, const char* what, const char* format = "", ...)
{
	char detail[512];
	va_list args;
	va_start(args, format);
	vsnprintf(detail, sizeof(detail), format, args);
	va_end(args);

	if (ok)
		sPassed++;
	else
		sFailed++;
	printf("  %s  %-40s %s\n", ok ? "yes" : "NO ", what, detail);
	errno = 0;
}


/*!	What went wrong, if something did: errno is whatever the last call left
	there, including those that were meant to fail.
*/
static const char*
error_if(bool failed)
{
	return failed && errno != 0 ? strerror(errno) : "";
}


static uint64
checksum(const uint8* data, size_t size, uint64 sum)
{
	// FNV-1a
	for (size_t i = 0; i < size; i++) {
		sum ^= data[i];
		sum *= 0x100000001b3ULL;
	}
	return sum;
}


/*!	Reads \a path from start to end in pieces of \a pieceSize. Returns the
	number of bytes, or -1.
*/
static off_t
read_file(const char* path, size_t pieceSize, uint64& _sum)
{
	int fd = open(path, O_RDONLY);
	if (fd < 0)
		return -1;

	uint8* buffer = (uint8*)malloc(pieceSize);
	off_t total = 0;
	_sum = 0xcbf29ce484222325ULL;

	while (true) {
		ssize_t bytesRead = read(fd, buffer, pieceSize);
		if (bytesRead < 0) {
			total = -1;
			break;
		}
		if (bytesRead == 0)
			break;

		_sum = checksum(buffer, bytesRead, _sum);
		total += bytesRead;
	}

	free(buffer);
	close(fd);
	return total;
}


/*!	Finds a file of some megabytes to read, not too deep in the share. */
static bool
find_file(const std::string& directory, int depth, std::string& _path,
	off_t& _size)
{
	DIR* dir = opendir(directory.c_str());
	if (dir == NULL)
		return false;

	bool found = false;
	while (struct dirent* entry = readdir(dir)) {
		if (entry->d_name[0] == '.')
			continue;

		const std::string path = directory + "/" + entry->d_name;
		struct stat st;
		if (stat(path.c_str(), &st) != 0)
			continue;

		if (S_ISREG(st.st_mode) && st.st_size >= 2 * 1024 * 1024
			&& st.st_size <= 64 * 1024 * 1024) {
			_path = path;
			_size = st.st_size;
			found = true;
		} else if (S_ISDIR(st.st_mode) && depth > 0)
			found = find_file(path, depth - 1, _path, _size);

		if (found)
			break;
	}

	closedir(dir);
	return found;
}


static std::string
contents(const std::string& path)
{
	std::string result;
	int fd = open(path.c_str(), O_RDONLY);
	if (fd < 0)
		return "<" + std::string(strerror(errno)) + ">";

	char buffer[256];
	ssize_t bytesRead;
	while ((bytesRead = read(fd, buffer, sizeof(buffer))) > 0)
		result.append(buffer, bytesRead);

	close(fd);
	return result;
}


static bool
write_file(const std::string& path, const char* text, int flags)
{
	int fd = open(path.c_str(), O_WRONLY | flags, 0644);
	if (fd < 0)
		return false;

	const bool ok = write(fd, text, strlen(text)) == (ssize_t)strlen(text);
	return close(fd) == 0 && ok;
}


int
main(int argc, char** argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <mount point> [--reading | --read-only]\n",
			argv[0]);
		return 2;
	}

	const std::string root = argv[1];
	const bool readOnly = argc > 2 && strcmp(argv[2], "--read-only") == 0;
	const bool readingOnly = argc > 2 && strcmp(argv[2], "--reading") == 0;

	printf("the volume\n");

	fs_info info;
	dev_t device = dev_for_path(root.c_str());
	if (device < 0 || fs_stat_dev(device, &info) != B_OK) {
		say(false, "there is a volume", "%s", error_if(true));
		printf("\n%d working, %d not\n", sPassed, sFailed);
		return 1;
	}

	say(device != dev_for_path("/boot") && device != dev_for_path("/"),
		"it is a volume of its own", "\"%s\" at %s", info.volume_name,
		root.c_str());
	say((info.flags & B_FS_IS_SHARED) != 0, "it says it is shared", "%s",
		info.device_name);
	say(info.total_blocks > 0, "it knows how large it is",
		"%.1f GiB, %.1f GiB free",
		info.total_blocks * (double)info.block_size / (1 << 30),
		info.free_blocks * (double)info.block_size / (1 << 30));

	printf("reading\n");

	int entries = 0;
	if (DIR* dir = opendir(root.c_str())) {
		while (readdir(dir) != NULL)
			entries++;
		closedir(dir);
	}
	say(entries > 2, "the top folder lists", "%d entries", entries - 2);

	std::string file;
	off_t size = 0;
	if (find_file(root, 3, file, size)) {
		uint64 large;
		uint64 small;
		bigtime_t start = system_time();
		off_t readLarge = read_file(file.c_str(), 256 * 1024, large);
		bigtime_t largeTime = system_time() - start;
		start = system_time();
		off_t readSmall = read_file(file.c_str(), 3001, small);
		bigtime_t smallTime = system_time() - start;

		say(readLarge == size, "a file reads to its end",
			"%.1f MiB, %.0f MiB/s in large pieces", size / 1048576.0,
			size / 1048576.0 / (largeTime / 1e6));
		say(readSmall == size && small == large,
			"and the same in small pieces", "%.0f MiB/s",
			size / 1048576.0 / (smallTime / 1e6));

		// the end first, as whoever looks for tags does
		int fd = open(file.c_str(), O_RDONLY);
		uint8 tail[4096];
		uint8 again[4096];
		bool same = fd >= 0
			&& pread(fd, tail, sizeof(tail), size - sizeof(tail))
				== (ssize_t)sizeof(tail)
			&& pread(fd, again, 100, 0) == 100
			&& pread(fd, again, sizeof(again), size - sizeof(again))
				== (ssize_t)sizeof(again)
			&& memcmp(tail, again, sizeof(tail)) == 0;
		if (fd >= 0)
			close(fd);
		say(same, "reading here and there agrees");
	} else
		say(false, "there is a file to read", "none of 2 to 64 MiB found");

	if (readingOnly) {
		printf("\n%d working, %d not\n", sPassed, sFailed);
		return sFailed == 0 ? 0 : 1;
	}

	if (readOnly) {
		printf("writing\n");
		const std::string path = root + "/.sharetest";
		bool refused = mkdir(path.c_str(), 0755) != 0
			&& errno == B_READ_ONLY_DEVICE;
		say(refused, "it refuses, being read-only", "%s",
			error_if(!refused));
		rmdir(path.c_str());

		printf("\n%d working, %d not\n", sPassed, sFailed);
		return sFailed == 0 ? 0 : 1;
	}

	printf("writing\n");

	char name[64];
	snprintf(name, sizeof(name), "/.sharetest-%d", (int)getpid());
	const std::string scratch = root + name;
	if (mkdir(scratch.c_str(), 0755) != 0) {
		say(false, "a folder can be made", "%s", error_if(true));
		printf("\n%d working, %d not\n", sPassed, sFailed);
		return 1;
	}
	say(true, "a folder can be made", "%s", scratch.c_str());

	const std::string a = scratch + "/a.txt";
	const std::string b = scratch + "/b.txt";
	const std::string c = scratch + "/c.txt";

	say(write_file(a, "hello\n", O_CREAT | O_EXCL) && contents(a) == "hello\n",
		"a file can be written and read back");

	bool refused = !write_file(a, "x", O_CREAT | O_EXCL)
		&& errno == B_FILE_EXISTS;
	say(refused, "a file that exists is not made anew", "%s",
		error_if(!refused));
	say(write_file(a, "more\n", O_APPEND) && contents(a) == "hello\nmore\n",
		"appending goes to the end");
	say(rename(a.c_str(), b.c_str()) == 0 && contents(b) == "hello\nmore\n"
			&& access(a.c_str(), F_OK) != 0,
		"a file can be renamed");
	say(write_file(c, "second\n", O_CREAT) && rename(c.c_str(), b.c_str()) == 0
			&& contents(b) == "second\n",
		"renaming replaces what is there");

	struct stat st;
	say(truncate(b.c_str(), 3) == 0 && stat(b.c_str(), &st) == 0
			&& st.st_size == 3 && contents(b) == "sec",
		"a file can be cut short");

	struct utimbuf times;
	times.actime = times.modtime = 1577934245;
		// 2020-01-02 03:04:05 UTC
	bool dated = utime(b.c_str(), &times) == 0 && stat(b.c_str(), &st) == 0
		&& st.st_mtime == times.modtime;
	say(dated, "a file can be dated", "%s", error_if(!dated));

	// some megabytes that are not all the same
	const size_t kSize = 8 * 1024 * 1024;
	uint8* data = (uint8*)malloc(kSize);
	for (size_t i = 0; i < kSize; i++)
		data[i] = (uint8)((i * 2654435761U) >> 13);
	const uint64 written = checksum(data, kSize, 0xcbf29ce484222325ULL);

	const std::string big = scratch + "/big.bin";
	int fd = open(big.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
	bigtime_t start = system_time();
	bool ok = fd >= 0;
	for (size_t offset = 0; ok && offset < kSize; offset += 65536)
		ok = write(fd, data + offset, 65536) == 65536;
	if (fd >= 0)
		ok = close(fd) == 0 && ok;
	bigtime_t writeTime = system_time() - start;
	free(data);

	uint64 readBack = 0;
	say(ok && read_file(big.c_str(), 65536, readBack) == (off_t)kSize
			&& readBack == written,
		"what was written is what is read", "8 MiB at %.0f MiB/s",
		8 / (writeTime / 1e6));

	const std::string sub = scratch + "/one/two";
	const std::string moved = scratch + "/moved";
	say(mkdir((scratch + "/one").c_str(), 0755) == 0
			&& mkdir(sub.c_str(), 0755) == 0
			&& write_file(sub + "/f", "x", O_CREAT)
			&& rename((scratch + "/one").c_str(), moved.c_str()) == 0
			&& contents(moved + "/two/f") == "x",
		"a folder moves with what is in it");
	// It has to be refused, and it has to still be there: a server that
	// does not delete it is one thing, being told so is another.
	refused = rmdir(moved.c_str()) != 0 && errno == B_DIRECTORY_NOT_EMPTY;
	say(refused && contents(moved + "/two/f") == "x",
		"a folder that is not empty stays", "%s",
		refused ? "" : "it was reported removed");
	refused = unlink((moved + "/two").c_str()) != 0;
	say(refused, "a folder is not a file to remove");

	bool removed = unlink((moved + "/two/f").c_str()) == 0
		&& rmdir((moved + "/two").c_str()) == 0
		&& rmdir(moved.c_str()) == 0
		&& unlink(big.c_str()) == 0
		&& unlink(b.c_str()) == 0
		&& rmdir(scratch.c_str()) == 0;
	say(removed && access(scratch.c_str(), F_OK) != 0,
		"nothing is left behind", "%s", error_if(!removed));

	printf("\n%d working, %d not\n", sPassed, sFailed);
	return sFailed == 0 ? 0 : 1;
}
