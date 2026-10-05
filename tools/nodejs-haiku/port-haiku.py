#!/usr/bin/env python3
"""Apply the Haiku port edits to an extracted Node.js source tree.

usage: port-haiku.py <node-source-dir> [ares_config.h]

This is how the per-major patches in nvm/haiku/patches were produced: run it
on a pristine node-vX.Y.Z tree (with PORT_HAIKU_ORIG=<dir> it keeps the
originals, so `diff -u` of the touched files gives the patch; or use git),
fix whatever it reports as MANUAL by hand, build and test. Every edit is
anchored on the upstream source and skipped when its marker shows it is
already there; an anchor that is missing is reported instead of guessed.
Edits that only exist in some majors are marked optional.

The optional ares_config.h is c-ares' own configure result on Haiku (cmake
on the bundled deps/cares), which becomes deps/cares/config/haiku/.
"""
import os, re, shutil, sys

root = sys.argv[1]
manual = []
# PORT_HAIKU_ORIG=<dir>: keep a copy of every file before its first change
# (lets a patch be made with plain diff, without a git checkout).
orig_dir = os.environ.get("PORT_HAIKU_ORIG")


def backup(full):
    if not orig_dir:
        return
    rel = os.path.relpath(full, root)
    dst = os.path.join(orig_dir, rel)
    if not os.path.exists(dst):
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        if os.path.exists(full):
            shutil.copy2(full, dst)
        else:
            open(dst + ".NEW", "w").close()


def path(p):
    return os.path.join(root, p)


def edit(p, fn, what, done, optional=False):
    """Apply fn to file p unless the marker `done` shows it was applied."""
    full = path(p)
    if not os.path.exists(full):
        if not optional:
            manual.append("%s: missing file (%s)" % (p, what))
        return
    s = open(full, encoding="utf-8").read()
    if done in s:
        return
    try:
        t = fn(s)
    except LookupError as e:
        if not optional:
            manual.append("%s: %s (%s)" % (p, what, e))
        return
    if t == s:
        if not optional:
            manual.append("%s: no change for %s" % (p, what))
    else:
        backup(full)
        open(full, "w", encoding="utf-8").write(t)


def once(s, old, new):
    if s.count(old) != 1:
        raise LookupError("anchor found %d times: %r" % (s.count(old), old[:70]))
    return s.replace(old, new)


def resub(s, pattern, repl, flags=0):
    t, n = re.subn(pattern, repl, s, count=1, flags=flags)
    if n != 1:
        raise LookupError("pattern not found: %r" % pattern[:70])
    return t


# --- build system -----------------------------------------------------------

edit("configure.py",
     lambda s: resub(s, r"(valid_os = \((?:[^)]*?))\)", r"\1, 'haiku')", re.S),
     "valid_os += haiku", "'haiku')")

edit("tools/gyp/pylib/gyp/common.py",
     lambda s: resub(s, r"(def GetFlavor(?:ByPlatform)?\((?:params)?\):.*?)(\n\n([ \t]+)return [\"']linux[\"'])",
                     r'\1\n\3if sys.platform.startswith("haiku"):\n\3    return "haiku"\2', re.S),
     "GetFlavor haiku", 'startswith("haiku")')

edit("tools/utils.py",
     lambda s: resub(s, r"(\n(\s+)elif id == 'FreeBSD':\n\s+return 'freebsd'\n)",
                     r"\1\2elif id == 'Haiku':\n\2  return 'haiku'\n"),
     "GuessOS haiku", "id == 'Haiku'")


def common_gypi(s):
    s = resub(s, r"\[ 'OS in \"(linux freebsd openbsd solaris aix[^\"]*)\"', \{\n(\s+)'cflags': \[ '-pthread' \]",
              r"""[ 'OS in "\1 haiku"', {\n\2'cflags': [ '-pthread' ]""")
    s = resub(s, r"\[ 'OS in \"(linux freebsd openbsd solaris android aix[^\"]*)\"', \{",
              r"""[ 'OS in "\1 haiku"', {""")
    return s


edit("common.gypi", common_gypi, "POSIX OS lists", ' haiku"')


