#include "ftp_http_proxy.h"
#include "esp_log.h"
#include <lwip/sockets.h>
#include <netdb.h>
#include <cstring>
#include <arpa/inet.h>
#include "esp_task_wdt.h"
#include "esp_heap_caps.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dirent.h>

static const char *TAG = "ftp_proxy";

namespace esphome {
namespace ftp_http_proxy {

void FTPHTTPProxy::setup() {
  ESP_LOGI(TAG, "Initializing FTP/HTTP proxy with shared path: %s", shared_path_.c_str());
  
  // Create shared directory if it doesn't exist
  struct stat st;
  if (stat(shared_path_.c_str(), &st) == -1) {
    ESP_LOGI(TAG, "Creating shared directory: %s", shared_path_.c_str());
    mkdir(shared_path_.c_str(), 0755);
  }

  this->setup_http_server();
}

bool FTPHTTPProxy::connect_to_ftp() {
  struct hostent *ftp_host = gethostbyname(ftp_server_.c_str());
  if (!ftp_host) {
    ESP_LOGE(TAG, "DNS resolution failed");
    return false;
  }

  sock_ = socket(AF_INET, SOCK_STREAM, 0);
  if (sock_ < 0) {
    ESP_LOGE(TAG, "Socket creation failed: %d", errno);
    return false;
  }

  // Set socket options
  int enable = 1;
  setsockopt(sock_, SOL_SOCKET, SO_KEEPALIVE, &enable, sizeof(enable));
  setsockopt(sock_, SOL_SOCKET, SO_RCVBUF, &enable, sizeof(enable));

  struct sockaddr_in server_addr;
  memset(&server_addr, 0, sizeof(server_addr));
  server_addr.sin_family = AF_INET;
  server_addr.sin_port = htons(ftp_port_);
  server_addr.sin_addr.s_addr = *((unsigned long *)ftp_host->h_addr);

  if (connect(sock_, (struct sockaddr *)&server_addr, sizeof(server_addr)) != 0) {
    ESP_LOGE(TAG, "FTP connection failed: %d", errno);
    close(sock_);
    sock_ = -1;
    return false;
  }

  char buffer[256];
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "220 ")) {
    ESP_LOGE(TAG, "FTP welcome message not received");
    close(sock_);
    sock_ = -1;
    return false;
  }

  // Authentication
  snprintf(buffer, sizeof(buffer), "USER %s\r\n", username_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  snprintf(buffer, sizeof(buffer), "PASS %s\r\n", password_.c_str());
  send(sock_, buffer, strlen(buffer), 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  // Binary mode
  send(sock_, "TYPE I\r\n", 8, 0);
  recv(sock_, buffer, sizeof(buffer) - 1, 0);

  return true;
}

bool FTPHTTPProxy::download_to_shared(const std::string &remote_path) {
  std::string filename = remote_path.substr(remote_path.find_last_of('/') + 1);
  std::string local_path = shared_path_ + "/" + filename;
  return download_to_shared_impl(remote_path, local_path);
}

bool FTPHTTPProxy::download_to_shared_impl(const std::string &remote_path, const std::string &local_path) {
  int data_sock = -1;
  bool success = false;
  char buffer[4096];
  FILE *local_file = nullptr;

  TaskHandle_t current_task = xTaskGetCurrentTaskHandle();
  esp_task_wdt_add(current_task);

  if (!connect_to_ftp()) {
    ESP_LOGE(TAG, "Failed to connect to FTP");
    goto cleanup;
  }

  // Open local file for writing
  local_file = fopen(local_path.c_str(), "wb");
  if (!local_file) {
    ESP_LOGE(TAG, "Failed to open local file: %s", local_path.c_str());
    goto cleanup;
  }

  // Passive mode
  send(sock_, "PASV\r\n", 6, 0);
  int bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "227 ")) {
    ESP_LOGE(TAG, "PASV mode failed");
    goto cleanup;
  }

  // Parse PASV response
  char *pasv_start = strchr(buffer, '(');
  if (!pasv_start) {
    ESP_LOGE(TAG, "Invalid PASV response");
    goto cleanup;
  }

  int ip[4], port[2];
  sscanf(pasv_start, "(%d,%d,%d,%d,%d,%d)", &ip[0], &ip[1], &ip[2], &ip[3], &port[0], &port[1]);
  int data_port = port[0] * 256 + port[1];

  // Connect to data port
  data_sock = socket(AF_INET, SOCK_STREAM, 0);
  if (data_sock < 0) {
    ESP_LOGE(TAG, "Data socket creation failed");
    goto cleanup;
  }

