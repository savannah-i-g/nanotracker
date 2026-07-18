# 07 — Plugin GUI Hosting: CLAP on Win32, VST3 IRunLoop

Grounds the Stage 13 editor-window abstraction
(`../02-release-engineering.md`) and Stage 20 VST3 editors
(`../09-plugin-platform.md`).

## CLAP on Win32

The simple case, confirmed by the reference tutorial:

- Host creates a plain `CreateWindowEx` top-level window, passes
  `clap_window{ .api = CLAP_WINDOW_API_WIN32, .win32 = hwnd }` to
  `gui->set_parent()`; the plugin calls `SetParent` internally.
- No fd/timer plumbing needed: the Win32 message pump *is* the event
  loop. `clap.posix_fd_support` is Linux-only and compiles out on
  Windows (as the Stage 13 doc already states); `clap.timer_support`
  drives via `SetTimer`/WM_TIMER or our frame-loop ticks.
- Implication for the Stage 13 abstraction: the platform interface
  needs create/destroy/resize/title + "pump events" + a native-handle
  getter; X11 impl keeps the fd/timer pump, Win32 impl is a message
  loop serviced from the frame loop.

## VST3 IRunLoop (Linux) — the Stage 20 contract

- On Linux there is no global event loop, so the *host* must provide
  `Steinberg::Linux::IRunLoop`. Plugins obtain it two ways — both
  must work: via the `IPlugFrame` passed to `IPlugView::setFrame`
  (queried with `queryInterface`), and via the host context set with
  `IPluginFactory3::setHostContext`.
- API surface: `registerEventHandler(fd)` / `registerTimer(ms)` +
  unregister counterparts. Semantically identical to what our CLAP
  host already implements (posix-fd + timer pump from the UI frame
  loop).
- **Threading trap (the load-bearing finding):** JUCE-built plugins
  assume IRunLoop callbacks arrive on the UI/main thread. A naive
  poll()-on-a-worker-thread implementation crashes or deadlocks them.
  Our frame-loop dispatch already satisfies this — keep it; never
  move the pump off the UI thread.
- Known host bugs to avoid (Cockos upstream fix): timer re-entrancy
  and unregister-during-dispatch; guard handler lists against
  mutation while iterating.
- On Windows, VST3 views need no IRunLoop (global message loop);
  the same Stage 13 abstraction carries `IPlugView::attached` with a
  `HWND`.

## MSVC lock (feeds Research/01)

The VST3 SDK is effectively MSVC-only on Windows: `__uuidof` usage,
documented MinGW runtime crashes (DLL crashes on load), MinGW-built
artifacts undetected by MSVC hosts, and Steinberg's own guidance
naming Visual Studio. This rules MinGW out for the whole app (the
host links SDK hosting sources directly), locking CI to MSVC.

## Sources

- https://steinbergmedia.github.io/vst3_dev_portal/pages/Technical+Documentation/Provide+A+Runloop+On+Linux/Index.html
- https://steinbergmedia.github.io/vst3_doc/base/classSteinberg_1_1Linux_1_1IRunLoop.html
- https://forums.steinberg.net/t/doc-clarification-on-irunloop/916726
- https://forum.cockos.com/showthread.php?p=2788440
- https://nakst.gitlab.io/tutorial/clap-part-3.html
- https://forum.juce.com/t/is-vst3-sdk-compatible-with-mingw/12385
- https://forums.steinberg.net/t/mingw-plugin-not-detected-by-the-msvc-testhost-but-validated-with-mingw-validator/825507
- https://steinbergmedia.github.io/vst3_dev_portal/pages/Getting+Started/How+to+setup+my+system.html
