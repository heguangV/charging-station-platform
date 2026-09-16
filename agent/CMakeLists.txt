# Agent 一级模块：LLM + Tool Calling + NCS 真实充电桩能力 + 腾讯地图 POI/路线。
# 依赖方向严格保持 server → agent → core / infrastructure；core 与 infrastructure 不得反向依赖
# agent（无 target_link_libraries 反向边，也没有 agent 头文件被下层包含）。
add_library(ncs_agent STATIC
    src/agent_service.cpp
    src/prompts/agent_prompts.cpp
    src/tools/station_search_tool.cpp
    src/tools/station_detail_tool.cpp
    src/tools/poi_search_tool.cpp
    src/tools/route_tool.cpp
)
add_library(ncs::agent ALIAS ncs_agent)

set_target_properties(ncs_agent PROPERTIES
    AUTOMOC OFF
    AUTOUIC OFF
    AUTORCC OFF
)

target_compile_features(ncs_agent PUBLIC cxx_std_17)

# 公共头以 agent/ 为前缀暴露：调用方写 #include "agent/agent_service.h"。
target_include_directories(ncs_agent
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${PROJECT_SOURCE_DIR}
)

# 只依赖 core 端口与 infrastructure 的地图能力；LLM 通过 core::application::LlmClient
# 注入，因此本模块不出现任何厂商特有协议。
target_link_libraries(ncs_agent
    PUBLIC
        ncs::core_application
        ncs::infrastructure_map
        Qt6::Core
)

ncs_set_project_warnings(ncs_agent)