  struct sockaddr_in data_addr;
  memset(&data_addr, 0, sizeof(data_addr));
  data_addr.sin_family = AF_INET;
  data_addr.sin_port = htons(data_port);
  data_addr.sin_addr.s_addr = htonl((ip[0] << 24) | (ip[1] << 16) | (ip[2] << 8) | ip[3]);

  if (connect(data_sock, (struct sockaddr *)&data_addr, sizeof(data_addr)) != 0) {
    ESP_LOGE(TAG, "Data connection failed");
    goto cleanup;
  }

  // Send RETR command
  snprintf(buffer, sizeof(buffer), "RETR %s\r\n", remote_path.c_str());
  send(sock_, buffer, strlen(buffer), 0);

  // Check for 150 response
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received <= 0 || !strstr(buffer, "150 ")) {
    ESP_LOGE(TAG, "File transfer not ready");
    goto cleanup;
  }

  // Download file
  while ((bytes_received = recv(data_sock, buffer, sizeof(buffer), 0)) > 0) {
    esp_task_wdt_reset();
    if (fwrite(buffer, 1, bytes_received, local_file) != bytes_received) {
      ESP_LOGE(TAG, "File write error");
      goto cleanup;
    }
  }

  // Check transfer completion
  bytes_received = recv(sock_, buffer, sizeof(buffer) - 1, 0);
  if (bytes_received > 0 && strstr(buffer, "226 ")) {
    success = true;
    ESP_LOGI(TAG, "File downloaded successfully to %s", local_path.c_str());
  }

cleanup:
  if (local_file) fclose(local_file);
  if (data_sock != -1) close(data_sock);
  if (sock_ != -1) {
    send(sock_, "QUIT\r\n", 6, 0);
    close(sock_);
    sock_ = -1;
  }
  esp_task_wdt_delete(current_task);
  return success;
}

esp_err_t FTPHTTPProxy::serve_shared_file(httpd_req_t *req, const std::string &filepath) {
  struct stat file_stat;
  if (stat(filepath.c_str(), &file_stat) != 0) {
    ESP_LOGE(TAG, "File not found: %s", filepath.c_str());
    return ESP_FAIL;
  }

  // Set content type based on file extension
  std::string extension = filepath.substr(filepath.find_last_of('.'));
  if (extension == ".mp3") {
    httpd_resp_set_type(req, "audio/mpeg");
  } else if (extension == ".wav") {
    httpd_resp_set_type(req, "audio/wav");
  } else {
    httpd_resp_set_type(req, "application/octet-stream");
  }

  int fd = open(filepath.c_str(), O_RDONLY);
  if (fd == -1) {
    ESP_LOGE(TAG, "Failed to open file: %s", filepath.c_str());
    return ESP_FAIL;
  }

  char *chunk = (char *)malloc(4096);
  if (!chunk) {
    close(fd);
    return ESP_FAIL;
  }

  ssize_t bytes_read;
  do {
    bytes_read = read(fd, chunk, 4096);
    if (bytes_read > 0) {
      httpd_resp_send_chunk(req, chunk, bytes_read);
    }
  } while (bytes_read > 0);

  free(chunk);
  close(fd);
  httpd_resp_send_chunk(req, NULL, 0);
  return ESP_OK;
}

esp_err_t FTPHTTPProxy::http_req_handler(httpd_req_t *req) {
  auto *proxy = (FTPHTTPProxy *)req->user_ctx;
  std::string requested_path = req->uri;

  if (!requested_path.empty() && requested_path[0] == '/') {
    requested_path.erase(0, 1);
  }

  std::string local_path = proxy->shared_path_ + "/" + requested_path;
  return proxy->serve_shared_file(req, local_path);
}

void FTPHTTPProxy::setup_http_server() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.server_port = local_port_;
  config.uri_match_fn = httpd_uri_match_wildcard;
  config.max_uri_handlers = 16;
  config.stack_size = 8192;

  if (httpd_start(&server_, &config) != ESP_OK) {
    ESP_LOGE(TAG, "Failed to start HTTP server");
    return;
  }

  httpd_uri_t uri_proxy = {
    .uri = "/*",
    .method = HTTP_GET,
    .handler = http_req_handler,
    .user_ctx = this
  };

  httpd_register_uri_handler(server_, &uri_proxy);
  ESP_LOGI(TAG, "HTTP server started on port %d", local_port_);
}

}  // namespace ftp_http_proxy
}  // namespace esphome
