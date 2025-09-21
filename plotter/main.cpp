// ultra fast ADC plotter: C++17 + ImGui + ImPlot + OpenGL3 + GLFW
// Linux/Windows 겸용(시리얼: POSIX termios / Win32 API)
// 도킹/뷰포트 기능 사용 안 함 → 구버전 ImGui 호환.

#include <atomic>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>
#include <algorithm>
#include <string>

#if defined(_WIN32)
  #define NOMINMAX
  #include <windows.h>
  #include <tchar.h>
#else
  #include <fcntl.h>
  #include <termios.h>
  #include <unistd.h>
  #include <sys/ioctl.h>
  #include <errno.h>
  #include <dirent.h>
  #include <sys/stat.h>
#endif

// ----------------- GUI -----------------
#include "imgui.h"
#include "implot.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GL/glew.h>
#include <GLFW/glfw3.h>

// ======== 설정 ========
static constexpr int   ADCLEN    = 300;
static constexpr int   CHANNELS  = 7;
static constexpr int   GROUPS_HW[5] = {2,1,1,2,1}; // MCU 프레임 그룹핑(2+1+1+2+1=7)
static constexpr int   GROUP_COUNT = 5;
static constexpr int   FRAME_ELEMS = ADCLEN * CHANNELS;
static constexpr size_t FRAME_BYTES = FRAME_ELEMS * 2;

static constexpr int   MAX_POINTS = ADCLEN * 3;
static constexpr int   FRAME_QUEUE_SLOTS = 3;

static constexpr int   GAP_MS = 3;

// ======== 레이아웃 제어 ========
static bool g_request_layout_reset = true; // 시작 시 기본 배치
static int  g_layout_apply_frames  = 0;    // 강제 프레임 수

// ======== 시리얼 제어(런타임 변경 가능) ========
static std::string g_serial_path =
#if defined(_WIN32)
    "COM3";
#else
    "/dev/ttyACM0";
#endif

// 지원 Baud 리스트 (UI용)
static const int g_baud_list[] = {
    9600, 19200, 38400, 57600, 115200,
    230400, 460800, 921600, 1000000, 2000000
};
static int g_baud_index = 4; // 기본 115200 (index 4)
static std::atomic<bool> g_serial_reopen{true};  // true면 스레드가 재오픈
static std::atomic<bool> g_serial_exit{false};   // 종료 플래그
static std::atomic<bool> g_serial_opened{false}; // 열림 상태
static std::atomic<uint32_t> g_serial_errs{0};   // 에러 카운트

// ============== 간단 링버퍼(FIFO) ==============
struct Ring {
    std::vector<float> buf;
    int cap{0}, head{0}, size{0};
    explicit Ring(int capacity=0): buf(capacity), cap(capacity) {}
    inline void push(float v){
        if (cap == 0) return;
        int idx = (head + size) % cap;
        buf[idx] = v;
        if (size < cap) size++;
        else head = (head + 1) % cap;
    }
    inline int toLinear(std::vector<float>& dst) const {
        dst.resize(size);
        if (size == 0) return 0;
        if (size == cap){
            int tail = cap - head;
            std::copy(buf.begin()+head, buf.end(), dst.begin());
            std::copy(buf.begin(), buf.begin()+head, dst.begin()+tail);
            return cap;
        } else {
            int end = (head + size) % cap;
            if (head < end){
                std::copy(buf.begin()+head, buf.begin()+end, dst.begin());
            } else {
                int first = cap - head;
                std::copy(buf.begin()+head, buf.end(), dst.begin());
                std::copy(buf.begin(), buf.begin()+end, dst.begin()+first);
            }
            return size;
        }
    }
};

// ======== 프레임 큐(SPSC) ========
struct FrameQueue {
    std::array<std::array<uint16_t, FRAME_ELEMS>, FRAME_QUEUE_SLOTS> slot{};
    std::atomic<uint32_t> seq{0};

