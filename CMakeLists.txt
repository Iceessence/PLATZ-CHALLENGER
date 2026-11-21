cmake_minimum_required(VERSION 3.28)
project(PLATZ VERSION 1.0.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W3 /utf-8 /EHsc /MP /permissive-)
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

# Verify and extract pinned dependencies if not already extracted
set(PLATZ_DEPS_EXTRACTED "${CMAKE_SOURCE_DIR}/out/deps_extracted")
set(PLATZ_TOOLS_DIR "${CMAKE_SOURCE_DIR}/out/tools")
if(NOT EXISTS "${PLATZ_DEPS_EXTRACTED}/vulkan_headers/Vulkan-Headers-vulkan-sdk-1.4.363.0/include/vulkan/vulkan.h")
    message(STATUS "[PLATZ] Bootstrapping pinned dependencies from dependency-lock.json...")
    execute_process(
        COMMAND powershell -NoProfile -ExecutionPolicy Bypass -File "${CMAKE_SOURCE_DIR}/tools/bootstrap_deps.ps1" -RepoRoot "${CMAKE_SOURCE_DIR}"
        RESULT_VARIABLE BOOTSTRAP_RESULT
    )
    if(NOT BOOTSTRAP_RESULT EQUAL 0)
        message(FATAL_ERROR "[PLATZ] Dependency bootstrap failed with exit code ${BOOTSTRAP_RESULT}")
    endif()
endif()

# Paths to pinned dependencies
set(VULKAN_HEADERS_INCLUDE "${PLATZ_DEPS_EXTRACTED}/vulkan_headers/Vulkan-Headers-vulkan-sdk-1.4.363.0/include")
set(VOLK_DIR "${PLATZ_DEPS_EXTRACTED}/volk/volk-vulkan-sdk-1.4.363.0")
set(VMA_INCLUDE "${PLATZ_DEPS_EXTRACTED}/vma/VulkanMemoryAllocator-3.5.0/include")
set(SDL3_DIR "${PLATZ_DEPS_EXTRACTED}/sdl3/SDL3-3.4.20")
set(SQLITE_DIR "${PLATZ_DEPS_EXTRACTED}/sqlite/sqlite-amalgamation-3540100")
set(IMGUI_DIR "${PLATZ_DEPS_EXTRACTED}/imgui/imgui-1.93.0-docking")
set(GLM_INCLUDE "${PLATZ_DEPS_EXTRACTED}/glm/glm-1.0.3")
set(SINGLE_HEADERS_INCLUDE "${PLATZ_DEPS_EXTRACTED}/single_headers")
set(SLANGC_EXECUTABLE "${PLATZ_TOOLS_DIR}/slang/bin/slangc.exe")
set(VULKAN_VALIDATION_LAYER_DIR "${PLATZ_TOOLS_DIR}/validation_layers")

# Compile Slang shaders to SPIR-V at build time (and provide runtime hot-reload support)
set(PLATZ_SHADER_SOURCES
    "${CMAKE_SOURCE_DIR}/shaders/pbr_surface.slang"
    "${CMAKE_SOURCE_DIR}/shaders/grid_sky.slang"
)
set(PLATZ_SHADER_OUTPUTS "")

file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/shaders")

add_custom_command(
    OUTPUT
        "${CMAKE_BINARY_DIR}/shaders/pbr_surface.vert.spv"
        "${CMAKE_BINARY_DIR}/shaders/pbr_surface.frag.spv"
        "${CMAKE_BINARY_DIR}/shaders/grid_sky.vert.spv"
        "${CMAKE_BINARY_DIR}/shaders/grid_sky.frag.spv"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/shaders/pbr_surface.slang" -target spirv -profile spirv_1_5 -entry vsMain -stage vertex -O2 -o "${CMAKE_BINARY_DIR}/shaders/pbr_surface.vert.spv"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/shaders/pbr_surface.slang" -target spirv -profile spirv_1_5 -entry fsMain -stage fragment -O2 -o "${CMAKE_BINARY_DIR}/shaders/pbr_surface.frag.spv"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/shaders/grid_sky.slang" -target spirv -profile spirv_1_5 -entry vsMain -stage vertex -O2 -o "${CMAKE_BINARY_DIR}/shaders/grid_sky.vert.spv"
    COMMAND "${SLANGC_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/shaders/grid_sky.slang" -target spirv -profile spirv_1_5 -entry fsMain -stage fragment -O2 -o "${CMAKE_BINARY_DIR}/shaders/grid_sky.frag.spv"
    DEPENDS
        "${CMAKE_SOURCE_DIR}/shaders/pbr_surface.slang"
        "${CMAKE_SOURCE_DIR}/shaders/grid_sky.slang"
    COMMENT "[PLATZ Slang 2026.22.1] Compiling Slang shaders to Vulkan SPIR-V 1.5..."
    VERBATIM
)
add_custom_target(platz_shaders ALL
    DEPENDS
        "${CMAKE_BINARY_DIR}/shaders/pbr_surface.vert.spv"
        "${CMAKE_BINARY_DIR}/shaders/pbr_surface.frag.spv"
        "${CMAKE_BINARY_DIR}/shaders/grid_sky.vert.spv"
        "${CMAKE_BINARY_DIR}/shaders/grid_sky.frag.spv"
)

