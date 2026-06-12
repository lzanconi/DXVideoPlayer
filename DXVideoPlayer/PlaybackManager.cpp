#include "PlaybackManager.h"
#include <iostream>
#include "App.h"
#include "utils.h"
#include "Sequence.h"
#include "DXRenderer.h"
#include "DXShader.h"
#include "Logger.h"

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
	std::string autoRunFilename = appInterface->GetConfig().autorun_filename;
	autoRunFilename = GetFilenameFromPath(autoRunFilename);

	//Initializes the video tracks for background, foreground, and cover layers using the first three video sources from the AppState.
	backgroundTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);
	foregroundTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);
	coverTrack = std::make_unique<VideoTrack>(state.sourcesMap[autoRunFilename]);

	//Enable blending for the foreground and cover tracks to allow for proper alpha compositing during fade-in and fade-out transitions, 
	//while keeping blending disabled for the background track since it will always be fully opaque.
	backgroundTrack->SetBlending(true);
	foregroundTrack->SetBlending(true);
	coverTrack->SetBlending(true);

	backgroundActive = true;
	backgroundTrack->Looped(true);

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
			int foundVideo = state.sourcesMap.count(pendingBackgroundCmd.filename);
			if (foundVideo)
			{
				PlayTrackOnLayer(pendingBackgroundCmd.filename, backgroundTrack, backgroundActive, LayerType::Background, &pendingBackgroundCmd);
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
					//int matchIdx = FindVideoSourceIndexByFilename(evt.filename, state.sources);
					int foundVideo = state.sourcesMap.count(evt.filename);
					if (foundVideo > 0)
					{
						//Creates a local DeferredCommand to play the foreground video
						DeferredCommand cmd;
						cmd.type = NetworkCommandType::PlayForeground;
						cmd.filename = evt.filename;

						if (evt.fadeInDuration < 0.0f)
						{
							cmd.fadeInDuration = state.sourcesMap[evt.filename]->fadeInDuration;
						}
						else
						{
							cmd.fadeInDuration = evt.fadeInDuration;
						}
						
						cmd.fadeOutDuration = evt.fadeOutDuration;
						cmd.looped = false;

						Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "HandleBackgroundEvents", "Firing foreground layer item: " + evt.filename + " at playhead pos: " + std::to_string(backgroundPTS) + "s");
						PlayTrackOnLayer(evt.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
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

		int foundVideo = state.sourcesMap.count(pendingForegroundCmd.filename);
		if (foundVideo)
		{
			// Instantly instantiate Video B and reset its properties ready to be played the next time we enter the Main Loop.
			// The actual playback of Video B will be triggered in the next iteration of the Run loop once the current foreground video has fully finished and foregroundActive becomes false.
			PlayTrackOnLayer(pendingForegroundCmd.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &pendingForegroundCmd);
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
		int foundVideo = state.sourcesMap.count(pendingCoverCmd.filename);
		if (foundVideo > 0)
		{
			PlayTrackOnLayer(pendingCoverCmd.filename, coverTrack, coverActive, LayerType::Cover, &pendingCoverCmd);
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
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "HandlePendingSequenceCmd", "Booting pending sequence: " + pendingSequenceCmd.filename);

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

			activeSequence->Stop(); // Reset the sequence state to ensure it starts from the beginning
			activeSequence->Play(pendingSequenceCmd.fadeInDuration);
		}
		else
		{
			Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "HandlePendingSequenceCmd", "Failed to boot pending sequence. File not matched: " + pendingSequenceCmd.filename);
		}
	}
}

