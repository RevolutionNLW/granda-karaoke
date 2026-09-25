#include "DisplaySleepBlocker.h"

#include "Logging.h"

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/pwr_mgt/IOPMLib.h>
#elif defined(_WIN32)
#include <windows.h>
#endif

DisplaySleepBlocker::~DisplaySleepBlocker()
{
    setActive(false);
}

void DisplaySleepBlocker::setActive(bool active)
{
    if (m_active == active)
        return;
    m_active = active;
    if (active) {
#if defined(__APPLE__)
        IOPMAssertionID assertion = kIOPMNullAssertionID;
        const IOReturn result = IOPMAssertionCreateWithName(
            kIOPMAssertPreventUserIdleDisplaySleep, kIOPMAssertionLevelOn,
            CFSTR("Frankie's Karaoke Studio is playing lyrics"), &assertion);
        m_acquired = result == kIOReturnSuccess;
        m_assertion = assertion;
        if (!m_acquired)
            qCWarning(lcUi) << "Display sleep assertion failed:" << result;
#elif defined(_WIN32)
        m_acquired = SetThreadExecutionState(ES_CONTINUOUS | ES_DISPLAY_REQUIRED) != 0;
        if (!m_acquired)
            qCWarning(lcUi) << "Display sleep request failed:" << GetLastError();
#endif
        qCInfo(lcUi) << "Display sleep blocker acquired; OS request installed:" << m_acquired;
    } else {
#if defined(__APPLE__)
        if (m_acquired) {
            const IOReturn result = IOPMAssertionRelease(m_assertion);
            if (result != kIOReturnSuccess)
                qCWarning(lcUi) << "Display sleep assertion release failed:" << result;
        }
#elif defined(_WIN32)
        if (m_acquired && SetThreadExecutionState(ES_CONTINUOUS) == 0)
            qCWarning(lcUi) << "Display sleep request reset failed:" << GetLastError();
#endif
        m_acquired = false;
        m_assertion = 0;
        qCInfo(lcUi) << "Display sleep blocker released";
    }
}
