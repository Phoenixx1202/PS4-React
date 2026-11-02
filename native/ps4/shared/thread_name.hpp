// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#pragma once
#if defined(__APPLE__)
#include <pthread.h>
#endif

// The OpenOrbis libc has no thread-naming call; keep the hook so call sites
// stay portable to hosts that do.
inline void name_thread(const char* name) {
#if defined(__APPLE__)
  pthread_setname_np(name);
#else
  (void)name;
#endif
}
