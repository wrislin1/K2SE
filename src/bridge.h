#pragma once
#include <cstdint>

// ============================================================================
// living-kotor2 bridge: LK2_Post / LK2_Poll and the socket worker behind them.
//
// The game thread only ever touches the mailbox under one critical section;
// a worker thread owns the TCP client socket to the Python responder on
// 127.0.0.1:kPort and does every blocking call. That split is exit criterion
// E4: with the responder stopped, slow or absent, a script's LK2_ call costs a
// lock and a memcpy and nothing else.
//
// Winsock is loaded with LoadLibrary on the worker, never linked: the shipped
// DLL keeps importing only KERNEL32 (tools/check_dll.py enforces it) and no
// socket code runs under the loader lock. The worker itself starts lazily on
// the first LK2_ call, from the game thread, so a save that never runs a
// bridge script never opens a socket.
// ============================================================================

namespace k2se {
namespace bridge {

constexpr int kPort = 47021;  // Windows -> WSL2 reachability proven 2026-10-09

// Game thread. Queues `text` for the responder and returns its id (>= 1), or 0
// if the outbound queue is full. Never blocks.
int Post(const char* text, uint32_t len);

// Game thread. Copies the reply for `id` (see lk2_mailbox.h for the -1 / 0
// conventions) into `out`, NUL-terminated, at most cap-1 bytes. False, and
// `out` untouched, when nothing has arrived. Never blocks.
bool Poll(int id, char* out, uint32_t cap, uint32_t* len);

// DLL_PROCESS_DETACH: stop the worker with a bounded wait and log the counts.
void Remove();

// bit 0 worker started, bit 1 socket connected.
int Status();

}  // namespace bridge
}  // namespace k2se
