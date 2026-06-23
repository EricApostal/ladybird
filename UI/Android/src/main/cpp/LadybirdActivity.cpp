/*
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include "ALooperEventLoopImplementation.h"
#include "JNIHelpers.h"
#include <AK/ByteString.h>
#include <AK/Format.h>
#include <AK/OwnPtr.h>
#include <LibCore/Environment.h>
#include <LibCore/EventLoop.h>
#include <LibCore/EventLoopImplementation.h>
#include <LibMain/Main.h>
#include <LibWebView/Application.h>
#include <LibWebView/Utilities.h>
#include <jni.h>

JavaVM* global_vm;
static jobject s_java_instance;
static jmethodID s_schedule_event_loop_method;
static jobject s_timer_service;

namespace Ladybird {

class Application final : public WebView::Application {
    WEB_VIEW_APPLICATION(Application)

public:
    virtual ~Application() override = default;

private:
    explicit Application(Optional<ByteString> ladybird_binary_path)
        : WebView::Application(move(ladybird_binary_path))
    {
    }

    virtual void create_platform_options(WebView::BrowserOptions&, WebView::RequestServerOptions& request_server_options, WebView::WebContentOptions& web_content_options) override
    {
        request_server_options.certificates.append(ByteString::formatted("{}/cacert.pem", WebView::s_ladybird_resource_root));

        web_content_options.force_cpu_painting = WebView::ForceCPUPainting::Yes;
    }

    virtual Core::EventLoop& create_platform_event_loop() override
    {
        auto& event_loop_manager = *new ALooperEventLoopManager(s_timer_service);
        event_loop_manager.on_did_post_event = [] {
            JavaEnvironment env(global_vm);
            env.get()->CallVoidMethod(s_java_instance, s_schedule_event_loop_method);
        };
        Core::EventLoopManager::install(event_loop_manager);

        return WebView::Application::create_platform_event_loop();
    }
};

}

static OwnPtr<Ladybird::Application> s_application;

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_initNativeCode(JNIEnv*, jobject, jstring, jstring, jobject, jstring, jstring);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_initNativeCode(JNIEnv* env, jobject thiz, jstring resource_dir, jstring tag_name, jobject timer_service, jstring user_dir, jstring native_library_dir)
{
    auto get_string = [&](jstring string) {
        char const* raw_string = env->GetStringUTFChars(string, nullptr);
        ByteString result { raw_string };
        env->ReleaseStringUTFChars(string, raw_string);
        return result;
    };

    auto resource_root = get_string(resource_dir);
    auto user_directory = get_string(user_dir);
    auto native_library_directory = get_string(native_library_dir);

    AK::set_log_tag_name(get_string(tag_name).characters());

    MUST(Core::Environment::set("LADYBIRD_RESOURCE_ROOT"sv, resource_root, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("HOME"sv, user_directory, Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_CONFIG_HOME"sv, ByteString::formatted("{}/config", user_directory), Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_DATA_HOME"sv, ByteString::formatted("{}/userdata", user_directory), Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("XDG_RUNTIME_DIR"sv, ByteString::formatted("{}/runtime", user_directory), Core::Environment::Overwrite::Yes));
    MUST(Core::Environment::set("TMPDIR"sv, ByteString::formatted("{}/cache", user_directory), Core::Environment::Overwrite::Yes));

    if (auto existing_library_path = Core::Environment::get("LD_LIBRARY_PATH"sv); existing_library_path.has_value())
        MUST(Core::Environment::set("LD_LIBRARY_PATH"sv, ByteString::formatted("{}:{}", native_library_directory, *existing_library_path), Core::Environment::Overwrite::Yes));
    else
        MUST(Core::Environment::set("LD_LIBRARY_PATH"sv, native_library_directory, Core::Environment::Overwrite::Yes));

    dbgln("Set resource root to {}", resource_root);

    env->GetJavaVM(&global_vm);
    VERIFY(global_vm);

    s_java_instance = env->NewGlobalRef(thiz);
    jclass clazz = env->GetObjectClass(s_java_instance);
    VERIFY(clazz);
    s_schedule_event_loop_method = env->GetMethodID(clazz, "scheduleEventLoop", "()V");
    VERIFY(s_schedule_event_loop_method);
    env->DeleteLocalRef(clazz);

    s_timer_service = env->NewGlobalRef(timer_service);

    static StringView s_program_name_argument = "ladybird"sv;
    static StringView s_devtools_argument = "--devtools=6000"sv;
    static StringView s_args[] = { s_program_name_argument, s_devtools_argument };

    Main::Arguments arguments = {
        .argc = 0,
        .argv = nullptr,
        .strings = Span<StringView> { s_args, 2 }
    };

    s_application = Ladybird::Application::create(arguments, native_library_directory).release_value_but_fixme_should_propagate_errors();
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_execMainEventLoop(JNIEnv*, jobject /* thiz */);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_execMainEventLoop(JNIEnv*, jobject /* thiz */)
{
    if (s_application)
        Core::EventLoop::current().pump(Core::EventLoop::WaitMode::PollForEvents);
}

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_disposeNativeCode(JNIEnv*, jobject /* thiz */);

extern "C" JNIEXPORT void JNICALL
Java_org_serenityos_ladybird_LadybirdActivity_disposeNativeCode(JNIEnv* env, jobject /* thiz */)
{
    s_application = nullptr;
    s_schedule_event_loop_method = nullptr;
    env->DeleteGlobalRef(s_java_instance);
    env->DeleteGlobalRef(s_timer_service);
    s_java_instance = nullptr;
    s_timer_service = nullptr;

    delete &Core::EventLoopManager::the();
}