# Third-party static library (SQLite 3.54.1, Volk 1.4.363.0, ImGui 1.93.0-docking)
add_library(platz_third_party STATIC
    "${SQLITE_DIR}/sqlite3.c"
    "${VOLK_DIR}/volk.c"
    "${IMGUI_DIR}/imgui.cpp"
    "${IMGUI_DIR}/imgui_draw.cpp"
    "${IMGUI_DIR}/imgui_tables.cpp"
    "${IMGUI_DIR}/imgui_widgets.cpp"
    "${IMGUI_DIR}/imgui_demo.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_sdl3.cpp"
    "${IMGUI_DIR}/backends/imgui_impl_vulkan.cpp"
)
target_include_directories(platz_third_party PUBLIC
    "${VULKAN_HEADERS_INCLUDE}"
    "${VOLK_DIR}"
    "${VMA_INCLUDE}"
    "${SDL3_DIR}/include"
    "${SQLITE_DIR}"
    "${IMGUI_DIR}"
    "${IMGUI_DIR}/backends"
    "${GLM_INCLUDE}"
    "${SINGLE_HEADERS_INCLUDE}"
)
target_compile_definitions(platz_third_party PUBLIC
    VK_NO_PROTOTYPES
    VK_USE_PLATFORM_WIN32_KHR
    IMGUI_IMPL_VULKAN_USE_VOLK
    GLM_FORCE_RADIANS
    GLM_FORCE_DEPTH_ZERO_TO_ONE
    SQLITE_ENABLE_WAL_SYNCHRONOUS=1
    SQLITE_THREADSAFE=1
    SQLITE_ENABLE_FTS5
    SQLITE_ENABLE_JSON1
)

# PLATZ Core + Engine + Platform + Creative + Runtime + AI + Studio unified library
add_library(platz_engine STATIC
    src/platform/platform_paths.cpp
    src/platform/sdl_window.cpp
    src/platform/audio_engine.cpp
    src/core/object_identity.cpp
    src/core/asset_store.cpp
    src/core/project_db.cpp
    src/core/project_state.cpp
    src/creative/mesh_generator.cpp
    src/creative/asset_importer.cpp
    src/creative/texture_generator.cpp
    src/engine/render/vulkan/vma_impl.cpp
    src/engine/render/vulkan/spirv_validator.cpp
    src/engine/render/vulkan/shader_compiler.cpp
    src/engine/render/vulkan/vulkan_context.cpp
    src/engine/render/vulkan/vulkan_renderer.cpp
    src/engine/scene/render_scene.cpp
    src/engine/runtime/runtime_session.cpp
    src/ai/command_registry.cpp
    src/ai/staged_session.cpp
    src/ai/specialist_model.cpp
    src/studio/validation_world.cpp
    src/studio/studio_ui.cpp
)
add_dependencies(platz_engine platz_shaders)
target_include_directories(platz_engine PUBLIC
    "${CMAKE_SOURCE_DIR}/src"
)
target_compile_definitions(platz_engine PUBLIC
    PLATZ_SOURCE_DIR="${CMAKE_SOURCE_DIR}"
    PLATZ_BINARY_DIR="${CMAKE_BINARY_DIR}"
    PLATZ_SLANGC_PATH="${SLANGC_EXECUTABLE}"
    PLATZ_VALIDATION_LAYER_PATH="${VULKAN_VALIDATION_LAYER_DIR}"
)
target_link_libraries(platz_engine PUBLIC
    platz_third_party
    "${SDL3_DIR}/lib/x64/SDL3.lib"
)

# Helper function to copy SDL3.dll and slang.dll next to executables
function(platz_copy_runtime_dlls TARGET_NAME)
    add_custom_command(TARGET ${TARGET_NAME} POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${SDL3_DIR}/lib/x64/SDL3.dll"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/SDL3.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PLATZ_TOOLS_DIR}/slang/bin/slang.dll"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/slang.dll"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${PLATZ_TOOLS_DIR}/slang/bin/slang-glslang.dll"
            "$<TARGET_FILE_DIR:${TARGET_NAME}>/slang-glslang.dll"
        COMMENT "[PLATZ] Ensuring runtime DLLs next to ${TARGET_NAME}"
    )
endfunction()

# Primary Deliverable Application: PLATZ.exe
add_executable(PLATZ
    apps/studio/main.cpp
)
target_link_libraries(PLATZ PRIVATE platz_engine)
platz_copy_runtime_dlls(PLATZ)

# Testing Suites (CTest)
enable_testing()

add_executable(platz_test_unit tests/unit/test_core_and_commands.cpp)
target_link_libraries(platz_test_unit PRIVATE platz_engine)
platz_copy_runtime_dlls(platz_test_unit)
add_test(NAME platz_test_unit COMMAND platz_test_unit)

add_executable(platz_test_integration tests/integration/test_persistence_and_runtime.cpp)
target_link_libraries(platz_test_integration PRIVATE platz_engine)
platz_copy_runtime_dlls(platz_test_integration)
add_test(NAME platz_test_integration COMMAND platz_test_integration)

add_executable(platz_test_graphics tests/graphics/test_vulkan_renderer.cpp)
target_link_libraries(platz_test_graphics PRIVATE platz_engine)
platz_copy_runtime_dlls(platz_test_graphics)
add_test(NAME platz_test_graphics COMMAND platz_test_graphics)

add_executable(platz_test_security tests/security/test_adversarial_safety.cpp)
target_link_libraries(platz_test_security PRIVATE platz_engine)
platz_copy_runtime_dlls(platz_test_security)
add_test(NAME platz_test_security COMMAND platz_test_security)
