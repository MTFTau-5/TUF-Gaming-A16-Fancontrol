#include <array>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <regex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace fs = std::filesystem;

static const int kPort = 8931;
static const int kCurvePoints = 8;


static std::string findHwmon(const std::string &wanted) {
    std::error_code ec;
    for (const auto &entry : fs::directory_iterator("/sys/class/hwmon", ec)) {
        std::ifstream f(entry.path() / "name");
        std::string name;
        if (f && std::getline(f, name) && name == wanted)
            return entry.path().string();
    }
    return "";
}

static bool readLong(const std::string &path, long &out) {
    std::ifstream f(path);
    return f && (f >> out);
}

struct FanPoint {
    int tempC = 0; // ℃
    int pwm = 0;   // 0~255
};

struct FanState {
    long rpm = -1;
    std::vector<FanPoint> points;
};

static std::vector<FanPoint> readCurve(const std::string &curveHwmon, int channel) {
    std::vector<FanPoint> pts;
    for (int i = 1; i <= kCurvePoints; ++i) {
        char base[128];
        std::snprintf(base, sizeof(base), "%s/pwm%d_auto_point%d_",
                      curveHwmon.c_str(), channel, i);
        long tempMc = 0, pwm = 0;
        if (!readLong(std::string(base) + "temp", tempMc) ||
            !readLong(std::string(base) + "pwm", pwm)) {
            pts.clear();
            return pts; 
        }
        // asus_armoury 实测存整摄氏度，文档所述毫摄氏度做兼容
        int tempC = tempMc > 1000 ? static_cast<int>(tempMc / 1000)
                                  : static_cast<int>(tempMc);
        pts.push_back({tempC, static_cast<int>(pwm)});
    }
    return pts;
}


struct CmdResult {
    int exitCode = -1;
    std::string output;
};

static CmdResult runCmd(const std::vector<std::string> &args) {
    int pipefd[2];
    if (pipe(pipefd) != 0) return {-1, "pipe failed"};

    pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return {-1, "fork failed"};
    }
    if (pid == 0) {
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[0]);
        close(pipefd[1]);
        std::vector<char *> argv;
        for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }

    close(pipefd[1]);
    CmdResult res;
    char buf[512];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0)
        res.output.append(buf, static_cast<size_t>(n));
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    res.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return res;
}

static bool profileValid(const std::string &p) {
    return p == "Balanced" || p == "Quiet" || p == "Performance";
}


static std::string extractString(const std::string &body, const std::string &key) {
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*\\\"([A-Za-z0-9_]+)\\\"");
    std::smatch m;
    if (std::regex_search(body, m, re)) return m[1].str();
    return "";
}

static bool extractBool(const std::string &body, const std::string &key, bool &out) {
    std::regex re("\\\"" + key + "\\\"\\s*:\\s*(true|false)");
    std::smatch m;
    if (!std::regex_search(body, m, re)) return false;
    out = (m[1].str() == "true");
    return true;
}

static std::vector<std::pair<int, int>> extractPoints(const std::string &body) {
    std::vector<std::pair<int, int>> pts;
    auto pos = body.find("\"points\"");
    if (pos == std::string::npos) return pts;
    auto start = body.find('[', pos);
    auto end = body.find(']', start == std::string::npos ? pos : start);
    std::string seg = body.substr(pos);
    std::regex re("\\[\\s*(-?\\d+)\\s*,\\s*(-?\\d+)\\s*\\]");
    for (std::sregex_iterator it(seg.begin(), seg.end(), re), endIt;
         it != endIt; ++it) {
        pts.emplace_back(std::stoi((*it)[1].str()), std::stoi((*it)[2].str()));
    }
    (void)start;
    (void)end;
    return pts;
}


static void sendAll(int fd, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        ssize_t n = send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (n <= 0) return;
        sent += static_cast<size_t>(n);
    }
}

static void respond(int fd, int code, const std::string &status,
                    const std::string &contentType, const std::string &body) {
    std::ostringstream head;
    head << "HTTP/1.1 " << code << " " << status << "\r\n"
         << "Content-Type: " << contentType << "\r\n"
         << "Content-Length: " << body.size() << "\r\n"
         << "Cache-Control: no-store\r\n"
         << "Connection: close\r\n\r\n";
    sendAll(fd, head.str());
    sendAll(fd, body);
}

static std::string jsonOk(const std::string &msg = "") {
    if (msg.empty()) return "{\"ok\":true}";
    return "{\"ok\":true,\"message\":\"" + msg + "\"}";
}

