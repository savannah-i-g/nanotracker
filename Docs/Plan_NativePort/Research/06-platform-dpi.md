# Research — Platform DPI and Node-Editor Prior Art

## Wayland / DPI findings

| Fact | Value |
| --- | --- |
| Known defect | ImGui windows blurry under Wayland fractional scaling (upstream issue imgui#7433, open since March 2024) |
| Protocol state | `wp-fractional-scale-v1` exists since Nov 2022; SDL supports it; GLFW/ImGui integration remains the weak link |
| Standard practice | query per-monitor content scale (`ImGui_ImplGlfw_GetContentScaleForMonitor`), re-rasterise fonts on scale change; many Linux ImGui apps simply ignore scale |

Plan implications ([../09-windows-ui.md](../09-windows-ui.md),
[../02-architecture.md](../02-architecture.md#platform-policy)):

- The dedicated DPI task in the platform layer is justified; blurry-
  under-fractional-scaling is an upstream reality, not a porting bug to
  chase.
- Practical posture: integer-scale rendering with font re-rasterisation
  on content-scale change; if a session's fractional scaling produces
  blur, `GLFW_PLATFORM_X11` is the documented user-facing escape hatch
  (external plugin editors are X11-bound anyway).

<a name="node-editor-prior-art"></a>
## Node-editor prior art

| Project | Relevance |
| --- | --- |
| `thedmd/imgui-node-editor` | mature ImGui node editor; ships `imgui_bezier_math` (length, subdivision, sampling, closest-point) |
| `Nelarius/imnodes` | small dependency-free node editor; documents bezier closest-point via hierarchical segment subdivision |

Plan implications ([../06-graph-cables.md](../06-graph-cables.md)):

- Cable rendering stays custom (verlet rope is part of NanoTracker's
  identity; these libraries impose their own node-window model that
  conflicts with our instrument windows).
- Adopt the *technique*, not the library: hierarchical closest-point
  subdivision for cable hit-testing (right-click delete, midpoint chip)
  works identically on verlet polylines.

## Sources

- [imgui#7433 — blurry on Wayland fractional scaling](https://github.com/ocornut/imgui/issues/7433)
- [HiDPI/dynamic scaling gist for ImGui+GLFW](https://gist.github.com/benpm/21afb58f2c8dfdbf881ca90c76ad602e)
- [Dear ImGui, Linux, and HiDPI displays](https://allyourfaultforever.com/posts/hidpi-imgui-linux/)
- [thedmd/imgui-node-editor](https://github.com/thedmd/imgui-node-editor)
- [imnodes design write-up](https://nelari.us/post/imnodes/)
