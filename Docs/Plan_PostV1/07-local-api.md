# 07 — Local API (Stage 18)

Native port of the web's remote-control surface
(`lib/trackerLocalApi.ts`, `lib/trackerLocalApiSchema.ts`,
`components/TrackerLocalApiWindow.tsx`) — locked decision 3: Savannah
uses it, it ships this cycle. WebSocket-based on **IXWebSocket**
(BSD-3-Clause, client+server in one library, TLS off for localhost,
pinned to a release tag; uWebSockets recorded as fallback —
Research/03, applied 2026-07-18).

## Shape

- A WebSocket server on localhost (port + token in settings),
  running on its own thread; requests marshal onto the UI thread's
  frame loop (single consumer of `ProjectSession` — same discipline
  as every view).
- Schema: port the web command surface against the session API —
  transport control, cell/pattern edits, instrument/sample queries,
  workspace/cable operations, plugin parameter get/set, project
  load/save/export triggers. JSON messages (nlohmann in-tree).
- Auth: bearer token (generated, shown in the window, stored in
  settings) — localhost-only binding by default.

## Fix-don't-retain: web fix-list #5 paid here

`trackerLocalApiSchema.ts:104-107` gaps, fixed in the port rather
than reproduced:
- Sample binary upload lands (base64 or binary frames → decoded
  through the standard sample path).
- Workspace-ID discovery lands (enumerate nodes/ports/cables).
- Strict ID validation: bogus IDs are rejected with typed errors,
  never silently accepted.

## Window

`src/ui/local_api_view.{h,cpp}`: enable toggle, port, token (with
regenerate), connected-client list, last-request log tail.

## File map

- `src/api/local_api.{h,cpp}` — server, schema dispatch (new)
- `src/ui/local_api_view.{h,cpp}` — window (new)
- `src/app/project_session.*` — no new surface expected; the API
  consumes what views already use (any gap found is a session gap
  worth having anyway)

## Verification

A loopback WebSocket client in the test suite drives the schema end
to end: transport start/stop reflected in snapshots; cell writes
diffed; invalid IDs → typed errors; sample upload → audible via the
preview path (unit-level: buffer present + decoded length). Token
rejection tested.
