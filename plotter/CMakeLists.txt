cmake_minimum_required(VERSION 3.20)
project(ultra_plot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_BUILD_TYPE Release)

include(FetchContent)

# ===== ImGui =====
FetchContent_Declare(
  imgui
  GIT_REPOSITORY https://github.com/ocornut/imgui.git
  GIT_TAG v1.90.9
)
FetchContent_MakeAvailable(imgui)

# ===== ImPlot =====
FetchContent_Declare(
  implot
  GIT_REPOSITORY https://github.com/epezent/implot.git
  GIT_TAG v0.16
)
FetchContent_MakeAvailable(implot)

# ===== GLFW =====
FetchContent_Declare(
  glfw
  GIT_REPOSITORY https://github.com/glfw/glfw.git
  GIT_TAG 3.3.8
)
FetchContent_MakeAvailable(glfw)

# ===== GLEW (시스템) =====
find_package(GLEW REQUIRED)

# ImGui 객체 라이브러리 (OpenGL3 + GLFW 백엔드)
set(IMGUI_SRC
  ${imgui_SOURCE_DIR}/imgui.cpp
  ${imgui_SOURCE_DIR}/imgui_draw.cpp
  ${imgui_SOURCE_DIR}/imgui_tables.cpp
  ${imgui_SOURCE_DIR}/imgui_widgets.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
  ${imgui_SOURCE_DIR}/backends/imgui_impl_opengl3.cpp
)
add_library(imgui_obj OBJECT ${IMGUI_SRC})
target_include_directories(imgui_obj PUBLIC
  ${imgui_SOURCE_DIR}
  ${imgui_SOURCE_DIR}/backends
)
# GLEW 사용을 백엔드에 알려줌
target_compile_definitions(imgui_obj PUBLIC IMGUI_IMPL_OPENGL_LOADER_GLEW)

# ImPlot
set(IMPLOT_SRC
  ${implot_SOURCE_DIR}/implot.cpp
  ${implot_SOURCE_DIR}/implot_items.cpp
)
add_library(implot_obj OBJECT ${IMPLOT_SRC})
target_include_directories(implot_obj PUBLIC
  ${implot_SOURCE_DIR}
  ${imgui_SOURCE_DIR}
)

# 실행 파일
add_executable(ultra_plot main.cpp)
target_link_libraries(ultra_plot PRIVATE
  imgui_obj implot_obj glfw GLEW::GLEW
)

# Linux 필수 라이브러리
if(UNIX AND NOT APPLE)
  find_package(Threads REQUIRED)
  target_link_libraries(ultra_plot PRIVATE Threads::Threads dl GL X11)
endif()

# 최적화 플래그
if (CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(ultra_plot PRIVATE -O3 -march=native -ffast-math -pipe)
endif()
