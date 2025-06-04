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
    on_config(function (target)
        if is_mode("debug") then
            target:add("cxflags", "-O0", "-g") -- 取消优化并添加调试符号
            target:add("mxflags", "-O0", "-g")
            target:add("asflags", "-O0", "-g")
            print("Debug模式已启用：关闭优化(-O0)")
        else
            target:add("cxflags", "-O2")       -- 发布模式默认优化
            print("Release模式：启用-O2优化")
        end
    end)