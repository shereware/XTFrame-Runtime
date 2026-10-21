#pragma once

#include "xt/document.h"

/// ElementRef (docs/design/13-api.md §13.3) is defined in xt/document.h, next to the Document it
/// borrows from. This header exists so a host can include the name listed in
/// docs/design/23-host-integration.md §23.6. See docs/tasks/T-100.md.
