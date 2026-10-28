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
#include <cjson/cJSON.h>

#define BUFFER_SIZE 131072
#define WEB_PORT 8080

int is_valid_service_name(const char *name) {
    if (!name || strlen(name) == 0 || strlen(name) > 128) return 0;
    for (int i = 0; name[i] != '\0'; i++) {
        char c = name[i];
        if (!isalnum(c) && c != '_' && c != '-' && c != '.' && c != '@') return 0;
    }
    return 1;
}

char* run_cmd_safe(char *const argv[]) {
    int pipefd[2];
    if (pipe(pipefd) == -1) return strdup("Lỗi: Không tạo được pipe.");

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return strdup("Lỗi: Không fork được tiến trình.");
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
    output[0] = '\0';

    ssize_t bytes_read;
    while ((bytes_read = read(pipefd[0], buffer, sizeof(buffer) - 1)) > 0) {
        buffer[bytes_read] = '\0';
        char *new_output = realloc(output, total_len + bytes_read + 1);
        if (!new_output) {
            free(output);
            close(pipefd[0]);
            return strdup("Lỗi: Hết RAM.");
        }
        output = new_output;
        memcpy(output + total_len, buffer, bytes_read + 1);
        total_len += bytes_read;
    }
    close(pipefd[0]);
    waitpid(pid, NULL, 0);
    return output;
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
    if (len >= dest_sz) len = dest_sz - 1;
    
    size_t d_idx = 0;
    for (size_t i = 0; i < len && d_idx < dest_sz - 1; i++) {
        if (start[i] == '+') dest[d_idx++] = ' ';
        else if (start[i] == '\r' || start[i] == '\n') break;
        else dest[d_idx++] = start[i];
    }
    dest[d_idx] = '\0';
}

