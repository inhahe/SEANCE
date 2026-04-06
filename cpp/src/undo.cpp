#include "undo.h"
#include <cassert>

namespace SoundShop {

UndoTree::UndoTree() {
    // Root node (no command)
    UndoNode root;
    root.id = nextId++;
    root.parentId = -1;
    nodes.push_back(std::move(root));
    currentNodeId = 0;
}

void UndoTree::execute(std::unique_ptr<Command> cmd) {
    cmd->execute();

    UndoNode node;
    node.id = nextId++;
    node.parentId = currentNodeId;
    node.command = std::move(cmd);

    int newId = (int)nodes.size();
    nodes.push_back(std::move(node));
    nodes[currentNodeId].childIds.push_back(newId);
    currentNodeId = newId;
}

void UndoTree::pushDone(std::unique_ptr<Command> cmd) {
    // Same as execute but doesn't call cmd->execute() — action was already done
    UndoNode node;
    node.id = nextId++;
    node.parentId = currentNodeId;
    node.command = std::move(cmd);

    int newId = (int)nodes.size();
    nodes.push_back(std::move(node));
    nodes[currentNodeId].childIds.push_back(newId);
    currentNodeId = newId;
}

bool UndoTree::canUndo() const {
    return nodes[currentNodeId].parentId >= 0;
}

bool UndoTree::canRedo() const {
    return !nodes[currentNodeId].childIds.empty();
}

void UndoTree::doUndo() {
    if (!canUndo()) return;
    nodes[currentNodeId].command->undo();
    currentNodeId = nodes[currentNodeId].parentId;
}

void UndoTree::doRedo(int branchIndex) {
    if (!canRedo()) return;
    auto& children = nodes[currentNodeId].childIds;
    branchIndex = std::min(branchIndex, (int)children.size() - 1);
    currentNodeId = children[branchIndex];
    nodes[currentNodeId].command->execute();
}

int UndoTree::redoBranchCount() const {
    return (int)nodes[currentNodeId].childIds.size();
}

std::string UndoTree::currentDescription() const {
    if (nodes[currentNodeId].command)
        return nodes[currentNodeId].command->description();
    return "Initial state";
}

std::string UndoTree::redoBranchDescription(int index) const {
    auto& children = nodes[currentNodeId].childIds;
    if (index < 0 || index >= (int)children.size()) return "";
    if (nodes[children[index]].command)
        return nodes[children[index]].command->description();
    return "";
}

std::string UndoTree::redoBranchChainDescription(int index, int maxSteps) const {
    auto& children = nodes[currentNodeId].childIds;
    if (index < 0 || index >= (int)children.size()) return "";

    std::string result;
    int nodeId = children[index];
    int steps = 0;

    while (nodeId >= 0 && nodeId < (int)nodes.size() && steps < maxSteps) {
        auto& nd = nodes[nodeId];
        if (nd.command) {
            if (!result.empty()) result += " -> ";
            result += nd.command->description();
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
