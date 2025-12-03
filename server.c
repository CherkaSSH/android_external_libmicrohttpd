#include <microhttpd.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <dirent.h>
#include <unistd.h>
#include <errno.h>
#include <stdint.h>
#include <limits.h>
#include <pwd.h>
#include <grp.h>
#include <time.h>
#include <sys/statvfs.h>

#define PORT 80
#define MAX_PATH_LEN 512
#define MAX_CMD_OUTPUT 65536
#define POSTBUFFERSIZE 512

// 默认配置
static const char *DEFAULT_UPLOAD_DIR = "/tmp";
static const char *FS_ROOT = "/";
static const char *INDEX_HTML_PATH = "/system/etc/www/index.html";

// 语言包定义
typedef struct {
    const char *key;
    const char *zh;
    const char *en;
} translation_t;

static translation_t translations[] = {
    {"server_starting", "启动文件服务器，端口：%d，根目录：%s，上传目录：%s", "Starting file server, port: %d, root: %s, upload dir: %s"},
    {"server_running", "服务器运行中...", "Server running..."},
    {"server_start_failed", "服务器启动失败: %s", "Server start failed: %s"},
    {"missing_parameters", "缺少参数", "Missing parameters"},
    {"missing_path_param", "缺少路径参数", "Missing path parameter"},
    {"invalid_path", "无效路径", "Invalid path"},
    {"path_not_found", "路径不存在: %s", "Path not found: %s"},
    {"path_not_directory", "路径不是目录", "Path is not a directory"},
    {"directory_not_accessible", "目录不可访问: %s", "Directory not accessible: %s"},
    {"path_valid", "路径有效", "Path valid"},
    {"space_sufficient", "空间足够", "Space sufficient"},
    {"space_insufficient", "需要: %zu bytes, 可用: %llu bytes", "required: %zu bytes, available: %llu bytes"},
    {"disk_space_error", "无法获取目录空间信息: %s", "Cannot get directory space info: %s"},
    {"file_not_found", "文件不存在", "File not found"},
    {"not_regular_file", "不是常规文件", "Not a regular file"},
    {"failed_read_directory", "读取目录失败", "Failed to read directory"},
    {"index_not_found", "index.html 未找到", "index.html not found"},
    {"template_error", "模板错误", "Template error"},
    {"upload_ok", "上传成功", "Upload OK"},
    {"upload_failed", "上传失败", "Upload failed"},
    {"upload_completion_failed", "上传完成失败", "Upload completion failed"},
    {"missing_filename", "缺少文件名", "Missing filename"},
    {"no_upload_in_progress", "没有进行中的上传", "No upload in progress"},
    {"upload_cancelled", "上传已取消", "Upload cancelled"},
    {"rename_failed", "重命名失败: %s", "Rename failed: %s"},
    {"delete_failed", "删除失败: %s", "Delete failed: %s"},
    {"command_execution_failed", "命令执行失败", "Command execution failed"},
    {"no_command", "没有命令", "No command"},
    {"unknown_endpoint", "未知端点", "Unknown endpoint"},
    {"method_not_allowed", "方法不允许", "Method not allowed"},
    {"not_found", "未找到", "Not found"},
    {"upload_processing", "正在处理上传...", "Processing upload..."},
    {"upload_success", "文件上传成功", "File uploaded successfully"},
    {"upload_file_exists", "文件已存在", "File already exists"},
    {"upload_permission_denied", "权限不足，无法上传文件", "Permission denied, cannot upload file"},
    {"upload_disk_full", "磁盘空间不足", "Disk space is full"}
};

// 连接信息结构体
struct connection_info_struct {
    int is_post;
    int is_upload;
    int is_cmd;
    char *post_data;
    size_t post_data_size;
    size_t post_data_alloc;
    
    // 上传相关字段
    int upload_fd;
    char *upload_filename;
    char *upload_dir;
    int upload_started;
    char *upload_file_path;
    int upload_cancelled;
    struct MHD_Connection *connection;
    
    // 语言支持
    char language[10];
};

// 工具函数：检测客户端语言
static const char *detect_client_language(struct MHD_Connection *connection) {
    const char *accept_language = MHD_lookup_connection_value(connection, MHD_HEADER_KIND, "Accept-Language");
    
    if (accept_language) {
        if (strstr(accept_language, "zh") != NULL) {
            return "zh";
        }
    }
    
    return "en";
}

// 工具函数：获取翻译文本
static const char *get_translation(const char *key, const char *lang) {
    for (size_t i = 0; i < sizeof(translations) / sizeof(translations[0]); i++) {
        if (strcmp(translations[i].key, key) == 0) {
            if (strcmp(lang, "zh") == 0) {
                return translations[i].zh;
            } else {
                return translations[i].en;
            }
        }
    }
    return key;
}

