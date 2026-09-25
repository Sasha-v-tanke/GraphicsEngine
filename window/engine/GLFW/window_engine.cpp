#include "window_engine.h"

#include <algorithm>
#include <exception>
#include <format>
#include <glfw.h>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>

#include <lib/common/error/error.h>
#include <lib/common/error/exception.h>
#include <lib/common/wrapper/non_transferable.h>
#include <window/engine/application_thread.h>
#include <window/engine/event_queue.h>
#include <window/engine/event_sink.h>

namespace NWindow::NEngine::NGlfw {

namespace {

std::string GetGlfwErrorMessage(std::string_view fallback) {
    const char* description = nullptr;
    const int error = glfwGetError(&description);

    if (description == nullptr) {
        return std::string{fallback};
    }

    return std::format("{}: {} ({})", fallback, description, error);
}

class GlfwRuntime final: public NCommon::NonTransferable {
public:
    static std::shared_ptr<GlfwRuntime> Acquire() {
        ValidateApplicationThread("GLFW runtime initialization");

        std::lock_guard lock{GetMutex()};

        std::weak_ptr<GlfwRuntime>& weakRuntime = GetWeakRuntime();
        std::shared_ptr<GlfwRuntime> runtime = weakRuntime.lock();

        if (runtime != nullptr) {
            return runtime;
        }

        runtime = std::shared_ptr<GlfwRuntime>{new GlfwRuntime()};
        weakRuntime = runtime;

        return runtime;
    }

    ~GlfwRuntime() {
        if (!IsThread()) {
            std::terminate();
        }

        ValidateApplicationThread("GLFW runtime termination");

        glfwTerminate();
    }

    void ValidateThread() const {
        if (!IsThread()) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "GLFW window operations must run on the runtime main thread");
        }
    }

    [[nodiscard]] bool IsThread() const {
        return std::this_thread::get_id() == m_threadId;
    }

private:
    GlfwRuntime()
        : m_threadId(std::this_thread::get_id()) {
        ValidateApplicationThread("glfwInit");

        if (glfwInit() == GLFW_FALSE) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "{}",
                                  GetGlfwErrorMessage("Failed to initialize GLFW"));
        }
    }

    static std::mutex& GetMutex() {
        static std::mutex mutex;
        return mutex;
    }

    static std::weak_ptr<GlfwRuntime>& GetWeakRuntime() {
        static std::weak_ptr<GlfwRuntime> runtime;
        return runtime;
    }

private:
    std::thread::id m_threadId;
};

class WindowEngine final: public IWindowEngine {
public:
    explicit WindowEngine(const WindowConfig& config)
        : m_runtime(GlfwRuntime::Acquire())
        , m_window(CreateWindow(config)) {
        glfwSetWindowUserPointer(m_window.get(), this);
        glfwSetWindowSizeCallback(m_window.get(), &WindowEngine::HandleWindowSize);
        glfwSetFramebufferSizeCallback(m_window.get(), &WindowEngine::HandleFramebufferSize);
        glfwSetWindowCloseCallback(m_window.get(), &WindowEngine::HandleWindowClose);
    }

    ~WindowEngine() override {
        if (!m_runtime->IsThread()) {
            std::terminate();
        }
    }

    void AttachEventSink(IWindowEventSink& eventSink) override {
        m_runtime->ValidateThread();

        m_eventSink = &eventSink;
    }

    void SetTitle(std::string_view title) override {
        m_runtime->ValidateThread();

        glfwSetWindowTitle(m_window.get(), std::string{title}.c_str());
    }

    void SetSize(WindowSize size) override {
        m_runtime->ValidateThread();

        glfwSetWindowSize(m_window.get(), ClampSize(size.Width), ClampSize(size.Height));
    }

    [[nodiscard]] WindowSize GetSize() const override {
        m_runtime->ValidateThread();

        int width = 0;
        int height = 0;

        glfwGetWindowSize(m_window.get(), &width, &height);

        return {
                .Width = std::max(width, 0),
                .Height = std::max(height, 0),
        };
    }

    [[nodiscard]] WindowSize GetFramebufferSize() const override {
        m_runtime->ValidateThread();

        int width = 0;
        int height = 0;

        glfwGetFramebufferSize(m_window.get(), &width, &height);

        return {
                .Width = std::max(width, 0),
                .Height = std::max(height, 0),
        };
    }

    [[nodiscard]] bool ShouldClose() const override {
        m_runtime->ValidateThread();

        return glfwWindowShouldClose(m_window.get()) == GLFW_TRUE;
    }

    void RequestClose() override {
        m_runtime->ValidateThread();

        glfwSetWindowShouldClose(m_window.get(), GLFW_TRUE);
    }

    void ProcessEvents() override {
        m_runtime->ValidateThread();

        glfwPollEvents();

        DispatchQueuedEvents();
    }

private:
    struct WindowDeleter {
        void operator()(GLFWwindow* window) const noexcept {
            glfwDestroyWindow(window);
        }
    };

    using WindowPtr = std::unique_ptr<GLFWwindow, WindowDeleter>;

    static int ClampSize(int value) {
        return std::max(value, 1);
    }

    static WindowPtr CreateWindow(const WindowConfig& config) {
        ValidateApplicationThread("GLFW window creation");

        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);

        GLFWwindow* window = glfwCreateWindow(ClampSize(config.Size.Width),
                                              ClampSize(config.Size.Height),
                                              config.Title.c_str(),
                                              nullptr,
                                              nullptr);

        if (window == nullptr) {
            GRAPHICS_ENGINE_THROW(NCommon::EError::INVALID_STATE,
                                  "{}",
                                  GetGlfwErrorMessage("Failed to create GLFW window"));
        }

        return WindowPtr{window};
    }

    static WindowEngine& GetEngine(GLFWwindow* window) {
        return *static_cast<WindowEngine*>(glfwGetWindowUserPointer(window));
    }

    static WindowSize MakeSize(int width, int height) {
        return {
                .Width = std::max(width, 0),
                .Height = std::max(height, 0),
        };
    }

    static void HandleWindowSize(GLFWwindow* window, int width, int height) {
        WindowEngine& engine = GetEngine(window);

        engine.m_eventQueue.Enqueue({
                .Type = EWindowEventType::RESIZE,
                .Size = MakeSize(width, height),
        });
    }

    static void HandleFramebufferSize(GLFWwindow* window, int width, int height) {
        WindowEngine& engine = GetEngine(window);

        engine.m_eventQueue.Enqueue({
                .Type = EWindowEventType::FRAMEBUFFER_RESIZE,
                .Size = MakeSize(width, height),
        });
    }

    static void HandleWindowClose(GLFWwindow* window) {
        WindowEngine& engine = GetEngine(window);

        engine.m_eventQueue.Enqueue({
                .Type = EWindowEventType::CLOSE,
        });
    }

    void DispatchQueuedEvents() {
        if (m_eventSink == nullptr) {
            return;
        }

        m_eventQueue.Dispatch(*m_eventSink);
    }

private:
    std::shared_ptr<GlfwRuntime> m_runtime;
    WindowPtr m_window;
    IWindowEventSink* m_eventSink = nullptr;
    WindowEventQueue m_eventQueue;
};

} // namespace

std::unique_ptr<IWindowEngine> CreateWindowEngine(const WindowConfig& config) {
    return std::make_unique<WindowEngine>(config);
}

} // namespace NWindow::NEngine::NGlfw
