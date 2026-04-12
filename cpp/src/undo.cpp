#include "undo.h"
#include <cassert>
#include <iostream>
#include <sstream>
#include <string>

namespace SoundShop {

static const std::string kEmptyString;

UndoTree::UndoTree() {
    // Root node (no command, no snapshot yet — populated by setRootSnapshot
    // once the application has set up its initial graph).
    UndoNode root;
    root.id = nextId++;
    root.parentId = -1;
    root.description = "Initial state";
    nodes.push_back(std::move(root));
    currentNodeId = 0;
}

void UndoTree::execute(std::unique_ptr<Command> cmd, std::string snapshotText) {
    cmd->execute();

    UndoNode node;
    node.id = nextId++;
    node.parentId = currentNodeId;
    node.description = cmd->description();
    node.command = std::move(cmd);
    node.snapshotText = std::move(snapshotText);

    int newId = (int)nodes.size();
    nodes.push_back(std::move(node));
    nodes[currentNodeId].childIds.push_back(newId);
    currentNodeId = newId;

    if (onTreeChanged) onTreeChanged();
}

void UndoTree::pushDone(std::unique_ptr<Command> cmd, std::string snapshotText) {
    // Same as execute but doesn't call cmd->execute() — action was already done
    UndoNode node;
    node.id = nextId++;
    node.parentId = currentNodeId;
    node.description = cmd->description();
    node.command = std::move(cmd);
    node.snapshotText = std::move(snapshotText);

    int newId = (int)nodes.size();
    nodes.push_back(std::move(node));
    nodes[currentNodeId].childIds.push_back(newId);
    currentNodeId = newId;

    if (onTreeChanged) onTreeChanged();
}

void UndoTree::pushSnapshot(std::string snapshotText, std::string desc) {
    UndoNode node;
    node.id = nextId++;
    node.parentId = currentNodeId;
    node.description = std::move(desc);
    // No command — undo/redo for this step uses snapshotText only
    node.snapshotText = std::move(snapshotText);

    int newId = (int)nodes.size();
    nodes.push_back(std::move(node));
    nodes[currentNodeId].childIds.push_back(newId);
    currentNodeId = newId;

    if (onTreeChanged) onTreeChanged();
}

void UndoTree::setRootSnapshot(std::string snapshotText) {
    if (!nodes.empty())
        nodes[0].snapshotText = std::move(snapshotText);
}

bool UndoTree::currentSnapshotIsEmpty() const {
    return nodes[currentNodeId].snapshotText.empty();
}

void UndoTree::setCurrentSnapshot(std::string snapshotText) {
    nodes[currentNodeId].snapshotText = std::move(snapshotText);
}

const std::string& UndoTree::currentSnapshot() const {
    return nodes[currentNodeId].snapshotText;
}

// ============================================================================
// Persistence
// ============================================================================
//
// Format (text):
//
//   UndoTreeV1
//   currentNodeId=<int>     ; vector index (matches in-memory navigation)
//   nextId=<int>            ; next value to assign for UndoNode::id field
//   nodeCount=<int>
//   <nodeCount times:>
//     [Node]
//     parent=<int>          ; vector index, -1 for root
//     desc=<string until newline>
//     snapLen=<int N>
//     <N raw bytes of snapshot text, then a single \n>
//
// Vector index is implicit from node order in the file. childIds are
// reconstructed from parent links on load. Commands are not serialized
// (closures can't be); cross-session undo/redo always uses the snapshot
// path via onLoadSnapshot.

void UndoTree::serializeTo(std::ostream& out) const {
    out << "UndoTreeV1\n";
    out << "currentNodeId=" << currentNodeId << "\n";
    out << "nextId=" << nextId << "\n";
    out << "nodeCount=" << nodes.size() << "\n";
    for (auto& n : nodes) {
        out << "[Node]\n";
        out << "parent=" << n.parentId << "\n";
        out << "desc=" << n.description << "\n";
        out << "snapLen=" << n.snapshotText.size() << "\n";
        if (!n.snapshotText.empty())
            out.write(n.snapshotText.data(), (std::streamsize)n.snapshotText.size());
        out << "\n";
    }
}

bool UndoTree::restoreFrom(std::istream& in) {
    std::string line;
    if (!std::getline(in, line)) return false;
    if (line != "UndoTreeV1") return false;

    int restoredCurrent = 0;
    int restoredNextId = 0;
    int restoredCount = 0;

    auto getValue = [](const std::string& s) -> std::string {
        auto pos = s.find('=');
        if (pos == std::string::npos) return "";
        return s.substr(pos + 1);
    };

    // Read header lines until first [Node]
    bool sawFirstNode = false;
    while (std::getline(in, line)) {
        if (line == "[Node]") { sawFirstNode = true; break; }
        if (line.rfind("currentNodeId=", 0) == 0) restoredCurrent = std::stoi(getValue(line));
        else if (line.rfind("nextId=", 0) == 0)   restoredNextId   = std::stoi(getValue(line));
        else if (line.rfind("nodeCount=", 0) == 0) restoredCount   = std::stoi(getValue(line));
    }

    std::vector<UndoNode> restored;
    restored.reserve(restoredCount > 0 ? (size_t)restoredCount : 16);

    // Read one [Node] block. The opening "[Node]" line has already been
    // consumed before this is called.
    auto readNode = [&]() -> bool {
        UndoNode n;
        n.id = (int)restored.size(); // for completeness; nothing actually reads it
        while (std::getline(in, line)) {
            if (line.rfind("parent=", 0) == 0) n.parentId = std::stoi(getValue(line));
            else if (line.rfind("desc=", 0) == 0) n.description = getValue(line);
            else if (line.rfind("snapLen=", 0) == 0) {
                int snapLen = std::stoi(getValue(line));
                if (snapLen > 0) {
                    n.snapshotText.resize((size_t)snapLen);
                    in.read(n.snapshotText.data(), snapLen);
                }
                // Consume the trailing newline after the blob
                std::getline(in, line);
                restored.push_back(std::move(n));
                return true;
            }
        }
        return false;
    };

    if (sawFirstNode && restoredCount > 0) {
        if (!readNode()) return false;
        while (std::getline(in, line)) {
            if (line == "[Node]") {
                if (!readNode()) return false;
            }
        }
    }

    if ((int)restored.size() != restoredCount) return false;
    if (restored.empty()) return false; // need at least the root

    // Reconstruct childIds from parent links
    for (int i = 0; i < (int)restored.size(); ++i) {
        int p = restored[i].parentId;
        if (p >= 0 && p < (int)restored.size())
            restored[p].childIds.push_back(i);
    }

    nodes = std::move(restored);
    currentNodeId = restoredCurrent;
    nextId = restoredNextId;

    // Defensive bounds check
    if (currentNodeId < 0 || currentNodeId >= (int)nodes.size())
        currentNodeId = 0;

    return true;
}

bool UndoTree::canUndo() const {
    return nodes[currentNodeId].parentId >= 0;
}

bool UndoTree::canRedo() const {
    return !nodes[currentNodeId].childIds.empty();
}

const std::string& UndoTree::parentSnapshot(int nodeId) const {
    if (nodeId < 0 || nodeId >= (int)nodes.size()) return kEmptyString;
    int parentId = nodes[nodeId].parentId;
    if (parentId < 0 || parentId >= (int)nodes.size()) return kEmptyString;
    return nodes[parentId].snapshotText;
}

void UndoTree::doUndo() {
    if (!canUndo()) return;
    auto& cur = nodes[currentNodeId];

    if (cur.command) {
        // Fast path: the command knows how to revert itself in-place.
        cur.command->undo();
        currentNodeId = cur.parentId;
    } else {
        // Snapshot-only path: load the parent's post-state, which is the
        // state that existed before this step happened. Requires the
        // application to have wired up onLoadSnapshot.
        const std::string& snap = parentSnapshot(currentNodeId);
        if (!snap.empty() && onLoadSnapshot)
            onLoadSnapshot(snap);
        currentNodeId = cur.parentId;
    }

    if (onTreeChanged) onTreeChanged();
}

void UndoTree::doRedo(int branchIndex) {
    if (!canRedo()) return;
    auto& children = nodes[currentNodeId].childIds;
    branchIndex = std::min(branchIndex, (int)children.size() - 1);
    int targetId = children[branchIndex];
    currentNodeId = targetId;

    auto& target = nodes[targetId];
    if (target.command) {
        // Fast path: re-execute the command to reach this step's post-state.
        target.command->execute();
    } else {
        // Snapshot-only path: load this step's post-state directly.
        if (!target.snapshotText.empty() && onLoadSnapshot)
            onLoadSnapshot(target.snapshotText);
    }

    if (onTreeChanged) onTreeChanged();
}

int UndoTree::redoBranchCount() const {
    return (int)nodes[currentNodeId].childIds.size();
}

std::string UndoTree::currentDescription() const {
    return nodes[currentNodeId].description;
}

std::string UndoTree::redoBranchDescription(int index) const {
    auto& children = nodes[currentNodeId].childIds;
    if (index < 0 || index >= (int)children.size()) return "";
    return nodes[children[index]].description;
}

std::string UndoTree::redoBranchChainDescription(int index, int maxSteps) const {
    auto& children = nodes[currentNodeId].childIds;
    if (index < 0 || index >= (int)children.size()) return "";

    std::string result;
    int nodeId = children[index];
    int steps = 0;

    while (nodeId >= 0 && nodeId < (int)nodes.size() && steps < maxSteps) {
        auto& nd = nodes[nodeId];
        if (!nd.description.empty()) {
            if (!result.empty()) result += " -> ";
            result += nd.description;
        }
        steps++;
        // Follow the first child (main path)
        if (!nd.childIds.empty())
            nodeId = nd.childIds[0];
        else
            break;
    }

    // Count total steps on this branch
    int total = steps;
    int walkId = nodeId;
    while (walkId >= 0 && walkId < (int)nodes.size() && !nodes[walkId].childIds.empty()) {
        walkId = nodes[walkId].childIds[0];
        total++;
    }

    if (total > maxSteps)
        result += " ... (" + std::to_string(total) + " steps total)";
    else if (steps > 1)
        result += " (" + std::to_string(steps) + " steps)";

    return result;
}

} // namespace SoundShop