// 工具函数：安全路径构建
static int build_safe_path(const char *rel, char *out, size_t out_len) {
    if (!rel || !*rel) {
        snprintf(out, out_len, "%s", FS_ROOT);
        return 0;
    }

    if (rel[0] == '/') {
        if (strlen(rel) >= out_len) return -1;
        strcpy(out, rel);
        return 0;
    }

    if (strlen(FS_ROOT) + 1 + strlen(rel) >= out_len)
        return -1;

    snprintf(out, out_len, "%s/%s", FS_ROOT, rel);

    if (strstr(out, "/../") != NULL)
        return -1;

    return 0;
}

// 工具函数：检查路径有效性
static int check_path_valid(const char *path, char *errmsg, size_t errmsg_len, const char *lang) {
    char real_path[MAX_PATH_LEN];
    if (build_safe_path(path, real_path, sizeof(real_path)) != 0) {
        snprintf(errmsg, errmsg_len, "%s", get_translation("invalid_path", lang));
        return -1;
    }

    struct stat st;
    if (stat(real_path, &st) != 0) {
        snprintf(errmsg, errmsg_len, get_translation("path_not_found", lang), strerror(errno));
        return -1;
    }

    if (!S_ISDIR(st.st_mode)) {
        snprintf(errmsg, errmsg_len, "%s", get_translation("path_not_directory", lang));
        return -1;
    }

    if (access(real_path, R_OK) != 0) {
        snprintf(errmsg, errmsg_len, get_translation("directory_not_accessible", lang), strerror(errno));
        return -1;
    }

    return 0;
}

// 工具函数：权限字符串转换
static void mode_to_string(mode_t mode, char *buf, size_t len) {
    if (len < 11) {
        if (len > 0) buf[0] = '\0';
        return;
    }
    buf[0] = S_ISDIR(mode) ? 'd' : '-';
    buf[1] = (mode & S_IRUSR) ? 'r' : '-';
    buf[2] = (mode & S_IWUSR) ? 'w' : '-';
    buf[3] = (mode & S_IXUSR) ? 'x' : '-';
    buf[4] = (mode & S_IRGRP) ? 'r' : '-';
    buf[5] = (mode & S_IWGRP) ? 'w' : '-';
    buf[6] = (mode & S_IXGRP) ? 'x' : '-';
    buf[7] = (mode & S_IROTH) ? 'r' : '-';
    buf[8] = (mode & S_IWOTH) ? 'w' : '-';
    buf[9] = (mode & S_IXOTH) ? 'x' : '-';
    buf[10] = '\0';
}

// 工具函数：UID转用户名
static const char *uid_to_name(uid_t uid) {
    struct passwd *pw = getpwuid(uid);
    return pw ? pw->pw_name : "-";
}

// 工具函数：GID转组名
static const char *gid_to_name(gid_t gid) {
    struct group *gr = getgrgid(gid);
    return gr ? gr->gr_name : "-";
}

// 工具函数：智能显示文件大小
static char *format_size(long long size) {
    static char buf[32];
    if (size < 1024) {
        snprintf(buf, sizeof(buf), "%lld B", size);
    } else if (size < 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.2f KB", (double)size / 1024);
    } else if (size < 1024 * 1024 * 1024) {
        snprintf(buf, sizeof(buf), "%.2f MB", (double)size / (1024 * 1024));
    } else {
        snprintf(buf, sizeof(buf), "%.2f GB", (double)size / (1024 * 1024 * 1024));
    }
    return buf;
}

// 工具函数：检查磁盘空间
static int check_disk_space(const char *path, size_t required_size, char *errmsg, size_t errmsg_len, const char *lang) {
    struct statvfs st;
    if (statvfs(path, &st) != 0) {
        snprintf(errmsg, errmsg_len, get_translation("disk_space_error", lang), strerror(errno));
        return -1;
    }
    
    unsigned long long free_space = (unsigned long long)st.f_bavail * st.f_frsize;
    
    if (free_space < required_size) {
        snprintf(errmsg, errmsg_len, get_translation("space_insufficient", lang), required_size, free_space);
        return -1;
    }
    
    return 0;
}

// 工具函数：从路径提取文件名
static const char *get_filename_from_path(const char *path) {
    if (!path) return "download";
    const char *filename = strrchr(path, '/');
    return filename ? filename + 1 : path;
}

// 工具函数：URL编码文件名
static char *url_encode_filename(const char *filename) {
    if (!filename) return strdup("download");
    
    size_t len = strlen(filename);
    char *encoded = malloc(len * 3 + 1);
    if (!encoded) return NULL;
    
    char *p = encoded;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = filename[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || 
            (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.') {
            *p++ = c;
        } else {
            sprintf(p, "%%%02X", c);
            p += 3;
        }
    }
    *p = '\0';
    return encoded;
}

// 目录条目结构
struct dir_entry {
    char name[256];
    int is_dir;
    struct stat st;
};

// 比较函数用于排序
static int compare_entries(const void *a, const void *b) {
    const struct dir_entry *entry_a = (const struct dir_entry *)a;
    const struct dir_entry *entry_b = (const struct dir_entry *)b;
    
    if (entry_a->is_dir && !entry_b->is_dir) return -1;
    if (!entry_a->is_dir && entry_b->is_dir) return 1;
    
    return strcasecmp(entry_a->name, entry_b->name);
}

