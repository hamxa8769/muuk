#pragma once

namespace muuk::desktop {

// "Start Muuk with Windows": this program in the signed-in user's Run key
// (HKCU\Software\Microsoft\Windows\CurrentVersion\Run, value "Muuk"), so it
// opens after a restart or a power cut -- with Auto-connect it then picks up
// the running basket by itself. Only this user, no admin rights needed.
[[nodiscard]] bool startsWithWindows();
bool setStartWithWindows(bool on);

// A "Muuk" shortcut to this program on the desktop or in the Start menu
// (this user's), opening in its own folder; true when it's there.
enum class ShortcutPlace { Desktop, StartMenu };
[[nodiscard]] bool hasShortcut(ShortcutPlace where);
bool createShortcut(ShortcutPlace where);

}  // namespace muuk::desktop
