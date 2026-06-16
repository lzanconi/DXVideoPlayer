#include "Logger.h"
#include <iostream>
#include "App.h"

Logger::Logger(IApp* appInterface) : appInterface(appInterface)
{
}

void Logger::LogMessage(MESSAGE_TYPE type, const std::string& className, const std::string& methodName, const std::string& message)
{
	std::string messageTypeStr = (type == MESSAGE_TYPE::ERRORS) ? "ERROR" : "INFO";
	std::string logMsg = "[" + messageTypeStr + " " + (className.empty() ? "" : className + "::")
		+ (methodName.empty() ? "" : methodName) + "] " + message;
	
	if (type == MESSAGE_TYPE::ERRORS)
	{
		std::cerr << logMsg << std::endl;
	}
	else
	{
		std::cout << logMsg << std::endl;
	}

	appInterface->SendTCPMessage(logMsg);
}