    inline void write(const uint8_t* srcBytes){
        uint32_t w = seq.load(std::memory_order_relaxed);
        auto& s = slot[w % FRAME_QUEUE_SLOTS];
        std::memcpy(s.data(), srcBytes, FRAME_BYTES);
        seq.store(w+1, std::memory_order_release);
    }
    inline const uint16_t* read_latest(uint32_t& last_seq_out, size_t& len_out){
        uint32_t cur = seq.load(std::memory_order_acquire);
        if (cur == 0) return nullptr;
        if (cur == last_seq_out) return nullptr;
        uint32_t idx = (cur - 1) % FRAME_QUEUE_SLOTS;
        last_seq_out = cur;
        len_out = FRAME_ELEMS;
        return slot[idx].data();
    }
};
static FrameQueue g_frames;

// ======== 플랫폼별 시리얼 래퍼 ========
struct SerialHandle {
#if defined(_WIN32)
    HANDLE h = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool valid() const {
#if defined(_WIN32)
        return h != INVALID_HANDLE_VALUE;
#else
        return fd >= 0;
#endif
    }
};

#if defined(_WIN32)
static DWORD map_baud_win(int baud){
    // Win32는 임의의 Baud DWORD 값을 허용(드라이버 지원 시). 목록 그대로 반환.
    return (DWORD)baud;
}
#else
static speed_t map_baud_posix(int baud){
    switch(baud){
        case 9600: return B9600;
        case 19200: return B19200;
        case 38400: return B38400;
        case 57600: return B57600;
        case 115200: return B115200;
#ifdef B230400
        case 230400: return B230400;
#endif
#ifdef B460800
        case 460800: return B460800;
#endif
#ifdef B921600
        case 921600: return B921600;
#endif
#ifdef B1000000
        case 1000000: return B1000000;
#endif
        default: return B115200;
    }
}
#endif

static bool serial_open(SerialHandle& sh, const std::string& path, int baud){
#if defined(_WIN32)
    // "\\\\.\\COM10" 형태 필요 (COM1~9는 "COMx"도 동작하지만 일관성 위해 전체 사용)
    std::string full = path;
    if (full.rfind("\\\\.\\", 0) != 0) {
        if (full.rfind("COM", 0) == 0) full = "\\\\.\\" + full;
    }
    HANDLE h = CreateFileA(
        full.c_str(),
        GENERIC_READ | GENERIC_WRITE,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr
    );
    if (h == INVALID_HANDLE_VALUE) return false;

    DCB dcb;
    SecureZeroMemory(&dcb, sizeof(dcb));
    dcb.DCBlength = sizeof(dcb);
    if (!GetCommState(h, &dcb)) { CloseHandle(h); return false; }

    dcb.BaudRate = map_baud_win(baud);
    dcb.ByteSize = 8;
    dcb.Parity   = NOPARITY;
    dcb.StopBits = ONESTOPBIT;
    dcb.fBinary  = TRUE;
    dcb.fParity  = FALSE;
    dcb.fDtrControl = DTR_CONTROL_ENABLE;
    dcb.fRtsControl = RTS_CONTROL_ENABLE;

    if (!SetCommState(h, &dcb)) { CloseHandle(h); return false; }

    COMMTIMEOUTS to;
    to.ReadIntervalTimeout         = 50;
    to.ReadTotalTimeoutMultiplier  = 0;
    to.ReadTotalTimeoutConstant    = 50; // 50ms
    to.WriteTotalTimeoutMultiplier = 0;
    to.WriteTotalTimeoutConstant   = 50;
    SetCommTimeouts(h, &to);

    PurgeComm(h, PURGE_RXCLEAR | PURGE_TXCLEAR);

    sh.h = h;
    return true;
#else
    int fd = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0) return false;

    termios tio{};
    if (tcgetattr(fd, &tio) != 0){ ::close(fd); return false; }

    cfmakeraw(&tio);
    tio.c_cflag |= (CLOCAL | CREAD);
    tio.c_cflag &= ~CSTOPB;
    tio.c_cflag &= ~PARENB;
    tio.c_cflag &= ~CSIZE;
    tio.c_cflag |= CS8;

    tio.c_cc[VMIN]  = 0;
    tio.c_cc[VTIME] = 1; // 100ms

    speed_t spd = map_baud_posix(baud);
    cfsetispeed(&tio, spd);
    cfsetospeed(&tio, spd);

    if (tcsetattr(fd, TCSANOW, &tio) != 0){ ::close(fd); return false; }

    int flags;
    ioctl(fd, TIOCMGET, &flags);
    flags |= (TIOCM_DTR | TIOCM_RTS);
    ioctl(fd, TIOCMSET, &flags);

    // 논블록 → 블록으로 전환(선택): 여기선 논블록 유지 + 타임아웃으로 운용
    sh.fd = fd;
    return true;
