#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <d3dcompiler.h>
#include <iostream>
#include <string>
#include <vector>
#include <chrono>
#include <wrl/client.h>
#include "utils.h"
#include "DXShader.h"
#include "VideoSource.h"
#include "DXRenderer.h"
#include "App.h"    
#include "Logger.h"
#include "CrashHandler.h"

#pragma comment(lib, "d3d11.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "avcodec.lib")
#pragma comment(lib, "avformat.lib")
#pragma comment(lib, "avutil.lib")
#pragma comment(lib, "d3dcompiler.lib")

//WINDOW APP
//Make it a Windows application without a console window, and specify the entry point as mainCRTStartup to avoid linker errors about missing WinMain.
//#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")

//CONSOLE APP
//Make it a console application to allow for console output, and specify the entry point as mainCRTStartup to avoid linker errors about missing WinMain.
#pragma comment(linker, "/SUBSYSTEM:console /ENTRY:mainCRTStartup")


int main() 
{
	CrashHandler::Initialize();

    try
    {
		App app(1280, 720);
		app.Run();
    }
    catch (const std::exception& ex)
    {
		Logger::LogMessage(MESSAGE_TYPE::ERRORS, "main", "main", "Fatal application crash: " + std::string(ex.what())); 
        return -1;
	}
    catch (...)
    {
        Logger::LogMessage(MESSAGE_TYPE::ERRORS, "main", "main", "Fatal application crash due to an unknown/non-standard exception.");
        return -1;
    }

	return 0;
}