void PlaybackManager::PlayTrackOnLayerIndex(int videoSourceIdx, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
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

void PlaybackManager::PlayTrackOnLayer(const std::string& videoName, std::unique_ptr<VideoTrack>& targetTrack, bool& targetActiveFlag, const LayerType& layerType, DeferredCommand* cmd)
{
	AppState& state = appInterface->GetAppState();

	if (state.sourcesMap.find(videoName) == state.sourcesMap.end())
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "PlayTrackOnLayer", "Video source " + videoName + " not found!");
		return;
	}

	Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayTrackOnLayer", "Playing video: " + videoName + " on layer: " + LayerTypeToStr(layerType));
	
	//Update the video source properties based on the command parameters if provided
	if (cmd)
	{
		state.sourcesMap[videoName]->fadeInDuration = cmd->fadeInDuration;
		state.sourcesMap[videoName]->fadeOutDuration = cmd->fadeOutDuration;
		state.sourcesMap[videoName]->looped = cmd->looped;
	}

	//Set initial alpha to 0 for fade-in effect
	state.sourcesMap[videoName]->alpha = 0.0f;

	VideoSource* source = state.sourcesMap[videoName];

	// Dynamically update the passed track unique_ptr pointer structure
	targetTrack = std::make_unique<VideoTrack>(state.sourcesMap[videoName]);
	targetTrack->SetBlending(true);
	targetTrack->Rewind();
	targetTrack->Play(GetTimeStd());

	// Force wrapper synchronization status explicitly
	targetTrack->SetActive(true);

	// Bootstrap first frame mapping context
	ID3D11DeviceContext* ctx = renderer->GetContext();
	state.sourcesMap[videoName]->GetNextFrame(ctx);

	if (state.sourcesMap[videoName]->isFadingIn)
	{
		state.sourcesMap[videoName]->ComputeFadeIn();
	}
	else if (state.sourcesMap[videoName]->fadeInDuration > 0.0f)
	{
		state.sourcesMap[videoName]->alpha = 0.0f;
	}

	// Toggle the specific layer visibility state flag on
	targetActiveFlag = true;
}

void PlaybackManager::PlaySequenceItem(DeferredCommand& cmd)
{
	AppState& state = appInterface->GetAppState();
	//int matchIdx = FindVideoSourceIndexByFilename(cmd.filename, state.sources);
	int foundVideo = state.sourcesMap.count(GetFilenameFromPath(cmd.filename));
	if (foundVideo)
	{
		//PlayTrackOnLayerIndex(matchIdx, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
		PlayTrackOnLayer(GetFilenameFromPath(cmd.filename), foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
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
				//ForceStopBackgroundLayer(1.0f);
				ForceStopForegroundLayers(1.0f);
				break;
			}

			case NetworkCommandType::PlayBackground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_background': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);

				if (foundVideo > 0)
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
						PlayTrackOnLayer(cmd.filename, backgroundTrack, backgroundActive, LayerType::Background, &cmd);
					}
				}

				break;
			}

			case NetworkCommandType::PlayForeground:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_foreground': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);

				if (foundVideo > 0)
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
						PlayTrackOnLayer(cmd.filename, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
					}
				}
				break;
			}
			case NetworkCommandType::PlaySequence:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_sequence': " << cmd.filename << std::endl;

				Sequence* targetSequence = nullptr;	
				//Find the sequence matching the command filename
				for (auto seq : state.sequences)
				{
					if (seq->name == GetFilenameFromPath(cmd.filename))
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

				if (foregroundActive)
				{
					std::cout << "[PlaybackManager] Foreground busy. Buffering sequence, forcing fade out." << std::endl;

					pendingSequenceCmd = cmd;
					hasPendingSequenceCmd = true;

					if (activeSequence && activeSequence->isActive)
					{
						activeSequence->Stop();
					}

					// Only trigger the fade-out if the track is still actively rendering.
					// If it has already ended, ResetForegroundLayer() will clear foregroundActive
					// on the next UpdateLayers() call, which then triggers HandlePendingSequenceCmd().
					if (foregroundTrack && foregroundTrack->IsActive())
					{
						foregroundTrack->StartForcedFadeOut(1.0f);
					}
				}
				else
				{
					hasPendingSequenceCmd = false;
					activeSequence = targetSequence;

					if (activeSequence && !activeSequence->items.empty())
					{
						activeSequence->Stop();
						activeSequence->Play(cmd.fadeInDuration);
					}
				}

				break;
			}
			case NetworkCommandType::PlayCover:
			{
				std::cout << "[PlaybackManager] Processing deferred 'play_cover': " << cmd.filename << std::endl;
				int foundVideo = state.sourcesMap.count(cmd.filename);
				if (foundVideo > 0)
				{
					if (coverActive && coverTrack && coverTrack->IsActive())
					{
						pendingCoverCmd = cmd;
						hasPendingCoverCmd = true;
						coverTrack->StartForcedFadeOut(cmd.fadeOutDuration);
					}
					else
					{
						//PlayTrackOnLayerIndex(matchIdx, coverTrack, coverActive, LayerType::Cover, &cmd);
						PlayTrackOnLayer(cmd.filename, coverTrack, coverActive, LayerType::Cover, &cmd);	
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
		}
	}
}

