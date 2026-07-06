/*
 * Copyright (c) 2026, Ladybird contributors
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/NeverDestroyed.h>
#include <LibWeb/Bindings/PrincipalHostDefined.h>
#include <LibWeb/HTML/Scripting/Environments.h>
#include <LibWeb/Page/Page.h>
#include <LibWeb/ServiceWorker/ServiceWorkerAgent.h>

namespace Web::ServiceWorker {

GC_DEFINE_ALLOCATOR(ServiceWorkerAgent);

static HashMap<HTML::WorkerAgentOwnerToken, GC::Ref<ServiceWorkerAgent>>& service_worker_agents()
{
    static NeverDestroyed<HashMap<HTML::WorkerAgentOwnerToken, GC::Ref<ServiceWorkerAgent>>> map;
    return *map;
}

GC::Ref<ServiceWorkerAgent> ServiceWorkerAgent::create(GC::Ref<HTML::EnvironmentSettingsObject> client, URL::URL script_url, URL::URL scope_url, Bindings::WorkerType worker_type, StorageAPI::StorageKey storage_key)
{
    return client->realm().create<ServiceWorkerAgent>(client, move(script_url), move(scope_url), worker_type, move(storage_key));
}

ServiceWorkerAgent::ServiceWorkerAgent(GC::Ref<HTML::EnvironmentSettingsObject> client, URL::URL script_url, URL::URL scope_url, Bindings::WorkerType worker_type, StorageAPI::StorageKey storage_key)
    : m_client(client)
    , m_script_url(move(script_url))
    , m_scope_url(move(scope_url))
    , m_worker_type(worker_type)
    , m_storage_key(move(storage_key))
    , m_owner_token(HTML::next_worker_agent_owner_token())
{
}

void ServiceWorkerAgent::initialize(JS::Realm& realm)
{
    Base::initialize(realm);

    HTML::WorkerAgentStartRequest request {
        .url = m_script_url,
        .agent_type = Bindings::AgentType::ServiceWorker,
        .type = m_worker_type,
        .credentials = Bindings::RequestCredentials::SameOrigin,
        .name = String {},
        .outside_port = HTML::TransferDataEncoder {},
        .outside_settings = m_client->serialize(),
        .storage_key = m_storage_key,
        .caller_is_secure_context = HTML::is_secure_context(*m_client),
        .owner_token = m_owner_token,
        .scope_url = m_scope_url,
    };

    // NOTE: This blocking IPC call may launch another process, mirroring the identical tradeoff already
    //       accepted by WorkerAgentParent::initialize() for dedicated/shared workers.
    service_worker_agents().set(m_owner_token, *this);
    m_agent_id = Bindings::principal_host_defined_page(realm).client().start_worker_agent(move(request));
}

void ServiceWorkerAgent::did_finish_loading_worker_script(HTML::WorkerAgentOwnerToken owner_token)
{
    auto agent = service_worker_agents().find(owner_token);
    if (agent == service_worker_agents().end())
        return;
    if (agent->value->m_on_script_loaded)
        agent->value->m_on_script_loaded();
}

void ServiceWorkerAgent::did_fail_loading_worker_script(HTML::WorkerAgentOwnerToken owner_token)
{
    auto agent = service_worker_agents().find(owner_token);
    if (agent == service_worker_agents().end())
        return;
    if (agent->value->m_on_script_load_failed)
        agent->value->m_on_script_load_failed();
}

void ServiceWorkerAgent::dispatch_extendable_event(FlyString const& event_name, Function<void(bool completed_without_error)> on_complete)
{
    m_on_extendable_event_dispatched = move(on_complete);
    Bindings::principal_host_defined_page(m_client->realm()).client().dispatch_extendable_event(m_agent_id, event_name.to_string());
}

void ServiceWorkerAgent::did_dispatch_extendable_event(HTML::WorkerAgentOwnerToken owner_token, String event_name, bool completed_without_error)
{
    (void)event_name;
    auto agent = service_worker_agents().find(owner_token);
    if (agent == service_worker_agents().end())
        return;

    // NOTE: Move the callback out (clearing m_on_extendable_event_dispatched) *before* invoking it. The
    //       Install callback invokes Activate synchronously on success, which calls dispatch_extendable_event()
    //       again and reassigns m_on_extendable_event_dispatched - if that happened while this exact Function
    //       object were still executing (i.e. if we called `agent->value->m_on_extendable_event_dispatched(...)`
    //       directly), AK::Function's reentrancy guard trips a VERIFICATION FAILED crash on the reassignment.
    auto callback = move(agent->value->m_on_extendable_event_dispatched);
    if (callback)
        callback(completed_without_error);
}

void ServiceWorkerAgent::finalize()
{
    Base::finalize();

    service_worker_agents().remove(m_owner_token);

    if (m_agent_id != 0)
        Bindings::principal_host_defined_page(m_client->realm()).client().close_worker_agent(m_agent_id, m_owner_token);
}

void ServiceWorkerAgent::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_client);
}

}
