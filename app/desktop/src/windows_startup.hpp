#pragma once

namespace muuk::desktop {

// "Start Muuk with Windows": this program in the signed-in user's Run key
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "Muuk"), so it
// opens after a restart or a power cut -- with Auto-connect it then picks up
// the running basket by itself. Only this user, no admin rights needed.
[[nodiscard]] bool startsWithWindows();
bool setStartWithWindows(bool on);

}  // namespace muuk::desktop
