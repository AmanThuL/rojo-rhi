#include "Common/Example.h"

#include <GLFW/glfw3.h>

#if defined(__APPLE__)
#define GLFW_EXPOSE_NATIVE_COCOA
#include <GLFW/glfw3native.h>
#include <objc/message.h>
#include <objc/runtime.h>
#elif defined(_WIN32)
#define GLFW_EXPOSE_NATIVE_WIN32
#include <GLFW/glfw3native.h>
#endif

namespace example {
    namespace {
#if defined(__APPLE__)
        struct CGSize
        {
            double width;
            double height;
        };

        template<class R, class... A> R send(id object, const char* selector, A... args)
        {
            return reinterpret_cast<R (*)(id, SEL, A...)>(objc_msgSend)(object, sel_registerName(selector), args...);
        }

        void* createMetalLayer(GLFWwindow* window)
        {
            id view = send<id>(reinterpret_cast<id>(glfwGetCocoaWindow(window)), "contentView");
            id layer = send<id>(reinterpret_cast<id>(objc_getClass("CAMetalLayer")), "layer");

            float scale = 1.0f;
            glfwGetWindowContentScale(window, &scale, nullptr);
            int width = 0;
            int height = 0;
            glfwGetFramebufferSize(window, &width, &height);

            send<void>(layer, "setContentsScale:", double(scale));
            send<void>(layer, "setDrawableSize:", CGSize{ double(width), double(height) });
            send<void>(view, "setWantsLayer:", BOOL(YES));
            send<void>(view, "setLayer:", layer);
            return layer;
        }
#endif
    } // namespace

    Window::Window(const char* title, uint32_t width, uint32_t height)
    {
        if (!glfwInit())
        {
            std::fprintf(stderr, "glfwInit failed\n");
            std::exit(EXIT_FAILURE);
        }
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        m_window = glfwCreateWindow(int(width), int(height), title, nullptr, nullptr);
        if (m_window == nullptr)
        {
            std::fprintf(stderr, "glfwCreateWindow failed\n");
            std::exit(EXIT_FAILURE);
        }

#if defined(__APPLE__)
        m_surface = createMetalLayer(m_window);
#elif defined(_WIN32)
        m_surface = glfwGetWin32Window(m_window);
#endif
    }

    Window::~Window()
    {
        glfwDestroyWindow(m_window);
        glfwTerminate();
    }

    bool Window::poll()
    {
        glfwPollEvents();
        if (glfwGetKey(m_window, GLFW_KEY_ESCAPE) == GLFW_PRESS)
        {
            glfwSetWindowShouldClose(m_window, GLFW_TRUE);
        }
        return !glfwWindowShouldClose(m_window);
    }

    rojoRHI::Extent2D Window::extent() const
    {
        int width = 0;
        int height = 0;
        glfwGetFramebufferSize(m_window, &width, &height);
        return { uint32_t(width), uint32_t(height) };
    }

    double Window::time() const
    {
        return glfwGetTime();
    }
} // namespace example