static std::string jsonErr(const std::string &msg) {
    std::string esc;
    for (char c : msg) {
        if (c == '"' || c == '\\') esc += '\\';
        if (c != '\n' && c != '\r') esc += c;
    }
    return "{\"ok\":false,\"error\":\"" + esc + "\"}";
}

// 静态页面查找顺序：可执行文件旁 → 系统安装位置
static std::string exeDir() {
    char buf[4096];
    ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (n <= 0) return ".";
    buf[n] = '\0';
    return fs::path(buf).parent_path().string();
}

// 静态页面查找顺序：可执行文件旁 → 系统安装位置
static std::string indexHtmlPath() {
    std::string local = exeDir() + "/static/index.html";
    if (fs::exists(local)) return local;
    return "/usr/share/fancontrol/static/index.html";
}

static std::string buildStatusJson() {
    std::string fanHwmon = findHwmon("asus");
    std::string curveHwmon = findHwmon("asus_custom_fan_curve");
    std::string cpuTempHwmon = findHwmon("k10temp");
    std::string gpuTempHwmon = findHwmon("amdgpu");

    FanState cpu, gpu;
    if (!fanHwmon.empty()) {
        readLong(fanHwmon + "/fan1_input", cpu.rpm);
        readLong(fanHwmon + "/fan2_input", gpu.rpm);
    }
    if (!curveHwmon.empty()) {
        cpu.points = readCurve(curveHwmon, 1);
        gpu.points = readCurve(curveHwmon, 2);
    }

    long cpuTempMc = -1, gpuTempMc = -1;
    if (!cpuTempHwmon.empty())
        readLong(cpuTempHwmon + "/temp1_input", cpuTempMc); // Tctl
    if (!gpuTempHwmon.empty())
        readLong(gpuTempHwmon + "/temp1_input", gpuTempMc); // edge

    // 当前 asusctl 配置档（尽力而为，失败不影响主流程）
    std::string profile;
    auto pr = runCmd({"asusctl", "profile", "get"});
    if (pr.exitCode == 0) {
        std::smatch m;
        if (std::regex_search(pr.output, m, std::regex("(Balanced|Quiet|Performance)")))
            profile = m[1].str();
    }

    auto pointsJson = [](const std::vector<FanPoint> &pts) {
        std::ostringstream o;
        o << "[";
        for (size_t i = 0; i < pts.size(); ++i) {
            if (i) o << ",";
            o << "{\"tempC\":" << pts[i].tempC << ",\"pwm\":" << pts[i].pwm << "}";
        }
        o << "]";
        return o.str();
    };

    std::ostringstream o;
    o << "{\"ok\":true"
      << ",\"profile\":\"" << profile << "\""
      << ",\"cpuTempC\":" << (cpuTempMc >= 0 ? cpuTempMc / 1000 : -1)
      << ",\"gpuTempC\":" << (gpuTempMc >= 0 ? gpuTempMc / 1000 : -1)
      << ",\"hwmon\":{\"fansFound\":" << (fanHwmon.empty() ? "false" : "true")
      << ",\"curveFound\":" << (curveHwmon.empty() ? "false" : "true") << "}"
      << ",\"fans\":{"
      << "\"cpu\":{\"rpm\":" << cpu.rpm << ",\"points\":" << pointsJson(cpu.points) << "},"
      << "\"gpu\":{\"rpm\":" << gpu.rpm << ",\"points\":" << pointsJson(gpu.points) << "}"
      << "}}";
    return o.str();
}

static std::string apiSetCurve(const std::string &body) {
    std::string fan = extractString(body, "fan");
    std::string profile = extractString(body, "profile");
    if (profile.empty()) profile = "Balanced";

    if (fan != "cpu" && fan != "gpu")
        return jsonErr("fan must be cpu or gpu");
    if (!profileValid(profile))
        return jsonErr("invalid profile");

    auto pts = extractPoints(body); // [tempC, pwmPercent]
    if (pts.size() != kCurvePoints)
        return jsonErr("need exactly 8 points");

    // 校验：温度/PWM 单调不减，范围合法（接口文档第 7 节）
    for (size_t i = 0; i < pts.size(); ++i) {
        auto [t, p] = pts[i];
        if (t < 0 || t > 100) return jsonErr("temp out of range 0-100C");
        if (p < 0 || p > 100) return jsonErr("pwm percent out of range 0-100");
        if (i > 0 && (t < pts[i - 1].first || p < pts[i - 1].second))
            return jsonErr("points must be non-decreasing");
    }

    std::ostringstream data;
    for (size_t i = 0; i < pts.size(); ++i) {
        if (i) data << ",";
        data << pts[i].first << "c:" << pts[i].second << "%";
    }

    auto res = runCmd({"asusctl", "fan-curve", "--mod-profile", profile,
                       "--fan", fan, "--data", data.str()});
    if (res.exitCode != 0)
        return jsonErr("asusctl failed: " + res.output);
    return jsonOk("curve applied");
}