/*
* This method controls the playback of position-aware choreography videos, deciding whether to play them immediately or orchestrate a smooth transition using a cover layer 
* based on the physical position of the hardware system.
*/
void PlaybackManager::PlayChoreography(const std::string& filename, float fgFadeOut, float fadeIn, float fadeOut, int idVal, bool loopVid, bool forceCoverOnExit)
{
	Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Playing choreography '" + filename + "' fade_in=" + std::to_string(fadeIn)
		+ "s fade_out=" + std::to_string(fadeOut) + "s force_cover_on_exit=" + (forceCoverOnExit ? "true" : "false"));

	//If a valid ID (id_val != -1) is provided, it is cached into last_choreo_id. 
	//This serves as a fallback ID if secondary elements (like foreground events triggered by this video) don't have their own specific tracking IDs.
	if (idVal != -1)
		lastChoreoId = idVal;

	//Evaluates whether a cover video transition must be strictly enforced.
	//This is true if either the last background (active_force_cover_on_exit) or an active foreground event (fg_active_force_cover_on_exit) requested it.
	bool forceCover = activeForceCoverOnExit || fgActiveForceCoverOnExit;
	//Immediately resets the foreground flag (consumes the event).
	fgActiveForceCoverOnExit = false;
	//Arms the flag for the next incoming choreography cycle using the current parameter.
	activeForceCoverOnExit = forceCoverOnExit;

	AppState& state = appInterface->GetAppState();

	//Verifies the filename exists in the video sources map before attempting to play it
	if (state.sourcesMap.find(filename) == state.sourcesMap.end()) 
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "PlayChoreography", "File '" + filename + "' not found!");
		return;
	}

	//Extracts a reference to the metadata of the target video. This includes position data, event data, and other relevant information.
	VideoSource* videoSource = state.sourcesMap[filename];

	// Fallback to standard background playback if no explicit position timeline exists
	if (videoSource->positions.empty()) 
	{
		std::cout << "Choreo: No positions for '" << filename << "'. Playing as background." << std::endl;
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "No positions found for '" + filename + "'. Plays like a standard background video.");
		PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);
		return;
	}

	//Determines the starting coordinate mapped to the very first frame of the video.
	float firstPos = videoSource->positions.front();
	//Determines the last coordinate mapped to the very last frame of the video.
	float lastPos = videoSource->positions.back();
	//Reads the current actual position of the hardware/system as tracked by the master application loop.
	float currentPos = state.lastSentPosition;

	//DISTANCE CHECK
	//Check:
	//  1.if a cover is not forced
	//  2.if the current position is within a 1.0f threshold of the video's starting position
	//  3.if cover transitions have been explicitly globally deactivated in configuration.

	//PLAY WITHOUT COVER
	//Evaluate if player is already close enough to the start boundary to bypass transition mechanics
	if ((!forceCover && std::abs(currentPos - firstPos) < 1.0f) || appInterface->GetConfig().disable_cover) 
	{
		Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Current position (" + std::to_string(currentPos) + ") is close to start boundary (" + std::to_string(firstPos) + "). Playing immediately without cover.");
		PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);
	}
	//PLAY WITH COVER
	//If the screen is far away from the start position ofthe choreography video, a cover transition is used to mask the movement. 
	//The choreography video is only played once the system reaches the starting coordinate of the video.
	else
	{
		std::string evtFilename;
		float evtFadeOut = 0.0f;
		//Checks if the video has events data associated
		bool hasEvent = !videoSource->events.empty();

		if (hasEvent) 
		{
			auto& evt = videoSource->events.back();
			evtFilename = evt.filename;
			evtFadeOut = evt.fadeOutDuration;
		}

		//Define the callback function to be executed when the cover transition completes
		auto callback = [this, filename, idVal, loopVid, fadeIn, fadeOut, hasEvent, evtFilename, evtFadeOut]() 
		{
			Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreographyCallback", "Cover transition complete. Playing '" + filename + "' on background layer. Looping: " + (loopVid ? "true" : "false"));
			
			if (idVal != -1)
				lastChoreoId = idVal;

			//If the video is meant to loop, it calls play_background with zero fade times to run it normally. 
			if (loopVid) 
			{
				PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);
			}
			//If it is not a looping video, it invokes ShowBgLastFrame to freeze and park the background on its terminal frame to visually hold the destination.
			else 
			{
				ShowBgLastFrame(filename, idVal);
			}

			//If the video has embedded events, they are triggered here. 
			if (hasEvent) 
			{
				//Plays a sequence if the event file is a .txt
				bool isSequence = (evtFilename.find(".txt") != std::string::npos);
				if (isSequence) 
				{
					DeferredCommand cmd;
					cmd.type = NetworkCommandType::PlaySequence;
					cmd.filename = evtFilename;
					cmd.fadeInDuration = fadeIn;
					cmd.fadeOutDuration = evtFadeOut;
					cmd.looped = false;
					EnqueueNetworkCommand(cmd);
				}
				//...or plays a foreground video if the event file is a video format
				else 
				{
					DeferredCommand cmd;
					cmd.type = NetworkCommandType::PlayForeground;
					cmd.filename = evtFilename;
					cmd.fadeInDuration = fadeIn;
					cmd.fadeOutDuration = evtFadeOut;
					cmd.looped = false;
					PlayTrackOnLayer(evtFilename, foregroundTrack, foregroundActive, LayerType::Foreground, &cmd);
				}
			}

			//This concludes the lambda definition. It sets coverStopPending = true...
			coverStopPending = true;
			//...and updates the target fade-out clock configuration. 
			//This ensures that the active cover video does not dissolve prematurely before the underlying Direct3D context displays the first new background frame texture.
			coverStopPendingFade = fadeOut;
		};

		//An edge case check: If the cover isn't explicitly forced, and the screen happens to already be standing exactly at the end (last_pos) of this video, 
		//it completely skips loading a cover video pipeline and executes the lambda callback instantly.
		if (!forceCover && std::abs(currentPos - lastPos) < 1.0f) 
		{
			Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Current position (" + std::to_string(currentPos) + ") is close to end boundary (" + std::to_string(lastPos) + "). Skipping cover and playing immediately.");
			callback();
		}

		//COVER TRANSITION
		//If the system is far away from the video boundary, it initiates a full cover transition. 
		//It invokes transition_to, shifting the hardware motor targets toward last_pos while moving the lambda setup callback into ownership memory. 
		//The screen will stay hidden behind the looping cover until the movement finishes and the callback is invoked.
		else 
		{
			Logger::LogMessage(MESSAGE_TYPE::INFO, "PlaybackManager", "PlayChoreography", "Transitioning from current position (" + std::to_string(currentPos) + ") to start boundary (" + std::to_string(firstPos) + ") with cover. Target position for choreography: " + std::to_string(lastPos));
			TransitionTo(lastPos, std::move(callback), fadeIn, fadeOut, idVal, fgFadeOut);
		}
	}
}

