#pragma once
#include <cstddef>
#include <cstdint>

// ============================================================================
// living-kotor2 bridge: the wire codec.
//
// One message per line, newline-delimited JSON, a FLAT object with exactly the
// two keys the bridge needs:
//
//     {"id":7,"text":"hello"}\n
//
// Both directions use the same shape. The decoder ignores any other key, so the
// Python side can grow the protocol (conversation ids, event types) without a
// DLL rebuild, and tolerates whitespace and key order so a hand-typed line works.
//
// No Windows headers, no allocation, no CRT stream I/O: this file is unit-tested
// on Linux (tests/lk2_proto_test.cpp) and compiled into the DLL unchanged.
// ============================================================================

namespace k2se {
namespace lk2 {

// Writes {"id":<id>,"text":"<escaped text>"}\n into `out`. Returns the number of
// bytes written (no terminator counted; one is added when there is room), or 0
// if `cap` cannot hold the whole line -- in which case nothing is written.
size_t EncodeLine(int id, const char* text, size_t textLen, char* out, size_t cap);

// Parses one line (a trailing newline is tolerated). Requires an integer "id"
// and a string "text"; copies the text into `textOut` as a NUL-terminated
// string of at most cap-1 bytes and reports whether it had to cut it. Unicode
// escapes above U+00FF become '?', which is what the engine's 8-bit text would
// have shown anyway. Returns false and leaves the outputs undefined on any
// malformed input.
bool DecodeLine(const char* line, size_t lineLen, int* id, char* textOut, uint32_t cap,
                uint32_t* textLen, bool* truncated);

}  // namespace lk2
}  // namespace k2se
