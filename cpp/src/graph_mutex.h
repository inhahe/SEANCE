#pragma once
#include <atomic>
#include <chrono>
#include <functional>
#include <mutex>
#include <thread>

namespace SoundShop {

// =============================================================================
// GraphMutex - the node graph's lock (NodeGraph::mutationLock)
// =============================================================================
//
// A recursive mutex (batch mutators nest addNode/addLink inside their own hold -
// see the mutationLock comment in node_graph.h), plus what an offline render
// needs to share it.
//
// An offline render - Export Audio, a script's render(), Freeze, Bounce, capture
// from playback - reads the node graph for every block it renders, just as the
// audio callback does, so it holds the lock while it renders one; and capture
// from playback does that on its own thread while the project stays open to
// edits. Held block after block, the lock would shut out the audio callback -
// which only ever try-locks it, and plays a silent block when that fails - and
// could starve the UI thread waiting to make an edit. So:
//   - every lock() says it's waiting until it has the lock, and a render takes
//     the lock for its next block only when nobody is (lockForRenderBlock);
//   - while a render is running (RenderSession), the audio callback's try-lock
//     waits a moment for the render's current block to finish instead of
//     giving up at once (tryLockForAudio).
//
// Meets the standard Lockable requirements, so std::lock_guard / unique_lock
// work with it as they did with the plain std::recursive_mutex it replaced.
class GraphMutex {
public:
    void lock() {
        if (heldByThisThread()) {   // recursion: nobody to wait for
            m.lock();
            ++depth;
            return;
        }
        waiting.fetch_add(1, std::memory_order_acq_rel);
        m.lock();
        waiting.fetch_sub(1, std::memory_order_acq_rel);
        owner.store(std::this_thread::get_id(), std::memory_order_release);
        depth = 1;
    }

    bool try_lock() {
        if (!m.try_lock())
            return false;
        if (depth++ == 0)
            owner.store(std::this_thread::get_id(), std::memory_order_release);
        return true;
    }

    void unlock() {
        if (--depth == 0)
            owner.store(std::thread::id(), std::memory_order_release);
        m.unlock();
    }

    bool heldByThisThread() const {
        return owner.load(std::memory_order_acquire) == std::this_thread::get_id();
    }

    // Held by an offline render for as long as it runs - see tryLockForAudio.
    class RenderSession {
    public:
        explicit RenderSession(GraphMutex& gm) : mutex(gm) { mutex.renders.fetch_add(1); }
        ~RenderSession() { mutex.renders.fetch_sub(1); }
        RenderSession(const RenderSession&) = delete;
        RenderSession& operator=(const RenderSession&) = delete;
    private:
        GraphMutex& mutex;
    };

    // An offline render, before each block: take the lock once nobody else is
    // waiting for it. Returns false, without the lock, if `stop` says to give
    // up while waiting (a render thread being stopped). Release it after the
    // block with unlock() - e.g. std::unique_lock<GraphMutex>(m, std::adopt_lock).
    bool lockForRenderBlock(const std::function<bool()>& stop = {}) {
        if (heldByThisThread()) {   // held around the whole render: nobody else can have it anyway
            lock();
            return true;
        }
        for (int attempt = 0;; ++attempt) {
            if (stop && stop())
                return false;
            if (waiting.load(std::memory_order_acquire) == 0 && try_lock())
                return true;
            if (attempt < 64)
                std::this_thread::yield();
            else
                std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }

    // The audio callback's try-lock. Normally a plain try_lock: the callback
    // must never block. While an offline render is running, though, the lock
    // is held for one block at a time and handed to whoever waits: so the
    // callback waits up to `patience` for it, as a waiter, rather than playing
    // silence for as long as the render lasts.
    bool tryLockForAudio(std::chrono::microseconds patience) {
        if (try_lock())
            return true;
        if (renders.load(std::memory_order_acquire) == 0)
            return false;
        waiting.fetch_add(1, std::memory_order_acq_rel);
        const auto giveUp = std::chrono::steady_clock::now() + patience;
        bool got = false;
        while (!(got = try_lock()) && std::chrono::steady_clock::now() < giveUp)
            std::this_thread::yield();
        waiting.fetch_sub(1, std::memory_order_acq_rel);
        return got;
    }

private:
    std::recursive_mutex m;
    std::atomic<std::thread::id> owner { std::thread::id() };
    int depth = 0;                   // only touched by the thread holding `m`
    std::atomic<int> waiting { 0 };  // threads in lock() / tryLockForAudio
    std::atomic<int> renders { 0 };  // RenderSessions alive
};

} // namespace SoundShop
