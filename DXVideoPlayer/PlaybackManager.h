#pragma once
#include <windows.h>
#include <memory>
#include <queue>
#include <mutex>
#include <string>
#include "customtypes.h"
#include "VideoTrack.h"
#include <functional>

class IApp;
class IRenderer;
class DXShader;
class Sequence;
struct ID3D11DeviceContext;

class PlaybackManager
{
private:
	IApp* appInterface;
	IRenderer* renderer;
	DXShader* videoShader;

	std::queue<DeferredCommand> commandQueue;
	std::mutex queueMutex;

	bool hasPendingBackgroundCmd = false;
	DeferredCommand pendingBackgroundCmd;

	bool hasPendingForegroundCmd = false;
	DeferredCommand pendingForegroundCmd;

	bool hasPendingSequenceCmd = false;
	DeferredCommand pendingSequenceCmd;

	bool hasPendingCoverCmd = false;
	DeferredCommand pendingCoverCmd;

	bool hasPendingChoreographyCmd = false;
	DeferredCommand pendingChoreographyCmd;

	int lastChoreoID = -1;

	bool forceCoverOnExitActive = false;
	bool coverStopPending = false;
	float coverStopPendingFade = 0.0f;
	std::function<void()> onTransitionCompleteCallback = nullptr;

public:
	std::unique_ptr<VideoTrack> backgroundTrack;
	std::unique_ptr<VideoTrack> foregroundTrack;
	std::unique_ptr<VideoTrack> coverTrack;

	bool backgroundActive = false;
	bool foregroundActive = false;
	bool coverActive = false;
	Sequence* activeSequence = nullptr;

public:
	PlaybackManager(IApp* appInterface, DXShader* videoShader);
	~PlaybackManager() = default;

	void InitializeVideoTracks();
	
	//PHASE 1 Methods (non-blocking decoding, texture update and layer state management)
	void DecodeVideoFrameTextures(ID3D11DeviceContext* context);
	void UpdateLayers(ID3D11DeviceContext* context);

	void ResetForegroundLayer();
	void AdvanceSequence();
	void ResetCoverLayer();

	void HandlePendingBackgroundCmd(AppState& state);
	void HandleBackgroundEvents();
	void HandlePendingForegroundCmd(AppState& state);
	void HandleSequenceShutdown();
	void HandlePendingCoverCmd(AppState& state);
	void HandlePendingSequenceCmd();
	void PlayTrackOnLayerIndex(int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd = nullptr);
	void PlayTrackOnLayer(const std::string& videoName, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd = nullptr);
	void PlaySequenceItem(DeferredCommand& cmd);

	//PHASE 2 Methods (Direct3D rendering)
	void RenderLayers(float winW, float winH);
	void ForceStopForegroundLayers(float duration);
	void ForceStopBackgroundLayer(float duration);

	void EnqueueNetworkCommand(const DeferredCommand& cmd);
	void ProcessDeferredCommands();

	void PlayChoreography(const std::string& filename, float fgFadeOut, float fadeIn, float fadeOut, int idVal, bool loopVid, bool forceCoverOnExit = false);
	void TransitionTo(float targetPos, std::function<void()> onComplete, float fadeIn, float fadeOut, int idVal, float fgFadeOut);

	void ShowBgLastFrame(const std::string& filename, int idVal);
	void HandleCoverFadeDeferral();
};

