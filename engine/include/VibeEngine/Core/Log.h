#pragma once

#include <spdlog/spdlog.h>
#include <memory>

namespace VE {

class Log {
public:
    static void Init();

    static std::shared_ptr<spdlog::logger>& GetEngineLogger()  { return s_EngineLogger; }
    static std::shared_ptr<spdlog::logger>& GetClientLogger()  { return s_ClientLogger; }

private:
    static std::shared_ptr<spdlog::logger> s_EngineLogger;
    static std::shared_ptr<spdlog::logger> s_ClientLogger;
};

} // namespace VE

// Engine log macros
#define VE_ENGINE_TRACE(...)    ::VE::Log::GetEngineLogger()->trace(__VA_ARGS__)
#define VE_ENGINE_INFO(...)     ::VE::Log::GetEngineLogger()->info(__VA_ARGS__)
#define VE_ENGINE_WARN(...)     ::VE::Log::GetEngineLogger()->warn(__VA_ARGS__)
#define VE_ENGINE_ERROR(...)    ::VE::Log::GetEngineLogger()->error(__VA_ARGS__)

// Client log macros
#define VE_TRACE(...)           ::VE::Log::GetClientLogger()->trace(__VA_ARGS__)
#define VE_INFO(...)            ::VE::Log::GetClientLogger()->info(__VA_ARGS__)
#define VE_WARN(...)            ::VE::Log::GetClientLogger()->warn(__VA_ARGS__)
#define VE_ERROR(...)           ::VE::Log::GetClientLogger()->error(__VA_ARGS__)
