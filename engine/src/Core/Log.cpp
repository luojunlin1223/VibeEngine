#include "VibeEngine/Core/Log.h"
#include "VibeEngine/Core/Console.h"
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/sinks/basic_file_sink.h>

namespace VE {

std::shared_ptr<spdlog::logger> Log::s_EngineLogger;
std::shared_ptr<spdlog::logger> Log::s_ClientLogger;

void Log::Init() {
    Console::Init();

    // Create sinks: console + file + editor console
    auto consoleSink = std::make_shared<spdlog::sinks::stdout_color_sink_mt>();
    consoleSink->set_pattern("%^[%T] [%n] %v%$");

    auto fileSink = std::make_shared<spdlog::sinks::basic_file_sink_mt>("VibeEngine.log", true);
    fileSink->set_pattern("[%T] [%n] [%l] %v");

    auto editorSink = std::make_shared<ConsoleSink_mt>();
    editorSink->set_pattern("[%T] [%n] %v");

    std::vector<spdlog::sink_ptr> sinks = { consoleSink, fileSink, editorSink };

    s_EngineLogger = std::make_shared<spdlog::logger>("VIBE", sinks.begin(), sinks.end());
    s_EngineLogger->set_level(spdlog::level::trace);
    spdlog::register_logger(s_EngineLogger);

    s_ClientLogger = std::make_shared<spdlog::logger>("APP", sinks.begin(), sinks.end());
    s_ClientLogger->set_level(spdlog::level::trace);
    spdlog::register_logger(s_ClientLogger);
}

} // namespace VE