def node_gypi(s):
    s = resub(s, r"(\n    \[ 'OS==\"solaris\"', \{\n      'libraries': \[\n        '-lkstat',)",
              "\n    [ 'OS==\"haiku\"', {\n      'libraries': [\n        '-lbsd',\n        '-lnetwork',\n      ],\n"
              "      # libroot's locale backend loads Haiku's own ICU into the process;\n"
              "      # keep the bundled ICU out of the dynamic symbol table so the two\n"
              "      # copies cannot bind to each other (same-version names clash).\n"
              "      'ldflags': [\n        '-Wl,--exclude-libs,libicudata.a:libicui18n.a:libicuucx.a',\n      ],\n    }],\\1")
    # link all of V8 and OpenSSL into the binary, so that addons can use them
    s = resub(s, r'\(OS=="freebsd" or OS=="linux"((?: or OS=="\w+")*)\)', r'(OS=="freebsd" or OS=="linux"\1 or OS=="haiku")')
    s = resub(s, r"\['OS in \"linux freebsd([^\"]*)\" and node_shared==\"false\"', \{\n(\s+)'ldflags': \[\n\s+'-Wl,--whole-archive,?',?\n\s+'<\(obj_dir\)/deps/openssl/",
              lambda m: m.group(0).replace('"linux freebsd%s"' % m.group(1), '"linux freebsd%s haiku"' % m.group(1), 1))
    return s


# Node 22+: common_node.gypi switches Node's own sources to C++20 per OS.
edit("common_node.gypi",
     lambda s: resub(s, r"(\['OS in \"linux freebsd openbsd solaris android aix[^\"]*)(\"', \{\n\s+'cflags_cc!': \['-std=gnu\+\+17'\])",
                     r"\1 haiku\2"),
     "C++20 for Node core", ' haiku"', optional=True)

edit("node.gypi", node_gypi, "haiku libraries, whole-archive V8", '-lnetwork')

UV_HAIKU = """        [ 'OS=="haiku"', {
          'defines': [ '_BSD_SOURCE' ],
          'sources': [
            'src/unix/haiku.c',
            'src/unix/bsd-ifaddrs.c',
            'src/unix/no-fsevents.c',
            'src/unix/no-proctitle.c',
            'src/unix/posix-hrtime.c',
            'src/unix/posix-poll.c',
          ],
          'link_settings': {
            'libraries': [ '-lbsd', '-lnetwork' ],
          },
        }],
"""
edit("deps/uv/uv.gyp",
     lambda s: resub(s, r"(\n)(        \[ 'OS==\"freebsd\" or OS==\"dragonflybsd\"', \{\n)", r"\1" + UV_HAIKU.replace("\\", "\\\\") + r"\2"),
     "libuv haiku sources", 'src/unix/haiku.c')

CARES_HAIKU = """        [ 'OS=="haiku"', {
          'include_dirs': [ 'config/haiku' ],
          'sources': [ 'config/haiku/ares_config.h' ],
          'direct_dependent_settings': {
            'libraries': [ '-lnetwork' ],
          },
        }],
"""
edit("deps/cares/cares.gyp",
     lambda s: resub(s, r"(        \[ 'OS==\"openbsd\"', \{\n          'include_dirs': \[ 'config/openbsd' \],\n.*?\n        \}\],\n)",
                     lambda m: m.group(1) + CARES_HAIKU, re.S),
     "c-ares haiku config", 'config/haiku')
if len(sys.argv) > 2:
    os.makedirs(path("deps/cares/config/haiku"), exist_ok=True)
    backup(path("deps/cares/config/haiku/ares_config.h"))
    shutil.copy(sys.argv[2], path("deps/cares/config/haiku/ares_config.h"))
elif not os.path.exists(path("deps/cares/config/haiku/ares_config.h")):
    manual.append("deps/cares/config/haiku/ares_config.h: generate it with cmake on Haiku")

OPENSSL_HAIKU = """    }, 'OS=="haiku"', {
      'cflags': ['-Wno-missing-field-initializers',],
      'defines': [
        'OPENSSLDIR="/boot/system/data/ssl"',
        'ENGINESDIR="/dev/null"',
        'TERMIOS',
      ],
      # Haiku has no libdl (dlopen() is in libroot); sockets are in libnetwork.
      'libraries!': ['-ldl -pthread', '-lm -ldl -pthread'],
      'libraries': ['-lm', '-pthread', '-lnetwork'],
"""
edit("deps/openssl/openssl_common.gypi",
     lambda s: resub(s, r"(        '__EXTENSIONS__'\n      \],\n)(    \}, \{\n)", lambda m: m.group(1) + OPENSSL_HAIKU + m.group(2)),
     "openssl haiku defines", 'OS=="haiku"')

