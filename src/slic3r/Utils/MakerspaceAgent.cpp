#include "MakerspaceAgent.hpp"
#include "Http.hpp"
#include <boost/log/trivial.hpp>
#include <boost/filesystem.hpp>
#include <fstream>
#include <sstream>
#include <random>
#include <iomanip>

namespace fs = boost::filesystem;

namespace Slic3r {

MakerspaceAgent::MakerspaceAgent() = default;

void MakerspaceAgent::set_supabase_url(const std::string& url)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_supabase_url = url;
    // Strip trailing slash
    if (!m_supabase_url.empty() && m_supabase_url.back() == '/')
        m_supabase_url.pop_back();
}

void MakerspaceAgent::set_supabase_anon_key(const std::string& key)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_supabase_anon_key = key;
}

void MakerspaceAgent::set_local_only_presets(const std::string& semicolon_separated)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_local_only_presets.clear();
    std::istringstream ss(semicolon_separated);
    std::string token;
    while (std::getline(ss, token, ';')) {
        if (!token.empty())
            m_local_only_presets.push_back(token);
    }
}

bool MakerspaceAgent::is_configured() const
{
    return !m_supabase_url.empty() && !m_supabase_anon_key.empty();
}

// ============================================================================
// Lifecycle
// ============================================================================

int MakerspaceAgent::set_config_dir(std::string config_dir)
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_config_dir = std::move(config_dir);
    m_session_path = (fs::path(m_config_dir) / "makerspace_session.json").string();
    return 0;
}

int MakerspaceAgent::start()
{
    if (!is_configured())
        return 0;
    if (!m_session_path.empty())
        load_session();
    return 0;
}

// ============================================================================
// Session Management
// ============================================================================

int MakerspaceAgent::change_user(std::string user_info)
{
    if (user_info.empty())
        return -1;

    try {
        auto j = nlohmann::json::parse(user_info);

        // Token injection format
        if (j.contains("access_token") && j.contains("refresh_token")) {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_session.access_token  = j["access_token"].get<std::string>();
            m_session.refresh_token = j["refresh_token"].get<std::string>();
            m_session.user_id       = j.value("user_id", "");
            m_session.user_email    = j.value("user_email", "");
            m_session.logged_in     = true;
            save_session();
            return 0;
        }

        // Email/password login
        if (j.contains("email") && j.contains("password")) {
            std::string email    = j["email"].get<std::string>();
            std::string password = j["password"].get<std::string>();
            return login_with_password(email, password) ? 0 : -1;
        }

    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent::change_user parse error: " << e.what();
    }
    return -1;
}

bool MakerspaceAgent::is_user_login()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.logged_in && !m_session.access_token.empty();
}

int MakerspaceAgent::user_logout(bool request)
{
    if (request && is_user_login()) {
        std::string response;
        unsigned code = 0;
        auth_post("/logout", "{}", &response, &code);
    }
    clear_session();
    return 0;
}

std::string MakerspaceAgent::get_user_id()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.user_id;
}

std::string MakerspaceAgent::get_user_name()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.user_email;
}

std::string MakerspaceAgent::get_user_nickname()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.user_email;
}

// ============================================================================
// Login UI Support
// ============================================================================

std::string MakerspaceAgent::build_login_cmd()
{
    nlohmann::json j;
    j["provider"]     = MAKERSPACE_CLOUD_PROVIDER;
    j["supabase_url"] = m_supabase_url;
    return j.dump();
}

std::string MakerspaceAgent::build_logout_cmd()
{
    nlohmann::json j;
    j["command"] = "makerspace_logout";
    return j.dump();
}

std::string MakerspaceAgent::build_login_info()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    nlohmann::json j;
    j["logged_in"]  = m_session.logged_in;
    j["user_id"]    = m_session.user_id;
    j["user_email"] = m_session.user_email;
    return j.dump();
}

// ============================================================================
// Token Access
// ============================================================================

std::string MakerspaceAgent::get_access_token() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.access_token;
}

std::string MakerspaceAgent::get_refresh_token() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_session.refresh_token;
}

bool MakerspaceAgent::ensure_token_fresh(const std::string& reason)
{
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_session.logged_in || m_session.access_token.empty())
            return false;
        if (m_session.refresh_token.empty())
            return true;
    }
    // Release the lock before calling try_refresh_token (which acquires it internally)
    bool ok = try_refresh_token();
    if (!ok)
        BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: token refresh failed (" << reason << ")";
    return ok;
}

// ============================================================================
// Server Connectivity
// ============================================================================

std::string MakerspaceAgent::get_cloud_service_host()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_supabase_url;
}

