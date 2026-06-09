#include "PlaybackManager.h"
#include <iostream>
#include "App.h"
#include "utils.h"
#include "Sequence.h"
#include "DXRenderer.h"
#include "DXShader.h"

PlaybackManager::PlaybackManager(IApp* appInterface, DXShader* videoShader) 
	: appInterface(appInterface), renderer(appInterface->GetAppState().renderer), videoShader(videoShader)
{ }

/*
* Initializes the video tracks for background, foreground, and cover layers based on the video sources loaded in the AppState.
*/
void PlaybackManager::InitializeVideoTracks()
{
	//Retrieves the current application state from the IApp interface, which contains the loaded video sources and sequences
	AppState& state = appInterface->GetAppState();

	//Initializes the video tracks for background, foreground, and cover layers using the first three video sources from the AppState.
	backgroundTrack = std::make_unique<VideoTrack>(state.sources[0]);
	foregroundTrack = std::make_unique<VideoTrack>(state.sources[1]);
	coverTrack = std::make_unique<VideoTrack>(state.sources[2]);

	//Enable blending for the foreground and cover tracks to allow for proper alpha compositing during fade-in and fade-out transitions, 
	//while keeping blending disabled for the background track since it will always be fully opaque.
	backgroundTrack->SetBlending(true);
	foregroundTrack->SetBlending(true);
	coverTrack->SetBlending(true);

	backgroundActive = true;

	//Starts the playback of the background track immediately without any fade-in effect, using the current time as the starting point for synchronization.
	backgroundTrack->Play(GetTimeStd());

	//Sets the first sequence in the AppState as the active sequence if there are any sequences loaded, allowing it to be triggered later when a PlaySequence command is received.
	if (!state.sequences.empty())
	{
		activeSequence = state.sequences[0];
	}
}

/*
* Computes:
*	1.The next video frame for the active tracks (background always, foreground and cover if active)
*	2.The dynamic alpha values for fade-in and fade-out effects
*	3.Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
*/
void PlaybackManager::DecodeVideoFrameTextures(ID3D11DeviceContext* context)
{
	//1.Background video 
	if (backgroundTrack)
	{
		backgroundTrack->UpdateFrame(context);
	}

	//2.Foreground video
	if (foregroundActive && foregroundTrack)
	{
		foregroundTrack->UpdateFrame(context);
	}

	//3.Cover video
	if (coverActive && coverTrack)
	{
		coverTrack->UpdateFrame(context);
	}
}

/*
===========================================================================================================================
PHASE 1: NON-BLOCKING DECODING, TEXTURE UPDATE AND LAYER STATE MANAGEMENT
In PHASE 1:
	-Computes the next video frame for the active tracks (background always, foreground if active)
	-Computes the dynamic alpha values for fade-in and fade-out effects
	-Copy the decoded video frame data into Direct3D textures for rendering in PHASE 2
	-Manages the active states of the foreground and cover layers, automatically deactivating them when they have finished playing or completed their fade-out transitions.
	-Manages the automatic advancement of sequence items when a sequence is active

*/
void PlaybackManager::UpdateLayers(ID3D11DeviceContext* context)
{
	AppState& state = appInterface->GetAppState();

	//Decodes the next video frame for the active tracks (background always, foreground and cover if active), updates alpha values and updates their textures for rendering in PHASE 2
	DecodeVideoFrameTextures(context);

	HandleBackgroundEvents();
	
	HandlePendingBackgroundCmd(state);

	//Manages the active states of the foreground, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetForegroundLayer();

	//Advances to the next sequence item when a sequence is active
	AdvanceSequence();

	//Manages the active states of the cover, automatically deactivating it when it has finished playing or completed its fade-out transitions.
	ResetCoverLayer();

	//Checks if there is a pending command to play a new cover video and if the cover layer is not active, and if so, processes the pending command to start playing the new cover video.
	HandlePendingCoverCmd(state);

	//Checks if there is a pending command to play a new foreground video and if the foreground layer is not active, and if so, processes the pending command to start playing the new foreground video.
	HandlePendingForegroundCmd(state);

	//Checks if there is a pending command to stop the current sequence and if the foreground layer is not active, and if so, stops the active sequence to prevent it from advancing to the next item in the sequence.
	HandleSequenceShutdown();

	//Checks if there is a pending command to play a new sequence and if the foreground layer is not active, and if so, processes the pending command to start playing the new sequence.
	HandlePendingSequenceCmd();
}

