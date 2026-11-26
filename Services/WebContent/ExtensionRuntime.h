/*
 * Copyright (c) 2026-present, the Ladybird developers.
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteString.h>
#include <AK/HashMap.h>
#include <AK/HashTable.h>
#include <AK/Optional.h>
#include <AK/String.h>
#include <AK/Vector.h>
#include <AK/WeakPtr.h>
#include <LibGC/Ptr.h>
#include <LibGC/Root.h>
#include <LibJS/Heap/Cell.h>
#include <LibJS/Runtime/ExecutionContext.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/ExtensionHooks.h>
#include <LibWebCommon/Page/PageId.h>
#include <WebContent/ExtensionMatching.h>

namespace WebContent {

class ConnectionFromClient;

enum class ExtensionRunAt : u8 {
    DocumentStart,
    DocumentEnd,
    DocumentIdle,
};

// One file of a content script that matched a document, ready to run.
struct ExtensionScriptFile {
    ExtensionRunAt run_at { ExtensionRunAt::DocumentIdle };
    bool is_css { false };
    // Scripts of the page's own world, rather than the extension's isolated one.
    bool main_world { false };
    String path;
    String source;
};

// What a content script entry of the browser's extension state says.
struct ExtensionContentScript {
    Vector<MatchPattern> matches;
    Vector<MatchPattern> exclude_matches;
    Vector<ByteString> include_globs;
    Vector<ByteString> exclude_globs;
    bool all_frames { false };
    // Whether an about:blank or about:srcdoc frame matches when what made it does.
    bool match_about_blank { false };
    Vector<ExtensionScriptFile> files;

    bool matches_url(URL::URL const&) const;
};

// Files an extension lets web pages load: `web_accessible_resources`.
struct ExtensionWebAccessibleRule {
    Vector<ByteString> resources;
    Vector<MatchPattern> matches;
};

struct ExtensionEntry {
    String uuid;
    // JSON text for what the extension's scripts read as `__bird_init`.
    String init;
    Vector<ExtensionContentScript> content_scripts;
    Vector<ExtensionWebAccessibleRule> web_accessible;
    Vector<MatchPattern> host_permissions;
};

// One extension's JavaScript context in one document: the isolated world its content scripts run in, or the main world
// of one of its own pages. It holds what the context needs to stay alive and the scripts that are yet to run in it.
class ExtensionWorld final : public JS::Cell {
    GC_CELL(ExtensionWorld, JS::Cell);
    GC_DECLARE_ALLOCATOR(ExtensionWorld);

public:
    using JSValueConversionIsForbidden = void;

    // `world` and `execution_context` are null for an extension page, which runs in the document's own world.
    static GC::Ref<ExtensionWorld> create(u64 context_id, String extension_uuid, Web::PageId, Web::DOM::Document&, Web::HTML::Window&, JS::Realm&, Web::Bindings::WrapperWorld*, OwnPtr<JS::ExecutionContext>);

    u64 context_id() const { return m_context_id; }
    String const& extension_uuid() const { return m_extension_uuid; }
    Web::PageId page_id() const { return m_page_id; }
    Web::DOM::Document& document() const { return *m_document; }
    Web::HTML::Window& window() const { return *m_window; }
    JS::Realm& realm() const { return *m_realm; }

    Vector<ExtensionScriptFile>& scripts() { return m_scripts; }
    void set_receiver(JS::Object& receiver) { m_receiver = receiver; }
    GC::Ptr<JS::Object> receiver() const { return m_receiver; }

    bool is_isolated() const { return m_world != nullptr; }
    // The modules the world has imported, by address, so that each is one module however often it is imported.
    GC::Ptr<JS::Module> module(String const& url) const;
    void add_module(String const& url, GC::Ref<JS::Module> module) { m_modules.set(url, module); }

    // Must be called before an isolated world is dropped, or its wrappers outlive it.
    void detach();

private:
    ExtensionWorld(u64 context_id, String extension_uuid, Web::PageId, Web::DOM::Document&, Web::HTML::Window&, JS::Realm&, Web::Bindings::WrapperWorld*, OwnPtr<JS::ExecutionContext>);

    virtual void visit_edges(Visitor&) override;

    u64 m_context_id { 0 };
    String m_extension_uuid;
    Web::PageId m_page_id;
    GC::Ref<Web::DOM::Document> m_document;
    GC::Ref<Web::HTML::Window> m_window;
    GC::Ref<JS::Realm> m_realm;
    GC::Ptr<Web::Bindings::WrapperWorld> m_world;
    OwnPtr<JS::ExecutionContext> m_execution_context;
    GC::Ptr<JS::Object> m_receiver;
    Vector<ExtensionScriptFile> m_scripts;
    HashMap<String, GC::Ref<JS::Module>> m_modules;
};

// Runs the browser's extensions inside this process: it decides which content scripts a new document gets, gives them
// a world of their own with a bridge to the browser, gives an extension's own pages the same bridge, and carries
// messages between those contexts and the browser process. It speaks only in strings, so what an extension's API means
// is decided entirely by the browser. It also asks the browser for the files of extensions, which this process may not
// read.
class ExtensionRuntime final : public Web::HTML::ExtensionHooks {
public:
    static ExtensionRuntime& the();

    void set_connection(ConnectionFromClient&);
    void set_state(ReadonlyBytes json);
    void deliver_to_context(u64 context_id, String const& payload);
    void resource_loaded(u64 request_id, bool success, ReadonlyBytes data, i64 modified_time);
    void intercept_verdict(u64 request_id, String const& verdict_json);

    void send_from_context(ExtensionWorld&, String payload);
    void run_phase(ExtensionWorld&, ExtensionRunAt);

    virtual void document_did_start(Web::DOM::Document&) override;
    virtual void document_will_be_destroyed(Web::DOM::Document&) override;
    virtual bool allows_extension_resource(URL::URL const&, URL::Origin const*) override;
    virtual bool allows_cross_origin_request(URL::Origin const&, URL::URL const&) override;
    virtual bool load_extension_module(JS::Realm&, Utf16View specifier, GC::Ref<ModuleLoadCompletion>) override;
    virtual void document_element_inserted(Web::DOM::Document&) override;
    virtual Optional<String> content_script_extension(JS::Realm&) override;
    virtual bool wants_response_interception() override { return m_intercept_responses; }
    virtual void load_extension_resource(URL::URL const&, Web::PageId, Function<void(Optional<ExtensionResource>)>) override;
    virtual bool wants_request_interception() override { return m_intercept_requests; }
    virtual void intercept_request(InterceptedRequest, Function<void(InterceptVerdict)>) override;

private:
    ExtensionRuntime() = default;

    // Runs, in a web page, the content scripts that match it.
    void run_content_scripts(Web::DOM::Document&);
    // Gives `world` its bridge to the browser and runs the extension's shim in it. Returns after the document_start
    // scripts of the world have run.
    void start_context(Web::DOM::Document&, ExtensionEntry const&, ExtensionWorld&);
    void inject_content_scripts(Web::DOM::Document&, ExtensionEntry const&, Vector<ExtensionScriptFile>);
    void inject_extension_page(Web::DOM::Document&, ExtensionEntry const&);
    ExtensionEntry const* find_extension(StringView uuid) const;

    WeakPtr<ConnectionFromClient> m_connection;
    String m_shim_source;
    Vector<ExtensionEntry> m_extensions;
    HashMap<u64, GC::Root<ExtensionWorld>> m_worlds;
    // Documents that have had their scripts decided: a document becomes active again when history returns to it.
    HashTable<Web::DOM::Document*> m_started_documents;
    // Documents that became active before they had a root element, whose content scripts wait for it.
    HashTable<Web::DOM::Document*> m_documents_awaiting_root;
    u64 m_next_context_id { 1 };

    HashMap<u64, Function<void(Optional<ExtensionResource>)>> m_pending_resources;
    u64 m_next_resource_request_id { 1 };

    // Whether an extension wants to see requests before they are sent, and those waiting to hear what to do.
    bool m_intercept_requests { false };
    bool m_intercept_responses { false };
    HashMap<u64, Function<void(InterceptVerdict)>> m_pending_intercepts;
    u64 m_next_intercept_id { 1 };
};

}
