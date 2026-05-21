#include "VideoTrack.h"
#include "VideoSource.h"
#include "IRenderer.h"
#include "utils.h"

VideoTrack::VideoTrack(bool looped, float fadeIn, float fadeOut) 
	: videoSource(std::make_unique<VideoSource>())
{
	videoSource->looped = looped;
	videoSource->fadeInDuration = fadeIn;
	videoSource->fadeOutDuration = fadeOut;
}

VideoTrack::~VideoTrack() = default;

bool VideoTrack::Initialize(const std::string& path, ID3D11Device* device, ID3D11DeviceContext* context)
{
    return videoSource->OpenFile(path, device, context);
}

void VideoTrack::Play(double startTime)
{
    isActive = true;
    videoSource->Play(startTime);
}

void VideoTrack::Rewind()
{
    videoSource->Rewind();
}

void VideoTrack::Render(IRenderer* renderer, DXShader* shader, float winW, float winH)
{
    // If the track isn't active, don't waste any execution time
    if (!isActive) return;

    ID3D11DeviceContext* context = renderer->GetContext();

    // 1. Advance the video decoding context (runs on raw packet cadence)
    if (!videoSource->GetNextFrame(context))
    {
        // If GetNextFrame returns false, the video hit the end (and looped is false)
        isActive = false;
        return;
    }

    // 2. Compute alpha state at full engine loop cadence (60 FPS)
    videoSource->ComputeAlpha();

    // 3. Command the renderer to draw this specific track
    renderer->DrawVideo(videoSource.get(), shader, shouldBlend, winW, winH);
}