#pragma once
#include <vector>
#include <string>
#include <memory>
#include "customtypes.h"

class IApp;
class VideoTrack;
class IRenderer;
class DXShader;

class Sequence
{
private:
    std::string name;
    std::vector<SequenceItem*> items;
    IApp* appInterface;

    size_t currentIdx = 0;
    bool isPlaying = false;
    bool isLoopedSequence = false;

    // Track double-buffering to achieve gapless transitions
    std::unique_ptr<VideoTrack> activeTrack;
    std::unique_ptr<VideoTrack> standbyTrack;

public:
    Sequence(const std::string& name, const std::vector<SequenceItem*>& items, IApp* appInterface);
    Sequence(const Sequence& other);
    ~Sequence();

    void Start(double startTime, bool loopSequence = false);
    void Stop();
    void UpdateAndRender(IRenderer* renderer, DXShader* shader, float winW, float winH);

    bool IsPlaying() const { return isPlaying; }
    const std::string& GetName() const { return name; }

private:
    void PreloadNextTrack();
    void AdvanceSequence();
};

