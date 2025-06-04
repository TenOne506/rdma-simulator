set_toolchains("clang")
-- set_toolchains("cuda")
set_toolset("cc", "clang")
set_toolset("cxx", "clang++")

-- 定义目标可执行文件 test
target("test")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("main.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("rdma_simulator/*.cpp")          -- 添加 rdma_simulator 目录下的所有 .cpp 文件


target("multi")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("multi_connection_test.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("rdma_simulator/*.cpp")       