// 工具函数：生成目录列表HTML
static char *generate_dir_listing_html(const char *rel_path) {
    char real_path[MAX_PATH_LEN];
    if (build_safe_path(rel_path, real_path, sizeof(real_path)) != 0) {
        return NULL;
    }

    DIR *dir = opendir(real_path);
    if (!dir) {
        return NULL;
    }

    struct dir_entry *entries = NULL;
    int num_entries = 0;
    int allocated = 100;
    
    entries = malloc(allocated * sizeof(struct dir_entry));
    if (!entries) {
        closedir(dir);
        return NULL;
    }

    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL) {
        const char *name = ent->d_name;
        if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
            continue;

        if (num_entries >= allocated) {
            allocated *= 2;
            struct dir_entry *new_entries = realloc(entries, allocated * sizeof(struct dir_entry));
            if (!new_entries) {
                free(entries);
                closedir(dir);
                return NULL;
            }
            entries = new_entries;
        }

        char child_rel[MAX_PATH_LEN];
        if (strcmp(rel_path, "/") == 0) {
            snprintf(child_rel, sizeof(child_rel), "/%s", name);
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, name);
        }

        char child_real[MAX_PATH_LEN];
        if (build_safe_path(child_rel, child_real, sizeof(child_real)) != 0)
            continue;

        struct stat st;
        if (lstat(child_real, &st) != 0)
            continue;

        strncpy(entries[num_entries].name, name, sizeof(entries[num_entries].name) - 1);
        entries[num_entries].name[sizeof(entries[num_entries].name) - 1] = '\0';
        entries[num_entries].is_dir = S_ISDIR(st.st_mode);
        entries[num_entries].st = st;
        num_entries++;
    }

    closedir(dir);

    qsort(entries, num_entries, sizeof(struct dir_entry), compare_entries);

    size_t buf_size = 16384;
    char *buf = malloc(buf_size);
    if (!buf) {
        free(entries);
        return NULL;
    }
    buf[0] = '\0';

    for (int i = 0; i < num_entries; i++) {
        const char *name = entries[i].name;
        struct stat st = entries[i].st;
        int is_dir = entries[i].is_dir;

        char child_rel[MAX_PATH_LEN];
        if (strcmp(rel_path, "/") == 0) {
            snprintf(child_rel, sizeof(child_rel), "/%s", name);
        } else {
            snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, name);
        }

        char child_real[MAX_PATH_LEN];
        if (build_safe_path(child_rel, child_real, sizeof(child_real)) != 0)
            continue;

        char mode_str[11];
        mode_to_string(st.st_mode, mode_str, sizeof(mode_str));
        const char *owner = uid_to_name(st.st_uid);
        const char *group = gid_to_name(st.st_gid);

        char row[2048];
        if (is_dir) {
            snprintf(row, sizeof(row),
                     "<tr class=\"file-row\">"
                     "<td>目录</td>"
                     "<td><a href=\"/?path=%s\" class=\"dir-link\">%s</a></td>"
                     "<td>-</td>"
                     "<td>%s</td>"
                     "<td>%s</td>"
                     "<td>%s</td>"
                     "</tr>\n",
                     child_rel, name, mode_str, owner, group);
        } else {
            const char *size_str = format_size((long long)st.st_size);
            snprintf(row, sizeof(row),
                     "<tr class=\"file-row\">"
                     "<td>文件</td>"
                     "<td><a href=\"/download?path=%s\" class=\"file-link\">%s</a></td>"
                     "<td>%s</td>"
                     "<td>%s</td>"
                     "<td>%s</td>"
                     "<td>%s</td>"
                     "</tr>\n",
                     child_real, name, size_str, mode_str, owner, group);
        }

        if (strlen(buf) + strlen(row) + 1 < buf_size) {
            strcat(buf, row);
        } else {
            buf_size *= 2;
            char *new_buf = realloc(buf, buf_size);
            if (!new_buf) {
                free(buf);
                free(entries);
                return NULL;
            }
            buf = new_buf;
            strcat(buf, row);
        }
    }

    free(entries);
    return buf;
}

// 工具函数：读取文件内容
static char *read_file(const char *path, size_t *out_size) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    if (sz < 0) {
        fclose(fp);
        return NULL;
    }
    fseek(fp, 0, SEEK_SET);
    
    char *buf = malloc(sz + 1);
    if (!buf) {
        fclose(fp);
        return NULL;
    }
    
    size_t n = fread(buf, 1, sz, fp);
    fclose(fp);
    buf[n] = '\0';
    if (out_size) *out_size = n;
    return buf;
}

// 工具函数：替换模板占位符
static char *replace_placeholder(const char *tmpl, const char *token, const char *value) {
    const char *pos = strstr(tmpl, token);
    if (!pos) return strdup(tmpl);
    
    size_t token_len = strlen(token);
    size_t value_len = strlen(value);
    size_t tmpl_len = strlen(tmpl);
    
    char *result = malloc(tmpl_len - token_len + value_len + 1);
    if (!result) return NULL;
    
    size_t prefix_len = pos - tmpl;
    memcpy(result, tmpl, prefix_len);
    memcpy(result + prefix_len, value, value_len);
    strcpy(result + prefix_len + value_len, pos + token_len);
    
    return result;
}

