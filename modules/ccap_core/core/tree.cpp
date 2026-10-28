// CommandCapture - capture every command a script or executable spawns.
//
// Copyright (c) 2026 maijun
//
// SPDX-License-Identifier: MIT
//
// Released under the MIT License. See the LICENSE file at the repository
// root for the full text.

#include "core/tree.h"

#include <algorithm>
#include <cstdio>
#include <string>
#include <unordered_map>
#include <vector>

namespace ccap {

namespace {

// A tree node.
//
// The node does **not** own its children as values. It holds their indices
// into the node pool instead.
//
// Why: an earlier version moved the child node out of the pool into the
// parent's `std::vector<Node>`. That looked fine, but it left two problems:
//
//   1. The pool still owned the moved-from node, and the pid -> node index
//      still pointed at it. So a later event whose parent was that pid
//      resolved to a **moved-from** node, and got attached to a node that
//      is never rendered.
//
//      The symptom was silent: the raw event list had 4 events, but the
//      tree showed only 3 -- one event simply vanished from the tree.
//
//   2. The pid -> node index and the tree structure were two separate
//      notions of "the same process", and they could disagree.
//
// With indices, a node never moves, its address is stable, and the pid
// index can never point at a dead node.
struct Node {
    const CommandEvent* ev = nullptr;
    std::vector<size_t> children;   // indices into the pool
};

// The display text of a node: executable + arguments
std::string nodeLabel(const CommandEvent& e) {
    std::string s = e.executable;
    for (size_t i = 1; i < e.argv.size(); ++i) {
        s += ' ';
        s += e.argv[i];
    }
    if (s.empty()) s = "(unknown)";
    return s;
}

// Render recursively
void renderNode(const std::vector<Node>& pool, size_t index, int indent,
                std::string* out) {
    const Node& n = pool[index];
    const CommandEvent& e = *n.ev;
    out->append(static_cast<size_t>(indent) * 2, ' ');
    out->append("+- ");
    out->append(nodeLabel(e));

    char buf[64];
    std::snprintf(buf, sizeof(buf), "   [pid=%u", e.pid);
    out->append(buf);
    if (e.ppid) {
        std::snprintf(buf, sizeof(buf), " ppid=%u", e.ppid);
        out->append(buf);
    }
    std::snprintf(buf, sizeof(buf), " d=%d", e.depth);
    out->append(buf);
    out->append("]\n");

    for (size_t child : n.children) {
        renderNode(pool, child, indent + 1, out);
    }
}

}  // namespace

void renderTree(const std::vector<CommandEvent>& events, std::string* out) {
    if (!out) return;

    // Separate the runtime events from the static ones
    std::vector<const CommandEvent*> runtime;
    std::vector<const CommandEvent*> statics;
    for (const auto& e : events) {
        if (e.source == EventSource::Static) {
            statics.push_back(&e);
        } else {
            runtime.push_back(&e);
        }
    }

    if (runtime.empty()) {
        out->append("(no runtime events)\n");
    } else {
        // ─────────────────────────────────────────────────────────────
        // Build the tree
        //
        // The node pool is a plain vector and the children are indices into
        // it, so no node ever moves and no pointer can dangle.
        //
        // Why the same pid can have several nodes:
        //   On Linux, exec does not change the pid --- a process execs itself
        //   into another program, so the pid stays the same while the
        //   program changes. In the tree this should appear as a **sibling**
        //   relationship (two process replacements under the same parent),
        //   not as parent/child.
        //
        // Which node is the "parent" of a given pid? The first one seen for
        // that pid. That is the process that was running when the children
        // were created.
        // ─────────────────────────────────────────────────────────────
        std::vector<Node> pool;
        pool.reserve(runtime.size());
        for (const CommandEvent* e : runtime) {
            Node n;
            n.ev = e;
            pool.push_back(std::move(n));
        }

        // pid -> the index of the primary node of that pid (the first one
        // to appear).
        std::unordered_map<uint32_t, size_t> primaryOfPid;
        for (size_t i = 0; i < runtime.size(); ++i) {
            uint32_t pid = runtime[i]->pid;
            if (primaryOfPid.find(pid) == primaryOfPid.end()) {
                primaryOfPid[pid] = i;
            }
        }

        // Collect the nodes to attach under a parent, plus the root nodes
        std::vector<std::pair<size_t, size_t>> attachments;  // (parent, child)
        std::vector<size_t> roots;

        for (size_t i = 0; i < runtime.size(); ++i) {
            const CommandEvent* e = runtime[i];

            // Find the parent node: by ppid first
            bool hasParent = false;
            size_t parent = 0;
            if (e->ppid) {
                auto pit = primaryOfPid.find(e->ppid);
                if (pit != primaryOfPid.end()) {
                    parent = pit->second;
                    hasParent = true;
                }
            }

            if (!hasParent) {
                roots.push_back(i);
            } else {
                attachments.emplace_back(parent, i);
            }
        }

        // Attach. The children are appended in event order, so the tree
        // reads in the order the commands actually ran.
        for (const auto& a : attachments) {
            pool[a.first].children.push_back(a.second);
        }

        // ── Render ─────────────────────────────────────────────────────
        for (size_t r : roots) {
            renderNode(pool, r, 0, out);
        }
    }

    // ── Static events are listed separately ────────────────────────────
    if (!statics.empty()) {
        out->append("\n");
        out->append("static (script builtins, no process created):\n");
        for (const CommandEvent* e : statics) {
            out->append("  +- ");
            out->append(nodeLabel(*e));
            if (!e->scriptPath.empty()) {
                out->append("   [");
                out->append(e->scriptPath);
                out->append(":");
                out->append(std::to_string(e->scriptLine));
                out->append("]");
            }
            out->append("\n");
        }
    }
}

}  // namespace ccap
