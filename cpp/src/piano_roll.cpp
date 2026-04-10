#include "node_graph.h"
#include "music_theory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>

namespace SoundShop {

struct NoteHit {
    int ci = -1, ni = -1;
    enum Edge { Body, Left, Right } edge = Body;
    bool valid() const { return ci >= 0; }
};

static NoteHit findNoteAt(Node& node, float beat, int pitch,
                           float gridX, float gridW, float totalBeats,
                           float mouseX) {
    auto beatToX = [&](float b) { return gridX + (b / totalBeats) * gridW; };
    for (int ci = 0; ci < (int)node.clips.size(); ++ci) {
        auto& clip = node.clips[ci];
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& n = clip.notes[ni];
            float absBeat = clip.startBeat + n.offset;
            if (absBeat <= beat && beat <= absBeat + n.duration && n.pitch == pitch) {
                float nx1 = beatToX(absBeat);
                float nx2 = beatToX(absBeat + n.duration);
                NoteHit::Edge edge = NoteHit::Body;
                if (mouseX < nx1 + 7) edge = NoteHit::Left;
                else if (mouseX > nx2 - 7) edge = NoteHit::Right;
                return {ci, ni, edge};
            }
        }
    }
    return {};
}

// Capture/restore all notes in a node's clips for undo
struct NoteSnapshot {
    std::vector<std::vector<MidiNote>> clipNotes; // notes per clip
    static NoteSnapshot capture(Node& node) {
        NoteSnapshot s;
        for (auto& c : node.clips) s.clipNotes.push_back(c.notes);
        return s;
    }
    void restore(Node& node) const {
        for (int i = 0; i < (int)clipNotes.size() && i < (int)node.clips.size(); ++i)
            node.clips[i].notes = clipNotes[i];
    }
};

