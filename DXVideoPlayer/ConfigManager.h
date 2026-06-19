#pragma once
#include <filesystem>	
#include "customtypes.h"

namespace fs = std::filesystem;

class IApp;

class ConfigManager
{
public:
	Config config;
	IApp* appInterface;

public:
	ConfigManager(IApp* appInterface);
	~ConfigManager() = default;

	void LoadConfig(const fs::path& filePath);

private:
	void LoadEnvFile(const fs::path& filePath);
};

