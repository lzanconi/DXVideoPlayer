#include "VideoTrack.h"
#include "VideoSource.h"
#include "IRenderer.h"
#include "utils.h"
#include <iostream>

VideoTrack::VideoTrack(VideoSource* videoSource)
{
	this->videoSource = videoSource;
}

VideoTrack::~VideoTrack()
{
    if (videoSource)
    {
        delete videoSource;
        videoSource = nullptr;
	}
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

void VideoTrack::StartFadeIn(float fadeInTime)
{
    videoSource->StartFadeIn(fadeInTime);
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
	videoSource->ComputeFadeIn();
	videoSource->ComputeFadeOut();

    if (videoSource->isFadingIn)
        state = VideoTrackState::FadingIn;
    else if (videoSource->isFadingOut)
        state = VideoTrackState::FadingOut;
    else if (state == VideoTrackState::FadingIn || state == VideoTrackState::FadingOut)
        state = VideoTrackState::Playing; // Transition to playing after fades complete
    
    if (prevState != state)
    {
        std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
        prevState = state;
	}

    // 3. Command the renderer to draw this specific track
    renderer->DrawVideo(videoSource, shader, shouldBlend, winW, winH);
}