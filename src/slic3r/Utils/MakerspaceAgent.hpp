#pragma once

#include "ICloudServiceAgent.hpp"
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <functional>
#include <nlohmann/json.hpp>

namespace Slic3r {

static const std::string MAKERSPACE_CLOUD_PROVIDER("makerspace");

/**
 * MakerspaceAgent - Supabase-backed cloud service for makerspace profile sync.
 *
 * Implements ICloudServiceAgent for Supabase:
 * - Email/password auth via Supabase Auth API
 * - JWT token storage in {config_dir}/makerspace_session.json
 * - Profile sync (machine/filament/process) via Supabase REST API
 * - Local-only flag: presets listed in AppConfig "makerspace_local_only_presets"
 *   are never pushed to the cloud
 * - Admin-managed shared profiles pulled from makerspace_shared_profiles table
 *
 * Supabase tables required: see resources/makerspace/schema.sql
 */
class MakerspaceAgent : public ICloudServiceAgent {
public:
    explicit MakerspaceAgent();
    ~MakerspaceAgent() override = default;

    std::string get_id() const override { return MAKERSPACE_CLOUD_PROVIDER; }

    // Configure Supabase connection (called before start())
    void set_supabase_url(const std::string& url);
    void set_supabase_anon_key(const std::string& key);
    void set_local_only_presets(const std::string& semicolon_separated);

    bool is_configured() const;

    // ========================================================================
    // Lifecycle
    // ========================================================================
    int init_log() override { return 0; }
    int set_config_dir(std::string config_dir) override;
    int set_cert_file(std::string, std::string) override { return 0; }
    int set_country_code(std::string) override { return 0; }
    int start() override;

    // ========================================================================
    // Session Management
    // ========================================================================
    // Accepted JSON formats:
    //   {"email": "...", "password": "..."}           — dialog login
    //   {"access_token": "...", "refresh_token": "...", "user_id": "...", "user_email": "..."} — token inject
    int change_user(std::string user_info) override;
    bool is_user_login() override;
    int user_logout(bool request = false) override;
    std::string get_user_id() override;
    std::string get_user_name() override;
    std::string get_user_avatar() override { return {}; }
    std::string get_user_nickname() override;

    // ========================================================================
    // Login UI Support
    // ========================================================================
    std::string build_login_cmd() override;
    std::string build_logout_cmd() override;
    std::string build_login_info() override;

    // ========================================================================
    // Token Access
    // ========================================================================
    std::string get_access_token() const override;
    std::string get_refresh_token() const override;
    bool ensure_token_fresh(const std::string& reason) override;

    // ========================================================================
    // Server Connectivity
    // ========================================================================
    std::string get_cloud_service_host() override;
    std::string get_cloud_login_url(const std::string& = "") override { return {}; }
    int connect_server() override;
    bool is_server_connected() override;
    int refresh_connection() override;
    int start_subscribe(std::string) override { return 0; }
    int stop_subscribe(std::string) override { return 0; }
    int add_subscribe(std::vector<std::string>) override { return 0; }
    int del_subscribe(std::vector<std::string>) override { return 0; }
    void enable_multi_machine(bool) override {}

    // ========================================================================
    // Settings Synchronization
    // ========================================================================
    int get_user_presets(std::map<std::string, std::map<std::string, std::string>>* user_presets) override;
    std::string request_setting_id(std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code) override;
    int put_setting(std::string setting_id, std::string name, std::map<std::string, std::string>* values_map, unsigned int* http_code, bool force = false) override;
    int get_setting_list(std::string, ProgressFn = nullptr, WasCancelledFn = nullptr) override { return 0; }
    int get_setting_list2(std::string, CheckFn, ProgressFn = nullptr, WasCancelledFn = nullptr) override { return 0; }
    int delete_setting(std::string setting_id) override;

    // ========================================================================
    // Callbacks
    // ========================================================================
    int set_on_server_connected_fn(AppOnServerConnectedFn fn) override;
    int set_on_http_error_fn(AppOnHttpErrorFn fn) override;
    int set_get_country_code_fn(GetCountryCodeFn) override { return 0; }
    int set_queue_on_main_fn(QueueOnMainFn) override { return 0; }

