#include "ConfigManager.h"
#include <json.hpp>
#include <fstream>
#include <iostream>
#include "utils.h"


using json = nlohmann::json;

void ConfigManager::LoadConfig(const fs::path& filePath)
{
	std::string resolvedFilename = filePath.string();
    if (!fs::exists(resolvedFilename))
    {
		const std::string fallback = "config/video-player-config.json";
        if (fs::exists(fallback))
        {
            std::cout << "Config file '" << filePath.string() << "' not found. Falling back to '" << fallback << "'.\n";
            resolvedFilename = fallback;
        }
        else
        {
			std::cout << "Config file " << filePath.string() << " not found. Using defaults.\n";
			return;
        }
    }

    try
    {
		std::ifstream fstream(resolvedFilename);
		json j = json::parse(fstream);

        if (j.contains("choreos_config_file"))
            config.choreos_config_file = j["choreos_config_file"].get<std::string>();
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

        if (j.contains("render_delay_ms"))
            config.render_delay_ms = j["render_delay_ms"].get<int>();
        if (j.contains("positions") && j["positions"].is_object())
        {
            auto& positions = j["positions"];
            if (positions.contains("positions_framerate"))
                config.positions_framerate = positions["positions_framerate"].get<double>();
            if (positions.contains("send_period_ms"))
                config.send_period_ms = positions["send_period_ms"].get<int>();
            if (positions.contains("positions_delay_ms"))
                config.positions_delay_ms = positions["positions_delay_ms"].get<int>();
            if (positions.contains("positions_offset"))
                config.positions_offset = positions["positions_offset"].get<double>();
            if (positions.contains("positions_scale"))
                config.positions_scale = positions["positions_scale"].get<double>();
        }
        if (j.contains("autorun_id"))
        {
            config.autorun_id = j["autorun_id"].get<int>();
            // Resolve autorun_filename from choreos config based on autorun_id
            if (fs::exists(config.choreos_config_file))
            {
                try
                {
                    std::ifstream cf(config.choreos_config_file);
                    json cj = json::parse(cf);
                    if (cj.contains("choreographies") && cj["choreographies"].is_array())
                    {
                        for (const auto& choreo : cj["choreographies"])
                        {
                            if (choreo.value("id", -1) == config.autorun_id && choreo.contains("videoFile"))
                            {
                                config.autorun_filename = choreo["videoFile"].get<std::string>();
                                break;
                            }
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    std::cerr << "Warning: could not resolve autorun_id from choreos config: " << e.what() << "\n";
                }
            }
        }
        if (j.contains("cover") && j["cover"].is_object())
        {
            auto& cover = j["cover"];
            if (cover.contains("cover_filename"))
                config.cover_filename = cover["cover_filename"].get<std::string>();
            if (cover.contains("cover_reference_speed"))
                config.cover_reference_speed = cover["cover_reference_speed"].get<float>();
            if (cover.contains("cover_stop_acceleration"))
                config.cover_stop_acceleration = cover["cover_stop_acceleration"].get<float>();
            if (cover.contains("fade_in_time"))
                config.cover_fade_in_time = cover["fade_in_time"].get<float>();
            if (cover.contains("fade_out_time"))
                config.cover_fade_out_time = cover["fade_out_time"].get<float>();
            if (cover.contains("disable_cover"))
                config.disable_cover = cover["disable_cover"].get<bool>();
        }

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

        /*load_env_file("./.env");
        load_env_file("../.env");*/
        LoadEnvFile(".\\.env");

        std::string envVal;
        if (!(envVal = GetEnvVar("CHOREOS_CONFIG_FILE")).empty())
            config.choreos_config_file = envVal;
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
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_SCALE")).empty())
            config.positions_scale = std::strtod(envVal.c_str(), nullptr);
        if (!(envVal = GetEnvVar("VIDEO_POSITIONS_OFFSET")).empty())
            config.positions_offset = std::strtod(envVal.c_str(), nullptr);

        if (config.target_ip == "localhost")
            config.target_ip = "127.0.0.1";

        std::cout << "Configuration Loaded:\n";
        std::cout << "  Choreos Config File: " << config.choreos_config_file << "\n";
        std::cout << "  Control Server Port: " << config.control_port << "\n";
        std::cout << "  Target IP: " << config.target_ip << ":" << config.target_port << "\n";
        std::cout << "  Autorun: " << config.autorun_filename << " (id=" << config.autorun_id << ")\n";
        std::cout << "  Render delay: " << config.render_delay_ms << "ms\n";
        std::cout << "  Cover: " << config.cover_filename << "\n";
        std::cout << "    Reference speed: " << config.cover_reference_speed << " mm/s\n";
        std::cout << "    Stop acceleration: " << config.cover_stop_acceleration << " mm/s²\n";
        std::cout << "    Fade in: " << config.cover_fade_in_time << "s\n";
        std::cout << "    Fade out: " << config.cover_fade_out_time << "s\n";
        std::cout << "  Positions:\n";
        std::cout << "    Framerate: " << config.positions_framerate << " fps\n";
        std::cout << "    Send period: " << config.send_period_ms << "ms\n";
        std::cout << "    Delay: " << config.positions_delay_ms << "ms\n";
        std::cout << "    Scale: " << config.positions_scale << "\n";
        std::cout << "    Offset: " << config.positions_offset << "\n";

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