/*
* Checks if the foreground track is active but has finished playing or completed its fade-out transition, and if so, resets the foreground layer by deactivating it. 
* This ensures that the foreground layer is automatically turned off when it is no longer visible, allowing for proper management of layer states without blocking the main thread.
*/
void PlaybackManager::ResetForegroundLayer()
{
	if (foregroundActive && foregroundTrack && !foregroundTrack->IsActive())
	{
		//Turns off the foreground layer
		foregroundActive = false;
	}
}

/*
* Triggered precisely when the foreground track has finished playing (if active), provided no other override network commands are pending and there is an active sequence. 
*/
void PlaybackManager::AdvanceSequence()
{
	if (!foregroundActive && !hasPendingForegroundCmd && !hasPendingSequenceCmd && activeSequence && activeSequence->isActive)
	{
		activeSequence->AdvanceSequence();
	}
}

/*
* Checks if the cover track is active but has finished playing or completed its fade-out transition, and if so, resets the cover layer by deactivating it.
*/
void PlaybackManager::ResetCoverLayer()
{
	if (coverActive && coverTrack && !coverTrack->IsActive())
	{
		coverActive = false;
	}
}

void PlaybackManager::HandlePendingBackgroundCmd(AppState& state)
{
	if (backgroundActive && backgroundTrack && !backgroundTrack->IsActive())
	{
		backgroundActive = false;
	}

	if (backgroundTrack && !backgroundTrack->IsActive())
	{
		if (hasPendingBackgroundCmd)
		{
			hasPendingBackgroundCmd = false;
			int matchIdx = FindVideoSourceIndexByFilename(pendingBackgroundCmd.filename, state.sources);
			if (matchIdx != -1)
			{
				PlayTrackOnLayer(matchIdx, backgroundTrack, backgroundActive, LayerType::Background, &pendingBackgroundCmd);
			}
		}
	}
}

