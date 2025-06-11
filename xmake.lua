set_toolchains("clang")
-- set_toolchains("cuda")
set_toolset("cc", "clang")
set_toolset("cxx", "clang++")
-- 定义 GTest 路径变量（避免硬编码）
local GTEST_DIR = "/home/wyf/googletest"
local function add_gtest_settings(target)
    target:add("defines", "GTEST_HAS_PTHREAD=1")
    -- 方法1：使用包管理
    -- target:add("packages", "gtest")
    -- 或方法2：使用本地路径
    target:add("includedirs", "/home/wyf/googletest/googletest/include")
    target:add("includedirs", "/home/wyf/googletest/googletest")
    target:add("files", "/home/wyf/googletest/googletest/src/gtest-all.cc")
end
-- 定义目标可执行文件 test
target("test")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("main.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("rdma_simulator/*.cpp")          -- 添加 rdma_simulator 目录下的所有 .cpp 文件
   -- 在目标配置完成后添加 GTest 设置
    -- on_load(function(target)
    --     add_gtest_settings(target)
    -- end)


target("multi")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("multi_connection_test.cpp")     -- 添加当前目录下的 main.cpp
    add_files("rdma_simulator/*.cpp")       
    -- add_includedirs("../googletest/googletest/include")
    -- add_includedirs("../googletest/googletest")
    -- add_files("../googletest/googletest/src/gtest-all.cc")
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



target("googletest")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("rdma_simulator/tests/rdma_cache_test.cpp")                      -- 添加当前目录下的 main.cpp
    add_files("rdma_simulator/tests/main.cpp") 
    add_files("rdma_simulator/*.cpp")       
    --add_files("../googletest/googletest/src/*.cc")
    -- 在目标配置完成后添加 GTest 设置
    on_load(function(target)
        add_gtest_settings(target)
    end)

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
    

target("connection")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("rdma_simulator/connection_test/*.cpp")                      -- 添加当前目录下的 main.cpp
    --add_files("rdma_simulator/connection_test/rdma_connection_test.cpp")
    --add_files("rdma_simulator/connection_test/rdma_connection_test.h")
    --add_files("rdma_simulator/tests/main.cpp") 
    add_files("rdma_simulator/*.cpp")       
    --add_files("../googletest/googletest/src/*.cc")
    -- 在目标配置完成后添加 GTest 设置
    on_load(function(target)
        add_gtest_settings(target)
    end)

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

target("simple")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("rdma_simulator/simple/*.cpp")                      -- 添加当前目录下的 main.cpp
    --add_files("rdma_simulator/tests/main.cpp") 
    add_files("rdma_simulator/*.cpp")       
    --add_files("../googletest/googletest/src/*.cc")
    -- 在目标配置完成后添加 GTest 设置
    on_load(function(target)
        add_gtest_settings(target)
    end)

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

target("data")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("rdma_simulator/data/*.cpp")                      -- 添加当前目录下的 main.cpp
    --add_files("rdma_simulator/tests/main.cpp") 
    add_files("rdma_simulator/*.cpp")       
    --add_files("../googletest/googletest/src/*.cc")
    -- 在目标配置完成后添加 GTest 设置
    on_load(function(target)
        add_gtest_settings(target)
    end)

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


target("basic")
    set_kind("binary")                         -- 构建为可执行文件
    set_languages("c++20")                     -- 使用 C++20 标准
    add_files("rdma_simulator/basic/*.cpp")                      -- 添加当前目录下的 main.cpp
    --add_files("rdma_simulator/tests/main.cpp") 
    add_files("rdma_simulator/*.cpp")       
    --add_files("../googletest/googletest/src/*.cc")
    -- 在目标配置完成后添加 GTest 设置
    on_load(function(target)
        add_gtest_settings(target)
    end)

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