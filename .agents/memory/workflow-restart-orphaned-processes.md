---
name: Replit workflow restarts can orphan long-running native processes
description: Why a "workflow restart" can silently leave old instances of a compiled binary running, and how to defend against it
---

When a Replit workflow runs a shell pipeline like `make clean && make all && ./binary`, restarting the workflow sends a signal to the shell/process group, but a binary that has already forked/execed can end up reparented to the container's init (pid 2) instead of dying with its launching shell. Across several restarts (e.g. triggered by repeated task merges each rebuilding the same workflow), this can leave multiple copies of the same long-running binary alive simultaneously, each on its own pty, all fully functional and doing real work (e.g. all connected to the same external account/API).

Symptoms look like intermittent, unexplained "shutdown" messages in logs (a signal handler firing "Arrêt en cours..." / "shutting down...") even though no crash or exception is present — because a *different* orphaned instance is still running and producing dashboard/log output, making it look like the process that shut down is the only one.

**Why:** for anything that performs real-world side effects on a shared external resource (trading account, payment API, database writes with side effects), running two instances concurrently is far more dangerous than a normal restart blip — it causes duplicate/conflicting actions and corrupted shared state.

**How to apply:** for any long-running native process managed by a Replit workflow that talks to an external stateful account/API, add an OS-level single-instance guard in the process itself (e.g. `flock()` on a well-known lockfile path such as `/tmp/<app>.lock`), independent of whatever the workflow/process manager does. Verify by attempting to launch the binary a second time while the first is running and confirming it refuses to start.