# OpenSSL's per-arch headers are picked by the preprocessor; Haiku uses the
# Linux configurations (same ELF/gas/pthreads platform, same 64-bit longs).
cfg = path("deps/openssl/config")
n = 0
for f in sorted(os.listdir(cfg)):
    if f.endswith((".h", ".tmpl")):
        p = os.path.join(cfg, f)
        s = open(p).read()
        t = s.replace("#if defined(__linux) && !defined(__ANDROID__)\n",
                      "#if (defined(__linux) && !defined(__ANDROID__)) || defined(__HAIKU__)\n")
        if t != s:
            backup(p)
            open(p, "w").write(t)
            n += 1
if n == 0 and not any("defined(__HAIKU__)" in open(os.path.join(cfg, f)).read()
                      for f in os.listdir(cfg) if f.endswith(".h")):
    manual.append("deps/openssl/config: no arch-selection headers changed")

# --- Node.js ------------------------------------------------------------------

edit("src/node.cc",
     lambda s: resub(s, r"(\n(\s+)if \(nr == SIGKILL \|\| nr == SIGSTOP\)\n\s+continue;\n)",
                     r"\1#ifdef __HAIKU__\n\2// SIGKILLTHR cannot be caught or ignored either.\n\2if (nr == SIGKILLTHR)\n\2  continue;\n#endif\n"),
     "skip SIGKILLTHR", 'SIGKILLTHR')

# Node 16 resets the signal dispositions a second time, at exit.
edit("src/node_main_instance.cc",
     lambda s: resub(s, r"(\n(\s+)if \(nr == SIGKILL \|\| nr == SIGSTOP \|\| nr == SIGPROF\)\n\s+continue;\n)",
                     r"\1#ifdef __HAIKU__\n\2if (nr == SIGKILLTHR)\n\2  continue;\n#endif\n"),
     "skip SIGKILLTHR at exit", 'SIGKILLTHR', optional=True)

# npm 8 (Node 16) moves cache entries with link()+unlink(); BFS has no hard
# links (EPERM), so move the file instead.
edit("deps/npm/node_modules/cacache/lib/util/move-file.js",
     lambda s: resub(s, r"(\n  try \{\n    await fs\.link\(src, dest\)\n  \} catch \(err\) \{\n)(    if \(isWindows && err\.code === 'EPERM'\) \{)",
                     r"\1    if (process.platform === 'haiku' && err.code === 'EPERM') {\n"
                     r"      // Haiku's BFS has no hard links: move the file instead.\n"
                     r"      await move(src, dest)\n"
                     r"      return fs.chmod(dest, '0444')\n"
                     r"    }\n\2"),
     "npm cacache without hard links", "haiku", optional=True)

# Node 24+ (socket.setTypeOfService): Haiku has no IPV6_TCLASS; an invalid
# option makes setsockopt()/getsockopt() fail, which is reported to JS.
edit("src/tcp_wrap.cc",
     lambda s: s if "IPV6_TCLASS" not in s else resub(s, r"(\nnamespace node \{\n)",
                     r"\n#if defined(__HAIKU__) && !defined(IPV6_TCLASS)\n#define IPV6_TCLASS -1  // not supported by Haiku\n#endif\n\1"),
     "no IPV6_TCLASS", "__HAIKU__", optional=True)

edit("src/debug_utils.cc",
     lambda s: resub(s, r"(\n    defined\(_AIX\))(\n#define HAVE_EXECINFO_H 0)", r"\1 || defined(__HAIKU__)\2"),
     "no execinfo", '__HAIKU__')


def node_report(s):
    s = resub(s, r"#if !\(defined\(_AIX\) \|\| defined\(__sun\)\)\n(\s+\{\"max_locked_memory_bytes\")",
              r"#if !(defined(_AIX) || defined(__sun) || defined(__HAIKU__))\n\1")
    s = resub(s, r"#ifndef __sun\n(\s+\{\"max_memory_size_k?bytes\")", r"#if !(defined(__sun) || defined(__HAIKU__))\n\1")
    s = resub(s, r"#ifndef __sun\n(\s+\{\"max_user_processes\")", r"#if !(defined(__sun) || defined(__HAIKU__))\n\1")
    return s


edit("src/node_report.cc", node_report, "Haiku lacks RLIMIT_MEMLOCK/RSS/NPROC", '__HAIKU__')


