// Copyright (C) 2026 half144 and PS5 React contributors
// SPDX-License-Identifier: GPL-3.0-or-later
// Additional attribution term: see LICENSE-ATTRIBUTION.
#include "js_heap.hpp"

#include <algorithm>
#include <cstdio>

extern "C" {
#include "er_js_alloc.h"
}

namespace {
const JSMallocFunctions* inner = nullptr;
std::size_t live = 0;

void* counted(void* ptr) {
  if (ptr) live += inner->js_malloc_usable_size(ptr);
  return ptr;
}

void* js_calloc(void* opaque, std::size_t count, std::size_t size) { return counted(inner->js_calloc(opaque, count, size)); }
void* js_malloc(void* opaque, std::size_t size) { return counted(inner->js_malloc(opaque, size)); }

void js_free(void* opaque, void* ptr) {
  if (ptr) live -= inner->js_malloc_usable_size(ptr);
  inner->js_free(opaque, ptr);
}

void* js_realloc(void* opaque, void* ptr, std::size_t size) {
  const std::size_t before = ptr ? inner->js_malloc_usable_size(ptr) : 0;
  void* moved = inner->js_realloc(opaque, ptr, size);
  // A failed realloc leaves the block as it was; a zero size frees it.
  if (moved || size == 0) live = live - before + (moved ? inner->js_malloc_usable_size(moved) : 0);
  return moved;
}

std::size_t usable_size(const void* ptr) { return inner->js_malloc_usable_size(ptr); }

JSMallocFunctions functions = {js_calloc, js_malloc, js_free, js_realloc, usable_size};

// No idle collection right after input: the focus scroll it started is still moving.
constexpr std::int64_t kIdleUs = 500000;
// Below this much garbage a collection is not worth a frame.
constexpr std::size_t kMinGarbage = 512 * 1024;

constexpr std::size_t kCeiling = kJsMemoryLimit - kJsMemoryLimit / 8;

double mib(std::size_t bytes) { return bytes / (1024.0 * 1024.0); }
} // namespace

const JSMallocFunctions* js_heap_functions() {
  inner = er_js_default_malloc_functions();
  return &functions;
}

const char* GcScheduler::frame(JSRuntime* runtime, std::int64_t now_us) {
  const std::size_t threshold = JS_GetGCThreshold(runtime);
  if (threshold != threshold_) {
    // QuickJS collected while the frame ran and moved its threshold to 1.5 times what survived.
    const bool first = threshold_ == 0;
    threshold_ = std::min(threshold, kCeiling);
    if (threshold_ != threshold) JS_SetGCThreshold(runtime, threshold_);
    live_after_gc_ = live;
    if (first) return nullptr;
    std::snprintf(line_, sizeof line_, "gc: automatic, %.1f MiB live, next at %.1f MiB", mib(live), mib(threshold_));
    return line_;
  }
  const std::size_t room = threshold > live_after_gc_ ? threshold - live_after_gc_ : 0;
  const std::size_t garbage = live > live_after_gc_ ? live - live_after_gc_ : 0;
  if (now_us - last_input_us_ < kIdleUs || garbage < kMinGarbage || garbage < room / 3) return nullptr;
  const std::size_t before = live;
  const std::int64_t start = clock_();
  JS_RunGC(runtime);
  live_after_gc_ = live;
  // The threshold QuickJS sets after its own collections, so the next one comes no later than it would have.
  threshold_ = std::min(live + live / 2, kCeiling);
  JS_SetGCThreshold(runtime, threshold_);
  std::snprintf(line_, sizeof line_, "gc: idle, %.1f ms, %.1f -> %.1f MiB live", (clock_() - start) / 1000.0,
                mib(before), mib(live));
  return line_;
}
