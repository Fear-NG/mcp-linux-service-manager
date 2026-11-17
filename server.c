/**
 * MCP Linux Service Manager & Web Dashboard
 * Open-Source Linux System Management & AI MCP Server
 *
 * Authors:
 *   - Nguyễn Thành Nhân
 *   - Võ Minh Trí
 *   - Đặng Nguyễn Bách Việt
 *
 * License: MIT
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <pthread.h>
#include <ctype.h>
#include <signal.h>
#include <cjson/cJSON.h>

#define VERSION "2.2.0-foss"
#define BUFFER_SIZE 131072
#define DEFAULT_WEB_PORT 8080

static volatile int g_running = 1;
static int g_web_port = DEFAULT_WEB_PORT;
static int g_server_fd = -1;
static int g_enable_web = 1;
static int g_enable_mcp = 1;

void handle_signal(int sig) {
    (void)sig;
    g_running = 0;
    if (g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
    }
}

int is_valid_service_name(const char *name) {
    if (!name || strlen(name) == 0 || strlen(name) > 128) return 0;
    for (int i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '@') return 0;
    }
    return 1;
}

int is_allowed_action(const char *action) {
    if (!action) return 0;
    return (strcmp(action, "start") == 0 ||
            strcmp(action, "stop") == 0 ||
            strcmp(action, "restart") == 0 ||
            strcmp(action, "status") == 0 ||
            strcmp(action, "reload") == 0 ||
            strcmp(action, "enable") == 0 ||
            strcmp(action, "disable") == 0);
}

char* run_cmd_safe(char *const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return strdup("Error: Failed to create pipe.");

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return strdup("Error: Failed to fork process.");
    }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execv(argv[0], argv);
        _exit(127);
    }

    close(pipefd[1]);
    char buffer[4096];
    size_t total_len = 0;
    char *output = malloc(1);
    if (!output) {
        close(pipefd[0]);
        waitpid(pid, NULL, 0);
        return strdup("Error: Memory allocation failed.");
    }
    output[0] = '\0';

    ssize_t bytes_read;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        char *new_output = realloc(output, total_len + bytes_read + 1);
        if (!new_output) {
            free(output);
            close(pipefd[0]);
            waitpid(pid, NULL, 0);
            return strdup("Error: Out of memory.");
        }
        output = new_output;
        memcpy(output + total_len, buffer, bytes_read + 1);
        total_len += bytes_read;
    }
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return output;
}

static int hex_val(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

void parse_post_param(const char *body, const char *key, char *dest, size_t dest_sz) {
    dest[0] = '\0';
    char search_key[128];
    snprintf(search_key, sizeof(search_key), "%s=", key);

    const char *start = strstr(body, search_key);
    if (!start) return;
    start += strlen(search_key);

    const char *end = strchr(start, '&');
    size_t len = end ? (size_t)(end - start) : strlen(start);

    size_t d_idx = 0;
    for (size_t i = 0; i < len && d_idx < dest_sz - 1; i++) {
        if (start[i] == '+') {
            dest[d_idx++] = ' ';
        } else if (start[i] == '%' && i + 2 < len) {
            int h1 = hex_val(start[i + 1]);
            int h2 = hex_val(start[i + 2]);
            if (h1 >= 0 && h2 >= 0) {
                dest[d_idx++] = (char)((h1 << 4) | h2);
                i += 2;
            } else {
                dest[d_idx++] = start[i];
            }
        } else if (start[i] == '\r' || start[i] == '\n') {
            break;
        } else {
            dest[d_idx++] = start[i];
        }
    }
    dest[d_idx] = '\0';
}

void* web_dashboard_thread(void *arg) {
    (void)arg;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((g_server_fd = socket(AF_INET, SOCK_STREAM, 0)) == -1) return NULL;

    setsockopt(g_server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(g_web_port);

    if (bind(g_server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(g_server_fd);
        g_server_fd = -1;
        return NULL;
    }

    if (listen(g_server_fd, 16) < 0) {
        close(g_server_fd);
        g_server_fd = -1;
        return NULL;
    }

    while (g_running) {
        int new_socket = accept(g_server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) {
            if (!g_running) break;
            continue;
        }

        char req_buf[8192] = {0};
        ssize_t total_read = 0;
        while (total_read < (ssize_t)sizeof(req_buf) - 1) {
            ssize_t r = read(new_socket, req_buf + total_read, sizeof(req_buf) - 1 - total_read);
            if (r <= 0) break;
            total_read += r;
            req_buf[total_read] = '\0';
            char *hdr_end = strstr(req_buf, "\r\n\r\n");
            if (hdr_end) {
                char *cl = strcasestr(req_buf, "Content-Length:");
                if (cl) {
                    long content_len = strtol(cl + 15, NULL, 10);
                    ssize_t body_received = total_read - (hdr_end + 4 - req_buf);
                    if (body_received >= content_len) break;
                } else {
                    break;
                }
            }
        }
        if (total_read <= 0) {
            close(new_socket);
            continue;
        }

        // ===============================================
        // 1. HEALTH CHECK ENDPOINT: GET /health or /api/health
        // ===============================================
        if (strncmp(req_buf, "GET /health", 11) == 0 || strncmp(req_buf, "GET /api/health", 15) == 0) {
            char *status_argv[] = {"/bin/systemctl", "is-system-running", NULL};
            char *sys_state = run_cmd_safe(status_argv);
            sys_state[strcspn(sys_state, "\r\n")] = 0;

            char *uptime_argv[] = {"/usr/bin/uptime", "-p", NULL};
            char *uptime_str = run_cmd_safe(uptime_argv);
            uptime_str[strcspn(uptime_str, "\r\n")] = 0;

            cJSON *health_json = cJSON_CreateObject();
            cJSON_AddStringToObject(health_json, "status", "UP");
            cJSON_AddStringToObject(health_json, "version", VERSION);
            cJSON_AddStringToObject(health_json, "systemd_state", sys_state);
            cJSON_AddStringToObject(health_json, "uptime", uptime_str);
            cJSON_AddNumberToObject(health_json, "port", g_web_port);

            char *json_txt = cJSON_PrintUnformatted(health_json);
            cJSON_Delete(health_json);

            char http_hdr[512];
            snprintf(http_hdr, sizeof(http_hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     strlen(json_txt));
            ssize_t w1 = write(new_socket, http_hdr, strlen(http_hdr));
            ssize_t w2 = write(new_socket, json_txt, strlen(json_txt));
            (void)w1; (void)w2;
            close(new_socket);

            free(json_txt);
            free(sys_state);
            free(uptime_str);
            continue;
        }

        // ===============================================
        // 2. METRICS ENDPOINT: GET /api/metrics
        // ===============================================
        if (strncmp(req_buf, "GET /api/metrics", 16) == 0) {
            char *load_argv[] = {"/bin/cat", "/proc/loadavg", NULL};
            char *load_str = run_cmd_safe(load_argv);
            double l1 = 0.0, l5 = 0.0, l15 = 0.0;
            sscanf(load_str, "%lf %lf %lf", &l1, &l5, &l15);

            char *free_argv[] = {"/usr/bin/free", "-h", NULL};
            char *free_str = run_cmd_safe(free_argv);

            char *uptime_argv[] = {"/usr/bin/uptime", "-p", NULL};
            char *uptime_str = run_cmd_safe(uptime_argv);
            uptime_str[strcspn(uptime_str, "\r\n")] = 0;

            cJSON *metrics_json = cJSON_CreateObject();
            cJSON_AddStringToObject(metrics_json, "uptime", uptime_str);
            cJSON *load_obj = cJSON_CreateObject();
            cJSON_AddNumberToObject(load_obj, "1m", l1);
            cJSON_AddNumberToObject(load_obj, "5m", l5);
            cJSON_AddNumberToObject(load_obj, "15m", l15);
            cJSON_AddItemToObject(metrics_json, "loadavg", load_obj);
            cJSON_AddStringToObject(metrics_json, "memory_table", free_str);

            char *json_txt = cJSON_PrintUnformatted(metrics_json);
            cJSON_Delete(metrics_json);

            char http_hdr[512];
            snprintf(http_hdr, sizeof(http_hdr),
                     "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                     strlen(json_txt));
            ssize_t w1 = write(new_socket, http_hdr, strlen(http_hdr));
            ssize_t w2 = write(new_socket, json_txt, strlen(json_txt));
            (void)w1; (void)w2;
            close(new_socket);

            free(json_txt);
            free(load_str);
            free(free_str);
            free(uptime_str);
            continue;
        }

        // ===============================================
        // 3. ACTION HANDLER (POST /api/action)
        // ===============================================
        char current_service[128] = {0};
        char *action_feedback = malloc(16384);
        action_feedback[0] = '\0';
        int is_json_req = (strstr(req_buf, "Content-Type: application/json") != NULL);

        if (strncmp(req_buf, "POST /api/action", 16) == 0) {
            char *body = strstr(req_buf, "\r\n\r\n");
            if (body) {
                body += 4;
                char service[128] = {0};
                char action[32] = {0};

                if (is_json_req) {
                    cJSON *parsed_body = cJSON_Parse(body);
                    if (parsed_body) {
                        cJSON *s = cJSON_GetObjectItem(parsed_body, "service");
                        cJSON *a = cJSON_GetObjectItem(parsed_body, "action");
                        if (s && cJSON_IsString(s)) strncpy(service, s->valuestring, sizeof(service) - 1);
                        if (a && cJSON_IsString(a)) strncpy(action, a->valuestring, sizeof(action) - 1);
                        cJSON_Delete(parsed_body);
                    }
                } else {
                    parse_post_param(body, "service", service, sizeof(service));
                    parse_post_param(body, "action", action, sizeof(action));
                }

                // Tự động điền dịch vụ mặc định theo nút nếu ô nhập để trống
                int auto_filled = 0;
                if (strlen(service) == 0) {
                    auto_filled = 1;
                    if (strcmp(action, "diagnose") == 0 || strcmp(action, "stop") == 0) {
                        snprintf(service, sizeof(service), "demo-crash");
                    } else if (strcmp(action, "reload") == 0) {
                        snprintf(service, sizeof(service), "systemd-resolved");
                    } else {
                        snprintf(service, sizeof(service), "cron");
                    }
                }
                snprintf(current_service, sizeof(current_service), "%s", service);

                char fill_notice[256] = {0};
                if (auto_filled) {
                    snprintf(fill_notice, sizeof(fill_notice),
                             "<div style='margin-bottom:8px;font-size:0.85rem;color:#89b4fa;'>⚡ <b>Fill sẵn tự động:</b> Ô nhập để trống nên đã áp dụng dịch vụ mặc định <code>%s</code> cho hành động <b>%s</b>.</div>",
                             service, action);
                }

                if (!is_valid_service_name(service)) {
                    snprintf(action_feedback, 16384,
                             "<div class='alert err'>❌ <b>Lỗi:</b> Tên dịch vụ <code>%s</code> không hợp lệ! Chỉ cho phép [a-zA-Z0-9_-.@]</div>", service);
                } else if (strcmp(action, "diagnose") == 0) {
                    char *show_argv[] = {"/bin/systemctl", "show", service, "--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts,MainPID", NULL};
                    char *show_res = run_cmd_safe(show_argv);
                    char *log_argv[] = {"/usr/bin/sudo", "/bin/journalctl", "-u", service, "-n", "20", "--no-pager", NULL};
                    char *log_res = run_cmd_safe(log_argv);

                    snprintf(action_feedback, 16384,
                             "<div class='alert info'>%s🔍 <b>Báo cáo chẩn đoán chuyên sâu cho <code>%s</code>:</b>"
                             "<pre>--- THÔNG SỐ KỸ THUẬT ---\n%s\n--- 20 DÒNG LOG GẦN NHẤT ---\n%s</pre></div>",
                             fill_notice, service, show_res, strlen(log_res) > 0 ? log_res : "(Không có dữ liệu log)");
                    free(show_res);
                    free(log_res);
                } else if (is_allowed_action(action)) {
                    char *argv[] = {"/usr/bin/sudo", "/bin/systemctl", action, service, NULL};
                    char *cmd_res = run_cmd_safe(argv);
                    snprintf(action_feedback, 16384,
                             "<div class='alert ok'>%s✅ <b>Kết quả thực thi lệnh <code>systemctl %s %s</code>:</b>"
                             "<pre>%s</pre></div>",
                             fill_notice, action, service, strlen(cmd_res) > 0 ? cmd_res : "Thao tác hoàn tất thành công (Return code 0).");
                    free(cmd_res);
                } else {
                    snprintf(action_feedback, 16384,
                             "<div class='alert err'>❌ <b>Lỗi:</b> Hành động <code>%s</code> không được phép!</div>", action);
                }

                if (is_json_req) {
                    cJSON *act_resp = cJSON_CreateObject();
                    cJSON_AddStringToObject(act_resp, "service", service);
                    cJSON_AddStringToObject(act_resp, "action", action);
                    cJSON_AddStringToObject(act_resp, "status", is_allowed_action(action) || strcmp(action, "diagnose") == 0 ? "success" : "error");
                    char *json_out = cJSON_PrintUnformatted(act_resp);
                    cJSON_Delete(act_resp);

                    char http_hdr[512];
                    snprintf(http_hdr, sizeof(http_hdr),
                             "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n",
                             strlen(json_out));
                    ssize_t w1 = write(new_socket, http_hdr, strlen(http_hdr));
                    ssize_t w2 = write(new_socket, json_out, strlen(json_out));
                    (void)w1; (void)w2;
                    close(new_socket);

                    free(json_out);
                    free(action_feedback);
                    continue;
                }
            }
        }

        // ===============================================
        // 4. MAIN HTML DASHBOARD RENDER
        // ===============================================
        char *status_argv[] = {"/bin/systemctl", "is-system-running", NULL};
        char *sys_state = run_cmd_safe(status_argv);
        sys_state[strcspn(sys_state, "\r\n")] = 0;

        char *uptime_argv[] = {"/usr/bin/uptime", "-p", NULL};
        char *uptime_str = run_cmd_safe(uptime_argv);

        char *load_argv[] = {"/bin/cat", "/proc/loadavg", NULL};
        char *load_str = run_cmd_safe(load_argv);
        if (strlen(load_str) > 15) load_str[14] = '\0';

        char *free_argv[] = {"/usr/bin/free", "-h", NULL};
        char *free_str = run_cmd_safe(free_argv);

        char *failed_argv[] = {"/bin/systemctl", "--failed", "--no-legend", "--plain", NULL};
        char *failed_units = run_cmd_safe(failed_argv);

        char *running_argv[] = {"/bin/systemctl", "list-units", "--type=service", "--state=running", "--no-legend", "--plain", NULL};
        char *running_units = run_cmd_safe(running_argv);
        if (strlen(running_units) > 1800) {
            running_units[1800] = '\0';
            strcat(running_units, "\n... (Đã thu gọn danh sách)");
        }

        int is_degraded = (strcmp(sys_state, "running") != 0);

        char *html_body = malloc(BUFFER_SIZE);
        snprintf(html_body, BUFFER_SIZE,
            "<!DOCTYPE html><html lang='vi'><head><meta charset='utf-8'><title>MCP Linux Service Manager</title>"
            "<meta name='viewport' content='width=device-width, initial-scale=1.0'>"
            "<style>"
            "*{box-sizing:border-box;margin:0;padding:0;}"
            "@keyframes pulse{0%%{opacity:1;transform:scale(1);}50%%{opacity:0.4;transform:scale(1.2);}100%%{opacity:1;transform:scale(1);}}"
            "body{font-family:'Segoe UI',-apple-system,BlinkMacSystemFont,Roboto,Helvetica,Arial,sans-serif;background:#0f111a;color:#cdd6f4;line-height:1.6;padding:2rem 1rem;}"
            ".container{max-width:1100px;margin:0 auto;display:flex;flex-direction:column;gap:1.5rem;}"
            ".header{display:flex;justify-content:space-between;align-items:center;background:#181926;border:1px solid rgba(255,255,255,0.08);border-radius:14px;padding:1.25rem 1.75rem;box-shadow:0 8px 30px rgba(0,0,0,0.25);}"
            ".title-wrap{display:flex;align-items:center;gap:12px;}"
            "h1{font-size:1.75rem;font-weight:800;background:linear-gradient(135deg,#89b4fa,#cba6f7);-webkit-background-clip:text;-webkit-text-fill-color:transparent;display:flex;align-items:center;gap:10px;}"
            ".foss-tag{font-size:0.75rem;background:#313244;color:#a6e3a1;padding:3px 9px;border-radius:6px;font-weight:700;border:1px solid rgba(166,227,161,0.25);-webkit-text-fill-color:#a6e3a1;}"
            ".status-badge{display:flex;align-items:center;gap:8px;padding:7px 16px;border-radius:999px;font-weight:700;font-size:0.85rem;text-transform:uppercase;letter-spacing:0.6px;}"
            ".badge-ok{background:#1e3328;color:#a6e3a1;border:1px solid rgba(166,227,161,0.4);}"
            ".badge-warn{background:#3b202a;color:#f38ba8;border:1px solid rgba(243,139,168,0.4);}"
            ".pulse-dot{width:8px;height:8px;border-radius:50%%;display:inline-block;animation:pulse 2s infinite;}"
            ".dot-ok{background:#a6e3a1;box-shadow:0 0 8px #a6e3a1;}"
            ".dot-warn{background:#f38ba8;box-shadow:0 0 8px #f38ba8;}"
            ".metrics-grid{display:grid;grid-template-columns:repeat(4,1fr);gap:1.2rem;width:100%%;}"
            ".metric-box{background:#181926;border:1px solid rgba(255,255,255,0.08);padding:1.25rem;border-radius:14px;box-shadow:0 6px 24px rgba(0,0,0,0.2);transition:all 0.2s ease;display:flex;flex-direction:column;justify-content:center;}"
            ".metric-box:hover{transform:translateY(-2px);border-color:rgba(137,180,250,0.35);}"
            ".metric-title{font-size:0.72rem;color:#a6adc8;text-transform:uppercase;letter-spacing:0.8px;margin-bottom:8px;font-weight:700;display:flex;align-items:center;gap:6px;}"
            ".metric-val{font-size:1.15rem;font-weight:700;color:#f8fafc;word-break:break-word;}"
            ".card{width:100%%;background:#181926;border:1px solid rgba(255,255,255,0.08);border-radius:14px;padding:1.6rem;box-shadow:0 8px 30px rgba(0,0,0,0.25);display:flex;flex-direction:column;gap:1.1rem;}"
            ".card-header{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid rgba(255,255,255,0.06);padding-bottom:0.9rem;}"
            ".card-title-group{display:flex;align-items:center;gap:10px;}"
            ".card-title-group h3{color:#f8fafc;font-size:1.15rem;font-weight:700;}"
            ".card-badge{font-size:0.75rem;font-weight:700;padding:4px 10px;border-radius:6px;letter-spacing:0.5px;background:#24253a;color:#a6adc8;border:1px solid #3b3d54;}"
            ".input-wrap{position:relative;display:flex;align-items:center;width:100%%;}"
            ".input-wrap input{background:#11111b;border:1px solid #313244;color:#f8fafc;padding:14px 44px 14px 18px;border-radius:10px;width:100%%;font-size:0.95rem;outline:none;transition:all 0.25s ease;}"
            ".input-wrap input:focus{border-color:#89b4fa;box-shadow:0 0 0 3px rgba(137,180,250,0.25);background:#131422;}"
            ".btn-clear{position:absolute;right:14px;background:none;border:none;color:#6c7086;font-size:1.15rem;cursor:pointer;padding:4px;line-height:1;transition:color 0.15s;}"
            ".btn-clear:hover{color:#f38ba8;}"
            ".chips{display:flex;gap:8px;flex-wrap:wrap;align-items:center;}"
            ".chip-label{font-size:0.8rem;color:#a6adc8;margin-right:4px;font-weight:600;}"
            ".chip{background:#24253a;color:#cdd6f4;border:1px solid #3b3d54;padding:6px 13px;border-radius:8px;font-size:0.82rem;font-weight:600;cursor:pointer;transition:all 0.2s ease;}"
            ".chip:hover{background:#35374e;color:#fff;border-color:#89b4fa;transform:translateY(-1px);}"
            ".chip-danger{border-color:rgba(243,139,168,0.4);color:#f38ba8;background:rgba(243,139,168,0.08);}"
            ".chip-danger:hover{background:rgba(243,139,168,0.2);border-color:#f38ba8;}"
            ".rule-tip{font-size:0.8rem;color:#a6adc8;display:flex;align-items:center;gap:6px;background:rgba(255,255,255,0.03);padding:8px 12px;border-radius:8px;border:1px dashed rgba(255,255,255,0.1);}"
            ".btn-grid{display:grid;grid-template-columns:repeat(6,1fr);gap:10px;}"
            ".btn{display:flex;flex-direction:column;align-items:center;justify-content:center;gap:3px;height:54px;border:none;border-radius:10px;font-weight:700;font-size:0.86rem;cursor:pointer;color:#11111b;transition:all 0.2s cubic-bezier(0.4,0,0.2,1);padding:5px 4px;}"
            ".btn:hover{filter:brightness(1.12);transform:translateY(-2px);box-shadow:0 6px 18px rgba(0,0,0,0.3);}"
            ".btn:active{transform:translateY(0);filter:brightness(0.95);}"
            ".btn-top{display:flex;align-items:center;gap:6px;font-weight:700;}"
            ".btn-sub{font-size:0.67rem;font-weight:600;opacity:0.82;background:rgba(0,0,0,0.15);padding:1px 6px;border-radius:4px;white-space:nowrap;}"
            ".btn-sub:hover{opacity:1;background:rgba(0,0,0,0.28);}"
            ".btn-start{background:linear-gradient(135deg,#a6e3a1,#94e2d5);}"
            ".btn-stop{background:linear-gradient(135deg,#f38ba8,#eba0ac);}"
            ".btn-restart{background:linear-gradient(135deg,#f9e2af,#f5c2e7);}"
            ".btn-reload{background:linear-gradient(135deg,#fab387,#f9e2af);}"
            ".btn-status{background:linear-gradient(135deg,#89dceb,#89b4fa);}"
            ".btn-diag{background:linear-gradient(135deg,#cba6f7,#b4befe);}"
            ".log-view{width:100%%;background:#11111b;border:1px solid rgba(255,255,255,0.06);border-radius:10px;padding:1.2rem;font-family:'JetBrains Mono','Fira Code','Consolas',monospace;font-size:0.83rem;line-height:1.55;color:#a6e3a1;overflow-x:auto;max-height:220px;overflow-y:auto;box-shadow:inset 0 2px 10px rgba(0,0,0,0.4);}"
            ".log-failed{color:#f38ba8;border-color:rgba(243,139,168,0.25);}"
            ".log-memory{color:#89dceb;}"
            ".log-view::-webkit-scrollbar{width:8px;height:8px;}"
            ".log-view::-webkit-scrollbar-track{background:#11111b;border-radius:4px;}"
            ".log-view::-webkit-scrollbar-thumb{background:#313244;border-radius:4px;}"
            ".log-view::-webkit-scrollbar-thumb:hover{background:#45475a;}"
            ".alert{padding:1.25rem 1.5rem;border-radius:12px;box-shadow:0 4px 18px rgba(0,0,0,0.25);}"
            ".alert pre{margin-top:10px;background:#11111b;padding:1rem;border-radius:8px;font-family:monospace;font-size:0.82rem;overflow-x:auto;max-height:260px;}"
            ".alert.ok{background:#1a2b22;border:1px solid #a6e3a1;color:#a6e3a1;}"
            ".alert.err{background:#2e1b23;border:1px solid #f38ba8;color:#f38ba8;}"
            ".alert.info{background:#241f38;border:1px solid #cba6f7;color:#cba6f7;}"
            ".footer{text-align:center;font-size:0.82rem;color:#6c7086;background:#181926;border:1px solid rgba(255,255,255,0.08);border-radius:14px;padding:1.25rem;box-shadow:0 4px 20px rgba(0,0,0,0.2);}"
            ".footer a{color:#89b4fa;text-decoration:none;margin:0 12px;font-weight:600;transition:color 0.15s;}"
            ".footer a:hover{color:#b4befe;text-decoration:underline;}"
            "@media(max-width:850px){.metrics-grid{grid-template-columns:1fr 1fr;}.btn-grid{grid-template-columns:repeat(3,1fr);}}"
            "</style>"
            "<script>"
            "function pickService(name){var inp=document.getElementById('svc_input');inp.value=name;inp.focus();}"
            "function handleBtn(action,defaultSvc){var inp=document.getElementById('svc_input');if(!inp.value||!inp.value.trim()){inp.value=defaultSvc;}document.getElementById('action_input').value=action;document.getElementById('svc_form').submit();}"
            "</script>"
            "</head><body><div class='container'>"
            "<header class='header'>"
            "<div class='title-wrap'>"
            "<h1>🚀 MCP Linux Service Center</h1>"
            "<span class='foss-tag'>FOSS v" VERSION "</span>"
            "</div>"
            "<div class='status-badge %s'><span class='pulse-dot %s'></span>OS: %s</div>"
            "</header>"
            "<div class='metrics-grid'>"
            "<div class='metric-box'><div class='metric-title'>⏱ Uptime</div><div class='metric-val'>%s</div></div>"
            "<div class='metric-box'><div class='metric-title'>🔥 Tải CPU (1/5/15m)</div><div class='metric-val'>%s</div></div>"
            "<div class='metric-box'><div class='metric-title'>🧠 MCP Protocol</div><div class='metric-val' style='color:#a6e3a1;'>Standard Stdio</div></div>"
            "<div class='metric-box'><div class='metric-title'>🌐 REST & Web</div><div class='metric-val' style='color:#89b4fa;'>Port %d (Active)</div></div>"
            "</div>"
            "%s"
            "<!-- CARD 1: CONTROL PANEL -->"
            "<section class='card'>"
            "<div class='card-header'>"
            "<div class='card-title-group'><span>⚡</span><h3>Bảng điều khiển & Chẩn đoán dịch vụ</h3></div>"
            "<span class='card-badge'>Systemd Supervisor</span>"
            "</div>"
            "<form id='svc_form' action='/api/action' method='POST' style='display:flex;flex-direction:column;gap:1rem;'>"
            "<input type='hidden' id='action_input' name='action' value=''>"
            "<div class='input-wrap'>"
            "<input type='text' id='svc_input' name='service' value='%s' placeholder='Nhập tên dịch vụ (ví dụ: cron, ssh, getty@tty1, demo-crash)...'>"
            "<button type='button' class='btn-clear' onclick=\"document.getElementById('svc_input').value='';document.getElementById('svc_input').focus();\" title='Xóa trắng ô nhập'>✕</button>"
            "</div>"
            "<div class='chips'>"
            "<span class='chip-label'>⚡ Chọn nhanh:</span>"
            "<span class='chip' onclick=\"pickService('cron')\">cron</span>"
            "<span class='chip' onclick=\"pickService('ssh')\">ssh</span>"
            "<span class='chip' onclick=\"pickService('systemd-resolved')\">systemd-resolved</span>"
            "<span class='chip' onclick=\"pickService('getty@tty1')\">getty@tty1</span>"
            "<span class='chip chip-danger' onclick=\"pickService('demo-crash')\">🧪 demo-crash (Test lỗi)</span>"
            "<span class='chip' onclick=\"pickService('')\" style='background:#1e1e2e;color:#a6adc8;border-color:#313244;'>🗑 Xóa trắng</span>"
            "</div>"
            "<div class='rule-tip'>"
            "<span>💡</span><span><b>Cơ chế thông minh:</b> Để trống ô nhập ➔ nút tự động <b>Fill sẵn</b> mặc định & chạy ngay. Đã có tên ➔ <b>Fill nhanh</b> và thực thi ngay lập tức.</span>"
            "</div>"
            "<div class='btn-grid'>"
            "<button type='button' class='btn btn-start' onclick=\"handleBtn('start','cron')\" title='Trống: Chạy cron | Có sẵn: Chạy tên đó'>"
            "<div class='btn-top'><span>▶</span><span>Start</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('cron');\" title='Chỉ điền cron vào ô'>Mặc định: cron</span>"
            "</button>"
            "<button type='button' class='btn btn-stop' onclick=\"handleBtn('stop','demo-crash')\" title='Trống: Dừng demo-crash | Có sẵn: Dừng tên đó'>"
            "<div class='btn-top'><span>⏹</span><span>Stop</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('demo-crash');\" title='Chỉ điền demo-crash vào ô'>Mặc định: demo-crash</span>"
            "</button>"
            "<button type='button' class='btn btn-restart' onclick=\"handleBtn('restart','cron')\" title='Trống: Restart cron | Có sẵn: Restart tên đó'>"
            "<div class='btn-top'><span>🔄</span><span>Restart</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('cron');\" title='Chỉ điền cron vào ô'>Mặc định: cron</span>"
            "</button>"
            "<button type='button' class='btn btn-reload' onclick=\"handleBtn('reload','systemd-resolved')\" title='Trống: Reload systemd-resolved | Có sẵn: Reload tên đó'>"
            "<div class='btn-top'><span>🔁</span><span>Reload</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('systemd-resolved');\" title='Chỉ điền systemd-resolved vào ô'>Mặc định: systemd-resolved</span>"
            "</button>"
            "<button type='button' class='btn btn-status' onclick=\"handleBtn('status','cron')\" title='Trống: Xem status cron | Có sẵn: Xem status tên đó'>"
            "<div class='btn-top'><span>ℹ</span><span>Status</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('cron');\" title='Chỉ điền cron vào ô'>Mặc định: cron</span>"
            "</button>"
            "<button type='button' class='btn btn-diag' onclick=\"handleBtn('diagnose','demo-crash')\" title='Trống: Chẩn đoán demo-crash | Có sẵn: Chẩn đoán tên đó'>"
            "<div class='btn-top'><span>🔍</span><span>Diagnose</span></div>"
            "<span class='btn-sub' onclick=\"event.stopPropagation();pickService('demo-crash');\" title='Chỉ điền demo-crash vào ô'>Mặc định: demo-crash</span>"
            "</button>"
            "</div>"
            "</form>"
            "</section>"
            "<!-- CARD 2: FAILED UNITS (FULL WIDTH) -->"
            "<section class='card'>"
            "<div class='card-header'>"
            "<div class='card-title-group'><span>🚨</span><h3>Dịch vụ đang lỗi (Failed Units)</h3></div>"
            "<div style='display:flex;align-items:center;gap:8px;'>"
            "<button type='button' class='chip chip-danger' style='padding:4px 10px;font-size:0.75rem;' onclick=\"pickService('demo-crash');handleBtn('diagnose','demo-crash');\">🔍 Chẩn đoán ngay</button>"
            "<span class='card-badge' style='%s'>%s</span>"
            "</div>"
            "</div>"
            "<pre class='log-view log-failed' onclick=\"pickService('demo-crash');\" title='Nhấp để chọn demo-crash'>%s</pre>"
            "</section>"
            "<!-- CARD 3: RUNNING UNITS (FULL WIDTH) -->"
            "<section class='card'>"
            "<div class='card-header'>"
            "<div class='card-title-group'><span>🟢</span><h3>Dịch vụ đang hoạt động (Running Services)</h3></div>"
            "<span class='card-badge'>Loaded Active</span>"
            "</div>"
            "<pre class='log-view'>%s</pre>"
            "</section>"
            "<!-- CARD 4: MEMORY STATUS (FULL WIDTH) -->"
            "<section class='card'>"
            "<div class='card-header'>"
            "<div class='card-title-group'><span>📊</span><h3>Chi tiết bộ nhớ hệ thống (RAM & Swap)</h3></div>"
            "<span class='card-badge'>Host Telemetry</span>"
            "</div>"
            "<pre class='log-view log-memory'>%s</pre>"
            "</section>"
            "<footer class='footer'>"
            "<p style='margin-bottom:6px;'>Phần mềm mã nguồn mở MIT • Nhóm tác giả: <b>Nguyễn Thành Nhân</b>, <b>Võ Minh Trí</b>, <b>Đặng Nguyễn Bách Việt</b></p>"
            "<a href='/health' target='_blank'>Health Check (JSON)</a> | "
            "<a href='/api/metrics' target='_blank'>REST Metrics (JSON)</a>"
            "</footer>"
            "</div></body></html>",
            is_degraded ? "badge-warn" : "badge-ok",
            is_degraded ? "dot-warn" : "dot-ok",
            sys_state,
            uptime_str, load_str, g_web_port, action_feedback,
            current_service,
            is_degraded ? "color:#f38ba8;border-color:rgba(243,139,168,0.4);" : "color:#a6e3a1;border-color:rgba(166,227,161,0.4);",
            is_degraded ? "Attention Required" : "All Normal",
            strlen(failed_units) > 0 ? failed_units : "✅ Không có service nào bị lỗi. Hệ thống hoạt động hoàn hảo!",
            running_units, free_str);

        char *http_resp = malloc(BUFFER_SIZE + 1024);
        int len = snprintf(http_resp, BUFFER_SIZE + 1024,
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(html_body), html_body);

        ssize_t w_resp = write(new_socket, http_resp, len);
        (void)w_resp;
        close(new_socket);

        free(action_feedback);
        free(sys_state);
        free(uptime_str);
        free(load_str);
        free(free_str);
        free(failed_units);
        free(running_units);
        free(html_body);
        free(http_resp);
    }
    return NULL;
}

void send_response(cJSON *response) {
    char *rendered = cJSON_PrintUnformatted(response);
    if (rendered) {
        printf("%s\n", rendered);
        fflush(stdout);
        free(rendered);
    }
    cJSON_Delete(response);
}

cJSON* handle_tool_call(const char *tool_name, cJSON *args) {
    cJSON *content_arr = cJSON_CreateArray();
    cJSON *content_obj = cJSON_CreateObject();
    cJSON_AddStringToObject(content_obj, "type", "text");

    if (strcmp(tool_name, "list_failed_services") == 0) {
        char *argv[] = {"/bin/systemctl", "--failed", "--no-legend", "--plain", NULL};
        char *res = run_cmd_safe(argv);
        cJSON_AddStringToObject(content_obj, "text", strlen(res) > 0 ? res : "Hệ thống bình thường, không có service lỗi.");
        free(res);
    } else if (strcmp(tool_name, "list_running_services") == 0) {
        char *argv[] = {"/bin/systemctl", "list-units", "--type=service", "--state=running", "--no-legend", "--plain", NULL};
        char *res = run_cmd_safe(argv);
        cJSON_AddStringToObject(content_obj, "text", strlen(res) > 0 ? res : "Không tìm thấy service đang chạy.");
        free(res);
    } else if (strcmp(tool_name, "get_system_metrics") == 0) {
        char *uptime_argv[] = {"/usr/bin/uptime", "-p", NULL};
        char *uptime_str = run_cmd_safe(uptime_argv);

        char *load_argv[] = {"/bin/cat", "/proc/loadavg", NULL};
        char *load_str = run_cmd_safe(load_argv);

        char *free_argv[] = {"/usr/bin/free", "-h", NULL};
        char *free_str = run_cmd_safe(free_argv);

        char *status_argv[] = {"/bin/systemctl", "is-system-running", NULL};
        char *sys_state = run_cmd_safe(status_argv);

        size_t sz = strlen(uptime_str) + strlen(load_str) + strlen(free_str) + strlen(sys_state) + 256;
        char *report = malloc(sz);
        snprintf(report, sz, "=== OS STATE ===\n%s=== UPTIME ===\n%s=== CPU LOAD ===\n%s=== MEMORY ===\n%s",
                 sys_state, uptime_str, load_str, free_str);
        cJSON_AddStringToObject(content_obj, "text", report);

        free(uptime_str); free(load_str); free(free_str); free(sys_state); free(report);
    } else if (strcmp(tool_name, "service_diagnose") == 0) {
        cJSON *svc_item = cJSON_GetObjectItem(args, "service");
        if (!svc_item || !cJSON_IsString(svc_item) || !is_valid_service_name(svc_item->valuestring)) {
            cJSON_AddStringToObject(content_obj, "text", "Lỗi: Tên service không hợp lệ hoặc thiếu ký tự an toàn.");
        } else {
            const char *svc = svc_item->valuestring;
            char *show_argv[] = {"/bin/systemctl", "show", (char*)svc, "--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts,MainPID", NULL};
            char *show_res = run_cmd_safe(show_argv);
            char *log_argv[] = {"/usr/bin/sudo", "/bin/journalctl", "-u", (char*)svc, "-n", "20", "--no-pager", NULL};
            char *log_res = run_cmd_safe(log_argv);
            size_t sz = strlen(show_res) + strlen(log_res) + 128;
            char *report = malloc(sz);
            snprintf(report, sz, "=== THÔNG SỐ ===\n%s\n=== LOGS ===\n%s", show_res, log_res);
            cJSON_AddStringToObject(content_obj, "text", report);
            free(show_res); free(log_res); free(report);
        }
    } else if (strcmp(tool_name, "manage_service") == 0) {
        cJSON *svc_item = cJSON_GetObjectItem(args, "service");
        cJSON *action_item = cJSON_GetObjectItem(args, "action");
        if (!svc_item || !action_item || !cJSON_IsString(svc_item) || !cJSON_IsString(action_item)) {
            cJSON_AddStringToObject(content_obj, "text", "Lỗi: Thiếu tham số 'service' hoặc 'action'.");
        } else {
            const char *svc = svc_item->valuestring;
            const char *action = action_item->valuestring;
            if (!is_valid_service_name(svc)) {
                cJSON_AddStringToObject(content_obj, "text", "Lỗi: Ký tự tên service không an toàn.");
            } else if (!is_allowed_action(action)) {
                cJSON_AddStringToObject(content_obj, "text", "Lỗi: Hành động (action) không được phép. Chỉ cho phép: start, stop, restart, reload, status, enable, disable.");
            } else {
                char *argv[] = {"/usr/bin/sudo", "/bin/systemctl", (char*)action, (char*)svc, NULL};
                char *res = run_cmd_safe(argv);
                cJSON_AddStringToObject(content_obj, "text", strlen(res) > 0 ? res : "Thao tác hoàn tất thành công.");
                free(res);
            }
        }
    }
    cJSON_AddItemToArray(content_arr, content_obj);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content_arr);
    return result;
}

void print_help(const char *prog) {
    printf("MCP Linux Service Manager & Web Dashboard (FOSS Edition)\n");
    printf("Phiên bản: %s\n\n", VERSION);
    printf("Cách sử dụng: %s [tùy chọn]\n\n", prog);
    printf("Tùy chọn:\n");
    printf("  -p, --port <port>   Cổng HTTP cho Web Dashboard (Mặc định: %d hoặc biến môi trường PORT)\n", DEFAULT_WEB_PORT);
    printf("  --mcp-only          Chỉ chạy giao thức AI MCP (stdio), không mở Web Server\n");
    printf("  --web-only          Chỉ chạy Web Server Dashboard HTTP, không đọc MCP stdin\n");
    printf("  -v, --version       Hiển thị thông tin phiên bản và bản quyền\n");
    printf("  -h, --help          Hiển thị trợ giúp này\n");
}

int main(int argc, char *argv[]) {
    char *env_port = getenv("PORT");
    if (env_port) {
        int p = atoi(env_port);
        if (p > 0 && p <= 65535) g_web_port = p;
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_help(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-v") == 0 || strcmp(argv[i], "--version") == 0) {
            printf("mcp-service-manager version %s (MIT License)\n", VERSION);
            printf("Tác giả: Nguyễn Thành Nhân, Võ Minh Trí, Đặng Nguyễn Bách Việt\n");
            return 0;
        } else if ((strcmp(argv[i], "-p") == 0 || strcmp(argv[i], "--port") == 0) && i + 1 < argc) {
            int p = atoi(argv[++i]);
            if (p > 0 && p <= 65535) g_web_port = p;
        } else if (strcmp(argv[i], "--mcp-only") == 0) {
            g_enable_web = 0;
        } else if (strcmp(argv[i], "--web-only") == 0) {
            g_enable_mcp = 0;
        }
    }

    signal(SIGINT, handle_signal);
    signal(SIGTERM, handle_signal);
    signal(SIGPIPE, SIG_IGN);

    pthread_t web_tid;
    if (g_enable_web) {
        pthread_create(&web_tid, NULL, web_dashboard_thread, NULL);
    }

    if (g_enable_mcp) {
        char line[65536];
        while (g_running && fgets(line, sizeof(line), stdin)) {
            cJSON *req = cJSON_Parse(line);
            if (!req) continue;

            cJSON *id = cJSON_GetObjectItem(req, "id");
            cJSON *method = cJSON_GetObjectItem(req, "method");
            if (!method || !cJSON_IsString(method)) {
                cJSON_Delete(req);
                continue;
            }

            cJSON *resp = cJSON_CreateObject();
            cJSON_AddStringToObject(resp, "jsonrpc", "2.0");
            if (id) {
                if (cJSON_IsNumber(id)) cJSON_AddNumberToObject(resp, "id", id->valuedouble);
                else if (cJSON_IsString(id)) cJSON_AddStringToObject(resp, "id", id->valuestring);
            }

            if (strcmp(method->valuestring, "initialize") == 0) {
                cJSON *result = cJSON_CreateObject();
                cJSON_AddStringToObject(result, "protocolVersion", "2024-11-05");
                cJSON *server_info = cJSON_CreateObject();
                cJSON_AddStringToObject(server_info, "name", "ubuntu-service-manager-c");
                cJSON_AddStringToObject(server_info, "version", VERSION);
                cJSON_AddItemToObject(result, "serverInfo", server_info);
                
                // Chuẩn hóa capabilities với "tools": {} cho tương thích rộng
                cJSON *capabilities = cJSON_CreateObject();
                cJSON_AddItemToObject(capabilities, "tools", cJSON_CreateObject());
                cJSON_AddItemToObject(result, "capabilities", capabilities);

                cJSON_AddItemToObject(resp, "result", result);
                send_response(resp);
            } else if (strcmp(method->valuestring, "tools/list") == 0) {
                cJSON *result = cJSON_CreateObject();
                cJSON *tools = cJSON_CreateArray();

                // Tool 1: list_failed_services
                cJSON *t1 = cJSON_CreateObject();
                cJSON_AddStringToObject(t1, "name", "list_failed_services");
                cJSON_AddStringToObject(t1, "description", "Liệt kê các dịch vụ systemd bị lỗi (failed units)");
                cJSON *s1 = cJSON_CreateObject(); cJSON_AddStringToObject(s1, "type", "object");
                cJSON_AddItemToObject(t1, "inputSchema", s1);
                cJSON_AddItemToArray(tools, t1);

                // Tool 2: list_running_services
                cJSON *t2_run = cJSON_CreateObject();
                cJSON_AddStringToObject(t2_run, "name", "list_running_services");
                cJSON_AddStringToObject(t2_run, "description", "Liệt kê các dịch vụ systemd đang hoạt động bình thường (running)");
                cJSON *s2_run = cJSON_CreateObject(); cJSON_AddStringToObject(s2_run, "type", "object");
                cJSON_AddItemToObject(t2_run, "inputSchema", s2_run);
                cJSON_AddItemToArray(tools, t2_run);

                // Tool 3: get_system_metrics
                cJSON *t_met = cJSON_CreateObject();
                cJSON_AddStringToObject(t_met, "name", "get_system_metrics");
                cJSON_AddStringToObject(t_met, "description", "Lấy chỉ số phần cứng hệ thống: Uptime, Tải CPU, RAM và Swap");
                cJSON *s_met = cJSON_CreateObject(); cJSON_AddStringToObject(s_met, "type", "object");
                cJSON_AddItemToObject(t_met, "inputSchema", s_met);
                cJSON_AddItemToArray(tools, t_met);

                // Tool 4: service_diagnose
                cJSON *t2 = cJSON_CreateObject();
                cJSON_AddStringToObject(t2, "name", "service_diagnose");
                cJSON_AddStringToObject(t2, "description", "Chẩn đoán thông số và đọc 20 dòng log journalctl gần nhất");
                cJSON *s2 = cJSON_CreateObject(); cJSON_AddStringToObject(s2, "type", "object");
                cJSON *pr2 = cJSON_CreateObject();
                cJSON *p_svc2 = cJSON_CreateObject(); cJSON_AddStringToObject(p_svc2, "type", "string");
                cJSON_AddItemToObject(pr2, "service", p_svc2);
                cJSON_AddItemToObject(s2, "properties", pr2);
                cJSON_AddItemToObject(t2, "inputSchema", s2);
                cJSON_AddItemToArray(tools, t2);

                // Tool 5: manage_service
                cJSON *t3 = cJSON_CreateObject();
                cJSON_AddStringToObject(t3, "name", "manage_service");
                cJSON_AddStringToObject(t3, "description", "Điều khiển service: start, stop, restart, reload, status, enable, disable");
                cJSON *s3 = cJSON_CreateObject(); cJSON_AddStringToObject(s3, "type", "object");
                cJSON *props = cJSON_CreateObject();
                cJSON *p1 = cJSON_CreateObject(); cJSON_AddStringToObject(p1, "type", "string");
                cJSON *p2 = cJSON_CreateObject(); cJSON_AddStringToObject(p2, "type", "string");
                cJSON_AddItemToObject(props, "service", p1);
                cJSON_AddItemToObject(props, "action", p2);
                cJSON_AddItemToObject(s3, "properties", props);
                cJSON_AddItemToObject(t3, "inputSchema", s3);
                cJSON_AddItemToArray(tools, t3);

                cJSON_AddItemToObject(result, "tools", tools);
                cJSON_AddItemToObject(resp, "result", result);
                send_response(resp);
            } else if (strcmp(method->valuestring, "tools/call") == 0) {
                cJSON *params = cJSON_GetObjectItem(req, "params");
                cJSON *name = cJSON_GetObjectItem(params, "name");
                cJSON *args = cJSON_GetObjectItem(params, "arguments");
                if (name && cJSON_IsString(name)) {
                    cJSON *res = handle_tool_call(name->valuestring, args);
                    cJSON_AddItemToObject(resp, "result", res);
                    send_response(resp);
                }
            } else if (id) {
                // Tránh treo kết nối: Phản hồi mã lỗi chuẩn JSON-RPC -32601 Method not found
                cJSON *err = cJSON_CreateObject();
                cJSON_AddNumberToObject(err, "code", -32601);
                cJSON_AddStringToObject(err, "message", "Method not found");
                cJSON_AddItemToObject(resp, "error", err);
                send_response(resp);
            }
            cJSON_Delete(req);
        }
    } else {
        while (g_running) {
            sleep(1);
        }
    }

    if (g_enable_web && g_server_fd >= 0) {
        close(g_server_fd);
        g_server_fd = -1;
        pthread_join(web_tid, NULL);
    }

    return 0;
}
