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
        videoSource = nullptr;
	}
}

void VideoTrack::UpdateFrame(ID3D11DeviceContext* context)
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

    // Advance the video decoding context (now safe and completely non-blocking)
    if (!videoSource->GetNextFrame(context))
    {
        // FIX: Only terminate the track if it has genuinely advanced past 
        // the beginning, preventing initial cold-start seek misses from killing it.
        if (videoSource->internalPTS > 0.0 || videoSource->lastPTS > 0.0)
        {
            isActive = false;
            state = VideoTrackState::Stopped;
            if (prevState != state)
            {
                std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
                prevState = state;
            }
            return;
        }
    }

    // Compute alpha state updates at full loop cadence
    videoSource->ComputeFadeIn();

    bool fadeOutFinished = false;
    if (videoSource->isForcedFadingOut)
    {
        // Execute dedicated forced-stop tracking configurations
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

    // Sync state tracking variables smoothly
    if (videoSource->isFadingIn)
        state = VideoTrackState::FadingIn;
    else if (videoSource->isForcedFadingOut || state == VideoTrackState::FadingOut)
        state = VideoTrackState::FadingOut; // Anchored during natural/forced fades
    else if (state == VideoTrackState::FadingIn || state == VideoTrackState::FadingOut)
        state = VideoTrackState::Playing;

    if (prevState != state)
    {
        std::cout << "Track state changed: " << VideoTrackStateToStr(prevState) << " -> " << VideoTrackStateToStr(state) << std::endl;
        prevState = state;
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
    if (!isActive)
        return;

    // 3. Command the renderer to draw this specific track
    renderer->DrawVideo(videoSource, shader, shouldBlend, winW, winH);
}