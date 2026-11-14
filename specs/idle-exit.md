# Idle exit and on-demand start

Ticket 189056. alex, 2026-11-14: *"it shouldn't keep running when there are
no clients connected"*, and *"when nobody is watching it and no music is coming
through it can end after a few minutes of inactivity."*

Interview answers (2026-11-14): exit after **5 minutes**. Only **a viewer**
starts it again; music alone does not. During a cold start the Apple TV shows
**its Metal fallback**.

## Why

With no client connected, the service already releases the GPU after
`--idle-release` (30 s), but the process stays up. Measured on Iridium on
2026-11-14: ~27% of one CPU core. `top -H` puts it all on the simulation
thread: 120 Hz steps at ~2 ms each, which keep running with no viewer. The
exit bounds that cost to the 5 minutes after the last activity. Pausing the
simulation while idle is a separate optimisation, noted on the ticket.

## Behaviour

- **Activity** is any of: a TCP stream client (8770), an SRT client (8772), a
  browser reader on MediaMTX's `visualiser` path (the existing reader-count
  poll), or music playing (the engine's existing `playing`). A connected
  client is a viewer for as long as its socket is open, so the exit relies on
  the Apple TV closing the socket when its view closes or the set sleeps.
- **Not activity:** a request to the control port 8771, including the
  dashboard's status lookups.
- **Exit:** after `--idle-exit` seconds with no activity, the engine calls
  `requestStop()`, the same path as SIGTERM, with the same shutdown deadline.
  The process exits 0, so `Restart=on-failure` does not bring it back.
  - `--idle-exit 0` (the binary's default) disables it. Deployed:
    `NOVA_VISUALISER_IDLE_EXIT=300`.
  - The clock starts at launch, so a wake that no viewer follows ends after the
    same 5 minutes.
  - **With `--house-party` set, the idle exit is disabled**, so lighting
    driven by this service is never cut off by it. House party is not enabled
    on Iridium today.
- **Music keeps it alive but does not start it.** While the dashboard is
  unreachable, the last now-playing state is kept. A stale "playing" therefore
  keeps the renderer up rather than ending it, which fails safe.
- **The socket's port must equal `NOVA_VISUALISER_CONTROL_PORT`** (8771). The
  service adopts the inherited socket only when the ports match, and otherwise
  binds its own, which would collide with the socket.

## Starting again: only a viewer

Both viewers find the renderer through the dashboard route
`/api/phonoscope/renderer`, which calls `8771/status`.

- The Apple TV (SRT) retries the route at 2, 4, 8 s, then every 10 s, while its
  Phonoscope view is visible, and attaches once it answers `available: true`.
- The debug page calls the route too. No other code calls it.

So **the control port is the door**:

- **`nova-visualiser.socket`** holds `ListenStream=8771` and starts the service
  on the first connection.
- **The binary adopts the inherited socket** (`LISTEN_PID`/`LISTEN_FDS`,
  matched to the control port by `getsockname`). It never `shutdown()`s that
  socket on stop, because systemd holds the same socket; a 250 ms poll ends
  the accept loop instead. Without an inherited socket (run by hand, tests) it
  binds as before.
- **8770 and 8772 are bound before the control port answers**
  (`Engine::run` starts the stream and SRT servers before the control
  server). So a 200 from `/status` (it is 200 even with the GPU released) means
  the ports it advertises are already accepting.
- **The GPU starts only when a viewer connects.** The unconditional GPU start
  at the top of the render loop is gone. The loop's existing on-demand path
  (the one it takes after the idle release) starts it. So a lookup that no
  viewer follows costs a process and the simulation, never a GL/CUDA/NVENC
  init, and ends after 5 minutes. The debug page (a 1 Hz poller) attaches a
  WHEP player, so while it is open it is a real viewer.
- **Exit status.** A requested stop (SIGTERM or the idle exit) exits 0
  whatever non-fatal error was recorded during the run (a publish hiccup, an
  SRT rung that failed). Only a run that failed on its own, such as a port that
  would not bind, exits 1. Either way the process `_exit()`s, skipping static
  destructors: libsrt-gnutls 1.5's exit-time finaliser segfaults in
  `__cxa_finalize` (found in testing). A crash on an idle exit would read as a
  failure, and `Restart=on-failure` would restart it.
- **Browsers that skip the lookup cannot wake it.** A WHEP/HLS reader only
  counts as a viewer once the renderer is publishing, and it publishes only
  after a viewer has started the GPU. That was already true after an idle GPU
  release, so it is not a regression. Players that use the route (the debug
  page) wake it as the Apple TV does. A MediaMTX `runOnDemand` wake was tried
  in review and removed: it would have started the process and nothing more.
- **Route callers:** the Apple TV (its lookup while its view is visible) and
  the debug page (1 Hz while open). During a cold start, the first lookup may
  hit the route's 2 s timeout and read "unavailable" for one poll. The callers'
  retries cover it.
- **A lookup during the 8 s shutdown** waits in the socket backlog, and systemd
  starts the service again once it has stopped.
- **Cold start:** measured on Iridium and recorded below. Until frames arrive,
  the Apple TV shows its Metal engine. The engine draws nothing before its first
  configuration read, so the first streamed frame carries the current track.
  The user sees the Metal picture first, then the swap to the streamed look.

## GPU guard interplay (nova-monitoring, ticket 097318)

A shed must not be undone by the next lookup. The guard's shed list becomes
`nova-visualiser.socket`, then `nova-visualiser.service`: they are stopped in
that order, and on recovery the socket is started first. Tier 5 also stops the
socket. The live `/etc/nova-gpu-guard/guard.json` is updated in the same
deploy.

## Systemd

- `nova-visualiser.socket` is enabled (`WantedBy=sockets.target`).
  `nova-visualiser.service` loses `WantedBy=multi-user.target` and is disabled;
  it starts only through the socket or by hand.
- `StartLimitBurst` stays: a renderer that cannot start gives up, and the Apple
  TV stays on Metal.
- `deploy-nova-visualiser.ps1` (workspace repo) installs and enables the socket,
  disables the service's autostart, and runs the conformance corpus and GPU
  probe as before.

## Done means

- On Iridium: the process is gone 5 minutes after the last viewer and the
  music stop, and `systemctl status` shows the socket listening and the service
  inactive.
- The route `/api/phonoscope/renderer` (or a GET on 8771) starts it. A track
  starting does not. A lookup that no viewer follows ends after 5 minutes.
- The cold start is measured (start to `/status` 200, start to the first frame
  to a client) and recorded here.
- The guard's shed stops the socket and the service.
- Unit tests for the idle decision (activity resets it, music counts only while
  running, 0 disables, house party disables) and for picking the inherited
  socket by port.

## Cold start (measured)

Iridium, 2026-11-14, branch build under `systemd-socket-activate` on spare
ports, timed the way the Apple TV does it (renderer lookup, then connect):

| Step | Time from the lookup |
|---|---|
| `/status` answered (process started by the connection) | 0.05 s |
| First stream bytes after connecting | 0.05 s |
| 300 KB received (past the first keyframe) | 1.88 s |

That is well inside the Apple TV's 8 s attach stall limit, so no Apple TV
change is needed. A lookup with no viewer left no GPU context. The idle exit
fired on time and ended with status 0. A viewer disconnecting started the idle
clock, and the process exited one idle period later.

**Accepted trade-off:** the first open after an idle exit shows the Apple
TV's Metal engine for about 2 s, then swaps to the streamed look.

Produced by: the interactive session working ticket 189056 (no plan file; the
plan is the `PLAN:` comment on the ticket).
