#pragma once

// Process-level hardening for the MightyOS GUI (PLAN section 4, "Session
// hardening").
//
// disableCoreDumps(): RLIMIT_CORE soft limit 0 and prctl(PR_SET_DUMPABLE, 0).
// A core of the launcher could hold the SteamGridDB key, and later PIN /
// password entry; non-dumpable also keeps other processes of the same user
// from ptrace-attaching or reading /proc/<pid>/mem. Both are per process:
//  - "dumpable" is reset by execve(), so launched apps are unaffected,
//  - RLIMIT_CORE is inherited across fork/exec, so launch paths call
//    restoreChildLimits() in their child-process modifier to give apps the
//    soft limit MightyOS started with.
// Apport: with fs.suid_dumpable = 2 (Ubuntu with Apport on) the kernel still
// pipes a non-dumpable process's core to Apport as root-only; the README's
// Apport exclusion (share/mightyos/system/apport) covers that.
namespace ProcessHardening {

// Returns false if either call failed (logged). Idempotent.
bool disableCoreDumps();

// Async-signal-safe (only setrlimit()); for QProcess child modifiers, which
// run between fork() and execve(). No-op unless disableCoreDumps() ran.
void restoreChildLimits() noexcept;

// prctl(PR_GET_DUMPABLE) == 0.
bool isNonDumpable();

} // namespace ProcessHardening
