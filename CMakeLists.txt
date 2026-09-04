cmake_minimum_required(VERSION 3.24)
project(TransmissorNDIPortatil VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32)
  message(FATAL_ERROR "O prototipo inicial suporta apenas Windows.")
endif()

add_executable(transmissor-ndi WIN32
  src/main.cpp
  src/resources.rc
)

target_compile_features(transmissor-ndi PRIVATE cxx_std_20)
target_compile_definitions(transmissor-ndi PRIVATE
  UNICODE
  _UNICODE
  NOMINMAX
  WIN32_LEAN_AND_MEAN
  PROCESSINGNDILIB_STATIC
  NDILIB_CPP_DEFAULT_CONSTRUCTORS=0
)
target_include_directories(transmissor-ndi PRIVATE third_party/ndi/include)
target_link_libraries(transmissor-ndi PRIVATE user32 gdi32 comctl32 shell32)

if(MSVC)
  target_compile_options(transmissor-ndi PRIVATE /W4 /permissive- /utf-8 /EHsc)
  target_link_options(transmissor-ndi PRIVATE /SUBSYSTEM:WINDOWS)
endif()

set_target_properties(transmissor-ndi PROPERTIES
  OUTPUT_NAME "Transmissor NDI Portatil"
)
