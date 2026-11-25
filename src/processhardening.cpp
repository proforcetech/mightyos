#include "processhardening.h"

#include <QLoggingCategory>

#include <atomic>
#include <cerrno>
#include <cstring>
#include <sys/prctl.h>
#include <sys/resource.h>

Q_LOGGING_CATEGORY(lcHardening, "mightyos.session")

namespace {

// The soft limit MightyOS started with; written once before any launch,
// read in forked children (plain loads of a lock-free atomic).
std::atomic<bool> g_saved { false };
std::atomic<rlim_t> g_savedSoft { 0 };

} // namespace

namespace ProcessHardening {

bool disableCoreDumps()
{
    bool ok = true;
    rlimit limit {};
    if (::getrlimit(RLIMIT_CORE, &limit) == 0) {
        if (!g_saved.load()) {
            g_savedSoft.store(limit.rlim_cur);
            g_saved.store(true);
        }
        limit.rlim_cur = 0;
        if (::setrlimit(RLIMIT_CORE, &limit) != 0) {
            qCWarning(lcHardening) << "RLIMIT_CORE not lowered:" << std::strerror(errno);
            ok = false;
        }
    } else {
        ok = false;
    }
    if (::prctl(PR_SET_DUMPABLE, 0, 0, 0, 0) != 0) {
        qCWarning(lcHardening) << "PR_SET_DUMPABLE not cleared:" << std::strerror(errno);
        ok = false;
    }
    return ok;
}

void restoreChildLimits() noexcept
{
    if (!g_saved.load(std::memory_order_relaxed))
        return;
    rlimit limit {};
    if (::getrlimit(RLIMIT_CORE, &limit) != 0)
        return;
    const rlim_t soft = g_savedSoft.load(std::memory_order_relaxed);
    limit.rlim_cur = (limit.rlim_max == RLIM_INFINITY || soft <= limit.rlim_max) ? soft : limit.rlim_max;
    ::setrlimit(RLIMIT_CORE, &limit);
}

bool isNonDumpable()
{
    return ::prctl(PR_GET_DUMPABLE, 0, 0, 0, 0) == 0;
}

} // namespace ProcessHardening