/*
* The primary objective of this method is to initiate a spatial transition by firing an cover video. 
* While the cover loops seamlessly, the screen moves to its target position. 
* Once the target position is reached, a callback configuration maps the new background video to play.
* 
* -targetPos -> the destination to reach
* -onComplete() -> the callback to execute once the destination is reached.
* -fadeIn/fadeOut -> fade in and fade out durations for the cover video
* -idVal -> ID val of the choreography video
* -fgFadeOut -> fade out duration to clear the currently foreground video
*/
void PlaybackManager::TransitionTo(float targetPos, std::function<void()> onComplete, float fadeIn, float fadeOut, int idVal, float fgFadeOut)
{
	Config& config = appInterface->GetConfig();
	AppState& state = appInterface->GetAppState();
	std::string coverFilename = config.cover_filename;

	//Verifies the cover video filename exists in the video sources map before attempting to play it.
	if (coverFilename.empty() || state.sourcesMap.find(coverFilename) == state.sourcesMap.end()) 
	{
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "PlaybackManager", "TransitionTo", "Cover video '" + coverFilename + "' not found in sources. Transition aborted.");
		return;
	}

	//Extracts a reference to the metadata of the target video. This includes position data, event data, and other relevant information.
	VideoSource* coverSource = state.sourcesMap[coverFilename];

	//COVER VIDEO IS ALREADY PLAYING, JUST UPDATE THE TRANSITION PROPERTIES
	/*
	* If a transition request arrives while a cover transition is already rendering (coverActive == true), 
	* we do not reconstruct the video decoder pipeline from scratch. 
	* Instead we updates the data attached to the cover video that is playing
	*/
	if(coverSource->transitionMode && coverSource->isCover && coverActive) 
	{
		//Updates the target position to the new one passed as an argument
		coverSource->transitionPosition = targetPos;
		coverSource->transitionId = idVal;
		//Trigger a brake phase in NetworkManager
		coverSource->stopping = true;
		//Updates the callback to be executed once the transition completes to the new one passed as an argument
		coverSource->onTransitionComplete = std::move(onComplete);

		//If a foreground video is active and fade-out duration is valid
		if (foregroundActive && fgFadeOut > -99.0f) 
		{
			//Force fade-out the foreground video
			foregroundTrack->StartForcedFadeOut(fgFadeOut);
		}
		return;
	}

	//COVER VIDEO IS NOT PLAYING, BOOT A NEW TRANSITION
	//Activates transition mode
	coverSource->transitionMode = true;
	//Sets the stopping flag to true to trigger the braking phase in NetworkManager. 
	coverSource->stopping = true;
	//Flags this asset as a cover video
	coverSource->isCover = true;
	//Set the target position
	coverSource->transitionPosition = targetPos;
	coverSource->transitionId = idVal;
	//Sets the callback to be executed once the transition completes
	coverSource->onTransitionComplete = std::move(onComplete);
	coverSource->sequenceTriggered = false;

	//In case a cover video has attached -events, ensures they are all set to triggered=true so they don't accidentally fire during the transition
	for (auto& ev : coverSource->events) 
	{
		ev.triggered = true;
	}

	//If a foreground video is active and fade-out duration is valid
	if (foregroundActive && fgFadeOut > -99.0f) 
	{
		//Force fade-out the foreground video
		foregroundTrack->StartForcedFadeOut(fgFadeOut);
	}

	//Determines the effective fade-in and fade-out durations for the cover video. 
	//If the provided durations are 0, it falls back to the default values specified in the configuration.
	float effFadeIn = (fadeIn == 0.0f) ? config.cover_fade_in_time : fadeIn;
	float effFadeOut = (fadeOut == 0.0f) ? config.cover_fade_out_time : fadeOut;

	//Packages standard execution parameters into deferred command. 
	//It declares the command type explicitly as PlayCover and explicitly flags looped = true to guarantee the masking buffer repeats seamlessly.
	DeferredCommand cmd;
	cmd.type = NetworkCommandType::PlayCover;
	cmd.filename = coverFilename;
	cmd.fadeInDuration = effFadeIn;
	cmd.fadeOutDuration = effFadeOut;
	cmd.looped = true;

	//Play the cover video on the cover layer
	PlayTrackOnLayer(GetFilenameFromPath(coverFilename), coverTrack, coverActive, LayerType::Cover, &cmd);
}