void PlaybackManager::HandleBackgroundEvents()
{
	if (!backgroundTrack || !backgroundTrack->IsActive()) 
		return;

	VideoSource* bgSource = backgroundTrack->GetSource();
	if (!bgSource) 
		return;

	double backgroundPTS = bgSource->internalPTS;
	AppState& state = appInterface->GetAppState();

	// -------------------------------------------------------------
	// PHASE 1: EVALUATE TRIGGER CONDITIONS FOR SCHEDULED EVENTS
	// -------------------------------------------------------------

	//Loops through all the events loaded from -events.txt file 
	for (auto& evt : bgSource->events)
	{
		//Ensures that the event hasn't been fired yet and that the background playhead has reached or passed the event start time
		if (!evt.triggered && backgroundPTS >= evt.startTime)
		{
			//Provies a small grace period of 1 second after the event start time preventing missed triggers due to timing discrepancies
			if (backgroundPTS <= (evt.startTime + 1.0))
			{
				//Marks the event as triggered to prevent it from firing again in subsequent iterations of the loop
				evt.triggered = true;

				//Check if the event is a sequence
				bool isSequence = (evt.filename.find(".txt") != std::string::npos);

				//SEQUENCE
				if (isSequence)
				{
					//Creates a local DeferredCommand to play the sequence
					DeferredCommand cmd;
					cmd.type = NetworkCommandType::PlaySequence;
					cmd.filename = evt.filename;
					cmd.fadeInDuration = evt.fadeInDuration;
					cmd.fadeOutDuration = evt.fadeOutDuration;
					cmd.looped = false;

					std::cout << "[Timeline Event] Firing automated sequence file: " << evt.filename
						<< " at background playhead pos: " << backgroundPTS << "s" << std::endl;

					EnqueueNetworkCommand(cmd);
				}
				//VIDEO
				else
				{
					//Find the index of the video source matching the event filename
					int matchIdx = FindVideoSourceIndexByFilename(evt.filename, state.sources);
					if (matchIdx != -1)
					{
						//Creates a local DeferredCommand to play the foreground video
						DeferredCommand cmd;
						cmd.type = NetworkCommandType::PlayForeground;
						cmd.filename = evt.filename;
						cmd.fadeInDuration = evt.fadeInDuration;
						cmd.fadeOutDuration = evt.fadeOutDuration;
						cmd.looped = false;

						std::cout << "[Timeline Event] Firing foreground layer item: " << evt.filename
							<< " at playhead pos: " << backgroundPTS << "s" << std::endl;

						PlayTrackOnLayer(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
					}
				}
			}
		}
	}

	// -------------------------------------------------------------
	// PHASE 2: STOPS VIDEOS AND SEQUENCES THAT HAVE REACHED THEIR DURATION THRESHOLDS
	// -------------------------------------------------------------

	//STOPS VIDEO
	if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
	{
		VideoSource* fgSource = foregroundTrack->GetSource();
		if (fgSource)
		{
			for (const auto& evt : bgSource->events)
			{
				if (evt.filename == fgSource->filename && evt.triggered)
				{
					if (evt.duration <= 0.0f)
						continue;

					if (fgSource->internalPTS >= evt.duration && !fgSource->isFadingOut)
					{
						std::cout << "[Timeline Event] Duration reached for " << evt.filename
							<< ". Injecting automatic fade out." << std::endl;
						foregroundTrack->StartForcedFadeOut(evt.fadeOutDuration);
					}
				}
			}
		}
	}

	//STOPS SEQUENCES

	//Check if a sequence is alive and active
	if (activeSequence && activeSequence->isActive)
	{
		//Loops through all the events in from the -events.txt file
		for (const auto& evt : bgSource->events)
		{
			//Checks if the event is associated with the active sequence and has been triggered
			if (evt.filename == activeSequence->name && evt.triggered)
			{
				//If the event has a duration of <= 0, it means it has no duration so it can run indefinitely
				//This is necessary is sequence has an element set to loop
				if (evt.duration <= 0.0f)
					continue;

				//Calculates how many seconds are passed since the sequence has started playing
				double elapsedSequenceTime = backgroundPTS - evt.startTime;

				//If it's more >= than the event duration...
				if (elapsedSequenceTime >= evt.duration)
				{
					std::cout << "[Timeline Event] Duration threshold reached for active sequence: " << activeSequence->name
						<< ". Killing running foreground channels." << std::endl;

					//Halts the active sequence advancement, deactivates sequence flags and triggers a clean fade out (if set) on current 
					//sequence video that is playing
					ForceStopForegroundLayers(evt.fadeOutDuration);
				}
			}
		}
	}
}

/*
* Example:
* VIDEO A is playing in the foreground, a new command to play VIDEO B is received:
*	1.Video A starts a forced fade-out 
*	2.While Video A is fading out, Video B must wait in line
*	3.Once Video A finsihes (foregroundActive = false) it stops, then Video B instantly start plaing (usually with a fading-in)

* Checks if a previous foreground video has just finished (foregroundActive = false) and if there is a pending command to play a new foreground video. 
* If both conditions are met, it processes the pending command by finding the corresponding video source index and instantiating the foreground track with the new video.
*/
void PlaybackManager::HandlePendingForegroundCmd(AppState& state)
{
	if (!foregroundActive && hasPendingForegroundCmd)
	{
		hasPendingForegroundCmd = false;

		int matchIdx = FindVideoSourceIndexByFilename(pendingForegroundCmd.filename, state.sources);
		if (matchIdx != -1)
		{
			// Instantly instantiate Video B and reset its properties ready to be played the next time we enter the Main Loop.
			// The actual playback of Video B will be triggered in the next iteration of the Run loop once the current foreground video has fully finished and foregroundActive becomes false.
			PlayTrackOnLayer(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &pendingForegroundCmd);
		}
	}
}

/*
* Checks if there is a pending command to stop the current sequence and if the foreground layer is not active. 
* If both conditions are met, it stops the active sequence to prevent it from advancing to the next item in the sequence when we enter the Main Loop.
*/
void PlaybackManager::HandleSequenceShutdown()
{
	if (!foregroundActive && hasPendingForegroundCmd)
	{
		if (activeSequence && activeSequence->isActive)
		{
			// This does not stop the current sequence video immediately, it just stops the sequence preventing it to advance to the next item we enter the Main Loop.
			// The fade out of the current sequence video is called in ForceStopForegroundLayers() when a new foreground video command is received while a sequence is active.
			activeSequence->Stop();
		}
	}
}

/*
* Checks if a previous cover video has just finished (coverActive = false) and if there is a pending command to play a new cover video.
*/
void PlaybackManager::HandlePendingCoverCmd(AppState& state)
{
	if (!coverActive && hasPendingCoverCmd)
	{
		hasPendingCoverCmd = false;
		int matchIdx = FindVideoSourceIndexByFilename(pendingCoverCmd.filename, state.sources);
		if (matchIdx != -1)
		{
			PlayTrackOnLayer(matchIdx, coverTrack, coverActive, LayerType::Cover, &pendingCoverCmd);
		}
	}
}

/*
* Checks if a previous foreground video has just finished (foregroundActive = false) and if there is a pending command to play a new sequence.
*/
void PlaybackManager::HandlePendingSequenceCmd()
{
	if (!foregroundActive && hasPendingSequenceCmd)
	{
		hasPendingSequenceCmd = false;
		std::cerr << "[Main Thread] Foreground track cleared perfectly. Booting pending sequence now." << std::endl;

		Sequence* targetSequence = nullptr;	
		for (auto seq : appInterface->GetAppState().sequences)
		{
			if (seq->name == pendingSequenceCmd.filename)
			{
				targetSequence = seq;
				break;
			}
		}

		if (targetSequence)
		{
			activeSequence = targetSequence;

			if (!activeSequence->items.empty())
			{
				activeSequence->items[0].fadeInDuration = pendingSequenceCmd.fadeInDuration;
				activeSequence->items[activeSequence->items.size() - 1].fadeOutDuration = pendingSequenceCmd.fadeOutDuration;
			}

			activeSequence->Stop(); // Reset the sequence state to ensure it starts from the beginning
			activeSequence->Play(pendingSequenceCmd.looped);
		}
		else
		{
			std::cerr << "[Main Thread] Failed to boot pending sequence. File not matched: " << pendingSequenceCmd.filename << std::endl;
		}
	}
}

void PlaybackManager::PlayTrackOnLayer(int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
{
	AppState& state = appInterface->GetAppState();

	if (videoSourceIdx < 0 || videoSourceIdx >= static_cast<int>(state.sources.size()))
	{
		std::cerr << "Invalid video source index for " << LayerTypeToStr(layerType) << ": " << videoSourceIdx << std::endl;
		return;
	}

	std::cout << "[Main Thread] Swapping " << LayerTypeToStr(layerType) << " layer video to index: "
		<< videoSourceIdx << " (" << state.sources[videoSourceIdx]->filename << ")" << std::endl;

	//Update the video source properties based on the command parameters if provided
	if (cmd)
	{
		state.sources[videoSourceIdx]->fadeInDuration = cmd->fadeInDuration;
		state.sources[videoSourceIdx]->fadeOutDuration = cmd->fadeOutDuration;
		state.sources[videoSourceIdx]->looped = cmd->looped;
	}

	//Set initial alpha to 0 for fade-in effect
	state.sources[videoSourceIdx]->alpha = 0.0f;

	// Dynamically update the passed track unique_ptr pointer structure
	targetTrack = std::make_unique<VideoTrack>(state.sources[videoSourceIdx]);
	targetTrack->SetBlending(true);
	targetTrack->Rewind();
	targetTrack->Play(GetTimeStd());

	// Force wrapper synchronization status explicitly
	targetTrack->SetActive(true);

	// Bootstrap first frame mapping context
	ID3D11DeviceContext* ctx = renderer->GetContext();
	state.sources[videoSourceIdx]->GetNextFrame(ctx);

	if (state.sources[videoSourceIdx]->isFadingIn)
	{
		state.sources[videoSourceIdx]->ComputeFadeIn();
	}
	else if (state.sources[videoSourceIdx]->fadeInDuration > 0.0f)
	{
		state.sources[videoSourceIdx]->alpha = 0.0f;
	}

	// Toggle the specific layer visibility state flag on
	targetActiveFlag = true;
}

void PlaybackManager::PlaySequenceItem(DeferredCommand& cmd)
{
	AppState& state = appInterface->GetAppState();
	int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);
	if (matchIdx != -1)
	{
		PlayTrackOnLayer(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
	}
}

/*
===========================================================================================================================
PHASE 2: DIRECT3D RENDERING STAGE 
In PHASE 2:
	-Finally, renders the decoded textures for background layer, foreground and cover layer (if active)

*/
void PlaybackManager::RenderLayers(float winW, float winH)
{
	//Render background video texture
	if (backgroundTrack)
		backgroundTrack->Render(renderer, videoShader, winW, winH);

	//Render foreground video texture
	if (foregroundActive)
	{
		if (!foregroundTrack->IsActive())
		{
			foregroundActive = false;
		}
		else
		{
			foregroundTrack->Render(renderer, videoShader, winW, winH);	
		}
	}

	//Render cover video texture
	if (coverActive)
	{
		if (!coverTrack->IsActive())
		{
			coverActive = false;
		}
		else
		{
			coverTrack->Render(renderer, videoShader, winW, winH);
		}
	}
}

/*
* Triggers an immediate forced fade-out of the foreground and cover videos if they are active, and resets all pending command flags.
*/
void PlaybackManager::ForceStopForegroundLayers(float duration)
{
	hasPendingForegroundCmd = false;
	hasPendingSequenceCmd = false;
	hasPendingCoverCmd = false;

	if (activeSequence && activeSequence->isActive)
	{
		activeSequence->Stop();
	}

	if (foregroundActive && foregroundTrack)
	{
		foregroundTrack->StartForcedFadeOut(duration);
	}

	if (coverActive && coverTrack)
	{
		coverTrack->StartForcedFadeOut(duration);
	}
}

void PlaybackManager::ForceStopBackgroundLayer(float duration)
{
	// Clear any pending commands queued to start a new background video
	hasPendingBackgroundCmd = false;

	// If the background track is actively rendering, command its underlying source to start a forced fade-out
	if (backgroundActive && backgroundTrack)
	{
		backgroundTrack->StartForcedFadeOut(duration);
	}
}

void PlaybackManager::EnqueueNetworkCommand(const DeferredCommand& cmd)
{
	std::lock_guard<std::mutex> lock(queueMutex);
	commandQueue.push(cmd);
}

void PlaybackManager::ProcessDeferredCommands()
{
	std::queue<DeferredCommand> localQueue;
	AppState& state = appInterface->GetAppState();

	{
		std::lock_guard<std::mutex> lock(queueMutex);
		if (commandQueue.empty()) return;
		std::swap(commandQueue, localQueue);
	}

	while (!localQueue.empty())
	{
		DeferredCommand cmd = localQueue.front();
		localQueue.pop();

		switch (cmd.type)
		{
			case NetworkCommandType::Stop:
			{
				std::cout << ">>> [PlaybackManager] Processing deferred 'stop' action." << std::endl;
				ForceStopBackgroundLayer(1.0f);
				ForceStopForegroundLayers(1.0f);
				break;
			}

			case NetworkCommandType::PlayBackground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_background': " << cmd.filename << std::endl;
				int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);

				if (matchIdx != -1)
				{
					if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
					{
						std::cout << "[PlaybackManager] Background event video active on foreground layer. Forcing fade out." << std::endl;
						foregroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}

					if (activeSequence && activeSequence->isActive)
					{
						activeSequence->Stop();
					}

					if (backgroundTrack && backgroundTrack->IsActive())
					{
						pendingBackgroundCmd = cmd;
						hasPendingBackgroundCmd = true;

						backgroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						PlayTrackOnLayer(matchIdx, backgroundTrack, backgroundActive, LayerType::Background, &cmd);
					}
				}

				break;
			}

			case NetworkCommandType::PlayForeground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_foreground': " << cmd.filename << std::endl;
				int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);

				if (matchIdx != -1)
				{
					if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
					{
						pendingForegroundCmd = cmd;
						hasPendingForegroundCmd = true;

						if (activeSequence && activeSequence->isActive)
						{
							activeSequence->Stop();
						}
						foregroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						if (activeSequence && activeSequence->isActive)
						{
							activeSequence->Stop();
						}
						PlayTrackOnLayer(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
					}
				}
				break;
			}
			case NetworkCommandType::PlaySequence:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_sequence': " << cmd.filename << std::endl;

				Sequence* targetSequence = nullptr;	
				for (auto seq : state.sequences)
				{
					if (seq->name == cmd.filename)
					{
						targetSequence = seq;
						break;
					}
				}

				if (!targetSequence)
				{
					std::cerr << "[Main Thread] Error: Requested sequence file not found: " << cmd.filename << std::endl;
					break;
				}

				if (foregroundActive && foregroundTrack && foregroundTrack->IsActive())
				{
					std::cout << "[PlaybackManager] Foreground busy. Buffering sequence, forcing fade out." << std::endl;

					pendingSequenceCmd = cmd;
					hasPendingSequenceCmd = true;

					if (activeSequence && activeSequence->isActive)
					{
						activeSequence->Stop();
					}
					foregroundTrack->StartForcedFadeOut(cmd.fadeOutDuration);
				}
				else
				{
					hasPendingSequenceCmd = false;
					activeSequence = targetSequence;

					if (activeSequence && !activeSequence->items.empty())
					{
						activeSequence->items[0].fadeInDuration = cmd.fadeInDuration;
						activeSequence->items[activeSequence->items.size() - 1].fadeOutDuration = cmd.fadeOutDuration;
						activeSequence->Stop();
						activeSequence->Play(cmd.looped);
					}
				}
				break;
			}
			case NetworkCommandType::PlayCover:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_cover': " << cmd.filename << std::endl;
				int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);
				if (matchIdx != -1)
				{
					if (coverActive && coverTrack && coverTrack->IsActive())
					{
						pendingCoverCmd = cmd;
						hasPendingCoverCmd = true;
						coverTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						PlayTrackOnLayer(matchIdx, coverTrack, coverActive, LayerType::Cover, &cmd);
					}
				}
				break;
			}

			case NetworkCommandType::HideCover:
			{
				std::cout << ">>> [PlaybackManager] Processing deferred 'hide_cover' action." << std::endl;

				// Clear any pending commands queued to start a new cover video
				hasPendingCoverCmd = false;

				// If the cover track is actively rendering, command its underlying source to start a forced fade-out
				if (coverActive && coverTrack)
				{
					coverTrack->StartForcedFadeOut(1.0f);
				}
				break;
			}

			case NetworkCommandType::TransitionTo:
			{
				std::cout << "[PlaybackManager] Processing deferred 'transition_to' action: " << cmd.filename << std::endl;
				Config config = appInterface->GetConfig();
				stopping = true;
				transitionMode = true;
				std::string filename = GetNameFromFile(cmd.filename);
				auto targetPosition = config.target_positions[filename];
				transitionTargetPosition = targetPosition.position;
			}
		}
	}
}