void* web_dashboard_thread(void *arg) {
    (void)arg;
    int server_fd, new_socket;
    struct sockaddr_in address;
    int opt = 1;
    int addrlen = sizeof(address);

    if ((server_fd = socket(AF_INET, SOCK_STREAM, 0)) == 0) return NULL;

    setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(WEB_PORT);

    if (bind(server_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        close(server_fd);
        return NULL;
    }

    if (listen(server_fd, 10) < 0) {
        close(server_fd);
        return NULL;
    }

    while (1) {
        new_socket = accept(server_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (new_socket < 0) continue;

        char req_buf[8192] = {0};
        ssize_t r = read(new_socket, req_buf, sizeof(req_buf) - 1);
        if (r <= 0) {
            close(new_socket);
            continue;
        }

        char *action_feedback = malloc(16384);
        action_feedback[0] = '\0';

        if (strncmp(req_buf, "POST /api/action", 16) == 0) {
            char *body = strstr(req_buf, "\r\n\r\n");
            if (body) {
                body += 4;
                char service[128] = {0};
                char action[32] = {0};

                parse_post_param(body, "service", service, sizeof(service));
                parse_post_param(body, "action", action, sizeof(action));

                if (!is_valid_service_name(service)) {
                    snprintf(action_feedback, 16384,
                             "<div class='alert err'>❌ <b>Lỗi:</b> Tên dịch vụ <code>%s</code> không hợp lệ!</div>", service);
                } else if (strcmp(action, "diagnose") == 0) {
                    char *show_argv[] = {"/bin/systemctl", "show", service, "--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts,MainPID", NULL};
                    char *show_res = run_cmd_safe(show_argv);
                    char *log_argv[] = {"/usr/bin/sudo", "/bin/journalctl", "-u", service, "-n", "15", "--no-pager", NULL};
                    char *log_res = run_cmd_safe(log_argv);

                    snprintf(action_feedback, 16384,
                             "<div class='alert info'>🔍 <b>Báo cáo chẩn đoán chuyên sâu cho <code>%s</code>:</b>"
                             "<pre>--- THÔNG SỐ KỸ THUẬT ---\n%s\n--- 15 DÒNG LOG GẦN NHẤT ---\n%s</pre></div>",
                             service, show_res, strlen(log_res) > 0 ? log_res : "(Không có dữ liệu log)");
                    free(show_res);
                    free(log_res);
                } else if (strcmp(action, "start") == 0 || strcmp(action, "stop") == 0 ||
                           strcmp(action, "restart") == 0 || strcmp(action, "status") == 0) {
                    char *argv[] = {"/usr/bin/sudo", "/bin/systemctl", action, service, NULL};
                    char *cmd_res = run_cmd_safe(argv);
                    snprintf(action_feedback, 16384,
                             "<div class='alert ok'>✅ <b>Kết quả thực thi lệnh <code>%s</code> trên <code>%s</code>:</b>"
                             "<pre>%s</pre></div>",
                             action, service, strlen(cmd_res) > 0 ? cmd_res : "Thao tác hoàn tất thành công (Return code 0).");
                    free(cmd_res);
                }
            }
        }

        // Lấy thông số hệ thống
        char *status_argv[] = {"/bin/systemctl", "is-system-running", NULL};
        char *sys_state = run_cmd_safe(status_argv);
        sys_state[strcspn(sys_state, "\r\n")] = 0;

        char *uptime_argv[] = {"/usr/bin/uptime", "-p", NULL};
        char *uptime_str = run_cmd_safe(uptime_argv);

        char *load_argv[] = {"/bin/cat", "/proc/loadavg", NULL};
        char *load_str = run_cmd_safe(load_argv);
        if (strlen(load_str) > 15) load_str[14] = '\0'; // Chỉ lấy 3 chỉ số đầu

        char *free_argv[] = {"/usr/bin/free", "-h", NULL};
        char *free_str = run_cmd_safe(free_argv);

        char *failed_argv[] = {"/bin/systemctl", "--failed", "--no-legend", "--plain", NULL};
        char *failed_units = run_cmd_safe(failed_argv);

        char *running_argv[] = {"/bin/systemctl", "list-units", "--type=service", "--state=running", "--no-legend", "--plain", NULL};
        char *running_units = run_cmd_safe(running_argv);
        // Cắt gọn danh sách running để vừa khung hiển thị
        if (strlen(running_units) > 1800) {
            running_units[1800] = '\0';
            strcat(running_units, "\n... (Đã thu gọn bớt)");
        }

        int is_degraded = (strcmp(sys_state, "running") != 0);

        char *html_body = malloc(BUFFER_SIZE);
        snprintf(html_body, BUFFER_SIZE,
            "<!DOCTYPE html><html><head><meta charset='utf-8'><title>MCP Linux Dashboard</title>"
            "<style>"
            "*{box-sizing:border-box;}"
            "body{font-family:'Segoe UI',system-ui,sans-serif;background:#11111b;color:#cdd6f4;padding:2rem;max-width:1050px;margin:auto;line-height:1.5;}"
            ".header{display:flex;justify-content:space-between;align-items:center;border-bottom:1px solid #313244;padding-bottom:1rem;margin-bottom:1.5rem;}"
            "h1{color:#89b4fa;margin:0;font-size:1.75rem;display:flex;align-items:center;gap:10px;}"
            ".badge{padding:6px 14px;border-radius:999px;font-weight:700;font-size:0.85rem;text-transform:uppercase;letter-spacing:0.5px;}"
            ".badge-ok{background:#a6e3a1;color:#11111b;} .badge-warn{background:#f38ba8;color:#11111b;}"
            ".metrics-grid{display:grid;grid-template-columns:repeat(3,1fr);gap:1rem;margin-bottom:1.5rem;}"
            ".metric-box{background:#1e1e2e;border:1px solid #313244;padding:1.2rem;border-radius:12px;}"
            ".metric-title{font-size:0.8rem;color:#a6adc8;text-transform:uppercase;letter-spacing:0.5px;margin-bottom:6px;}"
            ".metric-val{font-size:1.1rem;font-weight:700;color:#f5e0dc;}"
            ".card{background:#1e1e2e;border:1px solid #313244;padding:1.5rem;border-radius:12px;margin-bottom:1.5rem;box-shadow:0 8px 20px rgba(0,0,0,0.25);}"
            ".card h3{margin-top:0;margin-bottom:1rem;color:#cba6f7;display:flex;align-items:center;gap:8px;font-size:1.15rem;}"
            ".input-row{margin-bottom:12px;}"
            "input[type=text]{background:#181825;border:1px solid #45475a;color:#cdd6f4;padding:12px 16px;border-radius:8px;width:100%%;font-size:0.95rem;outline:none;transition:0.2s;}"
            "input[type=text]:focus{border-color:#89b4fa;box-shadow:0 0 0 2px rgba(137,180,250,0.2);}"
            ".chips{display:flex;gap:8px;flex-wrap:wrap;margin-bottom:16px;align-items:center;}"
            ".chip-label{font-size:0.8rem;color:#a6adc8;margin-right:4px;}"
            ".chip{background:#313244;color:#cdd6f4;border:1px solid #45475a;padding:4px 10px;border-radius:6px;font-size:0.8rem;cursor:pointer;transition:0.15s;}"
            ".chip:hover{background:#45475a;color:#fff;}"
            ".chip-danger{border-color:#f38ba8;color:#f38ba8;}"
            ".btn-grid{display:grid;grid-template-columns:repeat(5,1fr);gap:10px;}"
            ".btn{display:flex;align-items:center;justify-content:center;gap:8px;height:44px;border:none;border-radius:8px;font-weight:700;font-size:0.9rem;cursor:pointer;color:#11111b;transition:all 0.15s ease;width:100%%;}"
            ".btn:hover{filter:brightness(1.1);transform:translateY(-2px);}"
            ".btn:active{transform:translateY(0);}"
            ".btn-start{background:#a6e3a1;} .btn-stop{background:#f38ba8;} .btn-restart{background:#f9e2af;} .btn-status{background:#89dceb;} .btn-diag{background:#cba6f7;}"
            ".two-col{display:grid;grid-template-columns:1fr 1fr;gap:1.5rem;}"
            "pre{background:#11111b;border:1px solid #313244;color:#a6e3a1;padding:1rem;border-radius:8px;overflow-x:auto;font-size:0.82rem;margin:0;max-height:280px;overflow-y:auto;}"
            ".alert{padding:1.2rem;border-radius:10px;margin-bottom:1.5rem;}"
            ".alert pre{margin-top:10px;background:#11111b;}"
            ".alert.ok{background:#1e2e28;border:1px solid #a6e3a1;color:#a6e3a1;}"
            ".alert.err{background:#2e1e24;border:1px solid #f38ba8;color:#f38ba8;}"
            ".alert.info{background:#242038;border:1px solid #cba6f7;color:#cba6f7;}"
            "</style>"
            "<script>"
            "function pickService(name){document.getElementById('svc_input').value=name;}"
            "</script>"
            "</head><body>"
            "<div class='header'>"
            "<h1>🚀 MCP Linux Service Center</h1>"
            "<div>Trạng thái OS: <span class='badge %s'>%s</span></div>"
            "</div>"
            "<div class='metrics-grid'>"
            "<div class='metric-box'><div class='metric-title'>⏱ Thời gian chạy (Uptime)</div><div class='metric-val'>%s</div></div>"
            "<div class='metric-box'><div class='metric-title'>🔥 Tải CPU (Load Avg 1/5/15m)</div><div class='metric-val'>%s</div></div>"
            "<div class='metric-box'><div class='metric-title'>🧠 Giao thức MCP</div><div class='metric-val' style='color:#a6e3a1;'>JSON-RPC 2.0 + HTTP:8080</div></div>"
            "</div>"
            "%s"
            "<div class='card'>"
            "<h3>⚡ Bảng điều khiển & Chẩn đoán dịch vụ</h3>"
            "<form action='/api/action' method='POST'>"
            "<div class='input-row'>"
            "<input type='text' id='svc_input' name='service' placeholder='Nhập tên dịch vụ hoặc chọn nhanh bên dưới...' required>"
            "</div>"
            "<div class='chips'>"
            "<span class='chip-label'>Chọn nhanh:</span>"
            "<span class='chip' onclick=\"pickService('cron')\">cron</span>"
            "<span class='chip' onclick=\"pickService('ssh')\">ssh</span>"
            "<span class='chip' onclick=\"pickService('systemd-resolved')\">systemd-resolved</span>"
            "<span class='chip' onclick=\"pickService('ufw')\">ufw</span>"
            "<span class='chip chip-danger' onclick=\"pickService('demo-crash')\">🧪 demo-crash (Test lỗi)</span>"
            "</div>"
            "<div class='btn-grid'>"
            "<button class='btn btn-start' name='action' value='start'><span>▶</span><span>Start</span></button>"
            "<button class='btn btn-stop' name='action' value='stop'><span>⏹</span><span>Stop</span></button>"
            "<button class='btn btn-restart' name='action' value='restart'><span>🔄</span><span>Restart</span></button>"
            "<button class='btn btn-status' name='action' value='status'><span>ℹ</span><span>Status</span></button>"
            "<button class='btn btn-diag' name='action' value='diagnose'><span>🔍</span><span>Diagnose</span></button>"
            "</div>"
            "</form>"
            "</div>"
            "<div class='two-col'>"
            "<div class='card'><h3>🚨 Dịch vụ đang lỗi (Failed Units)</h3><pre style='color:#f38ba8;'>%s</pre></div>"
            "<div class='card'><h3>🟢 Dịch vụ đang hoạt động (Running)</h3><pre>%s</pre></div>"
            "</div>"
            "<div class='card'><h3>📊 Chi tiết bộ nhớ hệ thống (RAM & Swap)</h3><pre style='color:#89dceb;'>%s</pre></div>"
            "</body></html>",
            is_degraded ? "badge-warn" : "badge-ok", sys_state,
            uptime_str, load_str, action_feedback,
            strlen(failed_units) > 0 ? failed_units : "✅ Không có service nào bị lỗi. Hệ thống hoạt động hoàn hảo!",
            running_units, free_str);

        char *http_resp = malloc(BUFFER_SIZE + 1024);
        int len = snprintf(http_resp, BUFFER_SIZE + 1024,
            "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=UTF-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
            strlen(html_body), html_body);

        ssize_t w = write(new_socket, http_resp, len);
        (void)w;
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
    } else if (strcmp(tool_name, "service_diagnose") == 0) {
        cJSON *svc_item = cJSON_GetObjectItem(args, "service");
        if (!svc_item || !cJSON_IsString(svc_item) || !is_valid_service_name(svc_item->valuestring)) {
            cJSON_AddStringToObject(content_obj, "text", "Lỗi: Tên service không hợp lệ.");
        } else {
            const char *svc = svc_item->valuestring;
            char *show_argv[] = {"/bin/systemctl", "show", (char*)svc, "--property=ActiveState,SubState,Result,ExecMainStatus,NRestarts", NULL};
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
            cJSON_AddStringToObject(content_obj, "text", "Lỗi: Thiếu 'service' hoặc 'action'.");
        } else {
            const char *svc = svc_item->valuestring;
            const char *action = action_item->valuestring;
            if (!is_valid_service_name(svc)) {
                cJSON_AddStringToObject(content_obj, "text", "Lỗi: Ký tự tên service không an toàn.");
            } else {
                char *argv[] = {"/usr/bin/sudo", "/bin/systemctl", (char*)action, (char*)svc, NULL};
                char *res = run_cmd_safe(argv);
                cJSON_AddStringToObject(content_obj, "text", strlen(res) > 0 ? res : "Thực thi thành công.");
                free(res);
            }
        }
    }
    cJSON_AddItemToArray(content_arr, content_obj);
    cJSON *result = cJSON_CreateObject();
    cJSON_AddItemToObject(result, "content", content_arr);
    return result;
}

int main() {
    pthread_t web_tid;
    pthread_create(&web_tid, NULL, web_dashboard_thread, NULL);

    char line[65536];
    while (fgets(line, sizeof(line), stdin)) {
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
            cJSON_AddStringToObject(server_info, "version", "2.0.0");
            cJSON_AddItemToObject(result, "serverInfo", server_info);
            cJSON_AddItemToObject(result, "capabilities", cJSON_CreateObject());
            cJSON_AddItemToObject(resp, "result", result);
            send_response(resp);
        } else if (strcmp(method->valuestring, "tools/list") == 0) {
            cJSON *result = cJSON_CreateObject();
            cJSON *tools = cJSON_CreateArray();

            cJSON *t1 = cJSON_CreateObject();
            cJSON_AddStringToObject(t1, "name", "list_failed_services");
            cJSON_AddStringToObject(t1, "description", "Liệt kê các dịch vụ systemd bị lỗi");
            cJSON *s1 = cJSON_CreateObject(); cJSON_AddStringToObject(s1, "type", "object");
            cJSON_AddItemToObject(t1, "inputSchema", s1);
            cJSON_AddItemToArray(tools, t1);

            cJSON *t2 = cJSON_CreateObject();
            cJSON_AddStringToObject(t2, "name", "service_diagnose");
            cJSON_AddStringToObject(t2, "description", "Chẩn đoán thông số và đọc log của service");
            cJSON *s2 = cJSON_CreateObject(); cJSON_AddStringToObject(s2, "type", "object");
            cJSON *pr2 = cJSON_CreateObject();
            cJSON *p_svc2 = cJSON_CreateObject(); cJSON_AddStringToObject(p_svc2, "type", "string");
            cJSON_AddItemToObject(pr2, "service", p_svc2);
            cJSON_AddItemToObject(s2, "properties", pr2);
            cJSON_AddItemToObject(t2, "inputSchema", s2);
            cJSON_AddItemToArray(tools, t2);

            cJSON *t3 = cJSON_CreateObject();
            cJSON_AddStringToObject(t3, "name", "manage_service");
            cJSON_AddStringToObject(t3, "description", "start, stop, restart hoặc xem status");
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
        }
        cJSON_Delete(req);
    }

    pthread_join(web_tid, NULL);
    return 0;
}
