#include "VideoTrack.h"
#include "VideoSource.h"
#include "IRenderer.h"
#include "utils.h"
#include <iostream>
#include "Logger.h"

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

/*
* It transitions a track from a stopped/idle state into an active playback state and configures the initial transparency parameters for rendering.
* "startTime" is the reference clock time at which the video should consider its playback to have started.
*/
void VideoTrack::Play(double startTime)
{
	//Set the track as active to allow UpdateFrame and Render to process it in the main loop
    isActive = true;

    //Pass the start time to the VideoSource, which uses it as a reference point to calculate the current playback position and manage frame decoding and fade timing
    videoSource->Play(startTime);

	//Check if the video source has a fade-in duration specified, and if so, start the fade-in process and set the state to FadingIn...
    if (videoSource->fadeInDuration > 0.0f)
    {
        //Instructs the VideoSource to begin the fade-in progression
        StartFadeIn(videoSource->fadeInDuration);
    }
	//...Otherwise, if there is no fade-in duration, it can immediately set the state to Playing
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

/*
* It initiates the fade-in process for this track, which will cause the alpha to ramp up from 0.0 to 1.0 over the specified duration
* "fadeInTime" allows overriding the default fade-in duration specified in the VideoSource
*/
void VideoTrack::StartFadeIn(float duration)
{
    //Updates VideoTrack state to FadingIn
	state = VideoTrackState::FadingIn;
    //Passes the fadeInTime duration to the VideoSource
    videoSource->StartFadeIn(duration);
}

/*
* Instructs the VideoSource to immediately start fading out from the current alpha level, regardless of the current playback position in the video.
*/
void VideoTrack::StartForcedFadeOut(float duration)
{
	//If the track is already in the process of fading out, it ignores additional fade-out commands to prevent conflicts and ensure a smooth transition
    if (state == VideoTrackState::FadingOut)
        return;

	//If the track is not active or if there is no valid video source, it does not attempt to start a fade-out since there is nothing to fade
    if (!isActive || !videoSource) 
        return;

	//Updates VideoTrack state to FadingOut
    state = VideoTrackState::FadingOut;
	//Instructs the VideoSource to begin the forced fade-out progression
    videoSource->StartForcedFadeOut(duration);
}

void VideoTrack::Looped(bool shouldLoop)
{
    GetSource()->looped = shouldLoop;
}

/*
* PHASE 1: UpdateFrame() -> Decodes the next frame from FFmpeg and updates alpha without any blocking calls
*/
void VideoTrack::UpdateFrame(ID3D11DeviceContext* context)
{
    //Checks if the track is NOT active
    if (!isActive)
    {
        //Ensures the state is set to Stopped
        if (state != VideoTrackState::Stopped)
        {
            state = VideoTrackState::Stopped;
            if (prevState != state)
            {
                //Logs the state change for debugging purposes, showing the previous and current state of the track
                Logger::LogMessage(MESSAGE_TYPE::INFO, "VideoTrack", "UpdateFrame", "TRACK: Track '" + GetSource()->filename + "' state changed " + VideoTrackStateToStr(prevState) + " -> " + VideoTrackStateToStr(state));
                prevState = state;
            }
        }
        return;
    }

    //VIDEO ENDS
    //Check if the video has reached it END
    if (!videoSource->GetNextFrame(context))
    {
        //Ensures that it has decoded at least one frame before stopping the playback 
        if (videoSource->internalPTS > 0.0 || videoSource->lastPTS > 0.0)
        {
            //Reset the state to Stopped and deactivate the track
            isActive = false;
            state = VideoTrackState::Stopped;
            if (prevState != state)
            {
                //Logs the state change for debugging purposes, showing the previous and current state of the track
                Logger::LogMessage(MESSAGE_TYPE::INFO, "VideoTrack", "UpdateFrame", "TRACK: Track '" + GetSource()->filename + "' state changed " + VideoTrackStateToStr(prevState) + " -> " + VideoTrackStateToStr(state));
                prevState = state;
            }
            return;
        }
    }

    //Commands the VideoSource to calculate the linear fade-in progression dynamically moving its alpha value toward 1.0 
    //if it is still within the opening fade-in timeline window. 
    //If videoSource "isFadingIn" flag is not true, ComputeFadeIn() will simply return immediately without modifying the alpha
    videoSource->ComputeFadeIn();

    //Tracks if the fade-out progression has completed
    bool fadeOutFinished = false;

    //FORCED FADE OUT
    //If a command overrode this track (e.g., a network command requested a new foreground video), isForcedFadingOut is flagged...
    if (videoSource->isForcedFadingOut)
    {
        //...and it triggers ComputeForcedFadeOut(), which calculates the fade-out progression starting from the moment the command was received. 
        fadeOutFinished = videoSource->ComputeForcedFadeOut();
    }
    //NATURAL FADE OUT
    else
    {
        //Checks if the video has naturally entered the fade-out phase and if so, calculate the fade-out progression 
        fadeOutFinished = videoSource->ComputeNaturalFadeOut();
    }

    //If forced or natural fade-out has completed, it immediately stops the track and resets the state to Stopped
    if (fadeOutFinished)
    {
        isActive = false;
        state = VideoTrackState::Stopped;
        if (prevState != state)
        {
            //Logs the state change for debugging purposes
            Logger::LogMessage(MESSAGE_TYPE::INFO, "VideoTrack", "UpdateFrame", "TRACK: Track '" + GetSource()->filename + "' state changed " + VideoTrackStateToStr(prevState) + " -> " + VideoTrackStateToStr(state));
            prevState = state;
        }
        //Exit right away to guarantee zero frame ghosting artifacts
        return;
    }

    //STATE MACHINE SYNCHRONIZATION
    //Ensures the VideoTrack state machine stays perfectly synchronized with the underlying VideoSource's fade-in and fade-out flags
    if (videoSource->isFadingIn)
        state = VideoTrackState::FadingIn;
    else if (videoSource->isForcedFadingOut || state == VideoTrackState::FadingOut)
        state = VideoTrackState::FadingOut; // Anchored during natural/forced fades
    else if (state == VideoTrackState::FadingIn || state == VideoTrackState::FadingOut)
        state = VideoTrackState::Playing;

    if (prevState != state)
    {
        //Logs the state change for debugging purposes
        Logger::LogMessage(MESSAGE_TYPE::INFO, "VideoTrack", "UpdateFrame", "TRACK: Track '" + GetSource()->filename + "' state changed " + VideoTrackStateToStr(prevState) + " -> " + VideoTrackStateToStr(state));
        prevState = state;
    }
}

/*
* PHASE 2: Render() -> Submits the draw call to the Direct3D 11 renderer to composite the video texture onto the screen with the appropriate blending state
*/
void VideoTrack::Render(IRenderer* renderer, DXShader* shader, float winW, float winH)
{
    if (!isActive)
        return;

	//Calls the renderer to draw the current video frame texture onto the screen
    renderer->DrawVideo(videoSource, shader, shouldBlend, winW, winH);
}