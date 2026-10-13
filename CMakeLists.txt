cmake_minimum_required(VERSION 3.20)
project(sysy LANGUAGES CXX)

add_executable(compiler src/main.cpp src/frontend.cpp src/semantic.cpp src/lowering.cpp src/backend.cpp)
target_compile_features(compiler PRIVATE cxx_std_17)
target_compile_options(compiler PRIVATE -Wall -Wextra -Wpedantic)
