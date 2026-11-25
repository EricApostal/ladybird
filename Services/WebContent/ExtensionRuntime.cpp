/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/JsonArray.h>
#include <AK/JsonObject.h>
#include <AK/JsonValue.h>
#include <AK/StringBuilder.h>
#include <LibGC/Function.h>
#include <LibGC/Heap.h>
#include <LibJS/Runtime/AbstractOperations.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibJS/Runtime/NativeFunction.h>
#include <LibJS/Runtime/PrimitiveString.h>
#include <LibJS/Runtime/Realm.h>
#include <LibJS/Script.h>
#include <LibURL/Parser.h>
#include <LibWeb/Bindings/HostDefined.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MainThreadVM.h>
#include <LibWeb/Bindings/Wrappable.h>
#include <LibWeb/Bindings/WrapperWorld.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/BrowsingContext.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/Window.h>
#include <LibWeb/HTML/WindowProxy.h>
#include <LibWeb/Page/Page.h>
#include <WebContent/ConnectionFromClient.h>
#include <WebContent/ExtensionRuntime.h>

namespace WebContent {

GC_DEFINE_ALLOCATOR(ExtensionWorld);

// MARK: ExtensionWorld

GC::Ref<ExtensionWorld> ExtensionWorld::create(u64 context_id, String extension_uuid, Web::PageId page_id, Web::DOM::Document& document, Web::HTML::Window& window, JS::Realm& realm, Web::Bindings::WrapperWorld* world, OwnPtr<JS::ExecutionContext> execution_context)
{
    return GC::Heap::the().allocate<ExtensionWorld>(context_id, move(extension_uuid), page_id, document, window, realm, world, move(execution_context));
}

ExtensionWorld::ExtensionWorld(u64 context_id, String extension_uuid, Web::PageId page_id, Web::DOM::Document& document, Web::HTML::Window& window, JS::Realm& realm, Web::Bindings::WrapperWorld* world, OwnPtr<JS::ExecutionContext> execution_context)
    : m_context_id(context_id)
    , m_extension_uuid(move(extension_uuid))
    , m_page_id(page_id)
    , m_document(document)
    , m_window(window)
    , m_realm(realm)
    , m_world(world)
    , m_execution_context(move(execution_context))
{
}

void ExtensionWorld::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_document);
    visitor.visit(m_window);
    visitor.visit(m_realm);
    visitor.visit(m_world);
    visitor.visit(m_receiver);
    if (m_execution_context)
        m_execution_context->visit_edges(visitor);
}

void ExtensionWorld::detach()
{
    m_receiver = nullptr;
    // NB: An extension page runs in the document's own world, which is not ours to detach.
    if (m_world)
        m_world->detach();
}

// MARK: Content script rules

bool ExtensionContentScript::matches_url(URL::URL const& url) const
{
    auto any_matches = [&](Vector<MatchPattern> const& patterns) {
        for (auto const& pattern : patterns) {
            if (pattern.matches(url))
                return true;
        }
        return false;
    };

    if (!any_matches(matches) || any_matches(exclude_matches))
        return false;

    // include_globs and exclude_globs narrow what the match patterns allowed; they never widen it.
    auto serialized = url.serialize().bytes_as_string_view();
    if (!include_globs.is_empty()) {
        auto included = false;
        for (auto const& glob : include_globs) {
            if (glob_matches(glob, serialized, true, true)) {
                included = true;
                break;
            }
        }
        if (!included)
            return false;
    }
    for (auto const& glob : exclude_globs) {
        if (glob_matches(glob, serialized, true, true))
            return false;
    }
    return true;
}

// MARK: Helpers

static ExtensionRunAt parse_run_at(StringView value)
{
    if (value == "document_start"sv)
        return ExtensionRunAt::DocumentStart;
    if (value == "document_end"sv)
        return ExtensionRunAt::DocumentEnd;
    return ExtensionRunAt::DocumentIdle;
}

static Vector<String> read_strings(JsonObject const& object, StringView key)
{
    Vector<String> strings;
    if (auto array = object.get_array(key); array.has_value()) {
        array.value().for_each([&](JsonValue const& value) {
            if (value.is_string())
                strings.append(value.as_string());
        });
    }
    return strings;
}

