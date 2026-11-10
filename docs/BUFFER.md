
# Ideas
Add compiler checks and warning directly to the code!
If a macro requires a certain input stuff and it would be caught by the compiler but the user wants to set a custom message or the user wants to do a compiler level check manually then the language should support that!
Might be usefull for libraries/macros/templates, to tell the user in greater detail what the problem is, this might avois criptic erros like this:

```bash
FAILED: [code=1] plugin/renderer_vk_rasteriser/CMakeFiles/renderer_vk_rasterizer_static.dir/src/scene/scene_renderer.cpp.o 
/opt/gcc-16.2/bin/g++ -DDEBUG -DGLT_MODULE_NAME=\"renderer_vk_rasterizer\" -DIMGUI_DEFINE_MATH_OPERATORS -DPLATFORM_LINUX -DUSE_IMGUI -I/home/dominikm/workspace/Gluttony/core/src -I/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src -I/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/include -I/home/dominikm/workspace/Gluttony/vendor/stb_image/include -I/home/dominikm/workspace/Gluttony/vendor/imgui -I/home/dominikm/workspace/Gluttony/vendor/glm -I/home/dominikm/workspace/Gluttony/vendor/imgui/backends -isystem /home/dominikm/workspace/Gluttony/vendor/vk-bootstrap/src -isystem /home/dominikm/workspace/Gluttony/vendor/VulkanMemoryAllocator/include -isystem /home/dominikm/vulkan-sdk/1.4.363.1/x86_64/include -isystem /home/dominikm/workspace/Gluttony/vendor/slang-prebuilt/slang-2025.1/include -g -std=c++26 -Wall -Wextra -Wpedantic -std=c++26 -freflection -ftemplate-depth=512 -fdiagnostics-color=always -g3 -include/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/util/util_pch.h -Winvalid-pch -include /home/dominikm/workspace/Gluttony/build/engine/debug/plugin/renderer_vk_rasteriser/CMakeFiles/renderer_vk_rasterizer_static.dir/cmake_pch.hxx -MD -MT plugin/renderer_vk_rasteriser/CMakeFiles/renderer_vk_rasterizer_static.dir/src/scene/scene_renderer.cpp.o -MF plugin/renderer_vk_rasteriser/CMakeFiles/renderer_vk_rasterizer_static.dir/src/scene/scene_renderer.cpp.o.d -o plugin/renderer_vk_rasteriser/CMakeFiles/renderer_vk_rasterizer_static.dir/src/scene/scene_renderer.cpp.o -c /home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer.cpp
In file included from /home/dominikm/workspace/Gluttony/core/src/util/pch.h:104,
                 from /home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/util/util_pch.h:9,
                 from <command-line>:
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl: In function ‘vk::Pipeline GLT::renderer::vk_rasterizer::env_detail::create_compute(vk::Device, GLT::renderer::vk_rasterizer::util::device*, vk::PipelineLayout, const std::vector<unsigned int>&, const char*)’:
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:434:40: error: expected ‘)’ before ‘_VAL_ARGS’
  434 |                 LOG(fatal, failure_fmt _VAL_ARGS failure_args)                                                          \
      |                                        ^~~~~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:305:58: note: in definition of macro ‘LOG_Master’
  305 |             GLT_MODULE_NAME, std::this_thread::get_id(), fmt __VA_OPT__(,) __VA_ARGS__);                                \
      |                                                          ^~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:348:38: note: in expansion of macro ‘LOG_fatal’
  348 | #define LOG(severity, fmt, ...)      LOG_##severity(fmt __VA_OPT__(,) __VA_ARGS__)
      |                                      ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:434:17: note: in expansion of macro ‘LOG’
  434 |                 LOG(fatal, failure_fmt _VAL_ARGS failure_args)                                                          \
      |                 ^~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:372:47: note: in expansion of macro ‘ASSERT_4’
  372 | #define _ASSERT_PICK(_1,_2,_3,_4,_5,NAME,...) NAME
      |                                               ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:449:13: note: in expansion of macro ‘_ASSERT_PICK’
  449 |             _ASSERT_PICK(__VA_ARGS__, ASSERT_5, ASSERT_4, ASSERT_3, ASSERT_BAD, ASSERT_BAD)(__VA_ARGS__)
      |             ^~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl:113:13: note: in expansion of macro ‘ASSERT’
  113 |             ASSERT(result.result == vk::Result::eSuccess, "", "Failed to create {} pipeline", what)
      |             ^~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:304:29: note: to match this ‘(’
  304 |         GLT::logger::log_msg(GLT::logger::severity::severity_level, std::source_location::current(),                    \
      |                             ^
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:309:38: note: in expansion of macro ‘LOG_Master’
  309 |     #define LOG_fatal(fmt, ...)      LOG_Master(fatal, fmt __VA_OPT__(,) __VA_ARGS__)
      |                                      ^~~~~~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:348:38: note: in expansion of macro ‘LOG_fatal’
  348 | #define LOG(severity, fmt, ...)      LOG_##severity(fmt __VA_OPT__(,) __VA_ARGS__)
      |                                      ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:434:17: note: in expansion of macro ‘LOG’
  434 |                 LOG(fatal, failure_fmt _VAL_ARGS failure_args)                                                          \
      |                 ^~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:372:47: note: in expansion of macro ‘ASSERT_4’
  372 | #define _ASSERT_PICK(_1,_2,_3,_4,_5,NAME,...) NAME
      |                                               ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:449:13: note: in expansion of macro ‘_ASSERT_PICK’
  449 |             _ASSERT_PICK(__VA_ARGS__, ASSERT_5, ASSERT_4, ASSERT_3, ASSERT_BAD, ASSERT_BAD)(__VA_ARGS__)
      |             ^~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl:113:13: note: in expansion of macro ‘ASSERT’
  113 |             ASSERT(result.result == vk::Result::eSuccess, "", "Failed to create {} pipeline", what)
      |             ^~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:355:44: error: expected primary-expression before ‘(’ token
  355 |         throw GLT::logger::logged_exception(std::source_location::current(), GLT_MODULE_NAME,                           \
      |                                            ^
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:435:17: note: in expansion of macro ‘LOGGED_EXCEPTION’
  435 |                 LOGGED_EXCEPTION(failure_fmt _VAL_ARGS failure_args);                                                   \
      |                 ^~~~~~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:372:47: note: in expansion of macro ‘ASSERT_4’
  372 | #define _ASSERT_PICK(_1,_2,_3,_4,_5,NAME,...) NAME
      |                                               ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:449:13: note: in expansion of macro ‘_ASSERT_PICK’
  449 |             _ASSERT_PICK(__VA_ARGS__, ASSERT_5, ASSERT_4, ASSERT_3, ASSERT_BAD, ASSERT_BAD)(__VA_ARGS__)
      |             ^~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl:113:13: note: in expansion of macro ‘ASSERT’
  113 |             ASSERT(result.result == vk::Result::eSuccess, "", "Failed to create {} pipeline", what)
      |             ^~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:435:46: error: expected ‘)’ before ‘_VAL_ARGS’
  435 |                 LOGGED_EXCEPTION(failure_fmt _VAL_ARGS failure_args);                                                   \
      |                                              ^~~~~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:356:62: note: in definition of macro ‘LOGGED_EXCEPTION’
  356 |             std::this_thread::get_id(), "LOGGER EXCEPTION: " fmt __VA_OPT__(,) __VA_ARGS__);                            \
      |                                                              ^~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:372:47: note: in expansion of macro ‘ASSERT_4’
  372 | #define _ASSERT_PICK(_1,_2,_3,_4,_5,NAME,...) NAME
      |                                               ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:449:13: note: in expansion of macro ‘_ASSERT_PICK’
  449 |             _ASSERT_PICK(__VA_ARGS__, ASSERT_5, ASSERT_4, ASSERT_3, ASSERT_BAD, ASSERT_BAD)(__VA_ARGS__)
      |             ^~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl:113:13: note: in expansion of macro ‘ASSERT’
  113 |             ASSERT(result.result == vk::Result::eSuccess, "", "Failed to create {} pipeline", what)
      |             ^~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:355:44: note: to match this ‘(’
  355 |         throw GLT::logger::logged_exception(std::source_location::current(), GLT_MODULE_NAME,                           \
      |                                            ^
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:435:17: note: in expansion of macro ‘LOGGED_EXCEPTION’
  435 |                 LOGGED_EXCEPTION(failure_fmt _VAL_ARGS failure_args);                                                   \
      |                 ^~~~~~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:372:47: note: in expansion of macro ‘ASSERT_4’
  372 | #define _ASSERT_PICK(_1,_2,_3,_4,_5,NAME,...) NAME
      |                                               ^~~~
/home/dominikm/workspace/Gluttony/core/src/util/io/logger.h:449:13: note: in expansion of macro ‘_ASSERT_PICK’
  449 |             _ASSERT_PICK(__VA_ARGS__, ASSERT_5, ASSERT_4, ASSERT_3, ASSERT_BAD, ASSERT_BAD)(__VA_ARGS__)
      |             ^~~~~~~~~~~~
/home/dominikm/workspace/Gluttony/plugin/renderer_vk_rasteriser/src/scene/scene_renderer_environment.inl:113:13: note: in expansion of macro ‘ASSERT’
  113 |             ASSERT(result.result == vk::Result::eSuccess, "", "Failed to create {} pipeline", what)
      |             ^~~~~~
```



