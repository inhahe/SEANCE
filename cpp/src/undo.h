#pragma once
#include <vector>
#include <string>
#include <memory>
#include <functional>
#include <iosfwd>

namespace SoundShop {

// A command knows how to do and undo itself
class Command {
public:
    virtual ~Command() = default;
    virtual void execute() = 0;   // do / redo
    virtual void undo() = 0;
    virtual std::string description() const = 0;
};

// Undo tree node — stores a command and links to parent/children.
//
// snapshotText holds the full graph state at the moment this step
// completed (post-state). It's populated regardless of whether the step
// came from a LambdaCommand or commitSnapshot(). Two reasons it's always
// captured:
//   1. Cross-session undo: closures vanish when the app restarts, so the
//      snapshot is the only revert mechanism that survives a restart. The
//      persistence layer serializes nodes by their snapshotText.
//   2. Snapshot-only commands: commitSnapshot() creates an UndoNode with
//      no Command at all — undo/redo navigate by loading the parent's or
//      target's snapshot.
//
// In-session, if a node HAS a Command, doUndo/doRedo prefer that fast path
// because it's much cheaper than parsing the snapshot and rebuilding the
// audio graph. The snapshot is just along for the ride until persistence
// or a session-boundary undo needs it.
struct UndoNode {
    int id;
    int parentId = -1;
    std::vector<int> childIds;
    std::unique_ptr<Command> command; // null for snapshot-only steps and root
    std::string snapshotText;         // post-state of the graph after this step
    std::string description;          // mirrors command->description() when present;
                                       // populated directly for snapshot-only steps
};

class UndoTree {
public:
    UndoTree();

    // Execute a command and push it onto the tree. snapshotText is the
    // state AFTER the command runs — pass an empty string if the caller
    // doesn't have a snapshot yet (cross-session undo will then be limited
    // for that step, but in-session undo via the command still works).
    void execute(std::unique_ptr<Command> cmd, std::string snapshotText = {});

    // Push a command without executing it (action was already performed).
    void pushDone(std::unique_ptr<Command> cmd, std::string snapshotText = {});

    // Push a snapshot-only step. desc is the human-readable description.
    // Used by NodeGraph::commitSnapshot().
    void pushSnapshot(std::string snapshotText, std::string desc);

    // Capture the initial graph state on the root node. Called once at app
    // startup after the graph is set up but before any user edits, so the
    // first user edit has something to undo back to.
    void setRootSnapshot(std::string snapshotText);

    // True iff the currently-selected undo node has no snapshot text.
    // Used by the app's onTreeChanged callback to decide whether to lazily
    // serialize and fill in the snapshot — exec()/pushDone() typically push
    // a Command without a snapshot, and the app fills it in afterward via
    // setCurrentSnapshot.
    bool currentSnapshotIsEmpty() const;
    void setCurrentSnapshot(std::string snapshotText);
    const std::string& currentSnapshot() const;

    // Persistence — write/read the entire tree to/from a stream. The
    // format is text-based with explicit length-prefixed blobs for the
    // snapshot text (which itself contains newlines). Used by the
    // application's undo-tree persistence layer (#84) to save the tree
    // alongside autosave files in user app-data, so Ctrl+Z continues to
    // work across app restarts. Commands themselves are NOT serialized
    // (closures can't be); cross-session undo/redo therefore always uses
    // the snapshot path via onLoadSnapshot.
    void serializeTo(std::ostream& out) const;
    bool restoreFrom(std::istream& in);

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

    // Snapshot-revert callback. When doUndo/doRedo lands on a step whose
    // Command is null (or, in a future revision, when the app has opted
    // into "always use snapshots"), the tree calls this with the snapshot
    // text that should become the live graph state. The callback is
    // responsible for parsing the snapshot via ProjectFile::loadFromString
    // and triggering whatever rebuilds the audio graph and repaints the UI.
    //
    // Set once at app startup. If unset, snapshot-only steps fall back to
    // doing nothing on undo/redo (which is wrong but at least doesn't
    // crash); the wiring should always be present in the running app.
    std::function<void(const std::string&)> onLoadSnapshot;

    // Called whenever the tree changes (push, undo, redo). The application
    // sets this to "mark the persisted undo file as dirty"; the persist
    // worker writes to disk on the next opportunity. Optional.
    std::function<void()> onTreeChanged;

private:
    std::vector<UndoNode> nodes;
    int currentNodeId = 0;
    int nextId = 0;

    // Find the snapshot to load when undoing FROM `nodeId` (i.e., the
    // parent's snapshot). Returns empty string if there's no parent or
    // the parent has no snapshot.
    const std::string& parentSnapshot(int nodeId) const;
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
