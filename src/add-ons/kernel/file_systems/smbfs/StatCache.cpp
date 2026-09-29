/*
 * Copyright 2026, Haiku, Inc. All rights reserved.
 * Distributed under the terms of the MIT License.
 */


#include "StatCache.h"


// Others change the share too, and this is how long that can go unnoticed.
static const bigtime_t kLifetime = 5 * 1000000LL;

static const size_t kMaxEntries = 50000;


StatCache::StatCache()
{
	pthread_mutex_init(&fLock, NULL);
}


StatCache::~StatCache()
{
	pthread_mutex_destroy(&fLock);
}


bool
StatCache::Lookup(const std::string& path, struct stat& st)
{
	pthread_mutex_lock(&fLock);

	bool found = false;
	EntryMap::iterator it = fEntries.find(path);
	if (it != fEntries.end()) {
		if (it->second.expires > system_time()) {
			st = it->second.st;
			found = true;
		} else
			fEntries.erase(it);
	}

	pthread_mutex_unlock(&fLock);
	return found;
}


void
StatCache::Insert(const std::string& path, const struct stat& st)
{
	pthread_mutex_lock(&fLock);

	if (fEntries.size() >= kMaxEntries) {
		// most of them will have expired long ago
		const bigtime_t now = system_time();
		for (EntryMap::iterator it = fEntries.begin();
				it != fEntries.end();) {
			if (it->second.expires <= now)
				fEntries.erase(it++);
			else
				++it;
		}

		if (fEntries.size() >= kMaxEntries)
			fEntries.clear();
	}

	Entry& entry = fEntries[path];
	entry.st = st;
	entry.expires = system_time() + kLifetime;

	pthread_mutex_unlock(&fLock);
}


void
StatCache::Remove(const std::string& path)
{
	pthread_mutex_lock(&fLock);
	fEntries.erase(path);
	pthread_mutex_unlock(&fLock);
}


/*!	Forgets \a path and everything below it. */
void
StatCache::RemoveTree(const std::string& path)
{
	pthread_mutex_lock(&fLock);

	fEntries.erase(path);

	const std::string prefix = path + "/";
	EntryMap::iterator it = fEntries.lower_bound(prefix);
	while (it != fEntries.end()
		&& it->first.compare(0, prefix.length(), prefix) == 0) {
		fEntries.erase(it++);
	}

	pthread_mutex_unlock(&fLock);
}


void
StatCache::Clear()
{
	pthread_mutex_lock(&fLock);
	fEntries.clear();
	pthread_mutex_unlock(&fLock);
}