// 工具函数：执行命令
static char *run_command(const char *cmd) {
    if (!cmd || !*cmd) {
        char *empty = malloc(1);
        if (empty) empty[0] = '\0';
        return empty;
    }

    FILE *fp = popen(cmd, "r");
    if (!fp) {
        char *err = malloc(128);
        if (err) snprintf(err, 128, "Failed to run command: %s\n", strerror(errno));
        return err;
    }

    char *buf = malloc(MAX_CMD_OUTPUT);
    if (!buf) {
        pclose(fp);
        return NULL;
    }
    
    size_t total = 0;
    int c;
    while ((c = fgetc(fp)) != EOF && total + 1 < MAX_CMD_OUTPUT) {
        buf[total++] = c;
    }
    buf[total] = '\0';
    pclose(fp);
    return buf;
}

// HTTP响应函数（增强版，支持语言）
static enum MHD_Result send_response_with_lang(struct MHD_Connection *connection,
                         const char *content_type,
                         const char *data,
                         size_t size,
                         unsigned int status_code,
                         const char *lang) {
    struct MHD_Response *response = MHD_create_response_from_buffer(size,
                                                                    (void *)data,
                                                                    MHD_RESPMEM_MUST_COPY);
    if (!response) return MHD_NO;

    MHD_add_response_header(response, "Content-Type", content_type);
    MHD_add_response_header(response, "Access-Control-Allow-Origin", "*");
    MHD_add_response_header(response, "Content-Language", lang);
    
    enum MHD_Result ret = MHD_queue_response(connection, status_code, response);
    MHD_destroy_response(response);
    return ret;
}

// 简化的响应函数
static enum MHD_Result send_response(struct MHD_Connection *connection,
                         const char *content_type,
                         const char *data,
                         size_t size,
                         unsigned int status_code) {
    return send_response_with_lang(connection, content_type, data, size, status_code, "en");
}

// URL解码
static void urldecode(char *str) {
    char *src = str, *dst = str;
    while (*src) {
        if (*src == '+') {
            *dst++ = ' ';
            src++;
        } else if (*src == '%' && src[1] && src[2]) {
            char hex[3] = {src[1], src[2], 0};
            *dst++ = (char)strtol(hex, NULL, 16);
            src += 3;
        } else {
            *dst++ = *src++;
        }
    }
    *dst = '\0';
}

// 解析POST字段
static void parse_post_fields(char *body, char **out_cmd, char **out_upload_dir) {
    *out_cmd = NULL;
    *out_upload_dir = NULL;

    if (!body) return;

    char *saveptr;
    char *token = strtok_r(body, "&", &saveptr);
    while (token) {
        if (strncmp(token, "cmd=", 4) == 0) {
            *out_cmd = token + 4;
        } else if (strncmp(token, "upload_dir=", 11) == 0) {
            *out_upload_dir = token + 11;
        }
        token = strtok_r(NULL, "&", &saveptr);
    }
}

// 创建上传文件
static int create_upload_file(const char *target_dir, const char *filename, char *errmsg, size_t errmsg_len, const char *lang) {
    if (!filename || !*filename) {
        snprintf(errmsg, errmsg_len, "%s", get_translation("missing_filename", lang));
        return -1;
    }
    
    const char *dir = target_dir && *target_dir ? target_dir : DEFAULT_UPLOAD_DIR;
    
    // 确保目录存在
    if (mkdir(dir, 0777) != 0 && errno != EEXIST) {
        snprintf(errmsg, errmsg_len, "Permission denied: %s", strerror(errno));
        return -1;
    }

    char path[MAX_PATH_LEN];
    if (build_safe_path(dir, path, sizeof(path)) != 0) {
        snprintf(errmsg, errmsg_len, "%s", get_translation("invalid_path", lang));
        return -1;
    }

    size_t len = strlen(path);
    if (len > 0 && path[len-1] == '/')
        snprintf(path + len, sizeof(path) - len, "%s", filename);
    else
        snprintf(path + len, sizeof(path) - len, "/%s", filename);

    // 检查文件是否已存在
    if (access(path, F_OK) == 0) {
        snprintf(errmsg, errmsg_len, "%s", get_translation("upload_file_exists", lang));
        return -1;
    }

    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        snprintf(errmsg, errmsg_len, "Permission denied: %s", strerror(errno));
        return -1;
    }
    
    return fd;
}

// 删除已上传文件
static void delete_uploaded_file(const char *file_path) {
    if (file_path) {
        unlink(file_path);
    }
}

