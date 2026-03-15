#pragma once

// ── Tracy Profiler Integration ─────────────────────────────────────────────
//
// Enable at configure time:
//   cmake -DGLORY_TRACY=ON ...
//
// When TRACY_ENABLE is not defined (default release mode), every macro
// below expands to nothing — zero overhead, no headers pulled in.
//
// GPU Vulkan zones (TracyVkZone) would require a TracyVkCtx per queue
// and additional initialization in Renderer::init.  Deferred until a
// normal G-buffer prepass is added and the Vulkan init is stable.
// ──────────────────────────────────────────────────────────────────────────

#ifdef TRACY_ENABLE
#  include <tracy/Tracy.hpp>

// ── CPU zone helpers ──────────────────────────────────────────────────────
/// Auto-named zone (uses the enclosing function/scope name).
#  define GLORY_ZONE()              ZoneScoped
/// Explicitly named zone visible in the Tracy viewer.
#  define GLORY_ZONE_N(name)        ZoneScopedN(name)
/// Marks the end of a logical frame in the Tracy timeline.
#  define GLORY_FRAME_MARK          FrameMark
/// Named frame marker — useful when multiple loops run (e.g., physics sub-step).
#  define GLORY_FRAME_MARK_N(name)  FrameMarkNamed(name)

#else  // ── no-op stubs ───────────────────────────────────────────────────

#  define GLORY_ZONE()
#  define GLORY_ZONE_N(name)
#  define GLORY_FRAME_MARK
#  define GLORY_FRAME_MARK_N(name)

#endif // TRACY_ENABLE