# ICU (bundled, full): Haiku has no /etc/localtime and TZ is normally unset;
# the zone picked in the Time preferences is kept by the kernel.
def icu_putil(s):
    s = resub(s, r"(\nU_CAPI const char\* U_EXPORT2\nuprv_tzname\(int n\)\n)",
              "\n#if U_PLATFORM == U_PF_HAIKU\n"
              "extern \"C\" int32_t _kern_get_timezone(int32_t* offset, char* name, size_t length);\n"
              "#endif\n\\1")
    s = resub(s, r"(\n    /\* else U_TZNAME will give a better result. \*/\n#endif\n)",
              "\\1\n#if U_PLATFORM == U_PF_HAIKU\n"
              "    /* The zone chosen in Haiku's Time preferences, as the kernel keeps it. */\n"
              "    {\n"
              "        static char haikuTimeZone[64];\n"
              "        if (_kern_get_timezone(nullptr, haikuTimeZone, sizeof(haikuTimeZone)) == 0\n"
              "                && isValidOlsonID(haikuTimeZone)) {\n"
              "            return haikuTimeZone;\n"
              "        }\n"
              "    }\n"
              "#endif\n")
    return s


edit("deps/icu-small/source/common/putil.cpp", icu_putil, "Haiku default time zone", '_kern_get_timezone')

# --- V8 -----------------------------------------------------------------------


def v8config(s):
    s = resub(s, r"(//  V8_OS_FREEBSD +- FreeBSD\n)", r"\1//  V8_OS_HAIKU         - Haiku\n")
    s = resub(s, r"(# define V8_OS_STRING \"freebsd\"\n\n)",
              r'\1#elif defined(__HAIKU__)\n# define V8_OS_HAIKU 1\n# define V8_OS_POSIX 1\n# define V8_OS_STRING "haiku"\n\n')
    return s


edit("deps/v8/include/v8config.h", v8config, "V8_OS_HAIKU", 'V8_OS_HAIKU')


def platform_posix(s):
    s = resub(s, r"(#if !defined\(_AIX\) && !defined\(V8_OS_FUCHSIA\)[^\n]*)(\n#include <sys/syscall.h>)",
              r"\1 && !defined(__HAIKU__)\2")
    # Haiku has its own Stack::ObtainCurrentThreadStackStart / GetStackStart.
    s = resub(s, r"(\n#if !defined\(V8_OS_FREEBSD\) && !defined\(V8_OS_(?:DARWIN|MACOSX)\) && !defined\(_AIX\) && \\\n\s+!defined\(V8_OS_SOLARIS\))",
              r"\1 && !defined(V8_OS_HAIKU)")
    return s


edit("deps/v8/src/base/platform/platform-posix.cc", platform_posix, "no sys/syscall.h, own stack start", '__HAIKU__')


def sampler(s):
    s = resub(s, r"(#if !V8_OS_QNX && !V8_OS_AIX[^\n]*)(\n#include <sys/syscall.h>)", r"\1 && !V8_OS_HAIKU\2")
    s = resub(s, r"#elif !V8_OS_OPENBSD\n#include <ucontext.h>", "#elif !V8_OS_OPENBSD && !V8_OS_HAIKU\n#include <ucontext.h>")
    s = resub(s, r"(\n#elif V8_OS_NETBSD\n#if V8_HOST_ARCH_IA32\n)",
              "\n#elif V8_OS_HAIKU\n#if V8_HOST_ARCH_X64\n"
              "  state->pc = reinterpret_cast<void*>(mcontext.rip);\n"
              "  state->sp = reinterpret_cast<void*>(mcontext.rsp);\n"
              "  state->fp = reinterpret_cast<void*>(mcontext.rbp);\n"
              "#endif  // V8_HOST_ARCH_*\\1")
    return s


edit("deps/v8/src/libsampler/sampler.cc", sampler, "Haiku ucontext registers", 'V8_OS_HAIKU')


def export_template(s):
    s = once(s, "#define EXPORT_TEMPLATE_DEFINE_MSVC_HACK(export, _) export\n",
             "#define EXPORT_TEMPLATE_DEFINE_MSVC_HACK(export, _) export\n"
             "#define EXPORT_TEMPLATE_TEST_MSVC_HACK_DEFAULT(...) true\n")
    s = once(s, "#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_MSVC_HACK\n",
             "#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_MSVC_HACK\n#undef EXPORT_TEMPLATE_TEST_MSVC_HACK_DEFAULT\n")
    return s


edit("deps/v8/src/base/export-template.h", export_template, "__declspec is predefined on Haiku", 'MSVC_HACK_DEFAULT')

edit("deps/v8/src/trap-handler/trap-handler.h",
     lambda s: resub(s, r"#if defined\(V8_OS_AIX\)\n(// `thread_local` does not link on AIX)",
                     r"#if defined(V8_OS_AIX) || defined(V8_OS_HAIKU)\n\1"),
     "__thread for g_thread_in_wasm_code", 'V8_OS_HAIKU', optional=True)

