#include "ConfigManager.h"
#include <json.hpp>
#include <fstream>
#include <iostream>
#include "utils.h"


using json = nlohmann::json;

void ConfigManager::LoadConfig(const fs::path& filePath)
{
    if (!fs::exists(filePath))
    {
        std::cout << "Config file '" << filePath.string() << "' not found. Using defaults.\n";
        return;
    }
    try
    {
        std::ifstream f(filePath);
        json j = json::parse(f);

        if (j.contains("assets_path"))
            config.assets_path = j["assets_path"].get<std::string>();

        if (j.contains("target"))
        {
            auto& target = j["target"];
            if (target.contains("ip"))
                config.target_ip = target["ip"].get<std::string>();

            if (target.contains("port"))
                config.target_port = target["port"].get<int>();
        }

        if (j.contains("control_server"))
        {
            auto& cs = j["control_server"];
            if (cs.contains("port"))
                config.control_port = cs["port"].get<int>();
        }

        if (j.contains("positions_framerate"))
            config.positions_framerate = j["positions_framerate"].get<double>();
        if (j.contains("send_period_ms"))
            config.send_period_ms = j["send_period_ms"].get<int>();
        if (j.contains("render_delay_ms"))
            config.render_delay_ms = j["render_delay_ms"].get<int>();
        if (j.contains("positions_delay_ms"))
            config.positions_delay_ms = j["positions_delay_ms"].get<int>();
        if (j.contains("positions_offset"))
            config.positions_offset = j["positions_offset"].get<double>();
        if (j.contains("positions_scale"))
            config.positions_scale = j["positions_scale"].get<double>();
        if (j.contains("autorun"))
            config.autorun_filename = j["autorun"].get<std::string>();
        if (j.contains("autorun_id"))
            config.autorun_id = j["autorun_id"].get<int>();
        if (j.contains("cover_filename"))
            config.cover_filename = j["cover_filename"].get<std::string>();

        if (j.contains("target_positions") && j["target_positions"].is_object()) {
            config.target_positions.clear();
            for (auto& [key, value] : j["target_positions"].items()) {
                TargetPosition tp;
                tp.position = value.value("position", 0.0f);
                tp.foreground = value.value("foreground", "");
                tp.background = value.value("background", "");
                tp.fade_in_seconds = value.value("fade_in_seconds", 0.0f);
                config.target_positions[key] = tp;
            }
        }

        LoadEnvFile(".\\.env");
        //LoadEnvFile("./.env");
        //LoadEnvFile("../.env");

        std::string envVal;
        
        if (!(envVal = GetEnvVar("VIDEO_ASSETS_PATH")).empty())
            config.assets_path = envVal;
        if (!(envVal = GetEnvVar("VIDEO_TARGET_IP")).empty())
            config.target_ip = envVal;
        if (!(envVal = GetEnvVar("VIDEO_TARGET_PORT")).empty())
            config.target_port = std::stoi(envVal);
        if (!(envVal = GetEnvVar("VIDEO_CONTROL_PORT")).empty())
            config.control_port = std::stoi(envVal);
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_FRAMERATE")).empty())
            config.positions_framerate = std::stod(envVal);
        if (!(envVal = GetEnvVar("VIDEO_SEND_PERIOD_MS")).empty())
            config.send_period_ms = std::stoi(envVal);
        if (!(envVal = GetEnvVar("VIDEO_RENDER_DELAY_MS")).empty())
            config.render_delay_ms = std::stoi(envVal);
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_DELAY_MS")).empty())
            config.positions_delay_ms = std::stoi(envVal);
        if (!(envVal = GetEnvVar("VIDEO_AUTORUN_FILENAME")).empty())
            config.autorun_filename = envVal;
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_SCALE")).empty())
            config.positions_scale = std::strtod(envVal.c_str(), nullptr);
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_OFFSET")).empty())
            config.positions_offset = std::strtod(envVal.c_str(), nullptr);
    

        if (config.target_ip == "localhost")
            config.target_ip = "127.0.0.1";

        std::cout << "Configuration Loaded:\n";
        std::cout << "  Assets Path: " << config.assets_path << "\n";
        std::cout << "  Control Server Port: " << config.control_port << "\n";
        std::cout << "  Target IP: " << config.target_ip << ":" << config.target_port << "\n";
        std::cout << "  Positions FPS: " << config.positions_framerate << "\n";
        std::cout << "  Send Period: " << config.send_period_ms << "ms\n";
        std::cout << "  Autorun: " << config.autorun_filename << "\n";
        std::cout << "  Render delay: " << config.render_delay_ms << "ms\n";
        std::cout << "  Positions delay: " << config.positions_delay_ms << "ms\n";
        std::cout << "  Positions scale: " << config.positions_scale << "\n";
        std::cout << "  Positions offset: " << config.positions_offset << "\n";
    }
    catch (const std::exception& e)
    {
        std::cerr << "Error parsing config file: " << e.what() << ". Using defaults.\n";
    }
}

void ConfigManager::LoadEnvFile(const fs::path& filePath)
{
    std::ifstream file(filePath);
    if (!file.is_open())
        return;

    std::string line;
    while (std::getline(file, line))
    {
        TrimString(line);

        if (line.empty() || line[0] == '#')
            continue;

        auto delimiterPos = line.find('=');
        if (delimiterPos != std::string::npos)
        {
            std::string key = line.substr(0, delimiterPos);
            std::string value = line.substr(delimiterPos + 1);

            TrimString(key);
            TrimString(value);

            if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\'')))
            {
                value = value.substr(1, value.size() - 2);
            }

            if (!key.empty()) 
            {
                SetEnvVar(key, value);
            }
        }
    }
}