static void read_files(JsonObject const& object, StringView key, bool is_css, ExtensionRunAt run_at, bool main_world, Vector<ExtensionScriptFile>& files)
{
    auto array = object.get_array(key);
    if (!array.has_value())
        return;

    array.value().for_each([&](JsonValue const& value) {
        if (!value.is_object())
            return;
        auto const& file = value.as_object();
        auto path = file.get_string("path"sv);
        auto source = file.get_string("source"sv);
        if (!path.has_value() || !source.has_value())
            return;
        files.append({ run_at, is_css, main_world, *path, *source });
    });
}

// Runs `source` as a classic script in `realm`. Anything it throws is logged and does not reach the page.
static bool run_script(JS::Realm& realm, String const& source, String const& filename)
{
    Web::HTML::TemporaryExecutionContext execution_context { realm };

    auto utf16_source = Utf16String::from_utf8(source);
    auto script = JS::Script::parse(utf16_source.utf16_view(), realm, filename.bytes_as_string_view());
    if (script.is_error()) {
        dbgln("ExtensionRuntime: could not parse {}", filename);
        return false;
    }

    auto result = realm.vm().run(script.value());
    if (result.is_throw_completion()) {
        // The default formatting of an error object says only what class it is, which is no help for a DOMException.
        auto thrown = result.throw_completion().value();
        auto description = thrown.to_utf16_string(realm.vm());
        if (description.is_error())
            dbgln("ExtensionRuntime: {} threw: {}", filename, thrown);
        else
            dbgln("ExtensionRuntime: {} threw: {}", filename, description.value());
        return false;
    }
    return true;
}

static String filename_for(String const& uuid, String const& path)
{
    return MUST(String::formatted("moz-extension://{}/{}", uuid, path));
}

// MARK: ExtensionRuntime

ExtensionRuntime& ExtensionRuntime::the()
{
    // Deliberately never destroyed: documents are torn down while the process exits.
    static auto* runtime = new ExtensionRuntime;
    return *runtime;
}

void ExtensionRuntime::set_connection(ConnectionFromClient& connection)
{
    m_connection = connection.make_weak_ptr<ConnectionFromClient>();
    Web::HTML::ExtensionHooks::install(this);
}

ExtensionEntry const* ExtensionRuntime::find_extension(StringView uuid) const
{
    for (auto const& extension : m_extensions) {
        if (extension.uuid == uuid)
            return &extension;
    }
    return nullptr;
}

void ExtensionRuntime::set_state(ReadonlyBytes bytes)
{
    auto json = JsonValue::from_string(StringView { bytes });
    if (json.is_error() || !json.value().is_object()) {
        dbgln("ExtensionRuntime: ignoring a state update that is not a JSON object");
        return;
    }
    auto const& root = json.value().as_object();

    Vector<ExtensionEntry> extensions;
    if (auto list = root.get_array("extensions"sv); list.has_value()) {
        list.value().for_each([&](JsonValue const& value) {
            if (!value.is_object())
                return;
            auto const& object = value.as_object();

            auto uuid = object.get_string("uuid"sv);
            if (!uuid.has_value())
                return;

            ExtensionEntry entry;
            entry.uuid = *uuid;
            entry.init = object.get_string("init"sv).value_or("{}"_string);

            if (auto scripts = object.get_array("contentScripts"sv); scripts.has_value()) {
                scripts.value().for_each([&](JsonValue const& script_value) {
                    if (!script_value.is_object())
                        return;
                    auto const& script_object = script_value.as_object();

                    ExtensionContentScript script;
                    for (auto const& source : read_strings(script_object, "matches"sv)) {
                        if (auto pattern = MatchPattern::parse(source); pattern.has_value())
                            script.matches.append(pattern.release_value());
                    }
                    for (auto const& source : read_strings(script_object, "excludeMatches"sv)) {
                        if (auto pattern = MatchPattern::parse(source); pattern.has_value())
                            script.exclude_matches.append(pattern.release_value());
                    }
                    for (auto const& glob : read_strings(script_object, "includeGlobs"sv))
                        script.include_globs.append(glob.to_byte_string());
                    for (auto const& glob : read_strings(script_object, "excludeGlobs"sv))
                        script.exclude_globs.append(glob.to_byte_string());
                    script.all_frames = script_object.get_bool("allFrames"sv).value_or(false);

                    auto run_at = parse_run_at(script_object.get_string("runAt"sv).value_or("document_idle"_string));
                    auto main_world = script_object.get_string("world"sv).value_or("ISOLATED"_string) == "MAIN"sv;
                    read_files(script_object, "js"sv, false, run_at, main_world, script.files);
                    read_files(script_object, "css"sv, true, run_at, main_world, script.files);

                    if (!script.matches.is_empty() && !script.files.is_empty())
                        entry.content_scripts.append(move(script));
                });
            }

            if (auto rules = object.get_array("webAccessible"sv); rules.has_value()) {
                rules.value().for_each([&](JsonValue const& rule_value) {
                    if (!rule_value.is_object())
                        return;
                    auto const& rule_object = rule_value.as_object();

                    ExtensionWebAccessibleRule rule;
                    for (auto const& resource : read_strings(rule_object, "resources"sv))
                        rule.resources.append(resource.to_byte_string());
                    for (auto const& source : read_strings(rule_object, "matches"sv)) {
                        if (auto pattern = MatchPattern::parse(source); pattern.has_value())
                            rule.matches.append(pattern.release_value());
                    }
                    entry.web_accessible.append(move(rule));
                });
            }

            extensions.append(move(entry));
        });
    }

    m_shim_source = root.get_string("shim"sv).value_or(String {});
    m_extensions = move(extensions);
    m_intercept_requests = root.get_bool("interceptRequests"sv).value_or(false);
}

