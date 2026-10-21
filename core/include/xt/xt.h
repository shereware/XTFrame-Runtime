#pragma once

#include "xt/asset_resolver.h"
#include "xt/command_list.h"
#include "xt/context.h"
#include "xt/damage.h"
#include "xt/document.h"
#include "xt/element_ref.h"
#include "xt/input.h"
#include "xt/perf.h"
#include "xt/text_shaper.h"
#include "xt/version.h"

/// <xt/xt.h>: the whole core host contract in one include. Aggregates the core rows of the stable
/// interface list in docs/design/23-host-integration.md §23.6; see docs/tasks/T-100.md.
///
/// The rules that come with this surface (§23.6):
///   - From 0.3 on these headers are the host contract: within a major version they are added to,
///     never changed.
///   - None of them includes a private header (`*_internal.h`, anything under a module's `src/`)
///     and none of them exposes a third-party type.
///   - A new member may only be appended, and only with a default value. Removing one, or changing
///     what one means, bumps the major version.
///   - tools/check_public_headers.sh enforces the first two rules, and that
///     examples/engine_integration stays on this surface.
///
/// The non-core rows of §23.6 are not aggregated here -- a host includes the ones it uses:
///   - "xt/sw/sw_renderer.h"   SwRenderer, SwTarget  (xt_sw: the built-in software renderer)
///   - "xt/bundle/bundle.h"    Bundle, loadUi, reloadUi  (xt_bundle: published UI bundles)
///   - "xt/flow/flow_runtime.h"  FlowRuntime  (xt_flow: running a flow graph)
