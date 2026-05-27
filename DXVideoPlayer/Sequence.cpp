#include "Sequence.h"
#include "VideoTrack.h"
#include "VideoSource.h"
#include "IApp.h"
#include "IRenderer.h"
#include "utils.h"
#include <iostream>

Sequence::Sequence(const std::string& name, const std::vector<SequenceItem*>& items, IApp* appInterface)
	: name(name), items(items), appInterface(appInterface)
{ }

Sequence::Sequence(const Sequence& other)
    : name(other.name),
    appInterface(other.appInterface),
    currentIdx(0),
    isPlaying(false),
    isLoopedSequence(false)
{
    // Deep-copy the structural metadata configs safely
    for (const auto* item : other.items)
    {
        SequenceItem* newItem = new SequenceItem();
        newItem->filename = item->filename;
        newItem->fadeInDuration = item->fadeInDuration;
        newItem->fadeOutDuration = item->fadeOutDuration;
        newItem->looped = item->looped;
        this->items.push_back(newItem);
    }

    // Explicitly leave activeTrack and standbyTrack initialized as nullptr 
    // to bypass copying unique_ptrs. They will be instantiated normally on Start().
}

Sequence::~Sequence()
{
    Stop();
    for (auto item : items)
    {
        delete item;
    }
}

void Sequence::Start(double startTime, bool loopSequence)
{
    if (items.empty()) return;

    currentIdx = 0;
    isPlaying = true;
    isLoopedSequence = loopSequence;

    // Resolve the first asset match from pre-allocated App state sources
    AppState& state = appInterface->GetAppState();
    VideoSource* firstSource = nullptr;

    for (auto source : state.sources)
    {
        if (source->filename == items[currentIdx]->filename)
        {
            firstSource = source;
            break;
        }
    }

    if (firstSource)
    {
        // Override properties based on sequence file parameters
        firstSource->fadeInDuration = items[currentIdx]->fadeInDuration;
        firstSource->fadeOutDuration = items[currentIdx]->fadeOutDuration;
        firstSource->looped = items[currentIdx]->looped;

        activeTrack = std::make_unique<VideoTrack>(firstSource);
        activeTrack->SetBlending(true);
        activeTrack->Rewind();
        activeTrack->Play(startTime);

        std::cout << "[Sequence] Started sequence '" << name << "' with track: " << firstSource->filename << std::endl;

        // Gapless Preload Step: Prepare the upcoming source ahead of time
        PreloadNextTrack();
    }
    else
    {
        std::cerr << "[Sequence Error] Initial sequence video not found in loaded resources: " << items[currentIdx]->filename << std::endl;
        isPlaying = false;
    }
}

void Sequence::Stop()
{
    isPlaying = false;
    if (activeTrack) activeTrack->StartForcedFadeOut();
    if (standbyTrack) standbyTrack->Rewind();
    activeTrack.reset();
    standbyTrack.reset();
}

void Sequence::PreloadNextTrack()
{
    if (!isPlaying) return;

    size_t nextIdx = currentIdx + 1;
    if (nextIdx >= items.size())
    {
        if (isLoopedSequence) nextIdx = 0;
        else
        {
            standbyTrack.reset(); // No upcoming file to stage
            return;
        }
    }

    AppState& state = appInterface->GetAppState();
    VideoSource* nextSource = nullptr;

    for (auto source : state.sources)
    {
        if (source->filename == items[nextIdx]->filename)
        {
            nextSource = source;
            break;
        }
    }

    if (nextSource)
    {
        nextSource->fadeInDuration = items[nextIdx]->fadeInDuration;
        nextSource->fadeOutDuration = items[nextIdx]->fadeOutDuration;
        nextSource->looped = items[nextIdx]->looped;

        standbyTrack = std::make_unique<VideoTrack>(nextSource);
        standbyTrack->SetBlending(true);
        standbyTrack->Rewind();

        // Core Optimization: Decode and grab the initial keyframe instantly into the texture buffer
        // to bypass file seeking latency right as the transition takes place.
        ID3D11DeviceContext* context = state.renderer->GetContext();
        nextSource->GetNextFrame(context);

        std::cout << "[Sequence] Preloaded standby tracking asset: " << nextSource->filename << std::endl;
    }
}

void Sequence::AdvanceSequence()
{
    currentIdx++;
    if (currentIdx >= items.size())
    {
        if (isLoopedSequence)
        {
            currentIdx = 0;
        }
        else
        {
            std::cout << "[Sequence] Finished playing sequence chain safely." << std::endl;
            isPlaying = false;
            activeTrack.reset();
            return;
        }
    }

    if (standbyTrack)
    {
        // Smooth Handoff: Bring pre-warmed standby track online instantly
        activeTrack = std::move(standbyTrack);
        activeTrack->Play(GetTimeStd());
        std::cout << "[Sequence] Transitioned to next active slot index: " << currentIdx << " (" << items[currentIdx]->filename << ")" << std::endl;

        // Stage the following link down the tracklist chain
        PreloadNextTrack();
    }
    else
    {
        // Fallback context in case preloading failed or files went missing mid-sequence
        isPlaying = false;
        activeTrack.reset();
    }
}

void Sequence::UpdateFrame(ID3D11DeviceContext* context)
{
    if (!isPlaying || !activeTrack) return;

    // Advance the current active sequence track through our non-blocking update phase
    activeTrack->UpdateFrame(context);

    // Swap playlist components instantly if the active asset completes playback
    if (!activeTrack->IsActive())
    {
        AdvanceSequence();
    }
}

void Sequence::UpdateAndRender(IRenderer* renderer, DXShader* shader, float winW, float winH)
{
    if (!isPlaying || !activeTrack)
        return;

	activeTrack->Render(renderer, shader, winW, winH);
}