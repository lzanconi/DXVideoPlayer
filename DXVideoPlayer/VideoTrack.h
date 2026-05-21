#pragma once
#include <string>
#include <memory>

class VideoSource;
class DXShader;
class IRenderer;
struct ID3D11Device;
struct ID3D11DeviceContext;

class VideoTrack
{
private:
    std::unique_ptr<VideoSource> videoSource;
    bool isActive = false;
    bool shouldBlend = false;

public:
    VideoTrack(bool looped, float fadeIn = 2.0f, float fadeOut = 2.0f);
    ~VideoTrack();

    bool Initialize(const std::string& path, ID3D11Device* device, ID3D11DeviceContext* context);

    // Core lifecycle call executed inside App::Run every loop tick (60 FPS)
    void Render(IRenderer* renderer, DXShader* shader, float winW, float winH);

    // Control API
    void Play(double startTime);
    void Rewind();

    // Getters / Setters
    void SetActive(bool active) { isActive = active; }
    bool IsActive() const { return isActive; }
    void SetBlending(bool blend) { shouldBlend = blend; }

    VideoSource* GetSource() { return videoSource.get(); }
};

