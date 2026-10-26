// Peggle: profile collection of the PGO-instrumented Switch build (ZUMA_PGO=generate), see zuma_pgo.cpp.
// In every other build these do nothing. From sa2-nx (sa2_pgo.h).

#pragma once

namespace zuma::pgo {

// Picks this session's file (pgo/zuma_pgo_<N>.stream next to the executable) and starts the 2-minute dumps.
void Start();
// Writes every count so far to this session's file (the whole session each time, replacing the previous dump).
void DumpNow();
// The last dump, then the dump thread ends.
void Stop();

}  // namespace zuma::pgo