void ExtensionRuntime::document_did_start(Web::DOM::Document& document)
{
    if (document.url().scheme() == "moz-extension"sv)
        dbgln("ExtensionRuntime: extension document {} became active ({} extensions known)", document.url(), m_extensions.size());

    if (m_extensions.is_empty())
        return;

    auto* browsing_context = document.browsing_context().ptr();
    if (!document.window() || !browsing_context) {
        dbgln("ExtensionRuntime: {} has no window or browsing context", document.url());
        return;
    }

    if (m_started_documents.set(&document) != HashSetResult::InsertedNewEntry)
        return;

    auto const& url = document.url();

    // One of an extension's own pages, such as its background page or popup.
    if (url.scheme() == "moz-extension"sv) {
        if (auto const* extension = find_extension(url.serialized_host()))
            inject_extension_page(document, *extension);
        else
            dbgln("ExtensionRuntime: no extension for host '{}'", url.serialized_host());
        return;
    }

    auto is_top_level = browsing_context->is_top_level();
    for (auto const& extension : m_extensions) {
        Vector<ExtensionScriptFile> matched;
        for (auto const& script : extension.content_scripts) {
            if (!is_top_level && !script.all_frames)
                continue;
            if (!script.matches_url(url))
                continue;
            matched.extend(script.files);
        }

        if (!matched.is_empty())
            inject_content_scripts(document, extension, move(matched));
    }
}

void ExtensionRuntime::inject_content_scripts(Web::DOM::Document& document, ExtensionEntry const& extension, Vector<ExtensionScriptFile> scripts)
{
    if (!m_connection.strong_ref())
        return;

    auto& vm = Web::Bindings::main_thread_vm();
    auto window = GC::Ref { *document.window() };
    auto browsing_context = GC::Ref { *document.browsing_context() };

    // Scripts that ask for the page's own world need no world of ours.
    Vector<ExtensionScriptFile> isolated_scripts;
    for (auto& script : scripts) {
        if (script.main_world) {
            if (script.run_at == ExtensionRunAt::DocumentStart && !script.is_css)
                run_script(window->principal_realm(), script.source, filename_for(extension.uuid, script.path));
        } else {
            isolated_scripts.append(move(script));
        }
    }
    if (isolated_scripts.is_empty())
        return;

    // The extension's world is a second global over the same window: same DOM, separate JavaScript objects, so the
    // page cannot see or tamper with what the extension's scripts define.
    auto extension_world = GC::Heap::the().allocate<Web::Bindings::WrapperWorld>(Web::Bindings::WrapperWorld::Type::Extension);
    auto execution_context = Web::Bindings::create_a_new_javascript_realm(
        vm,
        Web::HTML::CrossOriginIsolationMode::None,
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            return Web::Bindings::create_global_object_wrapper(realm, window);
        },
        [&](JS::Realm& realm) -> GC::Ref<JS::Object> {
            // The document is not the browsing context's active one yet, so the world's proxy would still point at the
            // window of the document before it, which is another origin to the proxy's access checks.
            auto* proxy = browsing_context->window_proxy_for(*extension_world, realm);
            proxy->set_window(window);
            return proxy->object();
        });

    auto& realm = *execution_context->realm();
    auto intrinsics = realm.create<Web::Bindings::Intrinsics>(realm);
    realm.set_host_defined(realm.heap().allocate<Web::Bindings::HostDefined>(intrinsics, *extension_world, window->principal_realm()));
    Web::Bindings::cache_global_object_wrapper(realm);

    // Firefox gives a content script the page's own window, with the page's own objects rather than the script's view
    // of them, as `wrappedJSObject`. Extensions use it to see what the page's scripts did.
    if (auto* page_window_proxy = browsing_context->window_proxy())
        realm.global_object().define_direct_property("wrappedJSObject"_utf16_fly_string, JS::Value { static_cast<JS::Object const*>(&page_window_proxy->object()) }, JS::Attribute::Configurable | JS::Attribute::Writable);

    if (Web::Bindings::initialize_window_web_interfaces(*window, realm).is_error()) {
        dbgln("ExtensionRuntime: could not set up a world for {}", extension.uuid);
        extension_world->detach();
        return;
    }

    auto context_id = m_next_context_id++;
    auto world = ExtensionWorld::create(context_id, extension.uuid, document.page().client().id(), document, *window, realm, extension_world.ptr(), move(execution_context));
    world->scripts() = move(isolated_scripts);
    m_worlds.set(context_id, GC::make_root(*world));

    start_context(document, extension, *world);
}