int MakerspaceAgent::connect_server()
{
    if (!is_configured()) {
        m_server_connected = false;
        return -1;
    }

    // Hit the Supabase REST health endpoint
    std::string url = rest_url("") + "/";
    std::string body;
    unsigned code = 0;

    auto http = Http::get(url);
    http.tls_verify(true)
        .header("apikey", m_supabase_anon_key)
        .timeout_max(10)
        .on_complete([&](std::string b, unsigned s) { body = b; code = s; })
        .on_error([&](std::string, std::string err, unsigned s) {
            code = s;
            BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: connect_server error: " << err;
        })
        .perform_sync();

    m_server_connected = (code >= 200 && code < 300);
    if (m_on_server_connected_fn) {
        CloudEvent ev;
        ev.provider = MAKERSPACE_CLOUD_PROVIDER;
        m_on_server_connected_fn(ev, m_server_connected ? 0 : 1, 0);
    }
    return m_server_connected ? 0 : -1;
}

bool MakerspaceAgent::is_server_connected()
{
    return m_server_connected;
}

int MakerspaceAgent::refresh_connection()
{
    return connect_server();
}

// ============================================================================
// Settings Synchronization
// ============================================================================

int MakerspaceAgent::get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets)
{
    if (!user_presets || !is_user_login() || !is_configured())
        return -1;

    ensure_token_fresh("get_user_presets");

    // Fetch user's own profiles
    std::string body;
    unsigned code = 0;
    int rc = rest_get("makerspace_profiles", "select=*", &body, &code);
    if (rc != 0 || code < 200 || code >= 300) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: get_user_presets failed, http=" << code;
        return -1;
    }

    try {
        auto arr = nlohmann::json::parse(body);
        for (const auto& row : arr) {
            std::string profile_type = row.value("profile_type", "");
            std::string id           = row.value("id", "");
            auto content             = row.value("content", nlohmann::json::object());
            if (profile_type.empty() || id.empty())
                continue;
            (*user_presets)[profile_type][id] = content.dump();
        }
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: get_user_presets parse error: " << e.what();
        return -1;
    }

    // Fetch shared profiles (admin-uploaded, visible to all)
    std::string shared_body;
    unsigned shared_code = 0;
    int src = rest_get("makerspace_shared_profiles", "select=*&is_public=eq.true", &shared_body, &shared_code);
    if (src == 0 && shared_code >= 200 && shared_code < 300) {
        try {
            auto arr = nlohmann::json::parse(shared_body);
            for (const auto& row : arr) {
                std::string profile_type = row.value("profile_type", "");
                std::string id           = "shared_" + row.value("id", "");
                auto content             = row.value("content", nlohmann::json::object());
                if (profile_type.empty())
                    continue;
                // Tag shared profiles so the slicer can display them differently
                auto content_mut = content;
                content_mut["makerspace_shared"] = true;
                (*user_presets)[profile_type][id] = content_mut.dump();
            }
        } catch (const std::exception& e) {
            BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: shared profiles parse error: " << e.what();
        }
    }

    return 0;
}

std::string MakerspaceAgent::request_setting_id(std::string name, std::map<std::string, std::string>*, unsigned int* http_code)
{
    if (http_code) *http_code = 200;
    // Generate a UUID locally; Supabase will use it as the primary key on upsert.
    return generate_uuid();
}

int MakerspaceAgent::put_setting(std::string setting_id, std::string name,
                                  std::map<std::string, std::string>* values_map,
                                  unsigned int* http_code, bool /*force*/)
{
    if (!values_map || !is_user_login() || !is_configured()) {
        if (http_code) *http_code = 0;
        return -1;
    }

    // Respect local-only flag
    if (is_local_only(name)) {
        BOOST_LOG_TRIVIAL(info) << "MakerspaceAgent: skipping local-only preset: " << name;
        if (http_code) *http_code = 200;
        return 0;
    }

    ensure_token_fresh("put_setting");

    // Derive profile type from values map
    std::string profile_type;
    auto it = values_map->find("type");
    if (it != values_map->end())
        profile_type = it->second;
    if (profile_type.empty()) {
        BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: put_setting missing 'type' for preset: " << name;
        if (http_code) *http_code = 400;
        return -1;
    }

    // Serialize values_map as JSONB content
    nlohmann::json content;
    for (const auto& [k, v] : *values_map)
        content[k] = v;

    nlohmann::json row;
    row["id"]           = setting_id;
    row["profile_type"] = profile_type;
    row["name"]         = name;
    row["content"]      = content;
    row["updated_at"]   = "now()";

    std::string body_str = nlohmann::json::array({row}).dump();
    std::string response;
    unsigned code = 0;

    // Upsert: conflict on (user_id, profile_type, name) — the user_id is injected by RLS
    int rc = rest_post("makerspace_profiles", body_str, &response, &code,
                       "resolution=merge-duplicates");
    if (http_code) *http_code = code;
    if (rc != 0 || code < 200 || code >= 300) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: put_setting failed for '" << name
                                 << "', http=" << code << " body=" << response;
        return -1;
    }
    return 0;
}

