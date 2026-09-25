#pragma once

#include <cstdint>

// Owns an idle-display-sleep request. Used on the UI thread (required by the
// Windows execution-state API). OS failures are logged; isActive() tracks the
// requested lifetime, not the machine's power state.
class DisplaySleepBlocker {
public:
    DisplaySleepBlocker() = default;
    ~DisplaySleepBlocker();
    DisplaySleepBlocker(const DisplaySleepBlocker&) = delete;
    DisplaySleepBlocker& operator=(const DisplaySleepBlocker&) = delete;

    void setActive(bool active);
    bool isActive() const { return m_active; }

private:
    bool m_active = false;
    bool m_acquired = false;
    std::uint32_t m_assertion = 0;
};
