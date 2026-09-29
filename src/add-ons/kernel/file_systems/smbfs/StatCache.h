/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */
#ifndef SMBFS_STAT_CACHE_H
#define SMBFS_STAT_CACHE_H


#include <pthread.h>
#include <sys/stat.h>

#include <map>
#include <string>

#include <OS.h>


/*!	Remembers for a few seconds what the server said about files.

	Reading a directory tells everything about what is in it, and whoever
	reads a directory usually asks about each entry next: Tracker does, ls -l
	does. Without this each of these is a question to the server, which for a
	folder of some thousand files is what opening it takes.
*/
class StatCache {
public:
								StatCache();
								~StatCache();

			bool				Lookup(const std::string& path,
									struct stat& st);
			void				Insert(const std::string& path,
									const struct stat& st);
			void				Remove(const std::string& path);
			void				RemoveTree(const std::string& path);
			void				Clear();

private:
			struct Entry {
				struct stat		st;
				bigtime_t		expires;
			};
			typedef std::map<std::string, Entry> EntryMap;

			pthread_mutex_t		fLock;
			EntryMap			fEntries;
};


#endif	// SMBFS_STAT_CACHE_H
