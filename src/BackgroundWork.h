#pragma once

#include <QString>

#include <functional>

// Short jobs run off the GUI thread (counting song names, looking for sound
// outputs). Each job copies what it needs and never touches the object that
// started it, so that object can go away at any time. The program keeps track
// of the jobs so it can wait for them before Qt shuts down.
namespace background {

// Starts a job on its own thread. Call from the GUI thread.
void run(const QString& name, std::function<void()> job);
// Jobs started and not yet finished.
int running();
// Waits up to timeoutMs for every job to finish and tidies them away. Returns
// false if one is still running (stuck in the system), in which case the
// program must not shut Qt down around it. Call from the GUI thread.
bool waitForAll(int timeoutMs);

} // namespace background