/*
* The primary purpose of this method is to "park" a non-looping choreography video on its final frame instantly.
* This ensures that when an overhead cover transition layer fades out, the background is already showing a static image that perfectly matches the mechanical 
* landing destination of the hardware.
*/
void PlaybackManager::ShowBgLastFrame(const std::string& filename, int idVal)
{
	AppState& state = appInterface->GetAppState();
	//Verifies the filename exists in the video sources map before attempting to play it
	if (state.sourcesMap.find(filename) == state.sourcesMap.end()) 
		return;


	VideoSource* videoSource = state.sourcesMap[filename];
	//This pulls the total time duration of the video (in seconds)
	double duration = videoSource->duration;

	//Set the backgroundTrack to play the target video.
	//This initializes the underlying video source and decodes the first frame, but it will be immediately seeked to the end in the next step.
	PlayTrackOnLayer(filename, backgroundTrack, backgroundActive, LayerType::Background);

	// Seek to exactly 1 second before the clip duration ends to freeze on the final frame
	double seekTime = (duration > 1.0) ? (duration - 1.0) : 0.0;
	videoSource->Seek(seekTime);

	//This forces the video fade-in properties to turn off. 
	//Because the video must be frozen and immediately opaque underneath the fading-out cover track, we prevent any transparency fade-in operations
	videoSource->isFadingIn = false;
	videoSource->alpha = 1.0f;
	backgroundActive = true;

	//This loops through all events of the video. 
	//Because we are jumping straight to the end frame, we mark them all as triggered = true. This suppresses them from firing retroactively
	for (auto& ev : videoSource->events) 
	{
		ev.triggered = true;
	}
}