// 检查空间处理
static enum MHD_Result handle_check_space(struct MHD_Connection *connection, 
                                         const char *upload_dir, 
                                         size_t file_size,
                                         const char *lang) {
    char errmsg[256];
    char real_dir[MAX_PATH_LEN];
    
    if (build_safe_path(upload_dir, real_dir, sizeof(real_dir)) != 0) {
        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("invalid_path", lang), 
                                     strlen(get_translation("invalid_path", lang)), 400, lang);
    }
    
    if (check_disk_space(real_dir, file_size, errmsg, sizeof(errmsg), lang) != 0) {
        return send_response_with_lang(connection, "text/plain", errmsg, strlen(errmsg), 507, lang);
    }
    
    return send_response_with_lang(connection, "text/plain", 
                                 get_translation("space_sufficient", lang), 
                                 strlen(get_translation("space_sufficient", lang)), 200, lang);
}

// 检查路径处理
static enum MHD_Result handle_check_path(struct MHD_Connection *connection, 
                                        const char *path_param,
                                        const char *lang) {
    if (!path_param || !*path_param) {
        return send_response_with_lang(connection, "text/plain", 
                                      get_translation("missing_path_param", lang), 
                                      strlen(get_translation("missing_path_param", lang)), 400, lang);
    }
    
    char errmsg[256];
    if (check_path_valid(path_param, errmsg, sizeof(errmsg), lang) != 0) {
        return send_response_with_lang(connection, "text/plain", errmsg, strlen(errmsg), 404, lang);
    }
    
    return send_response_with_lang(connection, "text/plain", 
                                 get_translation("path_valid", lang), 
                                 strlen(get_translation("path_valid", lang)), 200, lang);
}

// 取消上传处理
static enum MHD_Result handle_cancel_upload(struct connection_info_struct *con_info,
                                           struct MHD_Connection *connection) {
    if (!con_info->upload_started) {
        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("no_upload_in_progress", con_info->language), 
                                     strlen(get_translation("no_upload_in_progress", con_info->language)), 400, 
                                     con_info->language);
    }
    
    if (con_info->upload_file_path) {
        delete_uploaded_file(con_info->upload_file_path);
        free(con_info->upload_file_path);
        con_info->upload_file_path = NULL;
    }
    
    if (con_info->upload_fd >= 0) {
        close(con_info->upload_fd);
        con_info->upload_fd = -1;
    }
    
    con_info->upload_started = 0;
    con_info->upload_cancelled = 1;
    
    return send_response_with_lang(connection, "text/plain", 
                                 get_translation("upload_cancelled", con_info->language), 
                                 strlen(get_translation("upload_cancelled", con_info->language)), 200, 
                                 con_info->language);
}

// 重命名文件处理
static enum MHD_Result handle_rename(struct MHD_Connection *connection, 
                                   const char *old_path, 
                                   const char *new_path,
                                   const char *lang) {
    char real_old_path[MAX_PATH_LEN];
    char real_new_path[MAX_PATH_LEN];
    
    if (build_safe_path(old_path, real_old_path, sizeof(real_old_path)) != 0 ||
        build_safe_path(new_path, real_new_path, sizeof(real_new_path)) != 0) {
        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("invalid_path", lang), 
                                     strlen(get_translation("invalid_path", lang)), 400, lang);
    }
    
    if (rename(real_old_path, real_new_path) != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), get_translation("rename_failed", lang), strerror(errno));
        return send_response_with_lang(connection, "text/plain", errmsg, strlen(errmsg), 500, lang);
    }
    
    return send_response_with_lang(connection, "text/plain", "OK", 2, 200, lang);
}

// 删除文件处理
static enum MHD_Result handle_delete(struct MHD_Connection *connection, 
                                     const char *path, 
                                     const char *type,
                                     const char *lang) {
    char real_path[MAX_PATH_LEN];
    
    if (build_safe_path(path, real_path, sizeof(real_path)) != 0) {
        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("invalid_path", lang), 
                                     strlen(get_translation("invalid_path", lang)), 400, lang);
    }
    
    int result;
    if (strcmp(type, "dir") == 0) {
        result = rmdir(real_path);
    } else {
        result = unlink(real_path);
    }
    
    if (result != 0) {
        char errmsg[256];
        snprintf(errmsg, sizeof(errmsg), get_translation("delete_failed", lang), strerror(errno));
        return send_response_with_lang(connection, "text/plain", errmsg, strlen(errmsg), 500, lang);
    }
    
    return send_response_with_lang(connection, "text/plain", "OK", 2, 200, lang);
}

