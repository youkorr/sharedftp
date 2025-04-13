```
ftp_http_proxy:
  id: my_proxy
  server: "ftp.example.com"
  username: !secret ftp_user
  password: !secret ftp_pass
  shared_path: "/share/ftp_files"
  local_port: 8080
```
