#include "Logger.h"
#include <iostream>
#include <fstream>
#include "utils.h"

std::mutex Logger::logMutex;

void Logger::ClearLog()
{
	std::lock_guard<std::mutex> lock(logMutex);
	std::ofstream logFile("videoplayer.log", std::ios::out | std::ios::trunc);
	if (logFile.is_open())
	{
		logFile << "=== Log File Initialized (" << GetTimestampStr() << ") ===\n";
		logFile.close();
	}
}

void Logger::LogMessage(MESSAGE_TYPE type, const std::string& className, const std::string& methodName, const std::string& message)
{
	std::lock_guard<std::mutex> lock(logMutex);

	const std::string RESET = "\033[0m";
	const std::string RED = "\033[31m";
	const std::string GREEN = "\033[32m";
	const std::string YELLOW = "\033[33m";
	const std::string BLUE = "\033[34m";
	const std::string MAGENTA = "\033[35m";
	const std::string CYAN = "\033[36m";

	std::string messageTypeStr = (type == MESSAGE_TYPE::ERRORS) ? "ERROR" : "MSG";

	std::string timeStamp = GetTimestampStr();

	std::string logMsg = "---------------------------------------------------------------------------------------------------------\n"
		"[" + messageTypeStr + " -> " + timeStamp + " - " + className + "::" + methodName + "()]:\n " + message + "\n";

	if (type == MESSAGE_TYPE::ERRORS)
	{
		std::cerr << RED << logMsg << RESET << std::endl;
	}
	else
	{
		std::cout << GREEN << logMsg << RESET << std::endl;
	}

	std::ofstream logFile("videoplayer.log", std::ios::app);
	if (logFile.is_open())
	{
		logFile << logMsg;
		logFile.close();
	}
}
