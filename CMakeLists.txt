cmake_minimum_required(VERSION 3.24)
project(TransmissorNDIPortatil VERSION 0.3.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "Este aplicativo suporta Windows 10/11 x64.")
endif()

add_executable(transmissor-ndi WIN32
  src/main.cpp
  src/capture.cpp
  src/ndi_sender.cpp
  src/settings.cpp
  src/resources.rc
)

target_compile_features(transmissor-ndi PRIVATE cxx_std_20)
target_compile_definitions(transmissor-ndi PRIVATE
  UNICODE
  _UNICODE
  WINVER=0x0A00
  _WIN32_WINNT=0x0A00
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  PROCESSINGNDILIB_STATIC
  NDILIB_CPP_DEFAULT_CONSTRUCTORS=0
)

target_include_directories(transmissor-ndi PRIVATE
  src
  third_party/ndi/include
)

target_link_libraries(transmissor-ndi PRIVATE
  user32
  gdi32
  comctl32
  shell32
  dwmapi
  uxtheme
)

if(MSVC)
  target_compile_options(transmissor-ndi PRIVATE
    /W4
    /permissive-
    /utf-8
    /EHsc
    /FIui_compat.h
  )
  target_link_options(transmissor-ndi PRIVATE /SUBSYSTEM:WINDOWS /MANIFEST:NO)
endif()

set_target_properties(transmissor-ndi PROPERTIES
  OUTPUT_NAME "Transmissor NDI Portatil"
)