void ExtensionRuntime::inject_extension_page(Web::DOM::Document& document, ExtensionEntry const& extension)
{
    if (!m_connection.strong_ref()) {
        dbgln("ExtensionRuntime: no connection to the browser for {}", document.url());
        return;
    }

    // An extension's page is the extension's own document, so it has no world of its own to protect from the page: its
    // scripts run in the document's world and the bridge is defined on that global.
    auto window = GC::Ref { *document.window() };
    auto context_id = m_next_context_id++;
    auto world = ExtensionWorld::create(context_id, extension.uuid, document.page().client().id(), document, *window, window->principal_realm(), nullptr, nullptr);
    m_worlds.set(context_id, GC::make_root(*world));

    start_context(document, extension, *world);
}

void ExtensionRuntime::start_context(Web::DOM::Document& document, ExtensionEntry const& extension, ExtensionWorld& world)
{
    auto connection = m_connection.strong_ref();
    if (!connection)
        return;

    auto& realm = world.realm();

    // Native functions belong to the VM's current realm unless told otherwise, and nothing is running yet.
    Web::HTML::TemporaryExecutionContext temporary_execution_context { realm };

    // The context's only way out is two functions: one that sends a string to the browser, and one that registers where
    // the browser's strings are delivered. Everything else an extension can do is built from them in the shim, which
    // removes them from the global again once it holds them.
    auto& global = realm.global_object();

    global.define_direct_property(
        "__bird_send"_utf16_fly_string,
        JS::NativeFunction::create(
            realm,
            GC::create_function(GC::Heap::the(), GC::captures(world), [](auto& world, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto payload = TRY(vm.argument(0).to_utf16_string(vm));
                ExtensionRuntime::the().send_from_context(world, payload.to_utf8());
                return JS::js_undefined();
            }),
            1),
        JS::default_attributes);

    global.define_direct_property(
        "__bird_set_receiver"_utf16_fly_string,
        JS::NativeFunction::create(
            realm,
            GC::create_function(GC::Heap::the(), GC::captures(world), [](auto& world, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto receiver = vm.argument(0);
                if (receiver.is_function())
                    world.set_receiver(receiver.as_object());
                return JS::js_undefined();
            }),
            1),
        JS::default_attributes);

    // Runs code for tabs.executeScript. `eval` is not used for this, because the page's Content-Security-Policy applies
    // to it and does not apply to an extension's scripts.
    global.define_direct_property(
        "__bird_eval"_utf16_fly_string,
        JS::NativeFunction::create(
            realm,
            GC::create_function(GC::Heap::the(), GC::captures(world), [](auto& world, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto source = TRY(vm.argument(0).to_utf16_string(vm));
                auto script = JS::Script::parse(source.utf16_view(), world.realm(), "moz-extension://bird/executeScript.js"sv);
                if (script.is_error()) {
                    auto const& errors = script.error();
                    return vm.throw_completion<JS::SyntaxError>(errors.is_empty() ? Utf16String::from_utf8("Syntax error"sv) : errors.first().message);
                }
                return vm.run(script.value());
            }),
            1),
        JS::default_attributes);

    global.define_direct_property(
        "__bird_phase"_utf16_fly_string,
        JS::NativeFunction::create(
            realm,
            GC::create_function(GC::Heap::the(), GC::captures(world), [](auto& world, JS::VM& vm) -> JS::ThrowCompletionOr<JS::Value> {
                auto phase = TRY(vm.argument(0).to_utf16_string(vm)).to_utf8();
                if (phase == "document_end"sv)
                    ExtensionRuntime::the().run_phase(world, ExtensionRunAt::DocumentEnd);
                else if (phase == "document_idle"sv)
                    ExtensionRuntime::the().run_phase(world, ExtensionRunAt::DocumentIdle);
                return JS::js_undefined();
            }),
            1),
        JS::default_attributes);

    auto is_top_level = document.browsing_context() && document.browsing_context()->is_top_level();
    connection->async_extension_context_created(world.page_id(), world.context_id(), extension.uuid, document.url().serialize(), is_top_level);

    // The extension's own data first, then the shim that builds `browser` on the functions above. The shim also
    // arranges for the document_end and document_idle scripts to be requested through __bird_phase.
    auto init_source = MUST(String::formatted("globalThis.__bird_init = {};", extension.init));
    run_script(realm, init_source, "moz-extension://bird/init.js"_string);
    if (!m_shim_source.is_empty())
        run_script(realm, m_shim_source, "moz-extension://bird/shim.js"_string);

    run_phase(world, ExtensionRunAt::DocumentStart);
}

