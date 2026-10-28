// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

// CommandCapture - process tree view
//
// The event stream is **flat**: every event carries depth, pid and ppid.
// But when a human looks at "what does this tree look like", they want an
// indented view.
//
// Why this is a separate file:
//   The tree view is a **derived view**, not new data ---
//   it has to use the same deduplication rules as the flat events, otherwise
//   the event counts of the two views do not match.
//   Keeping the rendering logic separate guarantees that it reads the same
//   events array.
#pragma once

#include <string>
#include <vector>

#include "core/event.h"

namespace ccap {

// Render a process tree (text format, indented).
//
// Tree construction rules:
//   - the root nodes are the runtime events whose parent was not captured
//   - child nodes are attached by the (pid, ppid) relationship
//   - when a pid has several events (Linux: exec does not change the pid),
//     the **first** one is the parent of that process's children, and the
//     later ones become **siblings** under the same parent --- they are
//     process replacements, not nested processes
//   - static events (source == static) have no process relationship and are
//     listed separately at the end
//
// Every runtime event appears in the tree exactly once. That is not
// automatic: an earlier version moved each child node out of the node pool
// into its parent's children vector, and the pid -> node index still
// pointed at the moved-from node. A later event resolving its parent
// through that index was attached to a dead node and silently vanished
// from the tree. The node pool now uses indices, so no node ever moves.
//
// Each node is rendered as:
//   <indent><executable> <args...>   [pid=NNN ppid=NNN]
//
// Parameters:
//   events  the already deduplicated and sorted events
//   out    the output text (appended to)
void renderTree(const std::vector<CommandEvent>& events, std::string* out);

}  // namespace ccap
