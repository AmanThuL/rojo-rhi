#pragma once

#include <rojoRHI/RHI.h>

#include <cstdio>
#include <cstdlib>
#include <string_view>
#include <utility>

struct GLFWwindow;

namespace example {
#ifdef NDEBUG
    inline constexpr bool kValidation = false;
#else
    inline constexpr bool kValidation = true;
#endif

    template<class T> T check(rojoRHI::Result<T> result, std::string_view what)
    {
        if (!result)
        {
            std::fprintf(stderr, "%.*s failed: %s\n", int(what.size()), what.data(), result.error().message.c_str());
            std::exit(EXIT_FAILURE);
        }
        return *std::move(result);
    }

    // A GLFW window and the native surface a swapchain presents to: a CAMetalLayer on macOS, an HWND on Windows.
    class Window
    {
      public:
        Window(const char* title, uint32_t width, uint32_t height);
        ~Window();

        Window(const Window&) = delete;
        Window& operator=(const Window&) = delete;

        bool poll();
        void* surface() const
        {
            return m_surface;
        }
        rojoRHI::Extent2D extent() const;
        double time() const;

      private:
        GLFWwindow* m_window = nullptr;
        void* m_surface = nullptr;
    };
} // namespace example