#endif
}

static void serial_close(SerialHandle& sh){
#if defined(_WIN32)
    if (sh.h != INVALID_HANDLE_VALUE){ CloseHandle(sh.h); sh.h = INVALID_HANDLE_VALUE; }
#else
    if (sh.fd >= 0){ ::close(sh.fd); sh.fd = -1; }
#endif
}

static int serial_read(SerialHandle& sh, uint8_t* buf, int cap){
#if defined(_WIN32)
    if (sh.h == INVALID_HANDLE_VALUE) return -1;
    DWORD n = 0;
    if (!ReadFile(sh.h, buf, (DWORD)cap, &n, nullptr)) return -1;
    return (int)n;
#else
    if (sh.fd < 0) return -1;
    ssize_t n = ::read(sh.fd, buf, (size_t)cap);
    if (n < 0){
        if (errno == EAGAIN || errno == EWOULDBLOCK) return 0;
        return -1;
    }
    return (int)n;
#endif
}

// ======== 포트 열람 ========
static std::vector<std::string> EnumerateSerialPorts(){
    std::vector<std::string> out;
#if defined(_WIN32)
    // 간단 스캔: COM1 ~ COM256
    for (int i=1; i<=256; ++i){
        std::string name = "COM" + std::to_string(i);
        // 실제 존재 여부를 빠르게 판별하기 어렵지만, UI에서 선택 후 연결 시도하도록 함
        out.push_back(name);
    }
#else
    DIR* d = opendir("/dev");
    if (!d) return out;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr){
        const char* n = ent->d_name;
        if (!strncmp(n, "ttyACM", 6) || !strncmp(n, "ttyUSB", 6)){
            out.emplace_back(std::string("/dev/") + n);
        }
    }
    closedir(d);
    std::sort(out.begin(), out.end());
#endif
    return out;
}

// ======== 스타일 ========
static void SetupNiceStyle(){
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 8.0f;
    s.FrameRounding  = 6.0f;
    s.GrabRounding   = 6.0f;
    s.TabRounding    = 6.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize  = 1.0f;
    s.ItemSpacing = ImVec2(8,8);
    s.FramePadding = ImVec2(10,6);
    s.WindowPadding = ImVec2(12,12);

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]        = ImVec4(0.08f,0.09f,0.12f,1.00f);
    c[ImGuiCol_TitleBg]         = ImVec4(0.12f,0.14f,0.18f,1.00f);
    c[ImGuiCol_TitleBgActive]   = ImVec4(0.16f,0.20f,0.26f,1.00f);
    c[ImGuiCol_FrameBg]         = ImVec4(0.13f,0.15f,0.19f,1.00f);
    c[ImGuiCol_FrameBgHovered]  = ImVec4(0.18f,0.24f,0.28f,1.00f);
    c[ImGuiCol_FrameBgActive]   = ImVec4(0.18f,0.28f,0.30f,1.00f);
    c[ImGuiCol_Button]          = ImVec4(0.18f,0.28f,0.30f,1.00f);
    c[ImGuiCol_ButtonHovered]   = ImVec4(0.22f,0.36f,0.40f,1.00f);
    c[ImGuiCol_ButtonActive]    = ImVec4(0.20f,0.32f,0.36f,1.00f);
}

// ======== 레이아웃 계산 & 강제적용 ========
static void CalcRects(ImVec2& ge_pos, ImVec2& ge_size,
                      ImVec2& pl_pos, ImVec2& pl_size,
                      ImVec2& st_pos, ImVec2& st_size) {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    const float M  = 8.0f;   // margin
    const float LW = 420.0f; // 좌측 패널 폭(포트/baud 콤보 포함으로 여유)
    const float SH = 48.0f;  // status height

    ImVec2 workPos  = vp->WorkPos;
    ImVec2 workSize = vp->WorkSize;

    ge_pos  = ImVec2(workPos.x + M, workPos.y + M);
    ge_size = ImVec2(LW, workSize.y - SH - M*3);

    pl_pos  = ImVec2(ge_pos.x + ge_size.x + M, workPos.y + M);
    pl_size = ImVec2(workPos.x + workSize.x - pl_pos.x - M, ge_size.y);

    st_pos  = ImVec2(workPos.x + M, workPos.y + workSize.y - SH - M);
    st_size = ImVec2(workSize.x - M*2, SH);
}

