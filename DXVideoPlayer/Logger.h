#pragma once
#include <string>
#include "customtypes.h"

class IApp;

class Logger
{
private:
	IApp* appInterface{ nullptr };

private:
	Logger() = default;

public:
	Logger(const Logger&) = delete;
	Logger& operator=(const Logger&) = delete;
	Logger(Logger&&) = delete;
	Logger& operator=(Logger&&) = delete;

	static Logger& GetInstance()
	{
		static Logger instance;
		return instance;
	}

	void SetAppInterface(IApp* appInterface);

	void LogMessage(MESSAGE_TYPE type, const std::string& className = "", const std::string& methodName = "", const std::string& message = "");
};