int MakerspaceAgent::delete_setting(std::string setting_id)
{
    if (setting_id.empty() || !is_user_login() || !is_configured())
        return -1;

    ensure_token_fresh("delete_setting");

    std::string response;
    unsigned code = 0;
    int rc = rest_delete("makerspace_profiles", "id=eq." + setting_id, &response, &code);
    if (rc != 0 || code < 200 || code >= 300) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: delete_setting failed, http=" << code;
        return -1;
    }
    return 0;
}

// ============================================================================
// Callbacks
// ============================================================================

int MakerspaceAgent::set_on_server_connected_fn(AppOnServerConnectedFn fn)
{
    m_on_server_connected_fn = std::move(fn);
    return 0;
}

int MakerspaceAgent::set_on_http_error_fn(AppOnHttpErrorFn fn)
{
    m_on_http_error_fn = std::move(fn);
    return 0;
}

// ============================================================================
// Private — HTTP helpers
// ============================================================================

std::string MakerspaceAgent::rest_url(const std::string& table) const
{
    return m_supabase_url + "/rest/v1/" + table;
}

std::string MakerspaceAgent::auth_url(const std::string& path) const
{
    return m_supabase_url + "/auth/v1" + path;
}

int MakerspaceAgent::rest_get(const std::string& table, const std::string& query,
                               std::string* body, unsigned* code)
{
    std::string url = rest_url(table);
    if (!query.empty())
        url += "?" + query;

    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = m_session.access_token;
    }

    auto http = Http::get(url);
    http.tls_verify(true)
        .header("apikey", m_supabase_anon_key)
        .header("Authorization", "Bearer " + token)
        .header("Accept", "application/json")
        .timeout_max(30)
        .on_complete([&](std::string b, unsigned s) { if (body) *body = b; if (code) *code = s; })
        .on_error([&](std::string b, std::string err, unsigned s) {
            if (body) *body = b;
            if (code) *code = s;
            BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent REST GET error: " << err;
        })
        .perform_sync();
    return 0;
}

int MakerspaceAgent::rest_post(const std::string& table, const std::string& json_body,
                                std::string* response, unsigned* code,
                                const std::string& prefer)
{
    std::string url = rest_url(table);

    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = m_session.access_token;
    }

    auto http = Http::post(url);
    http.tls_verify(true)
        .header("apikey", m_supabase_anon_key)
        .header("Authorization", "Bearer " + token)
        .header("Content-Type", "application/json")
        .header("Accept", "application/json");
    if (!prefer.empty())
        http.header("Prefer", prefer);

    http.set_post_body(json_body)
        .timeout_max(30)
        .on_complete([&](std::string b, unsigned s) { if (response) *response = b; if (code) *code = s; })
        .on_error([&](std::string b, std::string err, unsigned s) {
            if (response) *response = b;
            if (code) *code = s;
            BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent REST POST error: " << err;
        })
        .perform_sync();
    return 0;
}

int MakerspaceAgent::rest_delete(const std::string& table, const std::string& query,
                                  std::string* response, unsigned* code)
{
    std::string url = rest_url(table);
    if (!query.empty())
        url += "?" + query;

    std::string token;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        token = m_session.access_token;
    }

    auto http = Http::del(url);
    http.tls_verify(true)
        .header("apikey", m_supabase_anon_key)
        .header("Authorization", "Bearer " + token)
        .timeout_max(30)
        .on_complete([&](std::string b, unsigned s) { if (response) *response = b; if (code) *code = s; })
        .on_error([&](std::string b, std::string err, unsigned s) {
            if (response) *response = b;
            if (code) *code = s;
            BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent REST DELETE error: " << err;
        })
        .perform_sync();
    return 0;
}

int MakerspaceAgent::auth_post(const std::string& path, const std::string& json_body,
                                std::string* response, unsigned* code)
{
    std::string url = auth_url(path);

    auto http = Http::post(url);
    http.tls_verify(true)
        .header("apikey", m_supabase_anon_key)
        .header("Content-Type", "application/json")
        .set_post_body(json_body)
        .timeout_max(20)
        .on_complete([&](std::string b, unsigned s) { if (response) *response = b; if (code) *code = s; })
        .on_error([&](std::string b, std::string err, unsigned s) {
            if (response) *response = b;
            if (code) *code = s;
            BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent Auth POST error: " << err;
        })
        .perform_sync();
    return 0;
}

// ============================================================================
// Private — Auth helpers
// ============================================================================