static void SetNextForWindow(const char* /*name*/, const ImVec2& pos, const ImVec2& size, bool force) {
    ImGui::SetNextWindowPos(pos,  force ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSize(size,force ? ImGuiCond_Always : ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowCollapsed(false, ImGuiCond_Always);
}

static void EnforceInsideBegin(bool enforce, const ImVec2& pos, const ImVec2& size){
    if (!enforce) return;
    ImGui::SetWindowPos(pos,  ImGuiCond_Always);
    ImGui::SetWindowSize(size,ImGuiCond_Always);
}

// ======== 시리얼 스레드 (재연결 지원, 크로스플랫폼) ========
static void serial_thread_func(){
    SerialHandle sh;
    std::vector<uint8_t> acc; acc.resize(FRAME_BYTES * 4);
    size_t acc_len = 0;
    auto last_t = std::chrono::steady_clock::now();
    std::array<uint8_t, 8192> tmp;

    while(!g_serial_exit.load()){
        // 오픈 필요하면 시도
        if (!sh.valid() || g_serial_reopen.exchange(false)){
            serial_close(sh);
            const int baud = g_baud_list[g_baud_index];
            if (!serial_open(sh, g_serial_path, baud)) {
                g_serial_opened.store(false);
                std::this_thread::sleep_for(std::chrono::milliseconds(300));
                continue;
            } else {
                g_serial_opened.store(true);
                acc_len = 0; // 경계 리셋
#if defined(_WIN32)
                // 버퍼 purge
                PurgeComm(sh.h, PURGE_RXCLEAR | PURGE_TXCLEAR);
#endif
            }
        }

        // 읽기
        int n = serial_read(sh, tmp.data(), (int)tmp.size());
        if (n < 0){
            g_serial_errs.fetch_add(1);
            g_serial_reopen.store(true);
            g_serial_opened.store(false);
            continue;
        } else if (n == 0) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        auto now = std::chrono::steady_clock::now();
        auto gap = std::chrono::duration_cast<std::chrono::milliseconds>(now - last_t).count();
        last_t = now;

        if (gap >= GAP_MS) acc_len = 0; // 프레임 경계 동기화

        if (acc_len + (size_t)n > acc.size())
            acc.resize(std::max(acc.size()*2, acc_len + (size_t)n));

        std::memcpy(acc.data()+acc_len, tmp.data(), (size_t)n);
        acc_len += (size_t)n;

        while (acc_len >= FRAME_BYTES){
            g_frames.write(acc.data());
            std::memmove(acc.data(), acc.data()+FRAME_BYTES, acc_len - FRAME_BYTES);
            acc_len -= FRAME_BYTES;
        }
        if (acc_len > FRAME_BYTES * 3){
            std::memmove(acc.data(), acc.data()+acc_len-FRAME_BYTES, FRAME_BYTES);
            acc_len = FRAME_BYTES;
        }
    }

    serial_close(sh);
}

// ======== 그룹 자료구조 ========
struct Group {
    int id;                    // 불변 ID
    std::vector<int> ch;       // 채널 인덱스
};
static int g_next_group_id = 1;

static bool contains(const std::vector<int>& v, int x){
    return std::find(v.begin(), v.end(), x) != v.end();
}

// ======== 메인(렌더) ========
int main(){
    std::thread th(serial_thread_func);

    if (!glfwInit()){ std::fprintf(stderr, "glfwInit failed\n"); return 1; }
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    GLFWwindow* window = glfwCreateWindow(1400, 900, "Ultra Plot (ImPlot)", nullptr, nullptr);
    if (!window){ glfwTerminate(); return 1; }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);

    if (glewInit() != GLEW_OK) { std::fprintf(stderr, "glewInit failed\n"); return 1; }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImPlot::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    SetupNiceStyle();

    // 한글 폰트(윈도우/리눅스 경로 알아서 준비)
    io.Fonts->AddFontFromFileTTF("./NanumGothic.ttf", 18.0f, nullptr,
                                 io.Fonts->GetGlyphRangesKorean());

    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    std::array<Ring, CHANNELS> rings;
    for (auto& r: rings) r = Ring(MAX_POINTS);
    std::array<std::vector<float>, CHANNELS> scratch;

    std::vector<float> xIdx(MAX_POINTS);
    for (int i=0;i<MAX_POINTS;i++) xIdx[i] = (float)i;

    uint32_t last_seq = 0;

    // 초기: 각 채널 독립 그룹
    std::vector<Group> groups;
    for (int ch=0; ch<CHANNELS; ++ch) {
        Group g; g.id = g_next_group_id++; g.ch = {ch};
        groups.push_back(g);
    }

    // 포트/baud UI 데이터
    static std::vector<std::string> port_list = EnumerateSerialPorts();
    static int selected_port_idx = 0; // combo index (존재 안하면 0)

    while(!glfwWindowShouldClose(window)){
        glfwPollEvents();

        // 최신 프레임 소비
        {
            size_t len=0;
            const uint16_t* frame = g_frames.read_latest(last_seq, len);
            if (frame && len == (size_t)FRAME_ELEMS){
                size_t pos = 0;
                int base = 0;
                for (int g = 0; g < GROUP_COUNT; g++){
                    const int gc = GROUPS_HW[g];
                    for (int i=0;i<ADCLEN;i++){
                        for (int c=0;c<gc;c++){
                            rings[base+c].push((float)frame[pos++]);
                        }
                    }
                    base += gc;
                }
            }
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        // 메뉴바
        if (ImGui::BeginMainMenuBar()){
            if (ImGui::BeginMenu("파일")){
                if (ImGui::MenuItem("종료", "Alt+F4")) glfwSetWindowShouldClose(window, true);
                ImGui::EndMenu();
            }
            if (ImGui::BeginMenu("보기")){
                if (ImGui::MenuItem("레이아웃 초기화", "Ctrl+R")) g_request_layout_reset = true;
                ImGui::EndMenu();
            }
            ImGui::Separator();
            ImGui::Text("Serial: %s | %d baud | %s | Err:%u",
                        g_serial_path.c_str(),
                        g_baud_list[g_baud_index],
                        g_serial_opened.load() ? "OPEN" : "CLOSED",
                        (unsigned)g_serial_errs.load());
            ImGui::EndMainMenuBar();
        }

        // 레이아웃 강제처리
        if (g_request_layout_reset) {
            g_layout_apply_frames = 4;
            g_request_layout_reset = false;
        }
        bool enforce = (g_layout_apply_frames-- > 0);

        // 사각형 계산
        ImVec2 ge_pos, ge_size, pl_pos, pl_size, st_pos, st_size;
        CalcRects(ge_pos, ge_size, pl_pos, pl_size, st_pos, st_size);

        // ===== 좌측: Serial + Group Editor =====
        SetNextForWindow("Group Editor", ge_pos, ge_size, enforce);
        ImGuiWindowFlags ge_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings;
        if (enforce) ge_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        if (ImGui::Begin("Group Editor", NULL, ge_flags)){
            EnforceInsideBegin(enforce, ge_pos, ge_size);

            ImGui::BeginChild("GE_SCROLL", ImVec2(-1, -1), true);

            // ---- Serial Section ----
            ImGui::TextColored(ImVec4(0.7f,0.9f,0.8f,1.0f), "시리얼 연결");
            ImGui::Separator();

            // 포트 콤보
            ImGui::Text("포트:");
            ImGui::PushItemWidth(-1);
            const char* preview_port = port_list.empty() ? "포트 없음" : port_list[selected_port_idx].c_str();
            if (ImGui::BeginCombo("##port_combo", preview_port)) {
                for (int i=0; i<(int)port_list.size(); ++i){
                    bool sel = (i==selected_port_idx);
                    if (ImGui::Selectable(port_list[i].c_str(), sel))
                        selected_port_idx = i;
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();

            // Baud 콤보
            ImGui::Text("Baud:");
            ImGui::PushItemWidth(-1);
            char baud_label[32];
            std::snprintf(baud_label, sizeof(baud_label), "%d", g_baud_list[g_baud_index]);
            if (ImGui::BeginCombo("##baud_combo", baud_label)) {
                for (int i=0; i<(int)(sizeof(g_baud_list)/sizeof(g_baud_list[0])); ++i){
                    bool sel = (i == g_baud_index);
                    char item[32]; std::snprintf(item, sizeof(item), "%d", g_baud_list[i]);
                    if (ImGui::Selectable(item, sel)){
                        g_baud_index = i;
                        // 연결중이면 재오픈
                        g_serial_reopen.store(true);
                    }
                    if (sel) ImGui::SetItemDefaultFocus();
                }
                ImGui::EndCombo();
            }
            ImGui::PopItemWidth();

            // 버튼들
            if (ImGui::Button("새로고침")){
                port_list = EnumerateSerialPorts();
                if (!port_list.empty()) {
                    auto it = std::find(port_list.begin(), port_list.end(), g_serial_path);
                    selected_port_idx = (it==port_list.end()) ? 0 : (int)std::distance(port_list.begin(), it);
                } else {
                    selected_port_idx = 0;
                }
            }
            ImGui::SameLine(0, 8);
            if (ImGui::Button("연결")){
                if (!port_list.empty()){
                    g_serial_path = port_list[selected_port_idx];
                    g_serial_reopen.store(true);
                }
            }
            ImGui::SameLine(0, 8);
            if (ImGui::Button("닫기")){
                g_serial_reopen.store(true);
                g_serial_opened.store(false);
#if defined(_WIN32)
                g_serial_path = "COM0"; // 실패하도록
#else
                g_serial_path = "/dev/invalid";
#endif
            }

            ImGui::Spacing(); ImGui::Separator();

            // ---- Group Section ----
            ImGui::TextColored(ImVec4(0.6f,0.9f,0.9f,1.0f), "채널 그룹 관리");
            ImGui::TextDisabled("그룹을 만들어 여러 채널을 겹쳐 그립니다.");
            ImGui::Spacing();

            // 같은 줄에 두 버튼
            if (ImGui::Button("모두 독립(기본)")) {
                groups.clear();
                for (int ch=0; ch<CHANNELS; ++ch) { Group g; g.id=g_next_group_id++; g.ch={ch}; groups.push_back(g); }
            }
            ImGui::SameLine(0, 8);
            if (ImGui::Button("모두 삭제")) {
                groups.clear();
                for (int ch=0; ch<CHANNELS; ++ch) { Group g; g.id=g_next_group_id++; g.ch={}; groups.push_back(g); }
            }

            ImGui::Spacing(); ImGui::Separator();

            // 각 그룹 UI
            for (size_t idx = 0; idx < groups.size(); ++idx) {
                Group& g = groups[idx];
                ImGui::PushID(g.id);
                std::string header = "Group " + std::to_string(idx+1) + "##" + std::to_string(g.id);
                bool open = ImGui::CollapsingHeader(header.c_str(), ImGuiTreeNodeFlags_DefaultOpen);

                if (open) {
                    ImGui::Indent();

                    // 현재 채널 나열 + 개별 제거
                    if (g.ch.empty()) ImGui::TextDisabled("채널 없음");
                    else {
                        for (size_t i=0; i<g.ch.size(); /* */) {
                            ImGui::PushID((int)i);
                            ImGui::BulletText("CH%d", g.ch[i]+1);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("X")) { g.ch.erase(g.ch.begin()+i); ImGui::PopID(); continue; }
                            ImGui::PopID();
                            ++i;
                        }
                    }

                    // 채널 추가: 콤보에서 선택 즉시 추가
                    ImGui::Text("채널 추가:");
                    ImGui::PushItemWidth(-1);
                    const char* pv = "채널 선택";
                    if (ImGui::BeginCombo("##addch_immediate", pv)) {
                        for (int ch=0; ch<CHANNELS; ch++) {
                            std::string label = "CH" + std::to_string(ch+1);
                            bool sel_dummy = false;
                            if (ImGui::Selectable(label.c_str(), sel_dummy)) {
                                if (!contains(g.ch, ch)) g.ch.push_back(ch);
                            }
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::PopItemWidth();

                    ImGui::Unindent();
                }
                ImGui::Separator();
                ImGui::PopID();
            }

            ImGui::EndChild();
        }
        ImGui::End();

        // ===== Ultra Plot =====
        SetNextForWindow("Ultra Plot", pl_pos, pl_size, enforce);
        ImGuiWindowFlags pl_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (enforce) pl_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        if (ImGui::Begin("Ultra Plot", NULL, pl_flags)){
            EnforceInsideBegin(enforce, pl_pos, pl_size);

            // 채널이 하나 이상인 그룹만 렌더
            std::vector<Group*> render_groups;
            render_groups.reserve(groups.size());
            for (auto& g : groups) if (!g.ch.empty()) render_groups.push_back(&g);

            ImVec2 avail = ImGui::GetContentRegionAvail();
            if (render_groups.empty()) {
                ImGui::TextDisabled("표시할 그룹이 없습니다. 좌측에서 채널을 추가하세요.");
            } else {
                int cols = 1;
                int rows = (int)render_groups.size();
                ImPlotSubplotFlags sp_flags = ImPlotSubplotFlags_NoTitle | ImPlotSubplotFlags_NoLegend;

                if (ImPlot::BeginSubplots("Groups", rows, cols, avail, sp_flags)){
                    for (size_t i = 0; i < render_groups.size(); i++){
                        Group* g = render_groups[i];

                        // 왼쪽 라벨, 오른쪽 플롯
                        ImGui::Columns(2, NULL, false);
                        ImGui::SetColumnWidth(0, 90.0f);
                        std::string left_title = "Group " + std::to_string((int)i+1);
                        ImGui::TextUnformatted(left_title.c_str());
                        ImGui::NextColumn();

                        std::string plot_id = "##plot_" + std::to_string(g->id);
                        ImPlot::PushStyleVar(ImPlotStyleVar_PlotPadding, ImVec2(14, 10));  // 좌우/상하 내부 패딩
                        ImPlot::PushStyleVar(ImPlotStyleVar_FitPadding,  ImVec2(0.02f, 0.00f)); // Auto-fit 시 가장자리 여유
    
                        if (ImPlot::BeginPlot(plot_id.c_str(), ImVec2(-1,-1), 0)){
                            ImPlot::SetupAxesLimits(0, MAX_POINTS, 0, 4096, ImGuiCond_Once);
                            for (int ch : g->ch){
                                if (ch < 0 || ch >= CHANNELS) continue;
                                int n = rings[ch].toLinear(scratch[ch]);
                                if (n>0){
                                    std::string line = "CH" + std::to_string(ch+1);
                                    ImPlot::PlotLine(line.c_str(),
                                                     xIdx.data(), scratch[ch].data(), n, 0, 0, sizeof(float));
                                }
                            }
                            ImPlot::EndPlot();
                        }
                        ImPlot::PopStyleVar(2);
                        ImGui::Columns(1);
                    }
                    ImPlot::EndSubplots();
                }
            }
        }
        ImGui::End();

        // ===== Status =====
        SetNextForWindow("Status", st_pos, st_size, enforce);
        ImGuiWindowFlags st_flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings |
                                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
        if (enforce) st_flags |= ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize;
        if (ImGui::Begin("Status", NULL, st_flags)){
            EnforceInsideBegin(enforce, st_pos, st_size);
            ImGui::Text("FPS: %.1f", ImGui::GetIO().Framerate);
            ImGui::SameLine(0, 20); ImGui::Text("Points/CH: %d", MAX_POINTS);
            ImGui::SameLine(0, 20); ImGui::Text("Last Seq: %u", (unsigned)last_seq);
        }
        ImGui::End();

        // ===== 렌더 =====
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.07f, 0.09f, 0.16f, 1.00f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window);
    }

    // cleanup
    g_serial_exit.store(true);
    if (th.joinable()) th.join();

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImPlot::DestroyContext();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}