static std::string apiEnable(const std::string &body) {
    bool enabled = false;
    if (!extractBool(body, "enabled", enabled))
        return jsonErr("missing enabled");
    std::string profile = extractString(body, "profile");
    if (profile.empty()) profile = "Balanced";
    if (!profileValid(profile))
        return jsonErr("invalid profile");

    auto res = runCmd({"asusctl", "fan-curve", "--mod-profile", profile,
                       "--enable-fan-curves", enabled ? "true" : "false"});
    if (res.exitCode != 0)
        return jsonErr("asusctl failed: " + res.output);
    return jsonOk(enabled ? "fan curves enabled" : "fan curves disabled");
}

static std::string apiDefault(const std::string &body) {
    std::string profile = extractString(body, "profile");
    if (profile.empty()) profile = "Balanced";
    if (!profileValid(profile))
        return jsonErr("invalid profile");

    auto res = runCmd({"asusctl", "fan-curve", "--mod-profile", profile, "--default"});
    if (res.exitCode != 0)
        return jsonErr("asusctl failed: " + res.output);
    return jsonOk("default curve restored");
}


static void handleClient(int fd) {
    std::string req;
    req.reserve(8192);
    char buf[4096];

    ssize_t n;
    size_t headerEnd = std::string::npos;
    while ((n = recv(fd, buf, sizeof(buf), 0)) > 0) {
        req.append(buf, static_cast<size_t>(n));
        headerEnd = req.find("\r\n\r\n");
        if (headerEnd != std::string::npos) break;
        if (req.size() > 65536) { close(fd); return; }
    }
    if (n <= 0) { close(fd); return; }


    std::istringstream is(req.substr(0, req.find("\r\n")));
    std::string method, target, version;
    is >> method >> target >> version;

    size_t contentLength = 0;
    std::smatch m;
    if (std::regex_search(req, m, std::regex("Content-Length:\\s*(\\d+)",
                                             std::regex_constants::icase)))
        contentLength = std::stoul(m[1].str());

    std::string body;
    if (headerEnd != std::string::npos)
        body = req.substr(headerEnd + 4);
    while (body.size() < contentLength) {
        n = recv(fd, buf, std::min(sizeof(buf), contentLength - body.size()), 0);
        if (n <= 0) break;
        body.append(buf, static_cast<size_t>(n));
    }
    if (body.size() > contentLength) body.resize(contentLength);

    if (method == "GET" && (target == "/" || target == "/index.html")) {
        std::ifstream f(indexHtmlPath(), std::ios::binary);
        if (f) {
            std::ostringstream ss;
            ss << f.rdbuf();
            respond(fd, 200, "OK", "text/html; charset=utf-8", ss.str());
        } else {
            respond(fd, 404, "Not Found", "text/plain; charset=utf-8",
                    "static/index.html not found\n");
        }
    } else if (method == "GET" && target == "/api/status") {
        respond(fd, 200, "OK", "application/json", buildStatusJson());
    } else if (method == "POST" && target == "/api/curve") {
        respond(fd, 200, "OK", "application/json", apiSetCurve(body));
    } else if (method == "POST" && target == "/api/enable") {
        respond(fd, 200, "OK", "application/json", apiEnable(body));
    } else if (method == "POST" && target == "/api/default") {
        respond(fd, 200, "OK", "application/json", apiDefault(body));
    } else {
        respond(fd, 404, "Not Found", "application/json", jsonErr("not found"));
    }
    close(fd);
}

int main() {
    std::signal(SIGPIPE, SIG_IGN);

    int srv = socket(AF_INET, SOCK_STREAM, 0);
    if (srv < 0) { perror("socket"); return 1; }
    int opt = 1;
    setsockopt(srv, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kPort);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); // 仅本机

    if (bind(srv, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }
    if (listen(srv, 16) < 0) { perror("listen"); return 1; }

    std::cout << "fan-server listening on http://127.0.0.1:" << kPort << std::endl;

    while (true) {
        int fd = accept(srv, nullptr, nullptr);
        if (fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            break;
        }
        std::thread(handleClient, fd).detach();
    }
    close(srv);
    return 0;
}