// 处理文件上传
static enum MHD_Result handle_upload(struct connection_info_struct *con_info,
                                    struct MHD_Connection *connection,
                                    const char *upload_data,
                                    size_t upload_data_size) {
    const char *lang = con_info->language;
    
    // 如果是第一次调用，初始化上传
    if (!con_info->upload_started) {
        const char *filename = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "filename");
        const char *upload_dir = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "upload_dir");
        
        if (!filename || !*filename) {
            return send_response_with_lang(connection, "text/plain", 
                                         get_translation("missing_filename", lang), 
                                         strlen(get_translation("missing_filename", lang)), 400, lang);
        }
        
        char errmsg[256];
        con_info->upload_fd = create_upload_file(upload_dir, filename, errmsg, sizeof(errmsg), lang);
        if (con_info->upload_fd < 0) {
            return send_response_with_lang(connection, "text/plain", errmsg, strlen(errmsg), 500, lang);
        }
        
        // 保存文件路径
        const char *dir = upload_dir && *upload_dir ? upload_dir : DEFAULT_UPLOAD_DIR;
        char path[MAX_PATH_LEN];
        if (build_safe_path(dir, path, sizeof(path)) == 0) {
            size_t len = strlen(path);
            if (len > 0 && path[len-1] == '/')
                snprintf(path + len, sizeof(path) - len, "%s", filename);
            else
                snprintf(path + len, sizeof(path) - len, "/%s", filename);
            
            con_info->upload_file_path = strdup(path);
        }
        
        con_info->upload_started = 1;
        con_info->upload_filename = strdup(filename);
        con_info->upload_dir = strdup(upload_dir ? upload_dir : DEFAULT_UPLOAD_DIR);
    }
    
    // 写入数据到文件
    if (upload_data_size > 0) {
        ssize_t written = write(con_info->upload_fd, upload_data, upload_data_size);
        if (written != (ssize_t)upload_data_size) {
            // 写入失败
            close(con_info->upload_fd);
            con_info->upload_fd = -1;
            if (con_info->upload_file_path) {
                delete_uploaded_file(con_info->upload_file_path);
                free(con_info->upload_file_path);
                con_info->upload_file_path = NULL;
            }
            
            return send_response_with_lang(connection, "text/plain", 
                                         get_translation("upload_failed", lang), 
                                         strlen(get_translation("upload_failed", lang)), 500, lang);
        }
    }
    
    return MHD_YES;
}

// 完成文件上传
static enum MHD_Result finish_upload(struct connection_info_struct *con_info,
                                    struct MHD_Connection *connection) {
    const char *lang = con_info->language;
    
    if (con_info->upload_cancelled) {
        if (con_info->upload_file_path) {
            delete_uploaded_file(con_info->upload_file_path);
            free(con_info->upload_file_path);
            con_info->upload_file_path = NULL;
        }
        
        if (con_info->upload_fd >= 0) {
            close(con_info->upload_fd);
            con_info->upload_fd = -1;
        }
        
        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("upload_cancelled", lang), 
                                     strlen(get_translation("upload_cancelled", lang)), 200, lang);
    }
    
    if (con_info->upload_fd >= 0) {
        close(con_info->upload_fd);
        con_info->upload_fd = -1;
    }
    
    // 验证文件是否成功创建
    if (con_info->upload_file_path) {
        struct stat st;
        if (stat(con_info->upload_file_path, &st) != 0) {
            return send_response_with_lang(connection, "text/plain", 
                                         get_translation("upload_failed", lang), 
                                         strlen(get_translation("upload_failed", lang)), 500, lang);
        }
        
        free(con_info->upload_file_path);
        con_info->upload_file_path = NULL;
    }
    
    if (con_info->upload_filename) {
        free(con_info->upload_filename);
        con_info->upload_filename = NULL;
    }
    
    if (con_info->upload_dir) {
        free(con_info->upload_dir);
        con_info->upload_dir = NULL;
    }
    
    con_info->upload_started = 0;
    
    return send_response_with_lang(connection, "text/plain", 
                                 get_translation("upload_success", lang), 
                                 strlen(get_translation("upload_success", lang)), 200, lang);
}

