#pragma once
#include <string>
#include "customtypes.h"

class IApp;

class Logger
{
private:
	IApp* appInterface;

public:
	Logger(IApp* appInterface);
	~Logger() = default;

	void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "");
};

