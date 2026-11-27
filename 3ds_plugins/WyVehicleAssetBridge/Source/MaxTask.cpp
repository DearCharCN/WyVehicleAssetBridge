#include "MaxTask.h"
#include "MaxScene.h"
#include "MaxSceneChanges.h"
#include <commctrl.h>
#include <algorithm>
#include <stdexcept>

namespace
{
constexpr wchar_t ProgressClass[] = L"WyVehicleAssetBridge.Progress";
constexpr UINT RunSceneCall = WM_APP + 1, UpdateProgress = WM_APP + 2, TaskFinished = WM_APP + 3;
int Scale(HWND window, int value)
{
    return MulDiv(value, GetDpiForWindow(window), 96);
}
}

WyMaxTaskRunner::WyMaxTaskRunner(WyBridgeClient& client) : client_(client) {}
void WyMaxTaskRunner::Register(HINSTANCE module)
{
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_PROGRESS_CLASS};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{sizeof(wc)};
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = ProgressClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        throw std::runtime_error("无法注册进度窗口。");
}
void WyMaxTaskRunner::Unregister(HINSTANCE module) { UnregisterClassW(ProgressClass, module); }
WyMaxTaskRunner::~WyMaxTaskRunner() { Stop(); }

void WyMaxTaskRunner::Start(HWND owner, HWND host, const std::wstring& title,
                            std::function<void(const WyBridgeTask&)> work,
                            std::function<void(const std::string&)> complete, bool showProgress)
{
    if (worker_.joinable() || dialog_)
        throw std::logic_error("已有操作正在进行。");
    cancelled_ = false;
    showProgress_ = showProgress;
    progressPosted_ = false;
    marquee_ = false;
    done_ = total_ = 0;
    phase_ = displayedPhase_ = L"正在准备…";
    error_.clear();
    complete_ = std::move(complete);
    const HINSTANCE module = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    if (showProgress)
    {
        RECT ownerRect{};
        GetWindowRect(owner, &ownerRect);
        const int width = Scale(owner, 410), height = Scale(owner, 103);
        dialog_ = CreateWindowExW(WS_EX_TOOLWINDOW, ProgressClass, title.c_str(), WS_POPUP | WS_CAPTION | WS_CLIPCHILDREN,
                                  (ownerRect.left + ownerRect.right - width) / 2, (ownerRect.top + ownerRect.bottom - height) / 2,
                                  width, height, owner, nullptr, module, this);
    }
    else
        dialog_ = CreateWindowExW(0, ProgressClass, nullptr, 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, module, this);
    if (!dialog_)
        throw std::runtime_error("无法创建任务窗口。");
    try
    {
        if (showProgress)
        {
            sceneChanges_ = std::make_unique<WyMaxSceneChanges>();
            if (IsWindowEnabled(host))
            {
                hostToEnable_ = host;
                EnableWindow(host, FALSE);
            }
            ShowWindow(dialog_, SW_SHOWNOACTIVATE);
            ShowProgress();
            UpdateWindow(dialog_);
        }
        worker_ = std::thread([this, work = std::move(work)]
        {
            try
            {
                WyBridgeTask task;
                task.checkpoint = [this] { Check(); };
                task.onMain = [this](std::function<nlohmann::json()> call) { return OnMain(std::move(call), false); };
                task.writeOnMain = [this](std::function<nlohmann::json()> call) { return OnMain(std::move(call), true); };
                task.report = [this](const wchar_t* phase)
                {
                    Check();
                    if (!showProgress_)
                        return;
                    std::lock_guard guard(mutex_);
                    phase_ = phase;
                    done_ = total_ = 0;
                    PostProgress();
                };
                task.progress = [this](std::size_t done, std::size_t total)
                {
                    Check();
                    if (!showProgress_)
                        return;
                    std::lock_guard guard(mutex_);
                    done_ = done;
                    total_ = total;
                    PostProgress();
                };
                work(task);
            }
            catch (const std::exception& e) { error_ = e.what(); }
            catch (...) { error_ = "任务执行失败。"; }
            PostMessageW(dialog_, TaskFinished, 0, 0);
        });
    }
    catch (...)
    {
        Stop();
        throw;
    }
}

