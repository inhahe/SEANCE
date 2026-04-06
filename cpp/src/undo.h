#pragma once
#include <vector>
#include <string>
#include <memory>
#include <functional>

namespace SoundShop {

// A command knows how to do and undo itself
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;   // do / redo
    virtual void undo() = 0;
    virtual std::string description() const = 0;
};

// Undo tree node — stores a command and links to parent/children
struct UndoNode {
    int id;
    int parentId = -1;
    std::vector<int> childIds;
    std::unique_ptr<Command> command; // null for root
};

class UndoTree {
public:
    UndoTree();

    // Execute a command and push it onto the tree
    void execute(std::unique_ptr<Command> cmd);

    // Push a command without executing it (action was already performed)
    void pushDone(std::unique_ptr<Command> cmd);

    bool canUndo() const;
    bool canRedo() const;
    void doUndo();
    void doRedo(int branchIndex = 0);

    int redoBranchCount() const;
    std::string currentDescription() const;
    std::string redoBranchDescription(int index) const;
    // Walk forward along a branch and return a chain summary, e.g. "+1 octave → Move notes → x2 duration (3 steps)"
    std::string redoBranchChainDescription(int index, int maxSteps = 5) const;
    int nodeCount() const { return (int)nodes.size(); }

private:
    std::vector<UndoNode> nodes;
    int currentNodeId = 0;
    int nextId = 0;
};

// ============================================================================
// Concrete command types
// ============================================================================

// Generic lambda-based command for simple operations
class LambdaCommand : public Command {
public:
    LambdaCommand(std::string desc, std::function<void()> doFn, std::function<void()> undoFn)
        : desc_(std::move(desc)), doFn_(std::move(doFn)), undoFn_(std::move(undoFn)) {}
    void execute() override { doFn_(); }
    void undo() override { undoFn_(); }
    std::string description() const override { return desc_; }
private:
    std::string desc_;
    std::function<void()> doFn_, undoFn_;
};

} // namespace SoundShop