edit("tools/v8_gypfiles/features.gypi",
     lambda s: resub(s, r"(\n(\s+)\['OS==\"android\"', \{ # GYP reverts OS to linux)",
                     r"""\n\2['OS == "haiku"', {\n\2  'is_haiku': 1,\n\2}, {\n\2  'is_haiku': 0,\n\2}],\1"""),
     "is_haiku", 'is_haiku')

edit("tools/v8_gypfiles/v8.gyp",
     lambda s: resub(s, r"(\n(\s+)\['OS == \"mac\" or \(_toolset==\"host\" and host_os==\"mac\"\)', \{\n\s+'sources': \[\n\s+'<\(V8_ROOT\)/src/base/debug/stack_trace_posix.cc',)",
                     r"\n\2['is_haiku', {\n\2  'sources': [\n\2    '<(V8_ROOT)/src/base/debug/stack_trace_posix.cc',\n\2    '<(V8_ROOT)/src/base/platform/platform-haiku.cc',\n\2  ]\n\2}],\1"),
     "platform-haiku.cc", 'platform-haiku.cc')

# abseil (V8 12 and later): a thread ID for Haiku, whose pthread_t is a pointer.
def absl_sysinfo(s):
    s = resub(s, r"(\n#ifdef __NetBSD__\n#include <lwp.h>\n#endif\n)", r"\1\n#ifdef __HAIKU__\n#include <OS.h>\n#endif\n")
    s = resub(s, r"(\n#elif defined\(__NetBSD__\)\n\npid_t GetTID\(\) \{ return static_cast<pid_t>\(_lwp_self\(\)\); \}\n)",
              r"\1\n#elif defined(__HAIKU__)\n\npid_t GetTID() { return find_thread(nullptr); }\n")
    return s


edit("deps/v8/third_party/abseil-cpp/absl/base/internal/sysinfo.cc", absl_sysinfo, "abseil GetTID", "__HAIKU__", optional=True)

# V8 13+: Haiku links executables like shared objects, so the "local-exec"
# TLS model V8 uses in non-component builds does not link; use the
# shared-library mode (local-dynamic behind a getter).
edit("deps/v8/src/common/thread-local-storage.h",
     lambda s: resub(s, r"#if defined\(COMPONENT_BUILD\) \|\| defined\(V8_TLS_USED_IN_LIBRARY\)\n",
                     "#if defined(COMPONENT_BUILD) || defined(V8_TLS_USED_IN_LIBRARY) || defined(__HAIKU__)\n"),
     "TLS library mode", "__HAIKU__", optional=True)

# Node 26: the Rust crates (Temporal) are a staticlib whose std needs
# Haiku's socket (libnetwork) and arc4random (libbsd) functions.
edit("deps/crates/crates.gyp",
     lambda s: resub(s, r"(\n(\s+)'conditions': \[\n\s+\['OS==\"win\"', \{\n\s+'libraries': \[\n\s+'-lntdll',\n\s+'-luserenv'\n\s+\],\n\s+\}\],\n)",
                     lambda m: m.group(1) + m.group(2) + "  ['OS==\"haiku\"', {\n" + m.group(2) + "    'libraries': [ '-lnetwork', '-lbsd' ],\n" + m.group(2) + "  }],\n"),
     "Rust std needs libnetwork/libbsd", "haiku", optional=True)

# Node 21 builds QUIC (OpenSSL 3.0.13+quic -> ngtcp2/nghttp3), whose gyp
# only includes <arpa/inet.h>/<netinet/in.h> on Linux; without them htonl()
# and friends (macros on Haiku) end up as undefined functions. Only needed,
# and only tested, where the QUIC deps are actually built (21 of our majors).
_ver = open(path("src/node_version.h")).read()
_major = int(re.search(r"#define NODE_MAJOR_VERSION (\d+)", _ver).group(1))
if _major == 21:
    edit("deps/ngtcp2/ngtcp2.gyp",
         lambda s: s.replace("['OS==\"linux\" or OS==\"android\"', {\n          'defines': [\n            'HAVE_ARPA_INET_H',",
                             "['OS==\"linux\" or OS==\"android\" or OS==\"haiku\"', {\n          'defines': [\n            'HAVE_ARPA_INET_H',"),
         "QUIC deps need arpa/inet.h", "haiku")

# One platform-haiku.cc serves V8 9.4 to 14.6; it sits next to this script.
backup(path("deps/v8/src/base/platform/platform-haiku.cc"))
shutil.copy(os.path.join(os.path.dirname(os.path.abspath(__file__)), "platform-haiku.cc"),
            path("deps/v8/src/base/platform/platform-haiku.cc"))

for m in manual:
    print("MANUAL:", m)
print("done, %d manual items" % len(manual))