// 主请求处理函数
static enum MHD_Result answer_to_connection(void *cls, struct MHD_Connection *connection,
                                const char *url, const char *method, const char *version,
                                const char *upload_data, size_t *upload_data_size,
                                void **con_cls) {
    (void)cls; (void)version;

    struct connection_info_struct *con_info = *con_cls;

    if (!con_info) {
        con_info = calloc(1, sizeof(*con_info));
        if (!con_info) return MHD_NO;
        
        con_info->post_data_alloc = POSTBUFFERSIZE;
        con_info->post_data = malloc(con_info->post_data_alloc);
        if (!con_info->post_data) {
            free(con_info);
            return MHD_NO;
        }
        con_info->post_data[0] = '\0';
        con_info->is_post = (strcmp(method, "POST") == 0);
        con_info->upload_fd = -1;
        con_info->upload_started = 0;
        con_info->upload_cancelled = 0;
        con_info->connection = connection;
        
        const char *lang = detect_client_language(connection);
        strncpy(con_info->language, lang, sizeof(con_info->language) - 1);
        con_info->language[sizeof(con_info->language) - 1] = '\0';
        
        *con_cls = con_info;
        return MHD_YES;
    }

    if (strcmp(method, "GET") == 0) {        
        if (strncmp(url, "/check_space", 12) == 0) {
            const char *dir = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "dir");
            const char *size_str = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "size");
            
            if (!dir || !size_str) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("missing_parameters", con_info->language), 
                                            strlen(get_translation("missing_parameters", con_info->language)), 400, 
                                            con_info->language);
            }
            
            size_t file_size = atoll(size_str);
            return handle_check_space(connection, dir, file_size, con_info->language);
        }

        if (strncmp(url, "/check_path", 11) == 0) {
            const char *path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
            return handle_check_path(connection, path, con_info->language);
        }

        if (strncmp(url, "/cancel_upload", 14) == 0) {
            return handle_cancel_upload(con_info, connection);
        }

        if (strncmp(url, "/download", 9) == 0) {
            const char *file_path = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
            if (!file_path) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("missing_path_param", con_info->language), 
                                            strlen(get_translation("missing_path_param", con_info->language)), 400, 
                                            con_info->language);
            }

            char safe_path[MAX_PATH_LEN];
            if (build_safe_path(file_path, safe_path, sizeof(safe_path)) != 0) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("invalid_path", con_info->language), 
                                            strlen(get_translation("invalid_path", con_info->language)), 400, 
                                            con_info->language);
            }

            int fd = open(safe_path, O_RDONLY);
            if (fd < 0) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("file_not_found", con_info->language), 
                                            strlen(get_translation("file_not_found", con_info->language)), 404, 
                                            con_info->language);
            }

            struct stat st;
            if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
                close(fd);
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("not_regular_file", con_info->language), 
                                            strlen(get_translation("not_regular_file", con_info->language)), 400, 
                                            con_info->language);
            }

            struct MHD_Response *response = MHD_create_response_from_fd(st.st_size, fd);
            if (!response) {
                close(fd);
                return MHD_NO;
            }

            const char *filename = get_filename_from_path(file_path);
            char *encoded = url_encode_filename(filename);
            
            char disposition[512];
            if (encoded) {
                snprintf(disposition, sizeof(disposition), 
                        "attachment; filename=\"%s\"; filename*=UTF-8''%s", filename, encoded);
                free(encoded);
            } else {
                snprintf(disposition, sizeof(disposition), "attachment; filename=\"%s\"", filename);
            }

            MHD_add_response_header(response, "Content-Type", "application/octet-stream");
            MHD_add_response_header(response, "Content-Disposition", disposition);

            enum MHD_Result ret = MHD_queue_response(connection, 200, response);
            MHD_destroy_response(response);
            return ret;
        }

        if (strcmp(url, "/") == 0) {
            const char *path_param = MHD_lookup_connection_value(connection, MHD_GET_ARGUMENT_KIND, "path");
            const char *rel_path = path_param ? path_param : "/";

            char *dir_html = generate_dir_listing_html(rel_path);
            if (!dir_html) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("failed_read_directory", con_info->language), 
                                            strlen(get_translation("failed_read_directory", con_info->language)), 500, 
                                            con_info->language);
            }

            size_t tmpl_size;
            char *tmpl = read_file(INDEX_HTML_PATH, &tmpl_size);
            if (!tmpl) {
                free(dir_html);
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("index_not_found", con_info->language), 
                                            strlen(get_translation("index_not_found", con_info->language)), 500, 
                                            con_info->language);
            }

            char *tmp1 = replace_placeholder(tmpl, "{{FILE_TABLE}}", dir_html);
            free(tmpl);
            free(dir_html);
            
            if (!tmp1) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("template_error", con_info->language), 
                                            strlen(get_translation("template_error", con_info->language)), 500, 
                                            con_info->language);
            }

            char *final_html = replace_placeholder(tmp1, "{{CURRENT_PATH}}", rel_path);
            free(tmp1);
            
            if (!final_html) {
                return send_response_with_lang(connection, "text/plain", 
                                            get_translation("template_error", con_info->language), 
                                            strlen(get_translation("template_error", con_info->language)), 500, 
                                            con_info->language);
            }

            enum MHD_Result ret = send_response_with_lang(connection, "text/html", final_html, strlen(final_html), 200, con_info->language);
            free(final_html);
            return ret;
        }

        return send_response_with_lang(connection, "text/plain", 
                                     get_translation("not_found", con_info->language), 
                                     strlen(get_translation("not_found", con_info->language)), 404, 
                                     con_info->language);
    }
    else if (strcmp(method, "POST") == 0) {
        if (*upload_data_size != 0) {
            if (strcmp(url, "/upload") == 0) {
                // 处理文件上传数据
                enum MHD_Result result = handle_upload(con_info, connection, upload_data, *upload_data_size);
                if (result != MHD_YES) {
                    return result;
                }
                *upload_data_size = 0;
                return MHD_YES;
            } else {
                // 处理其他POST数据
                size_t new_size = con_info->post_data_size + *upload_data_size;
                if (new_size + 1 > con_info->post_data_alloc) {
                    size_t new_alloc = con_info->post_data_alloc * 2;
                    if (new_alloc < new_size + 1) new_alloc = new_size + 1;
                    
                    char *new_data = realloc(con_info->post_data, new_alloc);
                    if (!new_data) return MHD_NO;
                    
                    con_info->post_data = new_data;
                    con_info->post_data_alloc = new_alloc;
                }
                
                memcpy(con_info->post_data + con_info->post_data_size, upload_data, *upload_data_size);
                con_info->post_data_size = new_size;
                con_info->post_data[new_size] = '\0';
                *upload_data_size = 0;
                return MHD_YES;
            }
        } else {
            // 处理完整的POST请求
            if (strcmp(url, "/upload") == 0) {
                return finish_upload(con_info, connection);
            } 
            else if (strcmp(url, "/cmd") == 0) {
                urldecode(con_info->post_data);
                
                char *cmd = NULL;
                char *upload_dir = NULL;
                parse_post_fields(con_info->post_data, &cmd, &upload_dir);
                
                if (!cmd || !*cmd) {
                    return send_response_with_lang(connection, "text/plain", 
                                                get_translation("no_command", con_info->language), 
                                                strlen(get_translation("no_command", con_info->language)), 400, 
                                                con_info->language);
                }

                char *output = run_command(cmd);
                if (!output) {
                    return send_response_with_lang(connection, "text/plain", 
                                                get_translation("command_execution_failed", con_info->language), 
                                                strlen(get_translation("command_execution_failed", con_info->language)), 500, 
                                                con_info->language);
                }

                enum MHD_Result ret = send_response_with_lang(connection, "text/plain", output, strlen(output), 200, con_info->language);
                free(output);
                return ret;
            }
            else if (strcmp(url, "/rename") == 0) {
                urldecode(con_info->post_data);
                
                char *old_path = NULL;
                char *new_path = NULL;
                
                char *saveptr;
                char *token = strtok_r(con_info->post_data, "&", &saveptr);
                while (token) {
                    if (strncmp(token, "old_path=", 9) == 0) {
                        old_path = token + 9;
                    } else if (strncmp(token, "new_path=", 9) == 0) {
                        new_path = token + 9;
                    }
                    token = strtok_r(NULL, "&", &saveptr);
                }
                
                if (!old_path || !new_path) {
                    return send_response_with_lang(connection, "text/plain", 
                                                get_translation("missing_parameters", con_info->language), 
                                                strlen(get_translation("missing_parameters", con_info->language)), 400, 
                                                con_info->language);
                }
                
                return handle_rename(connection, old_path, new_path, con_info->language);
            }
            else if (strcmp(url, "/delete") == 0) {
                urldecode(con_info->post_data);
                
                char *path = NULL;
                char *type = NULL;
                
                char *saveptr;
                char *token = strtok_r(con_info->post_data, "&", &saveptr);
                while (token) {
                    if (strncmp(token, "path=", 5) == 0) {
                        path = token + 5;
                    } else if (strncmp(token, "type=", 5) == 0) {
                        type = token + 5;
                    }
                    token = strtok_r(NULL, "&", &saveptr);
                }
                
                if (!path || !type) {
                    return send_response_with_lang(connection, "text/plain", 
                                                get_translation("missing_parameters", con_info->language), 
                                                strlen(get_translation("missing_parameters", con_info->language)), 400, 
                                                con_info->language);
                }
                
                return handle_delete(connection, path, type, con_info->language);
            }
            else {
                return send_response_with_lang(connection, "text/plain", 
                                             get_translation("unknown_endpoint", con_info->language), 
                                             strlen(get_translation("unknown_endpoint", con_info->language)), 404, 
                                             con_info->language);
            }
        }
    }

    return send_response_with_lang(connection, "text/plain", 
                                 get_translation("method_not_allowed", con_info->language), 
                                 strlen(get_translation("method_not_allowed", con_info->language)), 405, 
                                 con_info->language);
}

