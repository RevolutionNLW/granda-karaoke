#include "library/WritePriority.h"

#include <atomic>

namespace writepriority {

namespace {

std::atomic_int g_waiting = 0;

} // namespace

UserWrite::UserWrite()
{
    g_waiting.fetch_add(1);
}

UserWrite::~UserWrite()
{
    g_waiting.fetch_sub(1);
}

bool userWriteWaiting()
{
    return g_waiting.load() > 0;
}

} // namespace writepriority