    // ========================================================================
    // No-op stubs for interface methods not used by Makerspace
    // ========================================================================
    int get_my_message(int, int, int, unsigned int*, std::string*) override { return 0; }
    int check_user_task_report(int*, bool*) override { return 0; }
    int get_user_print_info(unsigned int*, std::string*) override { return 0; }
    int get_user_tasks(TaskQueryParams, std::string*) override { return 0; }
    int get_printer_firmware(std::string, unsigned*, std::string*) override { return 0; }
    int get_task_plate_index(std::string, int*) override { return 0; }
    int get_user_info(int*) override { return 0; }
    int get_subtask_info(std::string, std::string*, unsigned int*, std::string*) override { return 0; }
    int get_slice_info(std::string, std::string, int, std::string*) override { return 0; }
    int query_bind_status(std::vector<std::string>, unsigned int*, std::string*) override { return 0; }
    int modify_printer_name(std::string, std::string) override { return 0; }
    int get_camera_url(std::string, std::function<void(std::string)>) override { return 0; }
    int get_design_staffpick(int, int, std::function<void(std::string)>) override { return 0; }
    int start_publish(PublishParams, OnUpdateStatusFn, WasCancelledFn, std::string*) override { return 0; }
    int get_model_publish_url(std::string*) override { return 0; }
    int get_subtask(BBLModelTask*, OnGetSubTaskFn) override { return 0; }
    int get_model_mall_home_url(std::string*) override { return 0; }
    int get_model_mall_detail_url(std::string*, std::string) override { return 0; }
    int get_my_profile(std::string, unsigned int*, std::string*) override { return 0; }
    int get_my_token(std::string, unsigned int*, std::string*) override { return 0; }
    int track_enable(bool) override { return 0; }
    int track_remove_files() override { return 0; }
    int track_event(std::string, std::string) override { return 0; }
    int track_header(std::string) override { return 0; }
    int track_update_property(std::string, std::string, std::string) override { return 0; }
    int track_get_property(std::string, std::string&, std::string) override { return 0; }
    bool get_track_enable() override { return false; }
    int put_model_mall_rating(int, int, std::string, std::vector<std::string>, unsigned int&, std::string&) override { return 0; }
    int get_oss_config(std::string&, std::string, unsigned int&, std::string&) override { return 0; }
    int put_rating_picture_oss(std::string&, std::string&, std::string, int, unsigned int&, std::string&) override { return 0; }
    int get_model_mall_rating_result(int, std::string&, unsigned int&, std::string&) override { return 0; }
    int get_mw_user_preference(std::function<void(std::string)>) override { return 0; }
    int get_mw_user_4ulist(int, int, std::function<void(std::string)>) override { return 0; }
    std::string get_version() override { return "1.0.0"; }

private:
    struct Session {
        std::string access_token;
        std::string refresh_token;
        std::string user_id;
        std::string user_email;
        bool logged_in{false};
    };

    // HTTP helpers targeting Supabase REST API
    int rest_get(const std::string& table, const std::string& query, std::string* body, unsigned* code);
    int rest_post(const std::string& table, const std::string& json_body, std::string* response, unsigned* code, const std::string& prefer = "");
    int rest_delete(const std::string& table, const std::string& query, std::string* response, unsigned* code);

    // HTTP helper targeting Supabase Auth API
    int auth_post(const std::string& path, const std::string& json_body, std::string* response, unsigned* code);

    bool login_with_password(const std::string& email, const std::string& password);
    bool try_refresh_token();
    void parse_auth_response(const nlohmann::json& j);

    void save_session() const;
    bool load_session();
    void clear_session();

    bool is_local_only(const std::string& preset_name) const;
    static std::string generate_uuid();

    std::string rest_url(const std::string& table) const;
    std::string auth_url(const std::string& path) const;

    std::string m_supabase_url;
    std::string m_supabase_anon_key;
    std::string m_config_dir;
    std::string m_session_path;

    Session m_session;
    mutable std::mutex m_mutex;

    bool m_server_connected{false};
    std::vector<std::string> m_local_only_presets;

    AppOnServerConnectedFn m_on_server_connected_fn;
    AppOnHttpErrorFn m_on_http_error_fn;
};

} // namespace Slic3r