void ExtensionRuntime::run_phase(ExtensionWorld& world, ExtensionRunAt phase)
{
    // Copy first: a script that runs here can cause the world's script list to change.
    Vector<ExtensionScriptFile> due;
    for (auto& script : world.scripts()) {
        if (script.run_at == phase)
            due.append(script);
    }

    for (auto const& script : due) {
        auto filename = filename_for(world.extension_uuid(), script.path);
        if (!script.is_css) {
            run_script(world.realm(), script.source, filename);
            continue;
        }

        // The shim defines how a stylesheet is applied, so the stylesheet is handed to it as a JSON string.
        auto escaped = JsonValue { script.source }.serialized();
        auto call = MUST(String::formatted("if (typeof __bird_insert_css === 'function') __bird_insert_css({});", escaped));
        run_script(world.realm(), call, filename);
    }
}

void ExtensionRuntime::send_from_context(ExtensionWorld& world, String payload)
{
    if (auto connection = m_connection.strong_ref())
        connection->async_extension_message_from_context(world.page_id(), world.context_id(), move(payload));
}

void ExtensionRuntime::deliver_to_context(u64 context_id, String const& payload)
{
    auto it = m_worlds.find(context_id);
    if (it == m_worlds.end())
        return;

    auto& world = *it->value;
    auto receiver = world.receiver();
    if (!receiver)
        return;

    auto& vm = world.realm().vm();
    Web::HTML::TemporaryExecutionContext execution_context { world.realm() };

    auto message = JS::PrimitiveString::create(vm, Utf16String::from_utf8(payload));
    auto result = JS::call(vm, JS::Value { receiver }, JS::js_undefined(), message);
    if (result.is_throw_completion())
        dbgln("ExtensionRuntime: a message receiver threw: {}", result.throw_completion().value());
}

void ExtensionRuntime::document_will_be_destroyed(Web::DOM::Document& document)
{
    m_started_documents.remove(&document);

    if (m_worlds.is_empty())
        return;

    Vector<u64> doomed;
    for (auto const& entry : m_worlds) {
        if (&entry.value->document() == &document)
            doomed.append(entry.key);
    }

    auto connection = m_connection.strong_ref();
    for (auto context_id : doomed) {
        auto world = m_worlds.take(context_id).release_value();
        if (connection)
            connection->async_extension_context_destroyed(world->page_id(), context_id);
        world->detach();
    }
}

// MARK: Extension files

