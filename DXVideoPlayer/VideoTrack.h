#pragma once
#include <string>
#include <memory>
#include "customtypes.h"

class VideoSource;
class DXShader;
class IRenderer;
struct ID3D11Device;
struct ID3D11DeviceContext;

class VideoTrack
{
private:
    VideoSource* videoSource;
    bool isActive = false;
    bool shouldBlend = false;

public:
	VideoTrackState state = VideoTrackState::Stopped;
	VideoTrackState prevState = VideoTrackState::Stopped;

public:
    VideoTrack(VideoSource* videoSource);
    ~VideoTrack();

	void UpdateFrame(ID3D11DeviceContext* context);

    // Core lifecycle call executed inside App::Run every loop tick (60 FPS)
    void Render(IRenderer* renderer, DXShader* shader, float winW, float winH);

    // Control API
    void Play(double startTime);
    void Rewind();
	void StartFadeIn(float fadeInTime = -1.0f);
    void StartForcedFadeOut();

    // Getters / Setters
    void SetActive(bool active) { isActive = active; }
    bool IsActive() const { return isActive; }
    void SetBlending(bool blend) { shouldBlend = blend; }

    VideoSource* GetSource() { return videoSource; }
};