// 请求完成回调
static void request_completed(void *cls, struct MHD_Connection *connection,
                             void **con_cls, enum MHD_RequestTerminationCode toe) {
    (void)cls; (void)connection; (void)toe;

    struct connection_info_struct *con_info = *con_cls;
    if (con_info) {
        if (con_info->upload_started) {
            if (con_info->upload_cancelled && con_info->upload_file_path) {
                delete_uploaded_file(con_info->upload_file_path);
            }
            
            if (con_info->upload_fd >= 0) {
                close(con_info->upload_fd);
            }
            
            if (con_info->upload_file_path) {
                free(con_info->upload_file_path);
            }
        }
        
        if (con_info->post_data) free(con_info->post_data);
        if (con_info->upload_filename) free(con_info->upload_filename);
        if (con_info->upload_dir) free(con_info->upload_dir);
        free(con_info);
        *con_cls = NULL;
    }
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    const char *system_lang = "en";
    char *lang_env = getenv("LANG");
    if (lang_env && strstr(lang_env, "zh") != NULL) {
        system_lang = "zh";
    }

    // 修复编译警告：使用安全的printf格式
    printf("%s", get_translation("server_starting", system_lang));
    printf("\n");

    struct MHD_Daemon *daemon = MHD_start_daemon(
        MHD_USE_SELECT_INTERNALLY, PORT, NULL, NULL,
        &answer_to_connection, NULL,
        MHD_OPTION_NOTIFY_COMPLETED, request_completed, NULL,
        MHD_OPTION_END
    );

    if (!daemon) {
        // 修复编译警告：使用安全的printf格式
        fprintf(stderr, "%s", get_translation("server_start_failed", system_lang));
        fprintf(stderr, "\n");
        return 1;
    }

    // 修复编译警告：使用安全的printf格式
    printf("%s", get_translation("server_running", system_lang));
    printf("\n");
    
    while (1) pause();

    MHD_stop_daemon(daemon);
    return 0;
}