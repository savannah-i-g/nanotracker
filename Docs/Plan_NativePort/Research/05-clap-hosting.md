# Research — CLAP Hosting

## Findings

| Fact | Value |
| --- | --- |
| Standard | single C header ABI, MIT; repos: `free-audio/clap`, `clap-helpers` (C++ glue), `clap-host` (reference Qt host), `clap-plugins` |
| Tutorial | nakst's CLAP tutorial covers plugin+host, incl. X11 GUI part |
| Linux GUI model | plugin creates an X11 window and embeds into a host-provided window via **XEmbed** (`_XEMBED_INFO` atom); embedding supported on Win32/Cocoa/X11, **not Wayland** |
| Event loop | host does not forward mouse events; plugin receives native X11 events; host must implement `clap.posix_fd_support` (and ideally `clap.timer_support`) so plugins get callbacks when their display fd has pending events |
| ImGui precedent | `schwaaa/clap-imgui` demonstrates ImGui-based CLAP GUI code |

## Plan implications

- Refines [../08-external-plugins.md](../08-external-plugins.md): the
  host creates one top-level X11 window per open editor; the plugin
  embeds into it via XEmbed. Under a Wayland session these windows run
  through XWayland — consistent with the platform policy.
- The host's extension surface for v1 CLAP: `params`, `state`,
  `audio-ports`, `note-ports`, `gui`, `posix-fd-support`,
  `timer-support`, `latency`. The fd/timer pair is not optional on
  Linux — plugin GUIs starve without it.
- `clap-helpers` and the reference host are implementation guides;
  the nakst tutorial is the fastest orientation for the hosting stage.

## Sources

- [CLAP developers — getting started](https://cleveraudio.org/developers-getting-started/)
- [free-audio/clap — gui.h extension header](https://github.com/free-audio/clap/blob/main/include/clap/ext/gui.h)
- [free-audio/clap-host — reference host](https://github.com/free-audio/clap-host)
- [nakst CLAP tutorial, part 3 (GUI/X11)](https://nakst.gitlab.io/tutorial/clap-part-3.html)
- [schwaaa/clap-imgui](https://github.com/schwaaa/clap-imgui/blob/main/README.md)
- [KVR — CLAP and Linux UI development](https://www.kvraudio.com/forum/viewtopic.php?t=619897)
