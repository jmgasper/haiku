/*
 * Copyright 2026, air/OS contributors.
 * Distributed under the terms of the MIT License.
 */

#include <OS.h>
#include <image.h>
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <filesystem>
#include <future>
#include <iomanip>
#include <locale>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <numeric>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <typeinfo>
#include <vector>
#include <unistd.h>

static void require(bool value, const char* message)
{
    if (!value) {
        std::fprintf(stderr, "CXX_PROBE_FAIL %s\n", message);
        std::exit(1);
    }
}

int main(int argc, char** argv)
{
    require(argc == 2, "plugin argument");
    alarm(30);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    std::set_terminate([] {
        std::fprintf(stderr, "CXX_PROBE_FAIL unexpected terminate\n");
        std::fflush(stderr);
        sync();
        std::_Exit(2);
    });
    int32 cookie = 0;
    image_info info;
    unsigned providers = 0;
    const char* expected = std::getenv("EXPECT_CXX_PROVIDER");
    while (get_next_image_info(B_CURRENT_TEAM, &cookie, &info) == B_OK) {
        if (std::strstr(info.name, "libstdc++.so") != nullptr) {
            std::printf("CXX_PROVIDER abi=%d path=%s\n", _GLIBCXX_USE_CXX11_ABI, info.name);
            require(expected == nullptr || std::strstr(info.name, expected) != nullptr,
                "selected runtime image");
            providers++;
        }
    }
    require(providers == 1, "exactly one C++ runtime");

    for (unsigned size : {0, 1, 15, 16, 23, 31, 64, 1024, 65537}) {
        std::string source(size, 'x');
        std::string copy = source;
        if (size != 0) copy[size / 2] = 'y';
        require(source == std::string(size, 'x'), "string copy independence");
        std::string moved(std::move(copy));
        require(moved.size() == size, "string move");
    }
    std::vector<int> values(4096);
    std::iota(values.begin(), values.end(), 0);
    std::reverse(values.begin(), values.end());
    std::sort(values.begin(), values.end());
    require(std::accumulate(values.begin(), values.end(), int64_t(0)) == 8386560,
        "vector and sorting");
    std::map<std::string, unsigned> table;
    for (unsigned i = 0; i < 200; i++) table[std::to_string(i)] = i;
    for (unsigned i = 0; i < 200; i++) require(table.at(std::to_string(i)) == i, "map");

    auto shared = std::make_shared<std::string>(4097, 's');
    std::weak_ptr<std::string> weak = shared;
    std::mutex mutex;
    std::condition_variable ready;
    bool start = false;
    unsigned count = 0;
    std::atomic<unsigned> checked{0};
    std::vector<std::thread> workers;
    for (unsigned worker = 0; worker < 8; worker++) {
        workers.emplace_back([&, worker] {
            {
                std::unique_lock<std::mutex> lock(mutex);
                ready.wait(lock, [&] { return start; });
            }
            for (unsigned i = 0; i < 1000; i++) {
                auto reference = weak.lock();
                require(reference && reference->size() == 4097, "concurrent shared ownership");
                std::string value = *reference;
                value.append(std::to_string(worker + i));
                require(value.size() > 4097, "concurrent string allocation");
                std::lock_guard<std::mutex> lock(mutex);
                count++;
            }
            checked.fetch_add(1);
        });
    }
    {
        std::lock_guard<std::mutex> lock(mutex);
        start = true;
    }
    ready.notify_all();
    for (auto& worker : workers) worker.join();
    require(count == 8000 && checked == 8, "eight joined workers");
    shared.reset();
    require(weak.expired(), "shared object retirement");
    auto future = std::async(std::launch::async, []() -> int {
        throw std::logic_error("future exception");
    });
    bool caught = false;
    try { (void)future.get(); }
    catch (const std::logic_error& error) { caught = std::strcmp(error.what(), "future exception") == 0; }
    require(caught, "asynchronous exception propagation");

    std::stringstream stream;
    stream.imbue(std::locale::classic());
    stream << std::hex << 0xabc123 << ' ' << std::fixed << std::setprecision(3) << 12.375;
    require(stream.str() == "abc123 12.375", "locale and numeric formatting");
    require(std::regex_match(std::string("native-12345"), std::regex("native-[0-9]+")), "regular expression");
    require(std::filesystem::path("alpha/./beta/../gamma").lexically_normal().string() == "alpha/gamma",
        "filesystem path normalization");

    void* plugin = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!plugin) std::fprintf(stderr, "dlopen: %s\n", dlerror());
    require(plugin != nullptr, "load C++ plugin");
    auto thrower = reinterpret_cast<void (*)(unsigned)>(dlsym(plugin, "throw_from_plugin"));
    auto makeString = reinterpret_cast<std::string* (*)(unsigned)>(dlsym(plugin, "string_from_plugin"));
    auto makeException = reinterpret_cast<std::exception* (*)()>(dlsym(plugin, "exception_from_plugin"));
    require(thrower && makeString && makeException, "plugin entry points");
    for (unsigned i = 0; i < 128; i++) {
        caught = false;
        try { thrower(i); }
        catch (const std::out_of_range& error) {
            caught = error.what() == "plugin boundary " + std::to_string(i);
        }
        require(caught, "exception crosses DSO boundary");
        std::unique_ptr<std::string> text(makeString(i * 37));
        require(*text == std::string(i * 37, 'p'), "cross-DSO string ownership");
        std::unique_ptr<std::exception> error(makeException());
        require(dynamic_cast<std::runtime_error*>(error.get()) != nullptr,
            "runtime type information across DSO boundary");
        require(std::strcmp(error->what(), "dynamic exception object") == 0, "virtual exception method");
    }
    require(dlclose(plugin) == 0, "close plugin after object retirement");
    std::printf("CXX_PROBE_PASS abi=%d workers=8 operations=8000 boundary_rounds=128\n", _GLIBCXX_USE_CXX11_ABI);
    return 0;
}
