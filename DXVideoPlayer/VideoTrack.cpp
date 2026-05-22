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

    if (videoSource->fadeInDuration > 0.0f)
    {
		state = VideoTrackState::FadingIn;
        StartFadeIn();
    }
    else
    {
        state = VideoTrackState::Playing;
    }

    prevState = VideoTrackState::Stopped;
}

void VideoTrack::Rewind()
{
    videoSource->Rewind();
}

void VideoTrack::StartFadeIn(float fadeInTime)
{
	state = VideoTrackState::FadingIn;
    videoSource->StartFadeIn(fadeInTime);
}

void VideoTrack::StartForcedFadeOut()
{
    if (state == VideoTrackState::FadingOut)
        return;

    if (!isActive || !videoSource) 
        return;

    state = VideoTrackState::FadingOut;
    videoSource->StartForcedFadeOut();
}

void VideoTrack::Render(IRenderer* renderer, DXShader* shader, float winW, float winH)
{
    // If the track isn't active, don't waste any execution time
    if (!isActive)
    {
        if (state != VideoTrackState::Stopped)
        {
            state = VideoTrackState::Stopped;
            if (prevState != state)
            {
                std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
                prevState = state;
            }
		}
        return;
    }

    ID3D11DeviceContext* context = renderer->GetContext();

    // 1. Advance the video decoding context (runs on raw packet cadence)
    if (!videoSource->GetNextFrame(context))
    {
        // If GetNextFrame returns false, the video hit the end (and looped is false)
        isActive = false;
		state = VideoTrackState::Stopped;
        if (prevState != state)
        {
            std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
            prevState = state;
        }
        return;
    }

    // 2. Compute alpha state at full engine loop cadence (60 FPS)
	videoSource->ComputeFadeIn();

    bool fadeOutFinished = false;
    if (videoSource->isForcedFadingOut)
    {
        // Execute dedicated forced-stop tracking tracking calculations
        fadeOutFinished = videoSource->ComputeForcedFadeOut();
    }
    else
    {
        // Fall back to standard, linear asset tracking timeline rules
        fadeOutFinished = videoSource->ComputeNaturalFadeOut();
    }


    if (fadeOutFinished)
    {
        isActive = false;
        state = VideoTrackState::Stopped;
        if (prevState != state)
        {
            std::cout << "Track state changed (Fade Complete): " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
            prevState = state;
        }
        return; // Exit right away to guarantee zero frame ghosting artifacts
    }

    if (videoSource->isFadingIn)
        state = VideoTrackState::FadingIn;
    else if (videoSource->isFadingOut)
        state = VideoTrackState::FadingOut; // This keeps it anchored during forced fades!
    else if (state == VideoTrackState::FadingIn || state == VideoTrackState::FadingOut)
        state = VideoTrackState::Playing;
    
    if (prevState != state)
    {
        std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
        prevState = state;
	}

    // 3. Command the renderer to draw this specific track
    renderer->DrawVideo(videoSource, shader, shouldBlend, winW, winH);
}