bool MakerspaceAgent::login_with_password(const std::string& email, const std::string& password)
{
    nlohmann::json body;
    body["email"]    = email;
    body["password"] = password;

    std::string response;
    unsigned code = 0;
    auth_post("/token?grant_type=password", body.dump(), &response, &code);

    if (code < 200 || code >= 300) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: login failed, http=" << code
                                 << " body=" << response;
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        std::lock_guard<std::mutex> lock(m_mutex);
        parse_auth_response(j);
        save_session();
        return m_session.logged_in;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: login parse error: " << e.what();
        return false;
    }
}

bool MakerspaceAgent::try_refresh_token()
{
    std::string refresh;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        refresh = m_session.refresh_token;
    }
    if (refresh.empty())
        return false;

    nlohmann::json body;
    body["refresh_token"] = refresh;

    std::string response;
    unsigned code = 0;
    auth_post("/token?grant_type=refresh_token", body.dump(), &response, &code);

    if (code < 200 || code >= 300) {
        BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: token refresh failed, http=" << code;
        return false;
    }

    try {
        auto j = nlohmann::json::parse(response);
        std::lock_guard<std::mutex> lock(m_mutex);
        parse_auth_response(j);
        save_session();
        return m_session.logged_in;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(error) << "MakerspaceAgent: refresh parse error: " << e.what();
        return false;
    }
}

void MakerspaceAgent::parse_auth_response(const nlohmann::json& j)
{
    // Expects the Supabase /auth/v1/token response:
    // {"access_token": "...", "refresh_token": "...", "user": {"id": "...", "email": "..."}}
    m_session.access_token  = j.value("access_token", "");
    m_session.refresh_token = j.value("refresh_token", "");

    if (j.contains("user") && j["user"].is_object()) {
        const auto& user       = j["user"];
        m_session.user_id      = user.value("id", "");
        m_session.user_email   = user.value("email", "");
    }

    m_session.logged_in = !m_session.access_token.empty();
}

// ============================================================================
// Private — Token persistence
// ============================================================================

void MakerspaceAgent::save_session() const
{
    if (m_session_path.empty())
        return;
    try {
        nlohmann::json j;
        j["access_token"]  = m_session.access_token;
        j["refresh_token"] = m_session.refresh_token;
        j["user_id"]       = m_session.user_id;
        j["user_email"]    = m_session.user_email;
        j["logged_in"]     = m_session.logged_in;

        std::ofstream f(m_session_path);
        f << j.dump(2);
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: failed to save session: " << e.what();
    }
}

bool MakerspaceAgent::load_session()
{
    if (m_session_path.empty() || !fs::exists(m_session_path))
        return false;
    try {
        std::ifstream f(m_session_path);
        auto j = nlohmann::json::parse(f);
        std::lock_guard<std::mutex> lock(m_mutex);
        m_session.access_token  = j.value("access_token", "");
        m_session.refresh_token = j.value("refresh_token", "");
        m_session.user_id       = j.value("user_id", "");
        m_session.user_email    = j.value("user_email", "");
        m_session.logged_in     = j.value("logged_in", false) && !m_session.access_token.empty();
        return m_session.logged_in;
    } catch (const std::exception& e) {
        BOOST_LOG_TRIVIAL(warning) << "MakerspaceAgent: failed to load session: " << e.what();
        return false;
    }
}

void MakerspaceAgent::clear_session()
{
    std::lock_guard<std::mutex> lock(m_mutex);
    m_session = Session{};
    if (!m_session_path.empty() && fs::exists(m_session_path)) {
        try { fs::remove(m_session_path); } catch (...) {}
    }
}

// ============================================================================
// Private — Utilities
// ============================================================================

bool MakerspaceAgent::is_local_only(const std::string& preset_name) const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& name : m_local_only_presets)
        if (name == preset_name)
            return true;
    return false;
}

std::string MakerspaceAgent::generate_uuid()
{
    std::random_device rd;
    std::mt19937_64 gen(rd());
    std::uniform_int_distribution<uint64_t> dis;

    uint64_t hi = dis(gen);
    uint64_t lo = dis(gen);

    // Set version 4 and variant bits
    hi = (hi & 0xFFFFFFFFFFFF0FFFULL) | 0x0000000000004000ULL;
    lo = (lo & 0x3FFFFFFFFFFFFFFFULL) | 0x8000000000000000ULL;

    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    ss << std::setw(8)  << (hi >> 32)
       << '-' << std::setw(4) << ((hi >> 16) & 0xFFFF)
       << '-' << std::setw(4) << (hi & 0xFFFF)
       << '-' << std::setw(4) << (lo >> 48)
       << '-' << std::setw(12) << (lo & 0x0000FFFFFFFFFFFFULL);
    return ss.str();
}

} // namespace Slic3r