void NodeGraph::drawPianoRoll(Node& node, float areaHeight) {
    auto& state = pianoRollStates[node.id];

    // Helper: wrap a note-editing operation with undo
    auto execNoteEdit = [&](const std::string& desc, auto fn) {
        auto before = NoteSnapshot::capture(node);
        fn();
        auto after = NoteSnapshot::capture(node);
        Node* np = &node;
        exec(desc,
            [np, after]() { after.restore(*np); },
            [np, before]() { before.restore(*np); });
    };

    // Track drag start for undo on drag end
    static NoteSnapshot dragStartSnapshot;
    static bool dragActive = false;

    // Get active scale intervals for degree analysis
    auto getActiveIntervals = [&]() -> std::vector<int> {
        const ScaleMap* table = nullptr;
        if (state.activeCategory == "key") table = &MusicTheory::keys();
        else if (state.activeCategory == "mode") table = &MusicTheory::modes();
        else if (state.activeCategory == "scale") table = &MusicTheory::scales();
        if (table) {
            auto* v = findScale(*table, state.activeName());
            if (v) return *v;
        }
        return {0,2,4,5,7,9,11};
    };
    auto assignDegree = [&](MidiNote& nn) {
        auto intervals = getActiveIntervals();
        auto info = MusicTheory::pitchToDegree(nn.pitch, state.keyRoot, intervals);
        nn.degree = info.degree;
        nn.octave = info.octave;
        nn.chromaticOffset = info.chromaticOffset;
    };

    auto cursor = ImGui::GetCursorScreenPos();
    auto* dl = ImGui::GetWindowDrawList();
    float availW = ImGui::GetContentRegionAvail().x;

    if (areaHeight < 30) { ImGui::Dummy(ImVec2(availW, areaHeight)); return; }

    // Layout
    float keyW = 40;
    float gridX = cursor.x + keyW;
    float gridW = availW - keyW;
    float gridY = cursor.y;
    float gridH = areaHeight;
    float totalBeats = getTimelineBeats(node);
    int scrollPitch = state.scrollPitch;
    int visRange = state.visibleRange;
    int pitchLo = scrollPitch - visRange / 2;
    int pitchHi = scrollPitch + visRange / 2;
    float rowH = gridH / std::max(visRange, 1);

    auto beatToX = [&](float b) { return gridX + (b / totalBeats) * gridW; };
    auto pitchToY = [&](int p) { return gridY + (pitchHi - p) * rowH; };

    // Background
    dl->AddRectFilled(cursor, ImVec2(cursor.x + availW, gridY + gridH),
                      IM_COL32(20, 20, 30, 255), 3.0f);

    // Build set of scale pitches (mod 12) for highlighting
    auto activeIntervals = getActiveIntervals();
    std::set<int> scaleNotes;
    for (int s : activeIntervals)
        scaleNotes.insert((s + state.keyRoot) % 12);
    bool isChromatic = (activeIntervals.size() >= 12);

    // Piano keys + horizontal pitch lines
    for (int i = 0; i <= visRange; ++i) {
        int pitch = pitchHi - i;
        float y = gridY + i * rowH;
        if (pitch < 0 || pitch > 127) continue;

        bool isBlack = MusicTheory::isBlackKey(pitch);
        bool inScale = isChromatic || scaleNotes.count(pitch % 12) > 0;
        if (i < visRange) {
            ImU32 rowCol;
            if (inScale)
                rowCol = isBlack ? IM_COL32(30, 30, 50, 255) : IM_COL32(38, 38, 55, 255);
            else
                rowCol = IM_COL32(18, 18, 22, 255); // darker for out-of-scale
            dl->AddRectFilled(ImVec2(gridX, y), ImVec2(gridX + gridW, y + rowH), rowCol);
        }
        int alpha = (pitch % 12 == 0) ? 60 : (inScale ? 25 : 10);
        dl->AddLine(ImVec2(gridX, y), ImVec2(gridX + gridW, y), IM_COL32(255, 255, 255, alpha));

        // Key label — pick font size based on row height, clip to row
        if (i < visRange) {
            // Check if this key is being auditioned
            bool isAuditioned = false;
            double now = ImGui::GetTime();
            for (auto& an : state.auditionNotes) {
                if (an.pitch == pitch && now - an.startTime < an.duration) {
                    isAuditioned = true;
                    break;
                }
            }

            ImU32 keyCol;
            if (isAuditioned)
                keyCol = isBlack ? IM_COL32(80, 100, 160, 255) : IM_COL32(100, 130, 200, 255);
            else if (!inScale && !isChromatic)
                keyCol = IM_COL32(25, 25, 28, 255); // very dim for out-of-scale
            else
                keyCol = isBlack ? IM_COL32(40, 40, 50, 255) : IM_COL32(60, 60, 70, 255);
            dl->AddRectFilled(ImVec2(cursor.x, y), ImVec2(cursor.x + keyW - 2, y + rowH), keyCol);
            if (rowH >= 8) {
                auto name = MusicTheory::noteName(pitch);
                ImU32 textCol;
                if (!inScale && !isChromatic)
                    textCol = IM_COL32(60, 60, 60, 255);
                else if (pitch % 12 == 0)
                    textCol = IM_COL32(180, 180, 180, 255);
                else
                    textCol = IM_COL32(120, 120, 120, 255);

                // Pick best font: target ~80% of row height, clamped to available sizes
                auto& fonts = ImGui::GetIO().Fonts->Fonts;
                int targetPx = std::max(8, (int)(rowH * 0.8f));
                // Fonts: index 0=13px, index 1..17 = 8..24px
                int fontIdx = std::clamp(targetPx - 8 + 1, 0, fonts.Size - 1);
                ImFont* font = fonts[fontIdx];

                dl->PushClipRect(ImVec2(cursor.x, y), ImVec2(cursor.x + keyW - 2, y + rowH));
                float textY = y + (rowH - font->FontSize) * 0.5f;
                dl->AddText(font, font->FontSize, ImVec2(cursor.x + 2, textY), textCol, name.c_str());
                dl->PopClipRect();
            }
        }
    }

    // Vertical beat grid
    for (int beat = 0; beat <= (int)totalBeats; ++beat) {
        float x = beatToX((float)beat);
        bool isBar = (beat % 4 == 0);
        dl->AddLine(ImVec2(x, gridY), ImVec2(x, gridY + gridH),
                    IM_COL32(255, 255, 255, isBar ? 100 : 25));
        if (isBar)
            dl->AddText(ImVec2(x + 2, gridY + 1), IM_COL32(200, 200, 200, 120),
                        std::to_string(beat / 4 + 1).c_str());
    }

    // Clip boundaries
    for (auto& clip : node.clips) {
        float cx1 = beatToX(clip.startBeat);
        float cx2 = beatToX(clip.startBeat + clip.lengthBeats);
        int cr = (clip.color >> 0) & 0xFF, cg = (clip.color >> 8) & 0xFF, cb = (clip.color >> 16) & 0xFF;
        dl->AddRectFilled(ImVec2(cx1, gridY), ImVec2(cx2, gridY + gridH), IM_COL32(cr, cg, cb, 15));
        dl->AddLine(ImVec2(cx1, gridY), ImVec2(cx1, gridY + gridH), IM_COL32(cr, cg, cb, 80));
    }

    // Draw notes
    auto& selected = state.selected;
    for (int ci = 0; ci < (int)node.clips.size(); ++ci) {
        auto& clip = node.clips[ci];
        int cr = (clip.color >> 0) & 0xFF, cg = (clip.color >> 8) & 0xFF, cb = (clip.color >> 16) & 0xFF;
        for (int ni = 0; ni < (int)clip.notes.size(); ++ni) {
            auto& note = clip.notes[ni];
            float absBeat = clip.startBeat + note.offset;
            float nx1 = beatToX(absBeat);
            float nx2 = beatToX(absBeat + note.duration);
            float ny = pitchToY(note.pitch);
            float detuneOff = -(note.detune / 100.0f) * rowH;
            ny += detuneOff;

            if (ny + rowH < gridY || ny > gridY + gridH) continue;

            bool isSel = selected.count({ci, ni}) > 0;
            bool isChromatic = note.chromaticOffset != 0;

            if (isSel) {
                dl->AddRectFilled(ImVec2(nx1+1, ny+1), ImVec2(nx2-1, ny+rowH-1),
                                  IM_COL32(255, 255, 255, 240), 2.0f);
                dl->AddRect(ImVec2(nx1, ny), ImVec2(nx2, ny+rowH),
                            IM_COL32(255, 220, 100, 255), 2.0f, 0, 2.0f);
            } else if (isChromatic) {
                dl->AddRectFilled(ImVec2(nx1+1, ny+1), ImVec2(nx2-1, ny+rowH-1),
                                  IM_COL32(200, 130, 60, 200), 2.0f);
                dl->AddRect(ImVec2(nx1+1, ny+1), ImVec2(nx2-1, ny+rowH-1),
                            IM_COL32(240, 170, 80, 255), 2.0f);
            } else {
                dl->AddRectFilled(ImVec2(nx1+1, ny+1), ImVec2(nx2-1, ny+rowH-1),
                                  IM_COL32(cr, cg, cb, 220), 2.0f);
                dl->AddRect(ImVec2(nx1+1, ny+1), ImVec2(nx2-1, ny+rowH-1),
                            IM_COL32(std::min(cr+40,255), std::min(cg+40,255), std::min(cb+40,255), 255), 2.0f);
            }

            // Note label with degree — sized to fit
            float noteW = nx2 - nx1;
            if (noteW > 14 && rowH > 7) {
                auto name = MusicTheory::noteName(note.pitch);
                std::string label;
                if (noteW > 80 && note.degree >= 0 && note.degree < 7)
                    label = name + " " + MusicTheory::DEGREE_NAMES[note.degree];
                else if (noteW > 40)
                    label = name;
                else
                    label = MusicTheory::NOTE_NAMES[note.pitch % 12];

                auto& fonts = ImGui::GetIO().Fonts->Fonts;
                int targetPx = std::max(8, (int)(rowH * 0.8f));
                int fontIdx = std::clamp(targetPx - 8 + 1, 0, fonts.Size - 1);
                ImFont* font = fonts[fontIdx];

                ImU32 textCol = isSel ? IM_COL32(0,0,0,220) : IM_COL32(255,255,255,200);
                dl->PushClipRect(ImVec2(nx1+2, ny), ImVec2(nx2-2, ny+rowH));
                float textY = ny + (rowH - font->FontSize) * 0.5f;
                dl->AddText(font, font->FontSize, ImVec2(nx1+3, textY), textCol, label.c_str());
                dl->PopClipRect();
            }

            // Resize handles
            dl->AddRectFilled(ImVec2(nx2-5, ny+1), ImVec2(nx2-1, ny+rowH-1),
                              IM_COL32(255, 255, 255, isSel ? 60 : 40), 1.0f);
            dl->AddRectFilled(ImVec2(nx1+1, ny+1), ImVec2(nx1+5, ny+rowH-1),
                              IM_COL32(255, 255, 255, isSel ? 60 : 40), 1.0f);
        }
    }

    // Box selection rectangle
    if (state.box.active) {
        float bx1 = beatToX(std::min(state.box.startBeat, state.box.endBeat));
        float bx2 = beatToX(std::max(state.box.startBeat, state.box.endBeat));
        float by1 = pitchToY(std::max((int)state.box.startPitch, (int)state.box.endPitch));
        float by2 = pitchToY(std::min((int)state.box.startPitch, (int)state.box.endPitch)) + rowH;
        dl->AddRectFilled(ImVec2(bx1, by1), ImVec2(bx2, by2), IM_COL32(255, 220, 100, 30));
        dl->AddRect(ImVec2(bx1, by1), ImVec2(bx2, by2), IM_COL32(255, 220, 100, 150));
    }

    // Interaction
    ImGui::SetCursorScreenPos(ImVec2(gridX, gridY));
    ImGui::InvisibleButton(("##piano_grid_" + std::to_string(node.id)).c_str(), ImVec2(gridW, gridH));
    bool isHovered = ImGui::IsItemHovered();

    auto& io = ImGui::GetIO();
    bool altHeld = io.KeyAlt;
    float snap = state.snap > 0 ? state.snap : 0.0625f;

    auto snapBeat = [&](float b) -> float {
        return altHeld ? b : std::round(b / snap) * snap;
    };

    if (isHovered) {
        auto mouse = ImGui::GetMousePos();
        float wheel = io.MouseWheel;
        if (wheel != 0)
            state.scrollPitch = std::clamp((int)(state.scrollPitch + wheel * 3), 12, 115);

        float relX = mouse.x - gridX;
        float relY = mouse.y - gridY;
        float hoverBeat = (relX / gridW) * totalBeats;
        int hoverPitch = pitchHi - (int)std::floor(relY / rowH);

        // Cursor shape
        auto probe = findNoteAt(node, hoverBeat, hoverPitch, gridX, gridW, totalBeats, mouse.x);
        if (probe.valid() && (probe.edge == NoteHit::Left || probe.edge == NoteHit::Right))
            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

        // Left click
        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            auto hit = findNoteAt(node, hoverBeat, hoverPitch, gridX, gridW, totalBeats, mouse.x);

            if (hit.valid() && io.KeyShift) {
                auto k = std::make_pair(hit.ci, hit.ni);
                if (selected.count(k)) selected.erase(k); else selected.insert(k);
            } else if (hit.valid()) {
                PianoRollState::DragInfo::Mode mode;
                if (hit.edge == NoteHit::Left) mode = PianoRollState::DragInfo::ResizeLeft;
                else if (hit.edge == NoteHit::Right) mode = PianoRollState::DragInfo::ResizeRight;
                else mode = PianoRollState::DragInfo::Move;

                if (!selected.count({hit.ci, hit.ni}))
                    selected.clear(); selected.insert({hit.ci, hit.ni});

                state.drag = {hit.ci, hit.ni, mode, hoverBeat, hoverPitch};
                dragStartSnapshot = NoteSnapshot::capture(node);
                dragActive = true;
            } else {
                // Empty space: start box select or place note
                if (!io.KeyShift) selected.clear();
                state.box = {true, hoverBeat, (float)hoverPitch, hoverBeat, (float)hoverPitch};
                state.emptyClickBeat = hoverBeat;
                state.emptyClickPitch = hoverPitch;
            }
        }

        // Box select drag
        if (state.box.active && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            state.box.endBeat = hoverBeat;
            state.box.endPitch = (float)hoverPitch;
        }

        // Finish box select or place note
        if (state.box.active && ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            float dragDist = std::abs(state.box.endBeat - state.box.startBeat)
                           + std::abs(state.box.endPitch - state.box.startPitch);
            if (dragDist < 0.3f) {
                // Place a note
                execNoteEdit("Place note", [&]() {
                    float sb = snapBeat(state.emptyClickBeat);
                    int sp = state.emptyClickPitch;
                    for (auto& clip : node.clips) {
                        if (clip.startBeat <= sb && sb < clip.startBeat + clip.lengthBeats) {
                            float local = sb - clip.startBeat;
                            MidiNote nn;
                            nn.offset = local;
                            nn.pitch = sp;
                            nn.duration = snap * 4;
                            assignDegree(nn);
                            clip.notes.push_back(nn);
                            break;
                        }
                    }
                });
            } else {
                // Select notes in box
                float b1 = std::min(state.box.startBeat, state.box.endBeat);
                float b2 = std::max(state.box.startBeat, state.box.endBeat);
                int p1 = std::min((int)state.box.startPitch, (int)state.box.endPitch);
                int p2 = std::max((int)state.box.startPitch, (int)state.box.endPitch);
                for (int ci = 0; ci < (int)node.clips.size(); ++ci) {
                    for (int ni = 0; ni < (int)node.clips[ci].notes.size(); ++ni) {
                        auto& n = node.clips[ci].notes[ni];
                        float ab = node.clips[ci].startBeat + n.offset;
                        if (ab + n.duration >= b1 && ab <= b2 && p1 <= n.pitch && n.pitch <= p2)
                            selected.insert({ci, ni});
                    }
                }
            }
            state.box.active = false;
        }

        // Dragging notes
        if (state.drag.mode != PianoRollState::DragInfo::None
            && ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
            int ci = state.drag.clipIdx, ni = state.drag.noteIdx;
            if (ci >= 0 && ci < (int)node.clips.size() && ni >= 0 && ni < (int)node.clips[ci].notes.size()) {
                if (state.drag.mode == PianoRollState::DragInfo::Move) {
                    // Snap the hover position, then compute delta from snapped start
                    float snappedHover = altHeld ? hoverBeat : snapBeat(hoverBeat);
                    float snappedStart = altHeld ? state.drag.startBeat : snapBeat(state.drag.startBeat);
                    float db = snappedHover - snappedStart;
                    int dp = hoverPitch - state.drag.startPitch;
                    if (db != 0 || dp != 0) {
                        for (auto& [sci, sni] : selected) {
                            if (sci < (int)node.clips.size() && sni < (int)node.clips[sci].notes.size()) {
                                auto& n = node.clips[sci].notes[sni];
                                n.offset = std::max(0.0f, n.offset + db);
                                n.pitch = std::clamp(n.pitch + dp, 0, 127);
                            }
                        }
                        state.drag.startBeat = hoverBeat;
                        state.drag.startPitch = hoverPitch;
                    }
                } else if (state.drag.mode == PianoRollState::DragInfo::ResizeRight) {
                    auto& n = node.clips[ci].notes[ni];
                    float absStart = node.clips[ci].startBeat + n.offset;
                    float newEnd = altHeld ? hoverBeat : snapBeat(hoverBeat);
                    n.duration = std::max(0.03125f, newEnd - absStart);
                } else if (state.drag.mode == PianoRollState::DragInfo::ResizeLeft) {
                    auto& n = node.clips[ci].notes[ni];
                    float absEnd = node.clips[ci].startBeat + n.offset + n.duration;
                    float newStart = altHeld ? hoverBeat : snapBeat(hoverBeat);
                    newStart = std::min(newStart, absEnd - 0.03125f);
                    newStart = std::max(node.clips[ci].startBeat, newStart);
                    n.duration = absEnd - newStart;
                    n.offset = newStart - node.clips[ci].startBeat;
                }
            }
        }

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            if (dragActive) {
                // Push undo for the completed drag operation
                auto after = NoteSnapshot::capture(node);
                auto before = dragStartSnapshot;
                Node* np = &node;
                exec("Move/resize notes",
                    [np, after]() { after.restore(*np); },
                    [np, before]() { before.restore(*np); });
                dragActive = false;
            }
            state.drag = {};
        }

        // Right-click context menu
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Right)
            && !ImGui::IsMouseDragging(ImGuiMouseButton_Right, 5.0f)) {
            auto hit = findNoteAt(node, hoverBeat, hoverPitch, gridX, gridW, totalBeats, mouse.x);
            if (hit.valid()) {
                if (!selected.count({hit.ci, hit.ni}))
                    selected.clear(); selected.insert({hit.ci, hit.ni});
                ImGui::OpenPopup(("##pr_note_ctx_" + std::to_string(node.id)).c_str());
            } else {
                state.emptyClickBeat = hoverBeat;
                state.emptyClickPitch = hoverPitch;
                ImGui::OpenPopup(("##pr_empty_ctx_" + std::to_string(node.id)).c_str());
            }
        }
    }

    // Note context menu
    if (ImGui::BeginPopup(("##pr_note_ctx_" + std::to_string(node.id)).c_str())) {
        int numSel = (int)selected.size();
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1), "%d note%s", numSel, numSel != 1 ? "s" : "");
        ImGui::Separator();

        if (ImGui::MenuItem("Delete")) {
            execNoteEdit("Delete notes", [&]() {
                for (auto it = selected.rbegin(); it != selected.rend(); ++it) {
                    auto [ci, ni] = *it;
                    if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                        node.clips[ci].notes.erase(node.clips[ci].notes.begin() + ni);
                }
                selected.clear();
            });
        }
        if (ImGui::MenuItem("Duplicate")) {
            execNoteEdit("Duplicate notes", [&]() {
                std::set<std::pair<int,int>> newSel;
                for (auto& [ci, ni] : selected) {
                    if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size()) {
                        auto dup = node.clips[ci].notes[ni];
                        dup.offset += 0.25f;
                        node.clips[ci].notes.push_back(dup);
                        newSel.insert({ci, (int)node.clips[ci].notes.size() - 1});
                    }
                }
                selected = newSel;
            });
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Transpose")) {
            auto transpose = [&](int semi, const char* desc) {
                execNoteEdit(desc, [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].pitch = std::clamp(node.clips[ci].notes[ni].pitch + semi, 0, 127);
                });
            };
            if (ImGui::MenuItem("+1 Semitone")) transpose(1, "Transpose +semitone");
            if (ImGui::MenuItem("-1 Semitone")) transpose(-1, "Transpose -semitone");
            if (ImGui::MenuItem("+1 Octave")) transpose(12, "Transpose +octave");
            if (ImGui::MenuItem("-1 Octave")) transpose(-12, "Transpose -octave");
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Duration")) {
            auto setDur = [&](float d, const char* desc) {
                execNoteEdit(desc, [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].duration = d;
                });
            };
            if (ImGui::MenuItem("Double")) {
                execNoteEdit("Double duration", [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].duration *= 2;
                });
            }
            if (ImGui::MenuItem("Halve")) {
                execNoteEdit("Halve duration", [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].duration = std::max(0.125f, node.clips[ci].notes[ni].duration / 2);
                });
            }
            ImGui::Separator();
            if (ImGui::MenuItem("1/16")) setDur(0.25f, "Set duration 1/16");
            if (ImGui::MenuItem("1/8")) setDur(0.5f, "Set duration 1/8");
            if (ImGui::MenuItem("1/4")) setDur(1.0f, "Set duration 1/4");
            if (ImGui::MenuItem("1/2")) setDur(2.0f, "Set duration 1/2");
            if (ImGui::MenuItem("1")) setDur(4.0f, "Set duration 1");
            ImGui::EndMenu();
        }
        // Time shift
        if (ImGui::BeginMenu("Time Shift")) {
            float snapVal = state.snap > 0 ? state.snap : 0.25f;
            if (ImGui::MenuItem("Shift Left")) {
                execNoteEdit("Shift left", [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].offset = std::max(0.0f, node.clips[ci].notes[ni].offset - snapVal);
                });
            }
            if (ImGui::MenuItem("Shift Right")) {
                execNoteEdit("Shift right", [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].offset += snapVal;
                });
            }
            ImGui::EndMenu();
        }

        if (ImGui::MenuItem("Reverse")) {
            execNoteEdit("Reverse", [&]() {
                if (!selected.empty()) {
                    float minOff = 1e9f, maxEnd = 0;
                    for (auto& [ci, ni] : selected) {
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size()) {
                            auto& n = node.clips[ci].notes[ni];
                            float ab = node.clips[ci].startBeat + n.offset;
                            minOff = std::min(minOff, ab);
                            maxEnd = std::max(maxEnd, ab + n.duration);
                        }
                    }
                    for (auto& [ci, ni] : selected) {
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size()) {
                            auto& n = node.clips[ci].notes[ni];
                            float ab = node.clips[ci].startBeat + n.offset;
                            float newAb = maxEnd - (ab - minOff) - n.duration;
                            n.offset = std::max(0.0f, newAb - node.clips[ci].startBeat);
                        }
                    }
                }
            });
        }

        if (ImGui::BeginMenu("Detune")) {
            if (ImGui::MenuItem("Reset to 0")) {
                execNoteEdit("Reset detune", [&]() {
                    for (auto& [ci, ni] : selected)
                        if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                            node.clips[ci].notes[ni].detune = 0;
                });
            }
            float vals[] = {-50, -25, 25, 50};
            const char* labels[] = {"-50 cents", "-25 cents", "+25 cents", "+50 cents"};
            for (int v = 0; v < 4; ++v) {
                if (ImGui::MenuItem(labels[v])) {
                    execNoteEdit("Set detune", [&, v]() {
                        for (auto& [ci, ni] : selected)
                            if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                                node.clips[ci].notes[ni].detune = vals[v];
                    });
                }
            }
            ImGui::EndMenu();
        }

        // Key/Scale settings
        ImGui::Separator();
        if (ImGui::BeginMenu("Root")) {
            for (int i = 0; i < 12; ++i) {
                if (ImGui::MenuItem(MusicTheory::NOTE_NAMES[i], nullptr, state.keyRoot == i))
                    state.keyRoot = i;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Key")) {
            for (auto& [name, _] : MusicTheory::keys()) {
                bool sel = state.activeCategory == "key" && state.keyName == name;
                if (ImGui::MenuItem(name.c_str(), nullptr, sel)) {
                    state.activeCategory = "key";
                    state.keyName = name;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mode")) {
            for (auto& [name, _] : MusicTheory::modes()) {
                bool sel = state.activeCategory == "mode" && state.modeName == name;
                if (ImGui::MenuItem(name.c_str(), nullptr, sel)) {
                    state.activeCategory = "mode";
                    state.modeName = name;
                }
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scale")) {
            for (auto& [name, _] : MusicTheory::scales()) {
                bool sel = state.activeCategory == "scale" && state.scaleName == name;
                if (ImGui::MenuItem(name.c_str(), nullptr, sel)) {
                    state.activeCategory = "scale";
                    state.scaleName = name;
                }
            }
            ImGui::EndMenu();
        }

        // Scale operations
        auto intervals = getActiveIntervals();
        if (ImGui::MenuItem("Snap to Scale")) {
            execNoteEdit("Snap to scale", [&]() {
                int root = state.keyRoot;
                for (auto& [ci, ni] : selected)
                    if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size())
                        node.clips[ci].notes[ni].pitch = MusicTheory::snapToScale(
                            node.clips[ci].notes[ni].pitch, root, intervals);
            });
        }
        if (ImGui::MenuItem("Change Key")) {
            execNoteEdit("Change key", [&]() {
                int root = state.keyRoot;
                for (auto& [ci, ni] : selected)
                    if (ci < (int)node.clips.size() && ni < (int)node.clips[ci].notes.size()) {
                        auto& n = node.clips[ci].notes[ni];
                        n.pitch = std::clamp(
                            MusicTheory::degreeToPitch(n.degree, n.octave, n.chromaticOffset, root, intervals),
                            0, 127);
                    }
            });
        }

        ImGui::Separator();
        if (ImGui::MenuItem("Select All")) {
            selected.clear();
            for (int ci = 0; ci < (int)node.clips.size(); ++ci)
                for (int ni = 0; ni < (int)node.clips[ci].notes.size(); ++ni)
                    selected.insert({ci, ni});
        }
        if (ImGui::MenuItem("Deselect All")) selected.clear();
        ImGui::EndPopup();
    }

    // Empty space context menu
    if (ImGui::BeginPopup(("##pr_empty_ctx_" + std::to_string(node.id)).c_str())) {
        if (ImGui::MenuItem("Place Note Here")) {
            execNoteEdit("Place note", [&]() {
                float sb = snapBeat(state.emptyClickBeat);
                for (auto& clip : node.clips) {
                    if (clip.startBeat <= sb && sb < clip.startBeat + clip.lengthBeats) {
                        MidiNote nn;
                        nn.offset = sb - clip.startBeat;
                        nn.pitch = state.emptyClickPitch;
                        nn.duration = snap * 4;
                        assignDegree(nn);
                        clip.notes.push_back(nn);
                        break;
                    }
                }
            });
        }
        ImGui::Separator();
        if (ImGui::BeginMenu("Snap Grid")) {
            struct S { const char* l; float v; };
            S snaps[] = {{"1/4 beat", 0.25f}, {"1/2 beat", 0.5f}, {"1 beat", 1.0f}, {"Off", 0.0f}};
            for (auto& s : snaps) {
                bool cur = std::abs(state.snap - s.v) < 0.01f;
                if (ImGui::MenuItem(s.l, nullptr, cur))
                    state.snap = s.v;
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Root")) {
            for (int i = 0; i < 12; ++i)
                if (ImGui::MenuItem(MusicTheory::NOTE_NAMES[i], nullptr, state.keyRoot == i))
                    state.keyRoot = i;
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Key")) {
            for (auto& [name, _] : MusicTheory::keys())
                if (ImGui::MenuItem(name.c_str(), nullptr, state.activeCategory == "key" && state.keyName == name))
                    { state.activeCategory = "key"; state.keyName = name; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Mode")) {
            for (auto& [name, _] : MusicTheory::modes())
                if (ImGui::MenuItem(name.c_str(), nullptr, state.activeCategory == "mode" && state.modeName == name))
                    { state.activeCategory = "mode"; state.modeName = name; }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Scale")) {
            for (auto& [name, _] : MusicTheory::scales())
                if (ImGui::MenuItem(name.c_str(), nullptr, state.activeCategory == "scale" && state.scaleName == name))
                    { state.activeCategory = "scale"; state.scaleName = name; }
            ImGui::EndMenu();
        }
        if (ImGui::MenuItem("Detect Key")) {
            std::vector<int> pitches;
            for (auto& clip : node.clips)
                for (auto& n : clip.notes)
                    pitches.push_back(n.pitch);
            state.detectResults = MusicTheory::detectKeys(pitches);
            state.showDetectResults = true;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("Select All")) {
            selected.clear();
            for (int ci = 0; ci < (int)node.clips.size(); ++ci)
                for (int ni = 0; ni < (int)node.clips[ci].notes.size(); ++ni)
                    selected.insert({ci, ni});
        }
        if (ImGui::MenuItem("Deselect All")) selected.clear();
        ImGui::EndPopup();
    }

    // Piano key column interaction — click to audition
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, gridY));
    ImGui::InvisibleButton(("##piano_keys_" + std::to_string(node.id)).c_str(),
                            ImVec2(keyW, gridH));
    if (ImGui::IsItemHovered()) {
        auto mouse = ImGui::GetMousePos();
        float relY = mouse.y - gridY;
        int clickPitch = pitchHi - (int)std::floor(relY / rowH);
        clickPitch = std::clamp(clickPitch, 0, 127);

        if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
            double now = ImGui::GetTime();
            double duration = 0.5; // half second preview

            // Add audition note
            state.auditionNotes.push_back({clickPitch, now, duration, false, false});

            // Send note-on to the node's audition buffer
            {
                std::lock_guard<std::mutex> lock(*node.auditionMutex);
                node.pendingAudition.push_back({true, clickPitch, 100});
            }
        }
    }

    // Expire audition notes and send note-offs
    {
        double now = ImGui::GetTime();
        for (auto& an : state.auditionNotes) {
            if (!an.noteOffSent && now - an.startTime >= an.duration) {
                an.noteOffSent = true;
                std::lock_guard<std::mutex> lock(*node.auditionMutex);
                node.pendingAudition.push_back({false, an.pitch, 0});
            }
        }
        // Remove expired notes (keep for a bit longer for visual fade)
        state.auditionNotes.erase(
            std::remove_if(state.auditionNotes.begin(), state.auditionNotes.end(),
                [now](auto& an) { return now - an.startTime > an.duration + 0.1; }),
            state.auditionNotes.end());
    }

    // Reserve space
    ImGui::SetCursorScreenPos(ImVec2(cursor.x, gridY + gridH));
    ImGui::Dummy(ImVec2(availW, 1));
}

} // namespace SoundShop
