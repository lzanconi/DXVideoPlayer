#include "Sequence.h"
#include <iostream>
#include "App.h"

Sequence::Sequence(std::string& name, const std::vector<SequenceItem>& items, IApp* appInterface)
	: name(name), items(items), appInterface(appInterface)
{ }

void Sequence::Play(bool shouldLoop)
{
	if (items.empty())
	{
		std::cerr << "[Sequence] No items to play in sequence: " << name << std::endl;
		return;
	}

	std::cout << "[Sequence] Starting sequence: " << name << " with " << items.size() << " items." << std::endl;
	isActive = true;
	currentIndex = 0;
	looped = shouldLoop;
	isFirstRun = true; // Ensure this is explicitly set to true on start

	DeferredCommand cmd;
	cmd.type = NetworkCommandType::PlayForeground;
	cmd.filename = items[currentIndex].filename;
	cmd.fadeInDuration = items[currentIndex].fadeInDuration;
	cmd.fadeOutDuration = items[currentIndex].fadeOutDuration;
	cmd.looped = false;

	appInterface->TriggerSequenceItem(cmd);
}

void Sequence::Stop()
{
	std::cout << "[Sequence] Manually stopping sequence: " << name << std::endl;
	isActive = false;
	currentIndex = -1;
	isFirstRun = true;
}	

void Sequence::AdvanceSequence()
{
	if (!isActive)
		return;

	currentIndex++;

	if (currentIndex >= static_cast<int>(items.size()))
	{
		if (looped)
		{
			std::cout << ">>> [Sequence] Looping sequence: " << name << std::endl;	
			currentIndex = 0;
			isFirstRun = false;
		}
		else
		{
			std::cout << "[Sequence] Sequence completed naturally: " << name << std::endl;
			isActive = false;
			currentIndex = -1;
			return;
		}
	}

	std::cout << "[Sequence] Advancing to next item in sequence: " << name << " (Index: " << currentIndex << ")" << std::endl;

	float appliedFadeIn = isFirstRun ? items[currentIndex].fadeInDuration : 0.0f;
	float appliedFadeOut = looped ? 0.0f : items[currentIndex].fadeOutDuration;

	DeferredCommand cmd;
	cmd.type = NetworkCommandType::PlayForeground;
	cmd.filename = items[currentIndex].filename;
	cmd.fadeInDuration = appliedFadeIn;
	cmd.fadeOutDuration = appliedFadeOut;
	cmd.looped = false;

	appInterface->TriggerSequenceItem(cmd);
}	