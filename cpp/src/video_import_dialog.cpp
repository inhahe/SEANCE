#include "video_import_dialog.h"
#include "node_graph.h"
#include <thread>
#include <cmath>

namespace SoundShop {

namespace {
    constexpr int   kProxyMaxDim    = 480;   // longest side of preview frames
    constexpr int   kProxyMaxFrames = 360;   // total preview frames
    constexpr float kHandle         = 7.0f;  // crop handle hit radius (px)
    constexpr int   kMinOutDim      = 4;
    constexpr int   kMaxOutDim      = 512;
    constexpr int   kMaxOutFrames   = 512;
}

VideoImportDialogComponent::VideoImportDialogComponent(NodeGraph& g, int id,
                                                       std::function<void()> onCommittedCb)
    : graph(g), nodeId(id), onCommitted(std::move(onCommittedCb)) {

    titleLabel.setText("Import Video into Terrain", juce::dontSendNotification);
    titleLabel.setFont(juce::Font(juce::FontOptions(17.0f, juce::Font::bold)));
    titleLabel.setColour(juce::Label::textColourId, juce::Colours::white);
    addAndMakeVisible(titleLabel);

    chooseBtn.onClick = [this] { chooseVideo(); };
    addAndMakeVisible(chooseBtn);

    playBtn.onClick = [this] {
        if (proxyCount <= 0) return;
        playing = !playing;
        playBtn.setButtonText(playing ? "Pause" : "Play");
        if (playing && currentTime >= tOut - 1.0e-4) currentTime = tIn;
    };
    playBtn.setTooltip("Play / pause the preview. Playback loops within the time "
                       "crop (the cyan in/out handles on the timeline below).");
    addAndMakeVisible(playBtn);

    statusLabel.setText("Choose a video file to begin.", juce::dontSendNotification);
    statusLabel.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
    statusLabel.setFont(juce::Font(juce::FontOptions(13.0f)));
    addAndMakeVisible(statusLabel);

    auto setupDimSlider = [this](juce::Slider& s, juce::Label& l, const juce::String& name,
                                 int lo, int hi, int val) {
        l.setText(name, juce::dontSendNotification);
        l.setColour(juce::Label::textColourId, juce::Colours::lightgrey);
        l.setFont(juce::Font(juce::FontOptions(12.0f)));
        addAndMakeVisible(l);
        s.setSliderStyle(juce::Slider::IncDecButtons);
        s.setRange((double)lo, (double)hi, 1.0);
        s.setValue((double)val, juce::dontSendNotification);
        s.setTextBoxStyle(juce::Slider::TextBoxLeft, false, 70, 22);
        addAndMakeVisible(s);
    };
    setupDimSlider(outWSlider, outWLabel, "Output W (px)", kMinOutDim, kMaxOutDim, outW);
    setupDimSlider(outHSlider, outHLabel, "Output H (px)", kMinOutDim, kMaxOutDim, outH);
    setupDimSlider(framesSlider, framesLabel, "Output frames", 1, kMaxOutFrames, outFrames);

    outWSlider.setTooltip("Width of the terrain grid in pixels. Linked to Output H "
                          "by the crop's aspect ratio - changing one updates the other.");
    outHSlider.setTooltip("Height of the terrain grid in pixels. Linked to Output W "
                          "by the crop's aspect ratio - changing one updates the other.");
    framesSlider.setTooltip("Number of time slices sampled across the cropped time range. "
                            "This is the depth (time axis) of the 3D terrain.");

    outWSlider.onValueChange = [this] {
        if (syncingDims) return;
        outW = (int)outWSlider.getValue();
        updateAspectLink(/*keepWidth*/ true);
    };
    outHSlider.onValueChange = [this] {
        if (syncingDims) return;
        outH = (int)outHSlider.getValue();
        updateAspectLink(/*keepWidth*/ false);
    };
    framesSlider.onValueChange = [this] {
        if (syncingDims) return;
        outFrames = (int)framesSlider.getValue();
        syncDimSlidersFromState();
    };

    gridInfoLabel.setColour(juce::Label::textColourId, juce::Colours::grey);
    gridInfoLabel.setFont(juce::Font(juce::FontOptions(11.0f)));
    gridInfoLabel.setJustificationType(juce::Justification::centredRight);
    addAndMakeVisible(gridInfoLabel);

    okBtn.onClick = [this] { doBuildAndCommit(); };
    addAndMakeVisible(okBtn);
    cancelBtn.onClick = [this] { closeSelf(); };
    addAndMakeVisible(cancelBtn);

    // Re-open: if the node already has a __video__ script, seed the controls
    // from it (params only - we don't need to decode the baked gray blob).
    if (auto* nd = graph.findNode(nodeId)) {
        VideoTerrainParams vp;
        if (parseVideoTerrainScript(nd->script, vp, /*wantGray*/ false)) {
            juce::File f(juce::String(vp.path));
            if (f.existsAsFile()) {
                // loadVideoFile probes the source and resets crop/time/dims to
                // defaults; apply the SAVED values afterwards so re-opening an
                // already-imported node restores exactly what was committed.
                loadVideoFile(f);
                if (info.ok) {
                    tIn  = juce::jlimit(0.0, info.durationSec, vp.t0);
                    tOut = juce::jlimit(tIn, info.durationSec, vp.t1);
                    currentTime = tIn;
                    if (info.width > 0 && info.height > 0) {
                        cropN.setBounds((float)vp.cropX / (float)info.width,
                                        (float)vp.cropY / (float)info.height,
                                        (float)vp.cropW / (float)info.width,
                                        (float)vp.cropH / (float)info.height);
                    }
                    outW = juce::jlimit(kMinOutDim, kMaxOutDim, vp.outW);
                    outH = juce::jlimit(kMinOutDim, kMaxOutDim, vp.outH);
                    outFrames = juce::jlimit(1, kMaxOutFrames, vp.outFrames);
                    syncDimSlidersFromState();
                }
            }
        }
    }

    if (!VideoDecoder::available()) {
        chooseBtn.setEnabled(false);
        chooseBtn.setTooltip("ffmpeg was not found on PATH. Install ffmpeg (and ffprobe) "
                             "to import video. See Help for details.");
        statusLabel.setText("ffmpeg not found - video import is unavailable. "
                            "Install ffmpeg / ffprobe and re-open this dialog.",
                            juce::dontSendNotification);
    }

    setSize(760, 660);
    updateControlEnablement();
    startTimer(33); // ~30 Hz: playback + job polling
}

VideoImportDialogComponent::~VideoImportDialogComponent() {
    stopTimer();
    if (proxyJob) proxyJob->abort = true;
    if (buildJob) buildJob->abort = true;
    // Detached worker threads hold their own shared_ptr copies; they finish and
    // release harmlessly. Clean up the temp proxy directory we own.
    if (proxyDir.isDirectory())
        proxyDir.deleteRecursively();
}

// ---------------------------------------------------------------------------
// Choosing / loading the video
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::chooseVideo() {
    chooser = std::make_shared<juce::FileChooser>(
        "Select a video file", juce::File(),
        "*.mp4;*.mov;*.mkv;*.avi;*.webm;*.m4v;*.wmv;*.flv;*.mpg;*.mpeg");
    auto flags = juce::FileBrowserComponent::openMode
               | juce::FileBrowserComponent::canSelectFiles;
    chooser->launchAsync(flags, [this](const juce::FileChooser& fc) {
        auto f = fc.getResult();
        if (f.existsAsFile()) loadVideoFile(f);
    });
}

void VideoImportDialogComponent::loadVideoFile(const juce::File& f) {
    playing = false;
    playBtn.setButtonText("Play");

    videoFile = f;
    info = VideoDecoder::probe(f);
    if (!info.ok) {
        statusLabel.setText("Could not read video: " + juce::String(info.error),
                            juce::dontSendNotification);
        updateControlEnablement();
        repaint();
        return;
    }

    // Reset crop / time to full unless a caller set them right after (re-open).
    cropN = { 0.0f, 0.0f, 1.0f, 1.0f };
    tIn = 0.0;
    tOut = info.durationSec;
    currentTime = 0.0;
    currentFrameImg = {};
    currentFrameIdx = -1;
    proxyCount = 0;
    proxyFps = 0.0;

    // Default output: cap width at 128, derive height from the frame aspect.
    outW = juce::jlimit(kMinOutDim, kMaxOutDim, std::min(128, info.width));
    updateAspectLink(true);
    syncDimSlidersFromState();

    statusLabel.setText(juce::String(info.width) + " x " + juce::String(info.height)
                        + "  -  " + juce::String(info.durationSec, 2) + " s  -  "
                        + juce::String(info.fps, 2) + " fps. Extracting preview...",
                        juce::dontSendNotification);

    startProxy();
    updateControlEnablement();
    repaint();
}

void VideoImportDialogComponent::startProxy() {
    if (proxyJob) proxyJob->abort = true; // abandon any prior extraction

    // Fresh temp dir per load so stale frames never leak across videos.
    if (proxyDir.isDirectory()) proxyDir.deleteRecursively();
    proxyDir = juce::File::getSpecialLocation(juce::File::tempDirectory)
                   .getChildFile("seance_video_proxy_"
                                 + juce::String::toHexString(juce::Random::getSystemRandom().nextInt64()));
    proxyDir.createDirectory();

    auto job = std::make_shared<ProxyJob>();
    job->dir = proxyDir;
    proxyJob = job;

    juce::File video = videoFile;
    std::thread([job, video] {
        double pf = 0.0;
        int n = VideoDecoder::extractProxy(video, job->dir, kProxyMaxDim, kProxyMaxFrames,
                                           pf, [job] { return job->abort.load(); });
        job->count = n;
        job->fps   = pf;
        job->done  = true;
    }).detach();
}

// ---------------------------------------------------------------------------
// Output dimension aspect linking
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::updateAspectLink(bool keepWidth) {
    if (!info.ok || info.width <= 0 || info.height <= 0) { syncDimSlidersFromState(); return; }

    // Aspect of the cropped source region in pixels.
    double cropPxW = std::max(1.0, (double)cropN.getWidth()  * info.width);
    double cropPxH = std::max(1.0, (double)cropN.getHeight() * info.height);
    double aspect  = cropPxW / cropPxH; // w / h

    if (keepWidth) {
        outH = (int)std::llround((double)outW / aspect);
    } else {
        outW = (int)std::llround((double)outH * aspect);
    }
    outW = juce::jlimit(kMinOutDim, kMaxOutDim, outW);
    outH = juce::jlimit(kMinOutDim, kMaxOutDim, outH);
    syncDimSlidersFromState();
}

void VideoImportDialogComponent::syncDimSlidersFromState() {
    syncingDims = true;
    outWSlider.setValue(outW, juce::dontSendNotification);
    outHSlider.setValue(outH, juce::dontSendNotification);
    framesSlider.setValue(outFrames, juce::dontSendNotification);
    syncingDims = false;
    const long long cells = (long long)outW * outH * outFrames;
    gridInfoLabel.setText("Grid: " + juce::String(outW) + " x " + juce::String(outH)
                          + " x " + juce::String(outFrames) + " = "
                          + juce::String(cells) + " cells",
                          juce::dontSendNotification);
}

// ---------------------------------------------------------------------------
// Preview frame loading
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::loadFrameForTime(double t) {
    if (proxyCount <= 0 || proxyFps <= 0.0) return;
    int idx = (int)std::llround(t * proxyFps);
    idx = juce::jlimit(0, proxyCount - 1, idx);
    if (idx == currentFrameIdx && currentFrameImg.isValid()) return;
    auto f = proxyDir.getChildFile(juce::String::formatted("f%06d.jpg", idx + 1));
    auto img = juce::ImageFileFormat::loadFrom(f);
    if (img.isValid()) {
        currentFrameImg = img;
        currentFrameIdx = idx;
    }
}

// ---------------------------------------------------------------------------
// Build the final grid and commit it to the node
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::doBuildAndCommit() {
    if (!info.ok || building) return;
    playing = false;
    playBtn.setButtonText("Play");

    VideoTerrainParams vp;
    vp.path = videoFile.getFullPathName().toStdString();
    vp.t0 = juce::jlimit(0.0, info.durationSec, std::min(tIn, tOut));
    vp.t1 = juce::jlimit(vp.t0, info.durationSec, std::max(tIn, tOut));

    int cx = juce::jlimit(0, info.width  - 1, (int)std::llround(cropN.getX() * info.width));
    int cy = juce::jlimit(0, info.height - 1, (int)std::llround(cropN.getY() * info.height));
    int cw = juce::jlimit(1, info.width  - cx, (int)std::llround(cropN.getWidth()  * info.width));
    int ch = juce::jlimit(1, info.height - cy, (int)std::llround(cropN.getHeight() * info.height));
    vp.cropX = cx; vp.cropY = cy; vp.cropW = cw; vp.cropH = ch;
    vp.outW = juce::jlimit(kMinOutDim, kMaxOutDim, outW);
    vp.outH = juce::jlimit(kMinOutDim, kMaxOutDim, outH);
    vp.outFrames = juce::jlimit(1, kMaxOutFrames, outFrames);

    auto job = std::make_shared<BuildJob>();
    job->params = vp;
    buildJob = job;
    building = true;
    statusLabel.setText("Decoding terrain from video at full quality...",
                        juce::dontSendNotification);
    updateControlEnablement();

    juce::File video = videoFile;
    std::thread([job, video] {
        std::string err;
        auto gray = VideoDecoder::decodeGrid(
            video, job->params.t0, job->params.t1,
            job->params.cropX, job->params.cropY, job->params.cropW, job->params.cropH,
            job->params.outW, job->params.outH, job->params.outFrames, &err);
        job->gray  = std::move(gray);
        job->error = err;
        job->ok    = !job->gray.empty();
        job->done  = true;
    }).detach();
}

void VideoImportDialogComponent::closeSelf() {
    if (auto* dw = findParentComponentOfClass<juce::DialogWindow>()) {
        juce::Component::SafePointer<juce::DialogWindow> safe(dw);
        juce::MessageManager::callAsync([safe] { if (safe) delete safe.getComponent(); });
    }
}

void VideoImportDialogComponent::updateControlEnablement() {
    const bool haveVideo = info.ok && !building;
    const bool ffmpegOk  = VideoDecoder::available();
    chooseBtn.setEnabled(ffmpegOk && !building);
    playBtn.setEnabled(haveVideo && proxyCount > 0);
    outWSlider.setEnabled(haveVideo);
    outHSlider.setEnabled(haveVideo);
    framesSlider.setEnabled(haveVideo);
    okBtn.setEnabled(haveVideo);
    okBtn.setTooltip(building ? "Decoding in progress..."
                     : (!info.ok ? "Load a video first." : "Decode and import the terrain."));
}

// ---------------------------------------------------------------------------
// Timer: playback advance + background job polling
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::timerCallback() {
    // 1. Proxy extraction finished?
    if (proxyJob && proxyJob->done.load()) {
        auto job = proxyJob;
        proxyJob.reset();
        if (!job->abort.load()) {
            proxyCount = job->count;
            proxyFps   = job->fps;
            if (proxyCount > 0) {
                loadFrameForTime(currentTime);
                statusLabel.setText(juce::String(info.width) + " x " + juce::String(info.height)
                                    + "  -  " + juce::String(info.durationSec, 2) + " s  -  "
                                    + juce::String(info.fps, 2)
                                    + " fps. Crop in space (drag the box) and time (drag the bar).",
                                    juce::dontSendNotification);
            } else {
                statusLabel.setText("Preview extraction failed (ffmpeg produced no frames).",
                                    juce::dontSendNotification);
            }
            updateControlEnablement();
            repaint();
        }
    }

    // 2. Final decode finished?
    if (buildJob && buildJob->done.load()) {
        auto job = buildJob;
        buildJob.reset();
        building = false;
        if (!job->abort.load()) {
            if (job->ok) {
                if (auto* nd = graph.findNode(nodeId)) {
                    nd->script = makeVideoTerrainScript(job->params);
                    if (onCommitted) onCommitted();
                }
                closeSelf();
                return;
            } else {
                statusLabel.setText("Decode failed: "
                                    + juce::String(job->error.empty() ? "unknown error" : job->error),
                                    juce::dontSendNotification);
                updateControlEnablement();
                repaint();
            }
        }
    }

    // 3. Playback advance.
    if (playing && proxyCount > 0) {
        currentTime += 1.0 / 30.0;
        if (currentTime >= tOut) currentTime = tIn; // loop within the time crop
        loadFrameForTime(currentTime);
        repaint();
    }
}

// ---------------------------------------------------------------------------
// Layout
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::resized() {
    auto r = getLocalBounds().reduced(14);

    titleLabel.setBounds(r.removeFromTop(26));
    r.removeFromTop(4);

    auto topRow = r.removeFromTop(28);
    chooseBtn.setBounds(topRow.removeFromLeft(140));
    r.removeFromTop(6);

    // Bottom controls reserved first so the canvas takes the middle.
    auto buttonRow = r.removeFromBottom(34);
    okBtn.setBounds(buttonRow.removeFromRight(110));
    buttonRow.removeFromRight(8);
    cancelBtn.setBounds(buttonRow.removeFromRight(110));
    r.removeFromBottom(8);

    auto dimsRow = r.removeFromBottom(28);
    gridInfoLabel.setBounds(dimsRow.removeFromRight(210));
    outWLabel.setBounds(dimsRow.removeFromLeft(86));
    outWSlider.setBounds(dimsRow.removeFromLeft(110));
    dimsRow.removeFromLeft(6);
    outHLabel.setBounds(dimsRow.removeFromLeft(86));
    outHSlider.setBounds(dimsRow.removeFromLeft(110));
    r.removeFromBottom(6);

    auto framesRow = r.removeFromBottom(28);
    framesLabel.setBounds(framesRow.removeFromLeft(86));
    framesSlider.setBounds(framesRow.removeFromLeft(140));
    r.removeFromBottom(8);

    statusLabel.setBounds(r.removeFromBottom(22));
    r.removeFromBottom(6);

    // Transport lives with the timeline: Play/Pause sits to the left of the
    // scrub bar (next to the playhead), where users expect transport controls -
    // not tucked up by the file picker.
    auto timelineRow = r.removeFromBottom(34);
    playBtn.setBounds(timelineRow.removeFromLeft(76).reduced(0, 3));
    timelineRow.removeFromLeft(8);
    timelineRect = timelineRow;
    r.removeFromBottom(8);

    canvasRect = r;
}

// ---------------------------------------------------------------------------
// Geometry helpers
// ---------------------------------------------------------------------------
juce::Rectangle<float> VideoImportDialogComponent::imageDisplayRect() const {
    auto area = canvasRect.toFloat().reduced(2.0f);
    if (!info.ok || info.width <= 0 || info.height <= 0) return area;
    float srcAspect = (float)info.width / (float)info.height;
    float areaAspect = area.getWidth() / area.getHeight();
    juce::Rectangle<float> out = area;
    if (areaAspect > srcAspect) {
        float w = area.getHeight() * srcAspect;
        out = { area.getCentreX() - w * 0.5f, area.getY(), w, area.getHeight() };
    } else {
        float h = area.getWidth() / srcAspect;
        out = { area.getX(), area.getCentreY() - h * 0.5f, area.getWidth(), h };
    }
    return out;
}

juce::Rectangle<float> VideoImportDialogComponent::cropScreenRect() const {
    auto img = imageDisplayRect();
    return { img.getX() + cropN.getX() * img.getWidth(),
             img.getY() + cropN.getY() * img.getHeight(),
             cropN.getWidth()  * img.getWidth(),
             cropN.getHeight() * img.getHeight() };
}

double VideoImportDialogComponent::timeForX(float x) const {
    if (!info.ok || info.durationSec <= 0.0) return 0.0;
    auto bar = timelineRect.toFloat().reduced(8.0f, 0.0f);
    float t = juce::jlimit(0.0f, 1.0f, (x - bar.getX()) / juce::jmax(1.0f, bar.getWidth()));
    return (double)t * info.durationSec;
}

float VideoImportDialogComponent::xForTime(double t) const {
    auto bar = timelineRect.toFloat().reduced(8.0f, 0.0f);
    if (!info.ok || info.durationSec <= 0.0) return bar.getX();
    float frac = (float)juce::jlimit(0.0, 1.0, t / info.durationSec);
    return bar.getX() + frac * bar.getWidth();
}

VideoImportDialogComponent::Drag
VideoImportDialogComponent::hitTest(juce::Point<float> p) const {
    // Timeline handles take priority when the cursor is over the bar.
    if (timelineRect.toFloat().contains(p)) {
        float xi = xForTime(tIn), xo = xForTime(tOut), xp = xForTime(currentTime);
        if (std::abs(p.x - xi) <= kHandle) return Drag::TimeIn;
        if (std::abs(p.x - xo) <= kHandle) return Drag::TimeOut;
        if (std::abs(p.x - xp) <= kHandle) return Drag::Playhead;
        return Drag::Playhead; // click anywhere on the bar seeks
    }

    if (proxyCount <= 0) return Drag::None;
    auto c = cropScreenRect();
    bool nearL = std::abs(p.x - c.getX()) <= kHandle;
    bool nearR = std::abs(p.x - c.getRight()) <= kHandle;
    bool nearT = std::abs(p.y - c.getY()) <= kHandle;
    bool nearB = std::abs(p.y - c.getBottom()) <= kHandle;
    bool inX = p.x >= c.getX() - kHandle && p.x <= c.getRight() + kHandle;
    bool inY = p.y >= c.getY() - kHandle && p.y <= c.getBottom() + kHandle;
    if (nearL && nearT) return Drag::CropTL;
    if (nearR && nearT) return Drag::CropTR;
    if (nearL && nearB) return Drag::CropBL;
    if (nearR && nearB) return Drag::CropBR;
    if (nearL && inY)   return Drag::CropL;
    if (nearR && inY)   return Drag::CropR;
    if (nearT && inX)   return Drag::CropT;
    if (nearB && inX)   return Drag::CropB;
    if (c.contains(p))  return Drag::CropMove;
    return Drag::None;
}

// ---------------------------------------------------------------------------
// Painting
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::paint(juce::Graphics& g) {
    g.fillAll(juce::Colour(0xff2b2b30));

    // ---- canvas ----
    g.setColour(juce::Colour(0xff1a1a1d));
    g.fillRect(canvasRect);

    auto img = imageDisplayRect();
    if (currentFrameImg.isValid()) {
        g.drawImage(currentFrameImg, img,
                    juce::RectanglePlacement::stretchToFit);
    } else {
        g.setColour(juce::Colours::darkgrey);
        g.drawRect(img, 1.0f);
        g.setColour(juce::Colours::grey);
        g.setFont(juce::Font(juce::FontOptions(14.0f)));
        g.drawText(info.ok ? "Extracting preview..." : "No video loaded",
                   canvasRect, juce::Justification::centred);
    }

    if (proxyCount > 0 && info.ok) {
        // Dim the area outside the crop, then outline the crop + handles.
        auto c = cropScreenRect();
        g.setColour(juce::Colours::black.withAlpha(0.55f));
        // top / bottom / left / right bands around the crop, clipped to img
        juce::Rectangle<float> top(img.getX(), img.getY(), img.getWidth(), c.getY() - img.getY());
        juce::Rectangle<float> bot(img.getX(), c.getBottom(), img.getWidth(), img.getBottom() - c.getBottom());
        juce::Rectangle<float> lft(img.getX(), c.getY(), c.getX() - img.getX(), c.getHeight());
        juce::Rectangle<float> rgt(c.getRight(), c.getY(), img.getRight() - c.getRight(), c.getHeight());
        for (auto& band : { top, bot, lft, rgt })
            if (band.getWidth() > 0 && band.getHeight() > 0) g.fillRect(band);

        g.setColour(juce::Colours::cyan);
        g.drawRect(c, 1.5f);
        g.setColour(juce::Colours::white);
        for (auto pt : { c.getTopLeft(), c.getTopRight(), c.getBottomLeft(), c.getBottomRight() })
            g.fillRect(juce::Rectangle<float>(7, 7).withCentre(pt));

        // Crop size readout (source pixels).
        int cw = juce::jmax(1, (int)std::llround(cropN.getWidth()  * info.width));
        int ch = juce::jmax(1, (int)std::llround(cropN.getHeight() * info.height));
        g.setColour(juce::Colours::cyan.withAlpha(0.9f));
        g.setFont(juce::Font(juce::FontOptions(11.0f)));
        g.drawText(juce::String(cw) + " x " + juce::String(ch) + " px",
                   c.toNearestInt().reduced(3), juce::Justification::topLeft);
    }

    // ---- timeline ----
    auto bar = timelineRect.toFloat().reduced(8.0f, 8.0f);
    g.setColour(juce::Colour(0xff15151a));
    g.fillRoundedRectangle(bar, 3.0f);

    if (info.ok && info.durationSec > 0.0) {
        float xi = xForTime(tIn), xo = xForTime(tOut), xp = xForTime(currentTime);
        // selected (kept) span
        g.setColour(juce::Colours::cyan.withAlpha(0.25f));
        g.fillRect(juce::Rectangle<float>(xi, bar.getY(), juce::jmax(1.0f, xo - xi), bar.getHeight()));
        // in / out handles
        g.setColour(juce::Colours::cyan);
        g.fillRect(juce::Rectangle<float>(3.0f, bar.getHeight() + 6.0f).withCentre({ xi, bar.getCentreY() }));
        g.fillRect(juce::Rectangle<float>(3.0f, bar.getHeight() + 6.0f).withCentre({ xo, bar.getCentreY() }));
        // playhead
        g.setColour(juce::Colours::white);
        g.fillRect(juce::Rectangle<float>(2.0f, bar.getHeight() + 4.0f).withCentre({ xp, bar.getCentreY() }));

        g.setColour(juce::Colours::lightgrey);
        g.setFont(juce::Font(juce::FontOptions(10.0f)));
        g.drawText(juce::String(currentTime, 2) + " s  [" + juce::String(std::min(tIn, tOut), 2)
                   + " - " + juce::String(std::max(tIn, tOut), 2) + " s]",
                   timelineRect.reduced(10, 0), juce::Justification::centredRight);
    }
}

// ---------------------------------------------------------------------------
// Mouse
// ---------------------------------------------------------------------------
void VideoImportDialogComponent::mouseDown(const juce::MouseEvent& e) {
    auto p = e.position;
    dragMode = hitTest(p);
    dragStartPos = p;
    dragStartCrop = cropN;

    if (dragMode == Drag::Playhead) {
        playing = false;
        playBtn.setButtonText("Play");
        currentTime = juce::jlimit(0.0, info.durationSec, timeForX(p.x));
        loadFrameForTime(currentTime);
        repaint();
    } else if (dragMode == Drag::TimeIn) {
        tIn = juce::jlimit(0.0, tOut, timeForX(p.x));
        currentTime = tIn; loadFrameForTime(currentTime); repaint();
    } else if (dragMode == Drag::TimeOut) {
        tOut = juce::jlimit(tIn, info.durationSec, timeForX(p.x));
        currentTime = tOut; loadFrameForTime(currentTime); repaint();
    }
}

void VideoImportDialogComponent::mouseDrag(const juce::MouseEvent& e) {
    if (dragMode == Drag::None) return;
    auto p = e.position;

    // ---- timeline drags ----
    if (dragMode == Drag::Playhead) {
        currentTime = juce::jlimit(0.0, info.durationSec, timeForX(p.x));
        loadFrameForTime(currentTime); repaint(); return;
    }
    if (dragMode == Drag::TimeIn) {
        tIn = juce::jlimit(0.0, tOut, timeForX(p.x));
        currentTime = tIn; loadFrameForTime(currentTime); repaint(); return;
    }
    if (dragMode == Drag::TimeOut) {
        tOut = juce::jlimit(tIn, info.durationSec, timeForX(p.x));
        currentTime = tOut; loadFrameForTime(currentTime); repaint(); return;
    }

    // ---- crop drags (work in normalized image space) ----
    auto img = imageDisplayRect();
    if (img.getWidth() <= 0 || img.getHeight() <= 0) return;
    float dxN = (p.x - dragStartPos.x) / img.getWidth();
    float dyN = (p.y - dragStartPos.y) / img.getHeight();

    juce::Rectangle<float> c = dragStartCrop;
    const float minSz = 0.02f;

    auto clampX = [](float v) { return juce::jlimit(0.0f, 1.0f, v); };

    switch (dragMode) {
        case Drag::CropMove: {
            float nx = clampX(dragStartCrop.getX() + dxN);
            float ny = clampX(dragStartCrop.getY() + dyN);
            nx = std::min(nx, 1.0f - dragStartCrop.getWidth());
            ny = std::min(ny, 1.0f - dragStartCrop.getHeight());
            c.setPosition(std::max(0.0f, nx), std::max(0.0f, ny));
            break;
        }
        case Drag::CropL: case Drag::CropTL: case Drag::CropBL: {
            float nl = clampX(dragStartCrop.getX() + dxN);
            nl = std::min(nl, dragStartCrop.getRight() - minSz);
            c.setLeft(nl);
            break;
        }
        case Drag::CropR: case Drag::CropTR: case Drag::CropBR: {
            float nr = clampX(dragStartCrop.getRight() + dxN);
            nr = std::max(nr, dragStartCrop.getX() + minSz);
            c.setRight(nr);
            break;
        }
        default: break;
    }
    switch (dragMode) {
        case Drag::CropT: case Drag::CropTL: case Drag::CropTR: {
            float nt = clampX(dragStartCrop.getY() + dyN);
            nt = std::min(nt, dragStartCrop.getBottom() - minSz);
            c.setTop(nt);
            break;
        }
        case Drag::CropB: case Drag::CropBL: case Drag::CropBR: {
            float nb = clampX(dragStartCrop.getBottom() + dyN);
            nb = std::max(nb, dragStartCrop.getY() + minSz);
            c.setBottom(nb);
            break;
        }
        default: break;
    }

    cropN = c;
    // Resizing the crop changes the source aspect, so refresh the linked dims
    // (keep the user's current width, derive height).
    if (dragMode != Drag::CropMove) updateAspectLink(true);
    repaint();
}

void VideoImportDialogComponent::mouseUp(const juce::MouseEvent&) {
    dragMode = Drag::None;
}

void VideoImportDialogComponent::mouseMove(const juce::MouseEvent& e) {
    auto d = hitTest(e.position);
    juce::MouseCursor cur = juce::MouseCursor::NormalCursor;
    switch (d) {
        case Drag::CropL: case Drag::CropR:
            cur = juce::MouseCursor::LeftRightResizeCursor; break;
        case Drag::CropT: case Drag::CropB:
            cur = juce::MouseCursor::UpDownResizeCursor; break;
        case Drag::CropTL: case Drag::CropBR:
            cur = juce::MouseCursor::TopLeftCornerResizeCursor; break;
        case Drag::CropTR: case Drag::CropBL:
            cur = juce::MouseCursor::TopRightCornerResizeCursor; break;
        case Drag::CropMove:
            cur = juce::MouseCursor::DraggingHandCursor; break;
        case Drag::TimeIn: case Drag::TimeOut: case Drag::Playhead:
            cur = juce::MouseCursor::LeftRightResizeCursor; break;
        default: break;
    }
    setMouseCursor(cur);
}

} // namespace SoundShop