bool ExtensionRuntime::allows_extension_resource(URL::URL const& url, URL::Origin const* requesting_origin)
{
    auto const* extension = find_extension(url.serialized_host());
    if (!extension)
        return false;

    // An extension's own documents may load its files.
    if (requesting_origin && !requesting_origin->is_opaque() && requesting_origin->scheme() == "moz-extension"sv) {
        auto requesting_extension = requesting_origin->host().serialize();
        if (requesting_extension == url.serialized_host())
            return true;
    }

    // Anything else may load only what the extension made web accessible, to origins it named.
    if (!requesting_origin)
        return false;

    auto decoded = URL::percent_decode(url.serialize_path());
    auto path = decoded.view();
    while (path.starts_with('/'))
        path = path.substring_view(1);

    auto requester = URL::Parser::basic_parse(MUST(String::formatted("{}/", requesting_origin->serialize())));

    for (auto const& rule : extension->web_accessible) {
        auto names_the_file = false;
        for (auto const& glob : rule.resources) {
            auto pattern = glob.view();
            while (pattern.starts_with('/'))
                pattern = pattern.substring_view(1);
            if (glob_matches(pattern, path, false, false)) {
                names_the_file = true;
                break;
            }
        }
        if (!names_the_file || !requester.has_value())
            continue;

        for (auto const& pattern : rule.matches) {
            if (pattern.matches(*requester))
                return true;
        }
    }
    return false;
}

void ExtensionRuntime::load_extension_resource(URL::URL const& url, Web::PageId page_id, Function<void(Optional<ExtensionResource>)> callback)
{
    auto connection = m_connection.strong_ref();
    if (!connection) {
        callback({});
        return;
    }

    auto request_id = m_next_resource_request_id++;
    m_pending_resources.set(request_id, move(callback));
    connection->async_request_extension_resource(page_id, request_id, url);
}

void ExtensionRuntime::resource_loaded(u64 request_id, bool success, ReadonlyBytes data, i64 modified_time)
{
    auto callback = m_pending_resources.take(request_id);
    if (!callback.has_value())
        return;

    if (!success) {
        (*callback)({});
        return;
    }

    auto copy = ByteBuffer::copy(data);
    if (copy.is_error()) {
        (*callback)({});
        return;
    }

    ExtensionResource resource { copy.release_value(), {} };
    if (modified_time > 0)
        resource.modified_time = static_cast<time_t>(modified_time);
    (*callback)(move(resource));
}

// MARK: Requests

void ExtensionRuntime::intercept_request(InterceptedRequest request, Function<void(InterceptVerdict)> callback)
{
    auto connection = m_connection.strong_ref();
    if (!connection) {
        callback({});
        return;
    }

    // The details an extension's webRequest listener is given.
    JsonObject details;
    details.set("requestId"sv, request.request_id);
    details.set("url"sv, request.url.serialize());
    details.set("method"sv, MUST(String::from_byte_string(request.method)));
    details.set("type"sv, request.resource_type);
    details.set("documentUrl"sv, request.document_url.serialize());
    details.set("originUrl"sv, request.document_url.serialize());
    details.set("frameId"sv, request.is_top_level ? 0 : 1);
    details.set("parentFrameId"sv, request.is_top_level ? -1 : 0);

    auto request_id = m_next_intercept_id++;
    m_pending_intercepts.set(request_id, move(callback));
    connection->async_request_extension_intercept(request.page_id, request_id, JsonValue { move(details) }.serialized());
}

void ExtensionRuntime::intercept_verdict(u64 request_id, String const& verdict_json)
{
    auto callback = m_pending_intercepts.take(request_id);
    if (!callback.has_value())
        return;

    InterceptVerdict verdict;
    if (auto json = JsonValue::from_string(verdict_json); !json.is_error() && json.value().is_object()) {
        auto const& object = json.value().as_object();
        verdict.cancel = object.get_bool("cancel"sv).value_or(false);

        if (auto redirect = object.get_string("redirectUrl"sv); redirect.has_value())
            verdict.redirect_url = URL::Parser::basic_parse(*redirect);

        if (auto headers = object.get_array("requestHeaders"sv); headers.has_value()) {
            headers.value().for_each([&](JsonValue const& value) {
                if (!value.is_object())
                    return;
                auto name = value.as_object().get_string("name"sv);
                auto header_value = value.as_object().get_string("value"sv);
                if (name.has_value() && header_value.has_value())
                    verdict.request_headers.append({ name->to_byte_string(), header_value->to_byte_string() });
            });
        }
    }
    (*callback)(move(verdict));
}

}
