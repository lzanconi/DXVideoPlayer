#pragma once
#include <string>
#include <memory>
#include "customtypes.h"

class VideoSource;
class DXShader;
class IRenderer;
struct ID3D11Device;
struct ID3D11DeviceContext;

/*
* A wrapper and a controller for an individual video playback layer. 
* It manages the lifecycle, state transitions, and rendering of a specific VideoSource (which interfaces directly with FFmpeg and Direct3D 11).
* 
* The application loop split-schedules the playback lifecycle into two distinct phases inside App::Run() to keep operations entirely smooth:
* 
* PHASE 1: UpdateFrame() -> Decodes the next frame from FFmpeg and updates alpha without any blocking calls
* 
* PHASE 2: Render() -> Submits the draw call to the Direct3D 11 renderer to composite the video texture onto the screen with the appropriate blending state
*/
class VideoTrack
{
private:
    //A pointer to the underlying asset containing the FFmpeg decoding contexts and GPU texture
    VideoSource* videoSource;
	//Whether this track is currently active and should be updated and rendered
    bool isActive = false;
	//Whether this track should be rendered with alpha blending enabled (true for foreground, false for background)
    bool shouldBlend = false;

public:
	//State management for the track's lifecycle, used to control fade-in, fade-out, and playback status
	VideoTrackState state = VideoTrackState::Stopped;
	VideoTrackState prevState = VideoTrackState::Stopped;

public:
    VideoTrack(VideoSource* videoSource);
    ~VideoTrack();

	//Decodeds frames and updates alpha 
	void UpdateFrame(ID3D11DeviceContext* context);

    //Draw the decoded frame texture to screen
    void Render(IRenderer* renderer, DXShader* shader, float winW, float winH);

    // Control API
    void Play(double startTime);
    void Rewind();
	void StartFadeIn(float fadeInTime = -1.0f);
    void StartForcedFadeOut(float duration);

    // Getters / Setters
    void SetActive(bool active) { isActive = active; }
    bool IsActive() const { return isActive; }
    void SetBlending(bool blend) { shouldBlend = blend; }
    VideoSource* GetSource() { return videoSource; }
};

