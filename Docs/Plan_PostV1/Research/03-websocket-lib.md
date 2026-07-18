# 03 — Embeddable WebSocket Server Library

Grounds Stage 18 (`../07-local-api.md`): the Local API needs an
embeddable C++ WebSocket *server* with a permissive licence and a
build that doesn't fight our CMake/FetchContent tree.

## Candidates

| Library | Licence | Server? | Build | Notes |
| --- | --- | --- | --- | --- |
| IXWebSocket | BSD-3-Clause | yes (client+server+HTTP) | CMake, minimal deps, TLS optional, MSVC/Windows in CI | maintainer signalled slowing maintenance (2026-06) but releases continue (latest 2026-06-25) |
| uWebSockets | Apache-2.0 (uSockets: Apache-2.0) | yes | no first-party CMake; needs uSockets + platform event loop | performance-first design, heavier integration than a localhost API needs |
| WebSocket++ | BSD | yes | header-only but Boost/ASIO-flavoured | older; ASIO dependency we don't otherwise carry |

## Decision

**IXWebSocket, pinned to a release tag.**

- BSD-3-Clause fits the GPLv3 app and the `LICENSES/` scrub.
- Server + client in one small library — the test suite's loopback
  client (Stage 18 verification) uses the same dependency.
- TLS stays off (localhost binding + bearer token per the stage doc);
  zlib/deflate support can be disabled at configure time, so no new
  transitive deps.
- Tested on Windows/MSVC in its own CI — no Stage 13 seam risk.
- The maintenance-slowdown note is the known trade-off: acceptable
  for a localhost control surface with a pinned version; uWebSockets
  is the recorded fallback if IXWebSocket bit-rots against a future
  toolchain.

## Plan implications

- `../07-local-api.md` names IXWebSocket; threading shape unchanged
  (its server runs worker threads; we marshal requests onto the UI
  frame loop exactly as planned).

## Sources

- https://github.com/machinezone/IXWebSocket
- https://machinezone.github.io/IXWebSocket/
- https://en.wikipedia.org/wiki/Comparison_of_WebSocket_implementations
- https://cpp.libhunt.com/compare-ixwebsocket-vs-uwebsockets-unetworking
- https://github.com/uNetworking/uWebSockets