void WyMaxTaskRunner::Check() const
{
    if (cancelled_)
        throw std::runtime_error("场景已切换或插件已关闭。");
}
nlohmann::json WyMaxTaskRunner::OnMain(std::function<nlohmann::json()> call, bool writing)
{
    std::future<nlohmann::json> result;
    {
        std::lock_guard guard(mutex_);
        Check();
        pending_ = std::packaged_task<nlohmann::json()>([this, call = std::move(call), writing]
        {
            Check();
            // Background handshakes can wait while the user edits the scene.
            // Start guarding the snapshot only when the worker first dispatches to Max.
            if (!sceneChanges_)
                sceneChanges_ = std::make_unique<WyMaxSceneChanges>();
            sceneChanges_->Check();
            std::function<void()> repaint;
            if (showProgress_)
                repaint = [this]
                {
                    // Only repaint, never dispatch input or another mutation inside a Max undo transaction.
                    if (HWND owner = GetParent(dialog_))
                        RedrawWindow(owner, nullptr, nullptr, RDW_UPDATENOW | RDW_ALLCHILDREN);
                    RedrawWindow(dialog_, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
                };
            try
            {
                auto result = WyRunSceneCall(call, repaint);
                if (writing)
                    sceneChanges_->AcceptOwnChanges();
                return result;
            }
            catch (...)
            {
                if (writing)
                    sceneChanges_->AcceptOwnChanges();
                throw;
            }
        });
        result = pending_.get_future();
        if (!PostMessageW(dialog_, RunSceneCall, 0, 0))
        {
            pending_ = {};
            throw std::runtime_error("无法调度 Max 场景操作。");
        }
    }
    return result.get();
}
void WyMaxTaskRunner::PostProgress()
{
    if (!progressPosted_)
        progressPosted_ = PostMessageW(dialog_, UpdateProgress, 0, 0) != FALSE;
}
void WyMaxTaskRunner::ShowProgress()
{
    std::size_t done, total;
    {
        std::lock_guard guard(mutex_);
        displayedPhase_ = phase_;
        done = done_;
        total = total_;
        progressPosted_ = false;
    }
    if (marquee_ != (total == 0))
    {
        marquee_ = total == 0;
        LONG_PTR style = GetWindowLongPtrW(bar_, GWL_STYLE);
        SetWindowLongPtrW(bar_, GWL_STYLE, marquee_ ? style | PBS_MARQUEE : style & ~PBS_MARQUEE);
        SendMessageW(bar_, PBM_SETMARQUEE, marquee_, 35);
    }
    if (total)
    {
        displayedPhase_ += L"    " + std::to_wstring(done) + L" / " + std::to_wstring(total);
        SendMessageW(bar_, PBM_SETPOS, static_cast<WPARAM>(1000.0 * (std::min)(done, total) / total), 0);
    }
    InvalidateRect(dialog_, nullptr, FALSE);
}
void WyMaxTaskRunner::Layout()
{
    RECT area{};
    GetClientRect(dialog_, &area);
    const int pad = Scale(dialog_, 15);
    MoveWindow(bar_, pad, Scale(dialog_, 39), (std::max)(1L, area.right - 2 * pad), Scale(dialog_, 12), TRUE);
    if (font_)
        DeleteObject(font_);
    font_ = CreateFontW(-Scale(dialog_, 14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
}
void WyMaxTaskRunner::RestoreHost()
{
    sceneChanges_.reset();
    if (hostToEnable_ && IsWindow(hostToEnable_))
        EnableWindow(hostToEnable_, TRUE);
    hostToEnable_ = nullptr;
}
void WyMaxTaskRunner::Cancel()
{
    if (!worker_.joinable())
        return;
    cancelled_ = true;
    client_.Interrupt();
    std::lock_guard guard(mutex_);
    pending_ = {}; // Release a worker waiting for a scene call that will never run.
}
void WyMaxTaskRunner::Stop()
{
    Cancel();
    if (worker_.joinable())
        worker_.join();
    complete_ = {};
    RestoreHost();
    if (dialog_)
        DestroyWindow(dialog_);
}
void WyMaxTaskRunner::Finish()
{
    if (!worker_.joinable())
        return;
    worker_.join(); // TaskFinished is posted after the last scene call returns.
    auto complete = std::move(complete_);
    auto error = std::move(error_);
    RestoreHost();
    DestroyWindow(dialog_);
    if (complete)
        complete(error);
}
LRESULT CALLBACK WyMaxTaskRunner::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* runner = reinterpret_cast<WyMaxTaskRunner*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        runner = static_cast<WyMaxTaskRunner*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        runner->dialog_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(runner));
    }
    if (!runner)
        return DefWindowProcW(window, message, wParam, lParam);
    switch (message)
    {
    case WM_CREATE:
        if (!runner->showProgress_)
            return 0;
        runner->bar_ = CreateWindowExW(0, PROGRESS_CLASSW, nullptr, WS_CHILD | WS_VISIBLE | PBS_SMOOTH, 0, 0, 0, 0,
                                       window, nullptr, reinterpret_cast<CREATESTRUCTW*>(lParam)->hInstance, nullptr);
        if (!runner->bar_)
            return -1;
        SendMessageW(runner->bar_, PBM_SETRANGE32, 0, 1000);
        runner->Layout();
        return 0;
    case WM_DPICHANGED:
    {
        if (!runner->bar_)
            break;
        const auto& rect = *reinterpret_cast<RECT*>(lParam);
        SetWindowPos(window, nullptr, rect.left, rect.top, rect.right - rect.left, rect.bottom - rect.top,
                     SWP_NOACTIVATE | SWP_NOZORDER);
        runner->Layout();
        return 0;
    }
    case WM_PAINT:
    {
        if (!runner->bar_)
            break;
        PAINTSTRUCT ps{};
        HDC dc = BeginPaint(window, &ps);
        RECT area{};
        GetClientRect(window, &area);
        SetDCBrushColor(dc, RGB(35, 42, 49));
        FillRect(dc, &area, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
        area.left += Scale(window, 15);
        area.right -= Scale(window, 15);
        area.top += Scale(window, 6);
        area.bottom = area.top + Scale(window, 28);
        auto old = SelectObject(dc, runner->font_);
        SetTextColor(dc, RGB(233, 240, 245));
        SetBkMode(dc, TRANSPARENT);
        DrawTextW(dc, runner->displayedPhase_.c_str(), -1, &area, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        SelectObject(dc, old);
        EndPaint(window, &ps);
        return 0;
    }
    case RunSceneCall:
    {
        std::packaged_task<nlohmann::json()> call;
        { std::lock_guard guard(runner->mutex_); call = std::move(runner->pending_); }
        if (call.valid())
            call();
        return 0;
    }
    case UpdateProgress:
        runner->ShowProgress();
        return 0;
    case TaskFinished:
        runner->Finish();
        return 0;
    case WM_CLOSE:
        return 0;
    case WM_DESTROY:
        runner->Cancel();
        if (runner->worker_.joinable())
            runner->worker_.join();
        runner->RestoreHost();
        if (runner->font_)
            DeleteObject(runner->font_);
        runner->font_ = nullptr;
        runner->dialog_ = runner->bar_ = nullptr;
        return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
