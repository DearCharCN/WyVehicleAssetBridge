#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "MaxPanel.h"
#include "MaxTask.h"
#include "BridgeClient.h"
#include "MaxColliderPreview.h"
#include "MaxScene.h"
#include <commctrl.h>
#include <windowsx.h>
#include <max.h>
#include <actiontable.h>
#include <custcont.h>
#include <notify.h>
#include <CUI/ICuiMenuManager.h>
#include <CUI/ICuiMenu.h>
#include <algorithm>
#include <cmath>
#include <functional>
#include <iterator>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WyVehicleAssetBridgeUI
{
namespace
{
using Json = nlohmann::json;
constexpr wchar_t PanelClass[] = L"WyVehicleAssetBridge.NativePanel";
constexpr wchar_t ColliderClass[] = L"WyVehicleAssetBridge.ColliderDialog";
constexpr UINT RefreshMessage = WM_APP + 0x41;
enum class ConnectionRequest { None, Check, Reconnect };
constexpr UINT CheckPointerExit = WM_APP + 0x42;
constexpr UINT_PTR ListSubclassId = 0x57595641;
constexpr ActionTableId ActionTableIdentifier = 0x681c4bd3;
constexpr int ShowActionId = 1;
constexpr const wchar_t* FilterLabels[] = {L"全部状态", L"有差异", L"任一端缺少", L"待匹配"};
constexpr const wchar_t* ColliderModes[] = {L"整体凸包", L"上下凸包"};
struct CellButtonColors
{
    COLORREF fill;
    COLORREF text;
};
constexpr CellButtonColors MaxToUEColors{RGB(51, 95, 207), RGB(255, 255, 255)};
constexpr CellButtonColors UEToMaxColors{RGB(255, 149, 71), RGB(32, 26, 21)};
const MaxSDK::MaxGuid MenuIdentifier("b4822673-0682-4acd-a512-ed4a4599d5d5");
const MaxSDK::MaxGuid MenuItemIdentifier("b66dfeb9-fc04-4dbb-b7be-00af0ec3ed67");
enum Control : int
{
    Reconnect = 2101,
    VehicleType,
    VehicleName,
    VehicleVariant,
    ApplyFields,
    RefreshButton,
    CreateAll,
    Search,
    Filter,
    Outline,
    Details,
    Connection,
    Footer,
    TargetPath,
    BuildCollider = 2116,
    BatchToUE,
    BatchToMax,
    ProjectPath,
    ColliderMode = 3101,
    ColliderHeight,
    ColliderGenerate,
    ColliderCancel,
    ColliderError,
    ColliderModeLabel,
    ColliderHeightLabel,
    ColliderDragHint
};
HINSTANCE Module = nullptr;
bool Initialized = false;
ActionTable* RegisteredActions = nullptr;
ActionItem* RegisteredAction = nullptr;
ActionCallback RegisteredCallback;

std::wstring Wide(const std::string& value)
{
    if (value.empty())
        return {};
    const int count =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr, 0);
    if (!count)
        return L"（文本编码无效）";
    std::wstring result(static_cast<size_t>(count), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count);
    return result;
}
std::string Utf8(const std::wstring& value)
{
    if (value.empty())
        return {};
    const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), nullptr,
                                          0, nullptr, nullptr);
    if (!count)
        throw std::runtime_error("Invalid Unicode text.");
    std::string result(static_cast<size_t>(count), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(), static_cast<int>(value.size()), result.data(), count,
                        nullptr, nullptr);
    return result;
}
std::wstring Text(HWND control)
{
    std::wstring value(static_cast<size_t>(GetWindowTextLengthW(control)) + 1, L'\0');
    const int length = GetWindowTextW(control, value.data(), static_cast<int>(value.size()));
    value.resize(static_cast<size_t>(std::max(0, length)));
    return value;
}
std::string String(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}
bool Boolean(const Json& object, const char* key)
{
    const auto it = object.find(key);
    return it != object.end() && it->is_boolean() && it->get<bool>();
}
std::wstring Lower(std::wstring value)
{
    if (!value.empty())
        CharLowerBuffW(value.data(), static_cast<DWORD>(value.size()));
    return value;
}
std::wstring Number(double value)
{
    std::wostringstream stream;
    stream.imbue(std::locale::classic());
    stream.precision(12);
    stream << value;
    return stream.str();
}
COLORREF Mix(COLORREF base, COLORREF tint, int percent)
{
    const auto channel = [percent](int a, int b) { return (a * (100 - percent) + b * percent) / 100; };
    return RGB(channel(GetRValue(base), GetRValue(tint)), channel(GetGValue(base), GetGValue(tint)),
               channel(GetBValue(base), GetBValue(tint)));
}
struct Node
{
    std::string id, parent, owner, kind, theme, status;
    std::wstring name, asset, error;
    std::vector<std::string> scopes;
    Json maxNode;
    bool toUE = false, toMax = false;
    std::vector<size_t> children;
    bool Preview() const { return kind == "bone" || kind == "hull"; }
    bool HasSelectButton() const { return Preview() || kind == "staticMesh" || kind == "skeletalPart"; }
};
struct VisibleNode
{
    size_t node = 0;
    int depth = 0;
    bool expanded = false;
};

class Panel final
{
  public:
    ~Panel();
    void Show();
    void Destroy();
    void SceneChanging();
    void SceneChanged(bool replaced);

  private:
    WyBridgeClient client_;
    WyMaxTaskRunner tasks_{client_};
    bool refreshAfterCancel_ = false, sceneChanging_ = false;
    HWND window_ = nullptr, colliderWindow_ = nullptr;
    HWND connection_ = nullptr, reconnect_ = nullptr, projectPath_ = nullptr;
    HWND type_ = nullptr, name_ = nullptr, variant_ = nullptr, target_ = nullptr;
    HWND list_ = nullptr, details_ = nullptr, search_ = nullptr, filter_ = nullptr, footer_ = nullptr;
    HWND actionTooltip_ = nullptr;
    HWND acceleratorOwner_ = nullptr;
    HWND pendingPointerExit_ = nullptr, composingField_ = nullptr;
    std::wstring tooltipText_;
    HWND mode_ = nullptr, height_ = nullptr, colliderError_ = nullptr;
    bool colliderRegistered_ = false, heightEdited_ = false, heightDragPending_ = false, draggingHeight_ = false;
    int heightDragStartX_ = 0;
    double heightDragStartCm_ = 0.0;
    HFONT font_ = nullptr, statusFont_ = nullptr;
    HIMAGELIST rowHeight_ = nullptr;
    HBRUSH background_ = CreateSolidBrush(RGB(35, 42, 49));
    HBRUSH editBackground_ = CreateSolidBrush(RGB(28, 34, 40));
    UINT dpi_ = 96;
    bool busy_ = false, loading_ = false, connected_ = false, metadataDraft_ = false;
    bool connecting_ = true;
    bool backgroundConnection_ = false;
    bool empty_ = false;
    Json snapshot_;
    std::vector<Node> nodes_;
    std::vector<VisibleNode> visible_;
    std::unordered_map<std::string, size_t> index_;
    std::unordered_set<std::string> expanded_, known_, filteredCollapsed_, selected_;
    int anchor_ = -1, focused_ = -1;
    int Scale(int pixels) const
    {
        return MulDiv(pixels, static_cast<int>(dpi_), 96);
    }

  public:
    static LRESULT CALLBACK WindowProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ColliderProc(HWND, UINT, WPARAM, LPARAM);
    static LRESULT CALLBACK ListProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK ColliderInputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
    static LRESULT CALLBACK TextInputProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);

  private:
    HWND ControlWindow(const wchar_t*, const wchar_t*, DWORD, int, HWND = nullptr, DWORD = 0);
    void CreateControls();
    void CreateFonts();
    void SizeFilterItems() const;
    void DrawComboItem(const DRAWITEMSTRUCT&) const;
    void UpdateConnectionHeader();
    void DrawConnectionStatus(const DRAWITEMSTRUCT&) const;
    bool DrawFormControl(const DRAWITEMSTRUCT&) const;
    void Layout();
    void LayoutColliderDialog();
    void Refresh();
    void ParseSnapshot(const Json&);
    void PopulateFields();
    void UpdatePreview();
    void EnableControls();
    void BuildVisible();
    void FillList();
    void UpdateSelection();
    void UpdateDetails();
    void Select(int, bool, bool);
    void Toggle(int);
    void ListKey(WPARAM);
    void ContextMenu(POINT);
    void Sync(const std::vector<std::string>&, bool);
    void CreateMissing();
    void BuildCollision();
    void ApplyVehicle();
    void Operate(std::wstring label, std::function<void(const WyBridgeTask&)> operation = {},
                 std::function<void()> onSuccess = {}, ConnectionRequest connection = ConnectionRequest::None);
    void SetStatus(const std::wstring&);
    void OpenColliderDialog();
    void CloseColliderDialog();
    bool HandleTextFocus(WPARAM, LPARAM);
    void ReleaseTextFocus();
    void CheckTextPointer(HWND field);
    void UpdateColliderPreview();
    bool ReadColliderHeight(double& heightCm) const;
    std::string SelectionId(size_t) const;
    int VisibleIndex(const std::string&) const;
    bool CanSync(const Node&, bool) const;
    const Node* ScopeRow(const std::string&) const;
    std::vector<std::string> ActionScopes(const std::vector<std::string>&, bool) const;
    std::vector<std::string> Eligible(bool) const;
    std::wstring StateText(const Node&) const;
    std::wstring AssetName(const Node&) const;
    std::wstring Scope(const Node&) const;
    COLORREF RowColor(size_t) const;
    LRESULT DrawList(NMLVCUSTOMDRAW*);
    void DrawActions(HDC, const RECT&, const Node&) const;
    void DrawCellButton(HDC, const RECT&, const wchar_t*, bool, CellButtonColors) const;
    RECT SelectButtonRect(RECT) const;
    void DrawStateIcon(HDC, RECT&, const Node&) const;
    void DrawName(HDC, const RECT&, size_t) const;
    void InvokeRowAction(int, int);
};
std::unique_ptr<Panel> MainPanel;

Panel::~Panel()
{
    Destroy();
    if (font_)
        DeleteObject(font_);
    if (statusFont_)
        DeleteObject(statusFont_);
    if (rowHeight_)
        ImageList_Destroy(rowHeight_);
    DeleteObject(background_);
    DeleteObject(editBackground_);
}
void Panel::Destroy()
{
    tasks_.Stop();
    CloseColliderDialog();
    if (window_)
        DestroyWindow(window_);
}
bool Panel::HandleTextFocus(WPARAM command, LPARAM source)
{
    const int id = LOWORD(command), event = HIWORD(command);
    const bool combo = id == VehicleType;
    if (!source || !(combo || id == VehicleName || id == VehicleVariant || id == Search || id == ColliderHeight))
        return false;
    const HWND field = reinterpret_cast<HWND>(source);
    if (combo && event == CBN_CLOSEUP && pendingPointerExit_ == field)
        PostMessageW(field, CheckPointerExit, 0, 0);
    // An editable combo reports its inner edit's focus through CBN notifications.
    if (event == (combo ? CBN_SETFOCUS : EN_SETFOCUS))
    {
        if (acceleratorOwner_ || AcceleratorsEnabled())
        {
            if (!acceleratorOwner_)
                DisableAccelerators();
            acceleratorOwner_ = field;
        }
        return true;
    }
    if (event == (combo ? CBN_KILLFOCUS : EN_KILLFOCUS))
    {
        if (pendingPointerExit_ == field)
            pendingPointerExit_ = nullptr;
        if (composingField_ == field)
            composingField_ = nullptr;
        if (acceleratorOwner_ == field)
            ReleaseTextFocus();
        return true;
    }
    return false;
}
void Panel::ReleaseTextFocus()
{
    if (acceleratorOwner_)
    {
        acceleratorOwner_ = nullptr;
        EnableAccelerators();
    }
}
void Panel::CheckTextPointer(HWND field)
{
    if (pendingPointerExit_ != field)
        return;
    const HWND focus = GetFocus();
    const HWND capture = GetCapture();
    if (focus == field || IsChild(field, focus))
    {
        if (composingField_ == field || capture == field || IsChild(field, capture) ||
            (field == type_ && SendMessageW(field, CB_GETDROPPEDSTATE, 0, 0)))
            return;
        RECT bounds{};
        POINT cursor{};
        if (GetCursorPos(&cursor) && GetWindowRect(field, &bounds) && !PtInRect(&bounds, cursor))
            SetFocus(GetAncestor(field, GA_ROOT));
    }
    pendingPointerExit_ = nullptr;
}
LRESULT CALLBACK Panel::TextInputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto* panel = reinterpret_cast<Panel*>(data);
    const HWND field = reinterpret_cast<HWND>(id); // The combo and its inner edit share one hit rectangle.
    switch (message)
    {
    case WM_MOUSEMOVE:
    case WM_LBUTTONDOWN:
    {
        TRACKMOUSEEVENT track{sizeof(track), TME_LEAVE, window, 0};
        TrackMouseEvent(&track);
        break;
    }
    case WM_MOUSELEAVE:
        if (const HWND focus = GetFocus(); field == focus || IsChild(field, focus))
        {
            panel->pendingPointerExit_ = field;
            panel->CheckTextPointer(field);
        }
        break;
    case WM_IME_STARTCOMPOSITION:
        panel->composingField_ = field;
        break;
    case WM_IME_ENDCOMPOSITION:
        if (panel->composingField_ == field)
            panel->composingField_ = nullptr;
        [[fallthrough]];
    case WM_CAPTURECHANGED:
    case WM_LBUTTONUP:
        // Let the native control finish the selection or commit the IME result first.
        if (panel->pendingPointerExit_ == field)
            PostMessageW(field, CheckPointerExit, 0, 0);
        break;
    case CheckPointerExit:
        panel->CheckTextPointer(field);
        return 0;
    case WM_NCDESTROY:
        if (panel->composingField_ == field)
            panel->composingField_ = nullptr;
        if (panel->pendingPointerExit_ == field)
            panel->pendingPointerExit_ = nullptr;
        RemoveWindowSubclass(window, TextInputProc, id);
        break;
    }
    return DefSubclassProc(window, message, wParam, lParam);
}
void Panel::SceneChanging()
{
    WyInvalidateSceneSelectionKeys();
    sceneChanging_ = true;
    CloseColliderDialog();
    if (busy_)
    {
        refreshAfterCancel_ = true;
        tasks_.Cancel();
    }
}
void Panel::SceneChanged(bool replaced)
{
    sceneChanging_ = false;
    if (busy_)
    {
        refreshAfterCancel_ = true;
        tasks_.Cancel();
    }
    // A draft belongs to the scene in which it was edited. Never carry it into
    // a different .max file or use a previous scene's selected scope IDs.
    if (replaced)
    {
        metadataDraft_ = false;
        selected_.clear();
        expanded_.clear();
        known_.clear();
        filteredCollapsed_.clear();
        anchor_ = focused_ = -1;
    }
    if (!busy_ && window_ && IsWindowVisible(window_))
        PostMessageW(window_, RefreshMessage, 0, 0);
}
void Panel::Show()
{
    if (colliderWindow_)
    {
        SetForegroundWindow(colliderWindow_);
        return;
    }
    if (!window_)
    {
        window_ = CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_CONTROLPARENT, PanelClass, L"望月载具工具",
                                  WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1120, 780,
                                  GetCOREInterface()->GetMAXHWnd(), nullptr, Module, this);
        if (!window_)
            throw std::runtime_error("Cannot create the native vehicle bridge panel.");
        GetCOREInterface()->RegisterDlgWnd(window_);
    }
    ShowWindow(window_, SW_SHOW);
    SetForegroundWindow(window_);
    PostMessageW(window_, RefreshMessage, 0, 0);
}
HWND Panel::ControlWindow(const wchar_t* type, const wchar_t* caption, DWORD style, int id, HWND parent, DWORD extended)
{
    HWND control =
        CreateWindowExW(extended, type, caption, WS_CHILD | WS_VISIBLE | style, 0, 0, 0, 0, parent ? parent : window_,
                        reinterpret_cast<HMENU>(static_cast<INT_PTR>(id)), Module, nullptr);
    if (control)
    {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font_), TRUE);
        if (id == VehicleType || id == VehicleName || id == VehicleVariant || id == Search || id == ColliderHeight)
        {
            const auto field = reinterpret_cast<UINT_PTR>(control);
            const auto panel = reinterpret_cast<DWORD_PTR>(this);
            SetWindowSubclass(control, TextInputProc, field, panel);
            COMBOBOXINFO combo{sizeof(combo)};
            if (id == VehicleType && GetComboBoxInfo(control, &combo) && combo.hwndItem)
                SetWindowSubclass(combo.hwndItem, TextInputProc, field, panel);
        }
    }
    return control;
}
void Panel::CreateFonts()
{
    if (font_)
        DeleteObject(font_);
    if (statusFont_)
        DeleteObject(statusFont_);
    font_ = CreateFontW(-Scale(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                        CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    statusFont_ = CreateFontW(-Scale(14), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS,
                              CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    EnumChildWindows(
        window_,
        [](HWND child, LPARAM font) -> BOOL
        {
            SendMessageW(child, WM_SETFONT, static_cast<WPARAM>(font), TRUE);
            return TRUE;
        },
        reinterpret_cast<LPARAM>(font_));
    if (filter_)
        SizeFilterItems();
    if (list_ && IsWindow(list_))
    {
        const auto old = rowHeight_;
        rowHeight_ = ImageList_Create(1, Scale(32), ILC_COLOR32, 1, 0);
        ListView_SetImageList(list_, rowHeight_, LVSIL_SMALL);
        if (old)
            ImageList_Destroy(old);
    }
}
void Panel::CreateControls()
{
    dpi_ = GetDpiForWindow(window_);
    CreateFonts();
    const auto label = [this](const wchar_t* text, int id) { return ControlWindow(L"STATIC", text, 0, id); };
    connection_ = ControlWindow(L"STATIC", L"正在连接", SS_OWNERDRAW, Connection);
    reconnect_ = ControlWindow(L"BUTTON", L"重新连接", BS_OWNERDRAW | WS_TABSTOP, Reconnect);
    projectPath_ = ControlWindow(L"STATIC", L"", SS_OWNERDRAW | SS_NOTIFY, ProjectPath);
    ControlWindow(L"STATIC", L"载具类型", SS_OWNERDRAW, 2001);
    ControlWindow(L"STATIC", L"载具名", SS_OWNERDRAW, 2002);
    ControlWindow(L"STATIC", L"变体名", SS_OWNERDRAW, 2003);
    type_ = ControlWindow(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWN | WS_VSCROLL, VehicleType);
    name_ = ControlWindow(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, VehicleName, nullptr, WS_EX_CLIENTEDGE);
    variant_ = ControlWindow(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, VehicleVariant, nullptr, WS_EX_CLIENTEDGE);
    ControlWindow(L"BUTTON", L"应用并比对", WS_TABSTOP | BS_OWNERDRAW, ApplyFields);
    target_ = ControlWindow(L"STATIC", L"", SS_OWNERDRAW, TargetPath);
    ControlWindow(L"BUTTON", L"重新比对", WS_TABSTOP | BS_OWNERDRAW, RefreshButton);
    ControlWindow(L"BUTTON", L"首次创建资源", WS_TABSTOP | BS_OWNERDRAW, CreateAll);
    search_ = ControlWindow(L"EDIT", L"", WS_TABSTOP | ES_AUTOHSCROLL, Search, nullptr, WS_EX_CLIENTEDGE);
    SendMessageW(search_, EM_SETCUEBANNER, FALSE, reinterpret_cast<LPARAM>(L"搜索节点或资源"));
    filter_ = ControlWindow(WC_COMBOBOXW, L"", WS_TABSTOP | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, Filter);
    for (const auto* value : FilterLabels)
        SendMessageW(filter_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(value));
    SendMessageW(filter_, CB_SETCURSEL, 0, 0);
    SizeFilterItems();
    list_ = ControlWindow(WC_LISTVIEWW, L"", WS_TABSTOP | LVS_REPORT | LVS_SHOWSELALWAYS, Outline, nullptr, WS_EX_CLIENTEDGE);
    ListView_SetExtendedListViewStyle(list_, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER);
    rowHeight_ = ImageList_Create(1, Scale(32), ILC_COLOR32, 1, 0);
    ListView_SetImageList(list_, rowHeight_, LVSIL_SMALL);
    ListView_SetBkColor(list_, RGB(28, 34, 40));
    ListView_SetTextColor(list_, RGB(230, 235, 239));
    ListView_SetTextBkColor(list_, CLR_NONE);
    const wchar_t* columns[] = {L"资源 / 部位 / LOD", L"所属 UE 资产", L"比对状态", L"同步方向", L"在大纲中选中"};
    for (int i = 0; i < static_cast<int>(std::size(columns)); ++i)
    {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_WIDTH;
        column.pszText = const_cast<wchar_t*>(columns[i]);
        column.cx = Scale(i == 0 ? 240 : 160);
        ListView_InsertColumn(list_, i, &column);
    }
    SetWindowSubclass(list_, ListProc, ListSubclassId, reinterpret_cast<DWORD_PTR>(this));
    actionTooltip_ =
        CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX, CW_USEDEFAULT,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, window_, nullptr, Module, nullptr);
    if (actionTooltip_)
    {
        TTTOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = window_;
        tool.uId = reinterpret_cast<UINT_PTR>(list_);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(actionTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        tool.uId = reinterpret_cast<UINT_PTR>(projectPath_);
        SendMessageW(actionTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        tool.uId = reinterpret_cast<UINT_PTR>(reconnect_);
        tool.lpszText = const_cast<wchar_t*>(L"重新连接 UE");
        SendMessageW(actionTooltip_, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(actionTooltip_, TTM_SETMAXTIPWIDTH, 0, Scale(420));
    }
    details_ = ControlWindow(L"EDIT", L"请选择一个节点。", ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, Details,
                             nullptr, WS_EX_CLIENTEDGE);
    footer_ = label(L"撤销：在接收端使用 Ctrl+Z。", Footer);
    HMENU bar = CreateMenu(), collision = CreatePopupMenu();
    AppendMenuW(collision, MF_STRING, BuildCollider, L"构建 BodyCollider");
    AppendMenuW(bar, MF_POPUP, reinterpret_cast<UINT_PTR>(collision), L"碰撞");
    SetMenu(window_, bar);
    UpdateConnectionHeader();
    EnableControls();
}
void Panel::UpdateConnectionHeader()
{
    SetWindowTextW(connection_, connected_ ? L"UE 已连接" : connecting_ ? L"正在连接" : L"UE 未连接");
    const auto path = connected_ ? Wide(String(snapshot_, "project")) : std::wstring{};
    SetWindowTextW(projectPath_, path.empty() ? L"" : (L"工程路径  " + path).c_str());
    ShowWindow(projectPath_, path.empty() ? SW_HIDE : SW_SHOWNA);
    Layout();
    InvalidateRect(connection_, nullptr, FALSE);
    InvalidateRect(projectPath_, nullptr, FALSE);
}
void Panel::DrawConnectionStatus(const DRAWITEMSTRUCT& item) const
{
    HDC dc = item.hDC;
    const int saved = SaveDC(dc);
    RECT bounds = item.rcItem;
    FillRect(dc, &bounds, background_);
    const COLORREF color = connected_ ? RGB(131, 209, 165) : connecting_ ? RGB(173, 182, 190) : RGB(239, 195, 124);
    // Both the icon's left edge and the project caption start at the header's left margin.
    const int x = bounds.left + Scale(1), y = (bounds.top + bounds.bottom) / 2;
    const auto pen = CreatePen(PS_SOLID, Scale(2), color);
    SelectObject(dc, pen);
    SelectObject(dc, GetStockObject(HOLLOW_BRUSH));
    if (connected_)
    {
        const POINT mark[] = {{x, y}, {x + Scale(5), y + Scale(5)}, {x + Scale(15), y - Scale(6)}};
        Polyline(dc, mark, 3);
    }
    else if (connecting_)
        Arc(dc, x, y - Scale(7), x + Scale(15), y + Scale(8), x + Scale(14), y, x + Scale(8), y - Scale(7));
    else
    {
        const POINT triangle[] = {{x + Scale(8), y - Scale(8)}, {x + Scale(16), y + Scale(7)}, {x, y + Scale(7)}};
        Polygon(dc, triangle, 3);
        MoveToEx(dc, x + Scale(8), y - Scale(3), nullptr);
        LineTo(dc, x + Scale(8), y + Scale(2));
        RECT dot{x + Scale(7), y + Scale(3), x + Scale(9), y + Scale(5)};
        SetDCBrushColor(dc, color);
        FillRect(dc, &dot, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    }
    bounds.left += Scale(25);
    SelectObject(dc, statusFont_ ? statusFont_ : font_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, color);
    const auto caption = Text(item.hwndItem);
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &bounds, DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
    if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
        DrawFocusRect(dc, &item.rcItem);
    RestoreDC(dc, saved);
    DeleteObject(pen);
}
void Panel::SizeFilterItems() const
{
    const int typeHeight = static_cast<int>(SendMessageW(type_, CB_GETITEMHEIGHT, static_cast<WPARAM>(-1), 0));
    const int height = std::max(typeHeight, Scale(21));
    SendMessageW(filter_, CB_SETITEMHEIGHT, 0, height);
    SendMessageW(filter_, CB_SETITEMHEIGHT, static_cast<WPARAM>(-1), height);
}
void Panel::DrawComboItem(const DRAWITEMSTRUCT& item) const
{
    COMBOBOXINFO typeInfo{sizeof(typeInfo)};
    const HFONT typeFont = GetComboBoxInfo(type_, &typeInfo) && typeInfo.hwndItem
                               ? reinterpret_cast<HFONT>(SendMessageW(typeInfo.hwndItem, WM_GETFONT, 0, 0))
                               : nullptr;
    const int saved = SaveDC(item.hDC);
    SelectObject(item.hDC, typeFont ? typeFont : font_);
    const bool disabled = (item.itemState & ODS_DISABLED) != 0;
    const bool selected = !disabled && (item.itemState & ODS_SELECTED) != 0;
    SetDCBrushColor(item.hDC, selected ? GetSysColor(COLOR_HIGHLIGHT) : RGB(48, 55, 62));
    FillRect(item.hDC, &item.rcItem, static_cast<HBRUSH>(GetStockObject(DC_BRUSH)));
    SetBkMode(item.hDC, TRANSPARENT);
    SetTextColor(item.hDC, disabled ? RGB(173, 184, 194) : selected ? GetSysColor(COLOR_HIGHLIGHTTEXT) : RGB(235, 241, 246));
    const bool filter = item.CtlID == Filter;
    const auto labels = filter ? FilterLabels : ColliderModes;
    if (item.itemID < (filter ? std::size(FilterLabels) : std::size(ColliderModes)))
    {
        RECT text = item.rcItem;
        text.left += Scale(4);
        DrawTextW(item.hDC, labels[item.itemID], -1, &text,
                  DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
    }
    if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
        DrawFocusRect(item.hDC, &item.rcItem);
    RestoreDC(item.hDC, saved);
}
bool Panel::DrawFormControl(const DRAWITEMSTRUCT& item) const
{
    if (item.CtlType == ODT_COMBOBOX && (item.CtlID == Filter || item.CtlID == ColliderMode))
    {
        DrawComboItem(item);
        return true;
    }
    if (item.CtlID == Connection)
    {
        DrawConnectionStatus(item);
        return true;
    }
    const bool button = item.CtlType == ODT_BUTTON &&
                        (item.CtlID == Reconnect || item.CtlID == ApplyFields || item.CtlID == RefreshButton ||
                         item.CtlID == CreateAll || item.CtlID == ColliderCancel || item.CtlID == ColliderGenerate);
    const bool label = item.CtlType == ODT_STATIC &&
                       ((item.CtlID >= 2001 && item.CtlID <= 2003) || item.CtlID == TargetPath || item.CtlID == ProjectPath ||
                        (item.CtlID >= ColliderError && item.CtlID <= ColliderDragHint));
    if (!button && !label)
        return false;

    const auto caption = Text(item.hwndItem);
    HDC dc = item.hDC;
    const int saved = SaveDC(dc);
    const auto outlineFont = list_ ? reinterpret_cast<HFONT>(SendMessageW(list_, WM_GETFONT, 0, 0)) : nullptr;
    SelectObject(dc, outlineFont ? outlineFont : font_);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, item.CtlID == ColliderError ? RGB(255, 190, 122) : RGB(230, 237, 243));
    RECT bounds = item.rcItem;
    UINT flags = DT_LEFT | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    if (button)
    {
        const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
        const bool disabled = (item.itemState & ODS_DISABLED) != 0;
        const bool pressed = (item.itemState & ODS_SELECTED) != 0;
        SetDCBrushColor(dc, disabled ? RGB(43, 49, 56) : pressed ? RGB(49, 66, 79) : RGB(63, 75, 86));
        FillRect(dc, &bounds, brush);
        SetDCBrushColor(dc, disabled ? RGB(71, 81, 91) : RGB(112, 129, 142));
        FrameRect(dc, &bounds, brush);
        if ((item.itemState & (ODS_FOCUS | ODS_NOFOCUSRECT)) == ODS_FOCUS)
        {
            RECT focus = bounds;
            InflateRect(&focus, -Scale(3), -Scale(3));
            DrawFocusRect(dc, &focus);
        }
        SetTextColor(dc, disabled ? RGB(173, 184, 194) : RGB(248, 250, 252));
        flags = DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX;
    }
    else
    {
        FillRect(dc, &bounds, background_);
        if (item.CtlID == TargetPath)
            flags = DT_LEFT | DT_WORDBREAK | DT_END_ELLIPSIS | DT_NOPREFIX;
        if (item.CtlID == ProjectPath)
            flags |= DT_END_ELLIPSIS;
    }
    DrawTextW(dc, caption.c_str(), static_cast<int>(caption.size()), &bounds, flags);
    RestoreDC(dc, saved);
    return true;
}
void Panel::Layout()
{
    RECT client{};
    GetClientRect(window_, &client);
    const int w = client.right, h = client.bottom, p = Scale(14), gap = Scale(9), line = Scale(27);
    const auto move = [this](int id, int x, int y, int width, int height)
    { MoveWindow(GetDlgItem(window_, id), x, y, std::max(1, width), std::max(1, height), TRUE); };
    const int reconnectWidth = Scale(100);
    move(Connection, p, p, w - p * 2 - reconnectWidth - gap, line);
    move(Reconnect, w - p - reconnectWidth, p, reconnectWidth, line);
    const int pathHeight = connected_ && GetWindowTextLengthW(projectPath_) ? Scale(24) : 0;
    if (pathHeight)
        move(ProjectPath, p, p + line, w - p * 2, pathHeight);
    const int applyWidth = Scale(116), fields = w - p * 2 - applyWidth - gap * 3;
    const int typeWidth = fields * 29 / 100, nameWidth = fields * 42 / 100, variantWidth = fields - typeWidth - nameWidth;
    const int y = p + line + pathHeight + gap;
    move(2001, p, y, typeWidth, Scale(22));
    move(2002, p + typeWidth + gap, y, nameWidth, Scale(22));
    move(2003, p + typeWidth + nameWidth + gap * 2, y, variantWidth, Scale(22));
    move(VehicleType, p, y + Scale(24), typeWidth, Scale(250));
    move(VehicleName, p + typeWidth + gap, y + Scale(24), nameWidth, line);
    move(VehicleVariant, p + typeWidth + nameWidth + gap * 2, y + Scale(24), variantWidth, line);
    move(ApplyFields, w - p - applyWidth, y + Scale(24), applyWidth, line);
    move(TargetPath, p, y + Scale(58), w - p * 2, Scale(39));
    const int controlsY = y + Scale(107);
    move(Search, p, controlsY, std::max(Scale(140), w - Scale(540)), line);
    move(Filter, std::max(Scale(170), w - Scale(512)), controlsY, Scale(140), Scale(220));
    move(RefreshButton, w - p - Scale(225), controlsY, Scale(100), line);
    move(CreateAll, w - p - Scale(116), controlsY, Scale(116), line);
    const int top = controlsY + line + gap, bottom = h - p - Scale(42), contentHeight = std::max(Scale(100), bottom - top);
    const bool stacked = w < Scale(1030);
    const int detailWidth = stacked ? 0 : std::clamp(w / 3, Scale(260), Scale(340));
    const int listWidth = w - p * 2 - (stacked ? 0 : detailWidth + gap);
    move(Outline, p, top, listWidth, stacked ? std::max(Scale(100), contentHeight - Scale(130) - gap) : contentHeight);
    move(Details, stacked ? p : w - p - detailWidth, stacked ? bottom - Scale(130) : top, stacked ? w - p * 2 : detailWidth,
         stacked ? Scale(130) : contentHeight);
    move(Footer, p, h - p - Scale(33), w - p * 2, Scale(33));
    if (list_)
    {
        const int available = listWidth - Scale(22), actions = Scale(150), status = Scale(84), locate = Scale(100);
        const int remaining = available - actions - status - locate;
        const int asset = std::max(Scale(100), remaining * 44 / 100);
        ListView_SetColumnWidth(list_, 0, std::max(Scale(125), remaining - asset));
        ListView_SetColumnWidth(list_, 1, asset);
        ListView_SetColumnWidth(list_, 2, status);
        ListView_SetColumnWidth(list_, 3, actions);
        ListView_SetColumnWidth(list_, 4, locate);
    }
}

void Panel::SetStatus(const std::wstring& value)
{
    SetWindowTextW(footer_, value.c_str());
}
void Panel::Refresh()
{
    Operate(L"重新比对");
}
void Panel::ParseSnapshot(const Json& snapshot)
{
    if (!snapshot.is_object() || !snapshot.contains("rows") || !snapshot["rows"].is_array())
        throw std::runtime_error("Bridge returned an invalid scene snapshot.");
    std::vector<Node> nodes;
    std::unordered_map<std::string, size_t> index;
    for (const auto& row : snapshot["rows"])
    {
        Node node;
        node.id = String(row, "id");
        node.parent = String(row, "parent");
        node.owner = String(row, "owner");
        node.kind = String(row, "kind");
        node.theme = String(row, "theme");
        node.scopes = row.at("scopes").get<std::vector<std::string>>();
        node.maxNode = row.at("maxNode");
        node.status = String(row, "status");
        node.name = Wide(String(row, "name"));
        node.asset = Wide(String(row, "asset"));
        node.error = Wide(String(row, "error"));
        node.toUE = Boolean(row, "canToUE");
        node.toMax = Boolean(row, "canToMax");
        if (node.id.empty() || !index.emplace(node.id, nodes.size()).second)
            throw std::runtime_error("Bridge returned duplicate or missing node IDs.");
        nodes.push_back(std::move(node));
    }
    for (size_t i = 0; i < nodes.size(); ++i)
    {
        const auto parent = index.find(nodes[i].parent);
        if (parent != index.end())
        {
            // Reject a cyclic hierarchy before touching the visible state.
            std::unordered_set<size_t> visited{i};
            auto next = parent;
            while (next != index.end())
            {
                if (!visited.insert(next->second).second)
                    throw std::runtime_error("Bridge returned a cyclic node hierarchy.");
                next = index.find(nodes[next->second].parent);
            }
            nodes[parent->second].children.push_back(i);
        }
    }
    nodes_ = std::move(nodes);
    index_ = std::move(index);
    snapshot_ = snapshot;
    connected_ = Boolean(snapshot, "connected");
    empty_ = Boolean(snapshot, "empty");
    for (const auto& node : nodes_)
        if (known_.insert(node.id).second && (node.kind == "category" || node.id == "Root" || node.kind == "physics"))
            expanded_.insert(node.id);
}
void Panel::PopulateFields()
{
    if (metadataDraft_)
    {
        UpdatePreview();
        return;
    }
    loading_ = true;
    SendMessageW(type_, CB_RESETCONTENT, 0, 0);
    if (snapshot_.contains("types") && snapshot_["types"].is_array())
        for (const auto& type : snapshot_["types"])
            if (type.is_string())
            {
                const auto label = Wide(type.get<std::string>());
                if (_wcsicmp(label.c_str(), L"CommonVH") != 0)
                    SendMessageW(type_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
            }
    const auto vehicle = snapshot_.value("vehicle", Json::object());
    SetWindowTextW(type_, Wide(String(vehicle, "type")).c_str());
    SetWindowTextW(name_, Wide(String(vehicle, "name")).c_str());
    SetWindowTextW(variant_, Wide(String(vehicle, "variant")).c_str());
    loading_ = false;
    UpdatePreview();
}
void Panel::UpdatePreview()
{
    const auto type = Text(type_), name = Text(name_), variant = Text(variant_);
    const std::wstring path = (type.empty() || name.empty() || variant.empty())
                                  ? L"请填写车辆类型、名称和变体。"
                                  : L"Content/Vehicle/" + type + L"/" + name + L"/" + name + L"_" + variant + L"/";
    SetWindowTextW(target_, ((metadataDraft_ ? L"待应用目录：" : L"目标目录：") + path).c_str());
    if (target_)
        InvalidateRect(target_, nullptr, FALSE);
    EnableControls();
}
void Panel::EnableControls()
{
    if (!window_)
        return;
    EnableWindow(GetDlgItem(window_, Reconnect), !busy_);
    EnableWindow(GetDlgItem(window_, RefreshButton), !busy_);
    EnableWindow(GetDlgItem(window_, ApplyFields), !busy_ && metadataDraft_);
    EnableWindow(GetDlgItem(window_, CreateAll), !busy_ && connected_ && empty_);
    ShowWindow(GetDlgItem(window_, CreateAll), empty_ ? SW_SHOW : SW_HIDE);
    for (HWND control : {type_, name_, variant_, search_, filter_, list_})
        if (control)
            EnableWindow(control, !busy_);
    const HMENU menu = GetMenu(window_);
    if (menu)
    {
        EnableMenuItem(menu, BuildCollider, MF_BYCOMMAND | ((!busy_ && connected_) ? MF_ENABLED : MF_GRAYED));
        DrawMenuBar(window_);
    }
    if (list_)
        InvalidateRect(list_, nullptr, FALSE);
}
void Panel::BuildVisible()
{
    visible_.clear();
    const auto search = Lower(Text(search_));
    const int filter = static_cast<int>(SendMessageW(filter_, CB_GETCURSEL, 0, 0));
    const bool filtering = !search.empty() || filter > 0;
    std::vector<bool> included(nodes_.size(), false);
    std::function<bool(size_t, bool)> include = [&](size_t i, bool parentMatchesText)
    {
        const auto& node = nodes_[i];
        const bool text = parentMatchesText || search.empty() ||
                          Lower(node.name + L" " + node.asset + L" " + Wide(node.id)).find(search) != std::wstring::npos;
        const bool state = filter <= 0 || (filter == 1 && node.status == "different") ||
                           (filter == 2 && (node.status == "maxMissing" || node.status == "ueMissing")) ||
                           (filter == 3 && node.status == "unmatched");
        bool value = text && state;
        for (const size_t child : nodes_[i].children)
            value = include(child, text) || value;
        included[i] = value;
        return value;
    };
    std::function<void(size_t, int)> append = [&](size_t i, int depth)
    {
        if (filtering && !included[i])
            return;
        const auto& node = nodes_[i];
        const bool expanded = filtering ? !filteredCollapsed_.count(node.id) : expanded_.count(node.id) != 0;
        visible_.push_back({i, depth, expanded});
        if (expanded)
            for (size_t child : node.children)
                append(child, depth + 1);
    };
    for (size_t i = 0; i < nodes_.size(); ++i)
        if (!index_.count(nodes_[i].parent))
        {
            include(i, false);
            append(i, 0);
        }
    std::unordered_set<std::string> allowed;
    for (const auto& row : visible_)
        allowed.insert(SelectionId(row.node));
    for (auto it = selected_.begin(); it != selected_.end();)
        if (!allowed.count(*it))
            it = selected_.erase(it);
        else
            ++it;
    focused_ = std::min(focused_, static_cast<int>(visible_.size()) - 1);
    anchor_ = std::min(anchor_, static_cast<int>(visible_.size()) - 1);
}
void Panel::FillList()
{
    const int oldTop = ListView_GetTopIndex(list_);
    SendMessageW(list_, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(list_);
    for (size_t i = 0; i < visible_.size(); ++i)
    {
        const auto& node = nodes_[visible_[i].node];
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<wchar_t*>(node.name.c_str());
        ListView_InsertItem(list_, &item);
        auto asset = AssetName(node), status = StateText(node);
        ListView_SetItemText(list_, static_cast<int>(i), 1, asset.data());
        ListView_SetItemText(list_, static_cast<int>(i), 2, status.data());
        if (!node.Preview())
            ListView_SetItemText(list_, static_cast<int>(i), 3, const_cast<wchar_t*>(L"Max → UE    UE → Max"));
        if (node.HasSelectButton())
            ListView_SetItemText(list_, static_cast<int>(i), 4, const_cast<wchar_t*>(L"选中"));
    }
    UpdateSelection();
    if (!visible_.empty())
        ListView_EnsureVisible(list_, std::min(oldTop, static_cast<int>(visible_.size()) - 1), FALSE);
    SendMessageW(list_, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(list_, nullptr, TRUE);
}
std::string Panel::SelectionId(size_t index) const
{
    const auto& node = nodes_[index];
    if (node.kind == "bone" && index_.count(node.owner))
        return node.owner;
    return node.id;
}
int Panel::VisibleIndex(const std::string& id) const
{
    for (size_t i = 0; i < visible_.size(); ++i)
        if (nodes_[visible_[i].node].id == id)
            return static_cast<int>(i);
    return -1;
}
void Panel::UpdateSelection()
{
    for (size_t i = 0; i < visible_.size(); ++i)
    {
        const auto& node = nodes_[visible_[i].node];
        const UINT flags =
            (selected_.count(node.id) ? LVIS_SELECTED : 0) | (static_cast<int>(i) == focused_ ? LVIS_FOCUSED : 0);
        ListView_SetItemState(list_, static_cast<int>(i), flags, LVIS_SELECTED | LVIS_FOCUSED);
    }
    UpdateDetails();
    InvalidateRect(list_, nullptr, FALSE);
}
void Panel::Select(int row, bool control, bool shift)
{
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return;
    if (shift && anchor_ >= 0)
    {
        if (!control)
            selected_.clear();
        for (int i = std::min(anchor_, row); i <= std::max(anchor_, row); ++i)
            selected_.insert(SelectionId(visible_[i].node));
    }
    else
    {
        const auto id = SelectionId(visible_[row].node);
        if (!control)
            selected_.clear();
        if (control && selected_.count(id))
            selected_.erase(id);
        else
            selected_.insert(id);
        anchor_ = row;
    }
    focused_ = row;
    UpdateSelection();
}
void Panel::Toggle(int row)
{
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return;
    const auto visible = visible_[row];
    const auto& node = nodes_[visible.node];
    if (node.children.empty())
        return;
    const bool filtering = !Text(search_).empty() || SendMessageW(filter_, CB_GETCURSEL, 0, 0) > 0;
    if (filtering)
    {
        if (visible.expanded)
            filteredCollapsed_.insert(node.id);
        else
            filteredCollapsed_.erase(node.id);
    }
    else
    {
        if (visible.expanded)
            expanded_.erase(node.id);
        else
            expanded_.insert(node.id);
    }
    BuildVisible();
    FillList();
}
bool Panel::CanSync(const Node& node, bool toUE) const
{
    return connected_ && !busy_ && !sceneChanging_ && !node.scopes.empty() && (toUE ? node.toUE : node.toMax);
}
const Node* Panel::ScopeRow(const std::string& id) const
{
    // The Root preview points to a Max node. Its permissions belong to the whole skeleton.
    const auto it = index_.find(id == "Root" ? "group:skeleton" : id);
    return it == index_.end() ? nullptr : &nodes_[it->second];
}
std::vector<std::string> Panel::ActionScopes(const std::vector<std::string>& candidates, bool toUE) const
{
    std::vector<std::string> result;
    std::unordered_set<std::string> seen;
    for (const auto& id : candidates)
        if (const auto* row = ScopeRow(id); row && CanSync(*row, toUE) && seen.insert(id).second)
            result.push_back(id);
    return result;
}
std::vector<std::string> Panel::Eligible(bool toUE) const
{
    std::vector<std::string> ids;
    for (const auto& node : nodes_)
        if (selected_.contains(node.id))
            ids.insert(ids.end(), node.scopes.begin(), node.scopes.end());
    return ActionScopes(ids, toUE);
}
std::wstring Panel::StateText(const Node& node) const
{
    if (node.Preview() || node.scopes.empty())
        return L"";
    if (!connected_)
        return L"未连接";
    if (node.status == "different")
        return L"有差异";
    if (node.status == "equal")
        return L"一致";
    if (node.status == "maxMissing")
        return L"Max 缺少";
    if (node.status == "ueMissing")
        return L"UE 缺少";
    if (node.status == "unmatched")
        return L"待匹配";
    if (node.status == "offline")
        return L"未连接";
    return L"";
}
std::wstring Panel::AssetName(const Node& node) const
{
    if (node.Preview())
        return {};
    return node.asset.substr(node.asset.find_last_of(L"/\\") + 1);
}
std::wstring Panel::Scope(const Node& node) const
{
    if (node.theme == "skeleton")
        return L"Root 及其全部骨骼";
    if (node.kind == "hull")
        return L"仅预览。整组同步请选中 BodyCollider 或碰撞体。";
    if (node.kind == "physics")
        return L"BodyCollider 下的整组碰撞形状";
    if (node.kind == "category")
        return node.name + L"下的全部同步范围（" + std::to_wstring(node.scopes.size()) + L" 项）";
    if (node.kind == "part")
        return node.name + L" 的全部 LOD（" + std::to_wstring(node.scopes.size()) + L" 项）";
    const auto parent = index_.find(node.parent);
    return parent == index_.end() ? node.name : nodes_[parent->second].name + L" / " + node.name;
}
void Panel::UpdateDetails()
{
    std::wstring text;
    if (selected_.empty())
        text = L"选中节点查看所属 UE 资产与同步范围。\r\n\r\nCtrl 多选，Shift 连续选择；右键批量同步。";
    else if (selected_.size() > 1)
    {
        text = L"已选择 " + std::to_wstring(selected_.size()) + L" 项\r\n\r\n可执行 Max → UE：" +
               std::to_wstring(Eligible(true).size()) + L" 项\r\n可执行 UE → Max：" + std::to_wstring(Eligible(false).size()) +
               L" 项\r\n\r\n只读、一致、未匹配或源端缺少的项会跳过。批量操作在接收端使用一次 Ctrl+Z 撤销。";
    }
    else
    {
        const auto found = index_.find(*selected_.begin());
        if (found != index_.end())
        {
            const auto& node = nodes_[found->second];
            text = node.name + L"\r\n\r\n所属 UE 资产\r\n" +
                   (node.asset.empty() ? L"—" : node.Preview() ? node.asset : AssetName(node) + L"\r\n" + node.asset) +
                   L"\r\n\r\n同步范围\r\n" + Scope(node);
            if (!StateText(node).empty())
                text += L"\r\n\r\n状态：" + StateText(node);
            if (node.kind == "bone")
                text += L"\r\n骨骼作为 Root 整组同步。";
            if (node.status == "unmatched")
                text += L"\r\n\r\n" + (node.error.empty() ? std::wstring(L"无法确认该部件的归属。") : node.error);
            if (!node.Preview() && !node.scopes.empty())
            {
                if (!connected_)
                    text += L"\r\n\r\n未连接 UE，两个方向均不可执行。";
                else if (node.status == "equal")
                    text += L"\r\n\r\n内容一致，无需同步。";
                else if (node.status == "maxMissing")
                    text += L"\r\n\r\nMax → UE 不可用：Max 中没有源节点。";
                else if (node.status == "ueMissing")
                    text += L"\r\n\r\nUE → Max 不可用：UE 中没有源资源。";
            }
        }
    }
    SetWindowTextW(details_, text.c_str());
}
void Panel::Operate(std::wstring label, std::function<void(const WyBridgeTask&)> operation, std::function<void()> onSuccess,
                    ConnectionRequest connection)
{
    if (busy_ || sceneChanging_)
        return;
    struct Outcome
    {
        Json snapshot;
        std::string operationError;
    };
    auto outcome = std::make_shared<Outcome>();
    const bool modifying = static_cast<bool>(operation);
    busy_ = true;
    backgroundConnection_ = connection != ConnectionRequest::None;
    if (backgroundConnection_)
        connected_ = false;
    connecting_ = !connected_;
    UpdateConnectionHeader();
    EnableControls();
    SetStatus(label);
    try
    {
        tasks_.Start(window_, GetCOREInterface()->GetMAXHWnd(), label,
            [this, outcome, operation = std::move(operation), connection](const WyBridgeTask& task)
            {
                if (operation)
                {
                    try { operation(task); }
                    catch (const std::exception& error) { outcome->operationError = error.what(); }
                }
                task.checkpoint();
                WyBridgeClient::ConnectionChanged onConnection;
                if (connection != ConnectionRequest::None)
                    onConnection = [this](bool connected, const std::string& project)
                    {
                        connecting_ = false;
                        connected_ = connected;
                        snapshot_["project"] = project;
                        UpdateConnectionHeader();
                        SetStatus(connected ? L"正在读取资源大纲…" : L"UE 未连接。");
                    };
                outcome->snapshot = client_.Refresh(task, connection == ConnectionRequest::Reconnect, onConnection);
            },
            [this, outcome, label, modifying, connection, onSuccess = std::move(onSuccess)](const std::string& workerError)
            {
                busy_ = connecting_ = backgroundConnection_ = false;
                if (refreshAfterCancel_)
                {
                    refreshAfterCancel_ = false;
                    if (!sceneChanging_ && IsWindowVisible(window_))
                        PostMessageW(window_, RefreshMessage, 0, 0);
                    return;
                }
                std::wstring error = Wide(workerError);
                try
                {
                    if (error.empty())
                    {
                        ParseSnapshot(outcome->snapshot);
                        if (onSuccess && outcome->operationError.empty())
                            onSuccess();
                        PopulateFields();
                        error = Wide(String(snapshot_, "error"));
                    }
                    else
                        empty_ = connected_ = false;
                }
                catch (const std::exception& e)
                {
                    empty_ = connected_ = false;
                    error = Wide(e.what());
                }
                UpdateConnectionHeader();
                EnableControls();
                BuildVisible();
                FillList();
                if (onSuccess && outcome->operationError.empty() && selected_.size() == 1)
                {
                    const int row = VisibleIndex(*selected_.begin());
                    if (row >= 0)
                        ListView_EnsureVisible(list_, row, FALSE);
                }
                UpdateDetails();
                if (!outcome->operationError.empty())
                {
                    const auto failure = Wide(outcome->operationError);
                    SetStatus(L"操作失败：" + failure);
                    MessageBoxW(window_, failure.c_str(), L"操作未完成", MB_OK | MB_ICONERROR);
                }
                else if (!error.empty())
                    SetStatus((connection != ConnectionRequest::None && !connected_ ? L"连接失败：" : L"比对未完成：") + error);
                else if (modifying)
                    SetStatus(label + L" 完成。接收端修改尚未保存，可使用 Ctrl+Z 撤销。");
                else
                    SetStatus(connected_ ? L"比对已更新。" : L"UE 未连接。");
            }, !backgroundConnection_);
    }
    catch (...)
    {
        busy_ = connecting_ = backgroundConnection_ = false;
        UpdateConnectionHeader();
        EnableControls();
        throw;
    }
}
void Panel::Sync(const std::vector<std::string>& ids, bool toUE)
{
    auto eligible = ActionScopes(ids, toUE);
    if (eligible.empty())
        return;
    const bool skeleton = std::find(eligible.begin(), eligible.end(), "Root") != eligible.end();
    const bool physics = std::find(eligible.begin(), eligible.end(), "BodyCollider") != eligible.end();
    if (skeleton || physics)
    {
        std::wstring prompt =
            L"同步 " + std::to_wstring(eligible.size()) + L" 个范围" + (toUE ? L"到 UE？" : L"到 Max？") + L"\r\n\r\n";
        if (skeleton)
            prompt += L"• Root 及其全部骨骼\r\n";
        if (physics)
            prompt += L"• BodyCollider 下的整组碰撞形状\r\n";
        prompt += L"\r\n将覆盖接收端的对应范围。";
        if (MessageBoxW(window_, prompt.c_str(), L"确认同步范围", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
            return;
    }
    Operate(toUE ? L"Max → UE" : L"UE → Max",
            [this, eligible = std::move(eligible), toUE](const WyBridgeTask& task) { client_.Sync(eligible, toUE, task); });
}
void Panel::CreateMissing()
{
    if (!connected_ || busy_ || !empty_)
        return;
    std::vector<std::string> candidates;
    for (const auto& node : nodes_)
        if (node.kind == "category")
            candidates.insert(candidates.end(), node.scopes.begin(), node.scopes.end());
    auto ids = ActionScopes(candidates, true);
    if (ids.empty())
    {
        SetStatus(L"当前场景没有可创建的资源。请检查节点命名与车辆字段。");
        return;
    }
    std::set<std::wstring> assets;
    for (const auto& id : ids)
        if (const auto* row = ScopeRow(id); row && !row->asset.empty())
            assets.insert(row->asset);
    for (const auto& path : snapshot_.value("blueprintPaths", Json::array()))
        assets.insert(Wide(path.get<std::string>()));
    std::wstring prompt = L"首次创建以下 UE 资源？\r\n\r\n";
    for (const auto& asset : assets)
        prompt += asset + L"\r\n";
    if (MessageBoxW(window_, prompt.c_str(), L"首次创建", MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON2) != IDOK)
        return;
    Operate(L"首次创建 UE 资源", [this, ids = std::move(ids)](const WyBridgeTask& task) { client_.Sync(ids, true, task, true); });
}
void Panel::BuildCollision()
{
    if (!colliderWindow_ || !connected_ || busy_ || sceneChanging_)
        return;
    const HWND dialog = colliderWindow_;
    WyColliderBuildSettings options;
    options.split = SendMessageW(mode_, CB_GETCURSEL, 0, 0) == 1;
    double heightCm;
    if (heightEdited_ && ReadColliderHeight(heightCm))
        options.worldZCm = heightCm;
    if (options.split && !options.worldZCm)
    {
        UpdateColliderPreview();
        SetFocus(height_);
        return;
    }
    // Read live Max state now: a manual child may have been added since Refresh.
    const bool existing = WyHasColliderMeshes();
    if (existing && MessageBoxW(dialog,
                                L"是否覆盖 BodyCollider？\r\n\r\n将清空 BodyCollider "
                                L"下的旧网格（包括手工网格），并生成新的碰撞形状。可在 Max 中一次撤销。",
                                L"覆盖 BodyCollider", MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2) != IDYES)
        return;
    // A scene change can close this dialog while the overwrite confirmation is open.
    if (colliderWindow_ != dialog || sceneChanging_)
        return;
    CloseColliderDialog();
    Operate(L"构建 BodyCollider", [this, options](const WyBridgeTask& task) { client_.BuildCollider(options, task); },
            [this]
            {
                if (index_.contains("BodyCollider"))
                {
                    expanded_.insert("group:physics");
                    expanded_.insert("BodyCollider");
                    selected_ = {"BodyCollider"};
                }
            });
}
void Panel::ApplyVehicle()
{
    const auto valid = [](const std::wstring& value)
    {
        return !value.empty() && value != L"." && value != L".." && value.front() != L' ' && value.back() != L' ' &&
               value.find_first_of(L"/\\:*?\"<>|\r\n\t.") == std::wstring::npos;
    };
    const auto type = Text(type_), name = Text(name_), variant = Text(variant_);
    if (!valid(type) || !valid(name) || !valid(variant))
    {
        MessageBoxW(window_, L"请填写有效的车辆类型、名称和变体，不要使用路径分隔符、句点或首尾空格。", L"车辆字段无效",
                    MB_OK | MB_ICONWARNING);
        return;
    }
    Operate(L"应用车辆字段",
            [this, fields = Json{{"type", Utf8(type)}, {"name", Utf8(name)}, {"variant", Utf8(variant)}}](const WyBridgeTask& task)
            { client_.SetVehicle(fields, task); },
            [this]
            {
                metadataDraft_ = false;
                selected_.clear();
            });
}

void Panel::ListKey(WPARAM key)
{
    const bool control = (GetKeyState(VK_CONTROL) & 0x8000) != 0, shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
    if (key == VK_F5)
    {
        Refresh();
        return;
    }
    if (key == 'A' && control)
    {
        selected_.clear();
        for (const auto& row : visible_)
            selected_.insert(SelectionId(row.node));
        UpdateSelection();
        return;
    }
    if (key == VK_APPS || (key == VK_F10 && shift))
    {
        RECT row{};
        POINT point{Scale(24), Scale(24)};
        if (focused_ >= 0 && ListView_GetItemRect(list_, focused_, &row, LVIR_BOUNDS))
            point = {row.left + Scale(40), row.bottom};
        ClientToScreen(list_, &point);
        ContextMenu(point);
        return;
    }
    if (visible_.empty())
        return;
    int row = focused_ >= 0 ? focused_ : 0;
    switch (key)
    {
    case VK_HOME:
        row = 0;
        break;
    case VK_END:
        row = static_cast<int>(visible_.size()) - 1;
        break;
    case VK_UP:
        row = std::max(0, row - 1);
        break;
    case VK_DOWN:
        row = std::min(static_cast<int>(visible_.size()) - 1, row + 1);
        break;
    case VK_PRIOR:
        row = std::max(0, row - std::max(1, ListView_GetCountPerPage(list_)));
        break;
    case VK_NEXT:
        row = std::min(static_cast<int>(visible_.size()) - 1, row + std::max(1, ListView_GetCountPerPage(list_)));
        break;
    case VK_LEFT:
        if (visible_[row].expanded && !nodes_[visible_[row].node].children.empty())
            Toggle(row);
        else
        {
            const int parent = VisibleIndex(nodes_[visible_[row].node].parent);
            if (parent >= 0)
                Select(parent, false, false);
        }
        return;
    case VK_RIGHT:
        if (!visible_[row].expanded && !nodes_[visible_[row].node].children.empty())
            Toggle(row);
        else if (row + 1 < static_cast<int>(visible_.size()) && visible_[row + 1].depth > visible_[row].depth)
            Select(row + 1, false, false);
        return;
    case VK_RETURN:
        Toggle(row);
        return;
    case VK_SPACE:
        Select(row, control, shift);
        return;
    default:
        return;
    }
    if (control && !shift)
    {
        focused_ = row;
        UpdateSelection();
    }
    else
        Select(row, false, shift);
    ListView_EnsureVisible(list_, row, FALSE);
}
void Panel::ContextMenu(POINT point)
{
    if (busy_)
        return;
    HMENU menu = CreatePopupMenu();
    const auto toUE = Eligible(true), toMax = Eligible(false);
    const auto count = L"已选择 " + std::to_wstring(selected_.size()) + L" 项；跳过不可执行项";
    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0, count.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    const auto ueLabel = L"Max → UE   (" + std::to_wstring(toUE.size()) + L" 项)";
    const auto maxLabel = L"UE → Max   (" + std::to_wstring(toMax.size()) + L" 项)";
    AppendMenuW(menu, MF_STRING | (toUE.empty() ? MF_GRAYED : MF_ENABLED), BatchToUE, ueLabel.c_str());
    AppendMenuW(menu, MF_STRING | (toMax.empty() ? MF_GRAYED : MF_ENABLED), BatchToMax, maxLabel.c_str());
    const UINT choice = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window_, nullptr);
    DestroyMenu(menu);
    if (choice == BatchToUE)
        Sync(toUE, true);
    else if (choice == BatchToMax)
        Sync(toMax, false);
}
void Panel::InvokeRowAction(int row, int x)
{
    if (row < 0 || row >= static_cast<int>(visible_.size()))
        return;
    RECT cell{};
    ListView_GetSubItemRect(list_, row, 3, LVIR_BOUNDS, &cell);
    const bool toUE = x < (cell.left + cell.right) / 2;
    Sync(nodes_[visible_[row].node].scopes, toUE);
}
COLORREF Panel::RowColor(size_t row) const
{
    const auto& node = nodes_[visible_[row].node];
    const auto& kind = node.theme;
    COLORREF base = RGB(28, 34, 40), color = RGB(150, 165, 180);
    if (kind == "staticMesh")
    {
        base = RGB(19, 29, 42);
        color = RGB(0, 255, 255);
    }
    if (kind == "skeletalPart")
    {
        base = RGB(32, 21, 30);
        color = RGB(241, 163, 241);
    }
    if (kind == "skeleton")
    {
        base = RGB(24, 30, 34);
        color = RGB(105, 181, 205);
    }
    if (kind == "physics")
    {
        base = RGB(32, 26, 21);
        color = RGB(255, 192, 128);
    }
    int strength = row % 2 ? 24 : 14;
    if (selected_.count(node.id))
        strength += 19;
    else if (node.Preview() && selected_.count(node.owner))
        strength += 8;
    return Mix(base, color, strength);
}
void Panel::DrawName(HDC dc, const RECT& bounds, size_t row) const
{
    const auto& item = visible_[row];
    const auto& node = nodes_[item.node];
    RECT text = bounds;
    text.left += Scale(9 + item.depth * 18);
    text.right = bounds.left + ListView_GetColumnWidth(list_, 0) - Scale(6);
    if (!node.children.empty())
    {
        const int center = (bounds.top + bounds.bottom) / 2, x = text.left + Scale(4), d = Scale(4);
        POINT points[3];
        if (item.expanded)
        {
            points[0] = {x - d, center - d / 2};
            points[1] = {x + d, center - d / 2};
            points[2] = {x, center + d};
        }
        else
        {
            points[0] = {x - d / 2, center - d};
            points[1] = {x - d / 2, center + d};
            points[2] = {x + d, center};
        }
        const auto brush = CreateSolidBrush(RGB(205, 215, 225));
        const auto oldBrush = SelectObject(dc, brush);
        const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
        Polygon(dc, points, 3);
        SelectObject(dc, oldPen);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
    }
    text.left += Scale(17);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, RGB(239, 244, 248));
    DrawTextW(dc, node.name.c_str(), -1, &text, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
}
void Panel::DrawActions(HDC dc, const RECT& bounds, const Node& node) const
{
    if (node.Preview())
        return;
    const int middle = (bounds.left + bounds.right) / 2;
    for (int direction = 0; direction < 2; ++direction)
    {
        RECT button = bounds;
        button.left = direction ? middle + Scale(2) : bounds.left + Scale(3);
        button.right = direction ? bounds.right - Scale(3) : middle - Scale(2);
        button.top += Scale(3);
        button.bottom -= Scale(3);
        const bool toUE = direction == 0;
        const bool enabled = CanSync(node, toUE);
        CellButtonColors colors = toUE ? MaxToUEColors : UEToMaxColors;
        if (!enabled)
            colors.text = Mix(colors.fill, colors.text, 60);
        DrawCellButton(dc, button, toUE ? L"Max → UE" : L"UE → Max", enabled, colors);
    }
}
void Panel::DrawCellButton(HDC dc, const RECT& button, const wchar_t* label, bool enabled, CellButtonColors colors) const
{
    const auto brush = static_cast<HBRUSH>(GetStockObject(DC_BRUSH));
    SetDCBrushColor(dc, colors.fill);
    FillRect(dc, &button, brush);
    SetDCBrushColor(dc, enabled ? RGB(116, 153, 173) : RGB(78, 88, 98));
    FrameRect(dc, &button, brush);
    SetBkMode(dc, TRANSPARENT);
    SetTextColor(dc, colors.text);
    RECT caption = button;
    DrawTextW(dc, label, -1, &caption, DT_CENTER | DT_SINGLELINE | DT_VCENTER | DT_NOPREFIX);
}
RECT Panel::SelectButtonRect(RECT bounds) const
{
    const int inset = std::max(Scale(3), static_cast<int>((bounds.right - bounds.left - Scale(58)) / 2));
    InflateRect(&bounds, -inset, -Scale(3));
    return bounds;
}
void Panel::DrawStateIcon(HDC dc, RECT& text, const Node& node) const
{
    const bool equal = node.status == "equal";
    const bool different = node.status == "different";
    if (!equal && !different && node.status != "ueMissing" && node.status != "maxMissing")
        return;
    const int radius = Scale(6);
    const int x = text.left + radius;
    const int y = (text.top + text.bottom) / 2;
    const COLORREF color = equal ? RGB(107, 229, 156) : different ? RGB(255, 214, 102) : RGB(103, 191, 255);
    const auto pen = CreatePen(PS_SOLID, Scale(2), color);
    const auto oldPen = SelectObject(dc, pen);
    if (equal)
    {
        const POINT mark[] = {{x - radius, y}, {x - Scale(2), y + Scale(4)}, {x + radius, y - Scale(4)}};
        Polyline(dc, mark, 3);
    }
    else if (different)
    {
        const POINT triangle[] = {{x, y - radius}, {x + radius, y + radius}, {x - radius, y + radius}};
        const auto brush = CreateSolidBrush(color);
        const auto oldBrush = SelectObject(dc, brush);
        Polygon(dc, triangle, 3);
        SelectObject(dc, oldBrush);
        DeleteObject(brush);
        const auto ink = CreateSolidBrush(RGB(59, 48, 24));
        RECT stroke{x - Scale(1), y - Scale(2), x + Scale(1), y + Scale(2)};
        RECT dot{x - Scale(1), y + Scale(3), x + Scale(1), y + Scale(5)};
        FillRect(dc, &stroke, ink);
        FillRect(dc, &dot, ink);
        DeleteObject(ink);
    }
    else
    {
        MoveToEx(dc, x - radius, y, nullptr);
        LineTo(dc, x + radius + 1, y);
        MoveToEx(dc, x, y - radius, nullptr);
        LineTo(dc, x, y + radius + 1);
    }
    SelectObject(dc, oldPen);
    DeleteObject(pen);
    text.left += radius * 2 + Scale(6);
}
LRESULT Panel::DrawList(NMLVCUSTOMDRAW* draw)
{
    const DWORD stage = draw->nmcd.dwDrawStage;
    if (stage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
    const size_t row = static_cast<size_t>(draw->nmcd.dwItemSpec);
    if (row >= visible_.size())
        return CDRF_DODEFAULT;
    const auto& node = nodes_[visible_[row].node];
    if (stage == CDDS_ITEMPREPAINT)
    {
        draw->clrTextBk = RowColor(row);
        draw->clrText = RGB(230, 237, 243);
        draw->nmcd.uItemState &= ~(CDIS_SELECTED | CDIS_FOCUS);
        return CDRF_NOTIFYSUBITEMDRAW | CDRF_NOTIFYPOSTPAINT;
    }
    if (stage == (CDDS_ITEMPREPAINT | CDDS_SUBITEM))
    {
        RECT bounds{};
        ListView_GetSubItemRect(list_, static_cast<int>(row), draw->iSubItem, LVIR_BOUNDS, &bounds);
        if (draw->iSubItem == 0)
            bounds.right = bounds.left + ListView_GetColumnWidth(list_, 0);
        const auto brush = CreateSolidBrush(RowColor(row));
        FillRect(draw->nmcd.hdc, &bounds, brush);
        DeleteObject(brush);
        if (draw->iSubItem == 0)
            DrawName(draw->nmcd.hdc, bounds, row);
        else if (draw->iSubItem == 3)
            DrawActions(draw->nmcd.hdc, bounds, node);
        else if (draw->iSubItem == 4)
        {
            if (node.HasSelectButton())
            {
                const bool enabled = !busy_ && !sceneChanging_ && !node.maxNode.is_null();
                const CellButtonColors colors = enabled ? CellButtonColors{RGB(58, 87, 104), RGB(246, 250, 253)}
                                                        : CellButtonColors{RGB(43, 49, 56), RGB(154, 169, 182)};
                DrawCellButton(draw->nmcd.hdc, SelectButtonRect(bounds), L"选中", enabled, colors);
            }
        }
        else
        {
            bounds.left += Scale(7);
            bounds.right -= Scale(5);
            if (draw->iSubItem == 2)
                DrawStateIcon(draw->nmcd.hdc, bounds, node);
            SetBkMode(draw->nmcd.hdc, TRANSPARENT);
            SetTextColor(draw->nmcd.hdc,
                         draw->iSubItem == 2 && node.status == "different" ? RGB(255, 218, 112) : RGB(230, 237, 243));
            const auto value = draw->iSubItem == 1 ? AssetName(node) : StateText(node);
            DrawTextW(draw->nmcd.hdc, value.c_str(), -1, &bounds, DT_SINGLELINE | DT_VCENTER | DT_END_ELLIPSIS | DT_NOPREFIX);
        }
        return CDRF_SKIPDEFAULT;
    }
    if (stage == CDDS_ITEMPOSTPAINT)
    {
        const bool selected = selected_.count(node.id) != 0, grouped = node.Preview() && selected_.count(node.owner);
        if (selected || grouped)
        {
            RECT bounds{};
            ListView_GetItemRect(list_, static_cast<int>(row), &bounds, LVIR_BOUNDS);
            const auto brush = CreateSolidBrush(selected ? RGB(214, 239, 250) : RGB(125, 181, 199));
            if (selected)
                FrameRect(draw->nmcd.hdc, &bounds, brush);
            bounds.right = bounds.left + Scale(selected ? 3 : 2);
            FillRect(draw->nmcd.hdc, &bounds, brush);
            DeleteObject(brush);
        }
    }
    return CDRF_DODEFAULT;
}
LRESULT CALLBACK Panel::ListProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR, DWORD_PTR data)
{
    auto& panel = *reinterpret_cast<Panel*>(data);
    try
    {
        if (message == WM_LBUTTONDOWN || message == WM_LBUTTONDBLCLK || message == WM_RBUTTONDOWN)
        {
            SetFocus(window);
            LVHITTESTINFO hit{};
            hit.pt = {GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            ListView_SubItemHitTest(window, &hit);
            if (hit.iItem >= 0 && hit.iItem < static_cast<int>(panel.visible_.size()))
            {
                const auto& row = panel.visible_[hit.iItem];
                const auto& node = panel.nodes_[row.node];
                if (message == WM_RBUTTONDOWN)
                {
                    if (!panel.selected_.count(panel.SelectionId(row.node)))
                        panel.Select(hit.iItem, false, false);
                    return 0;
                }
                const bool control = (wParam & MK_CONTROL) != 0, shift = (wParam & MK_SHIFT) != 0;
                RECT bounds{};
                ListView_GetItemRect(window, hit.iItem, &bounds, LVIR_BOUNDS);
                const int arrowX = bounds.left + panel.Scale(9 + row.depth * 18);
                const bool onArrow =
                    hit.iSubItem == 0 && hit.pt.x >= arrowX && hit.pt.x < arrowX + panel.Scale(14) && !node.children.empty();
                if (message == WM_LBUTTONDBLCLK)
                {
                    // The first click already selected; a label double-click toggles once.
                    if (hit.iSubItem == 0 && !onArrow)
                        panel.Toggle(hit.iItem);
                    return 0;
                }
                if (onArrow)
                {
                    panel.Toggle(hit.iItem);
                    return 0;
                }
                if (hit.iSubItem == 3 && !node.Preview())
                {
                    panel.InvokeRowAction(hit.iItem, hit.pt.x);
                    return 0;
                }
                if (hit.iSubItem == 4 && node.HasSelectButton())
                {
                    RECT cell{};
                    ListView_GetSubItemRect(window, hit.iItem, 4, LVIR_BOUNDS, &cell);
                    const auto button = panel.SelectButtonRect(cell);
                    if (PtInRect(&button, hit.pt) && !panel.busy_ && !panel.sceneChanging_ && !node.maxNode.is_null())
                    {
                        try { WySelectSceneNode(node.maxNode); }
                        catch (const std::exception& error) { panel.SetStatus(L"选中失败：" + Wide(error.what())); }
                    }
                    return 0;
                }
                panel.Select(hit.iItem, control, shift);
                return 0;
            }
            if (message == WM_LBUTTONDOWN && !(wParam & MK_CONTROL))
            {
                panel.selected_.clear();
                panel.UpdateSelection();
            }
            return 0;
        }
        if (message == WM_CONTEXTMENU)
        {
            POINT point{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
            if (point.x == -1 && point.y == -1)
                panel.ListKey(VK_APPS);
            else
                panel.ContextMenu(point);
            return 0;
        }
        if (message == WM_KEYDOWN || message == WM_SYSKEYDOWN)
        {
            const bool handled = wParam == VK_UP || wParam == VK_DOWN || wParam == VK_LEFT || wParam == VK_RIGHT ||
                                 wParam == VK_HOME || wParam == VK_END || wParam == VK_PRIOR || wParam == VK_NEXT ||
                                 wParam == VK_RETURN || wParam == VK_SPACE || wParam == VK_APPS || wParam == VK_F5 ||
                                 (wParam == VK_F10 && (GetKeyState(VK_SHIFT) & 0x8000)) ||
                                 (wParam == 'A' && (GetKeyState(VK_CONTROL) & 0x8000));
            if (handled)
            {
                panel.ListKey(wParam);
                return 0;
            }
        }
        if (message == WM_CHAR && (wParam == ' ' || wParam == '\r' || wParam == 1))
            return 0;
        if (message == WM_GETDLGCODE)
            return DLGC_WANTARROWS | DLGC_WANTCHARS | (wParam == VK_RETURN || wParam == VK_ESCAPE ? DLGC_WANTMESSAGE : 0);
        if (message == WM_NCDESTROY)
            RemoveWindowSubclass(window, ListProc, ListSubclassId);
    }
    catch (const std::exception& error)
    {
        panel.SetStatus(L"操作失败：" + Wide(error.what()));
    }
    return DefSubclassProc(window, message, wParam, lParam);
}

void Panel::OpenColliderDialog()
{
    if (colliderWindow_)
    {
        SetForegroundWindow(colliderWindow_);
        return;
    }
    if (!connected_ || busy_ || sceneChanging_)
        return;
    RECT parent{};
    GetWindowRect(window_, &parent);
    const HWND dialog = CreateWindowExW(WS_EX_DLGMODALFRAME | WS_EX_CONTROLPARENT, ColliderClass, L"构建 BodyCollider",
                                        WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                                        parent.left + Scale(45), parent.top + Scale(65), Scale(390), Scale(225),
                                        window_, nullptr, Module, this);
    if (!dialog)
        throw std::runtime_error("无法打开构建 BodyCollider 窗口。");
    GetCOREInterface()->RegisterDlgWnd(dialog);
    colliderRegistered_ = true;
    EnableWindow(window_, FALSE);
    ShowWindow(dialog, SW_SHOW);
    SetForegroundWindow(dialog);
    SetFocus(mode_);
}
void Panel::CloseColliderDialog()
{
    if (colliderWindow_)
        DestroyWindow(colliderWindow_);
}
void Panel::LayoutColliderDialog()
{
    if (!colliderWindow_)
        return;
    RECT bounds{};
    GetClientRect(colliderWindow_, &bounds);
    const int width = bounds.right, height = bounds.bottom, pad = Scale(14), inputX = Scale(126);
    const auto move = [&](int id, int x, int y, int w, int h)
    { MoveWindow(GetDlgItem(colliderWindow_, id), x, y, std::max(1, w), h, TRUE); };
    move(ColliderModeLabel, pad, pad + Scale(3), inputX - pad, Scale(24));
    move(ColliderMode, inputX, pad, width - inputX - pad, Scale(135));
    move(ColliderHeightLabel, pad, Scale(56), inputX - pad, Scale(24));
    move(ColliderHeight, inputX, Scale(52), width - inputX - pad, Scale(26));
    move(ColliderDragHint, inputX, Scale(82), width - inputX - pad, Scale(22));
    move(ColliderError, pad, Scale(107), width - pad * 2, Scale(30));
    const int buttonWidth = Scale(76), buttonHeight = Scale(28), buttonY = height - pad - buttonHeight;
    move(ColliderCancel, width - pad - 2 * buttonWidth - Scale(8), buttonY, buttonWidth, buttonHeight);
    move(ColliderGenerate, width - pad - buttonWidth, buttonY, buttonWidth, buttonHeight);
}
bool Panel::ReadColliderHeight(double& heightCm) const
{
    std::wistringstream stream(Text(height_));
    stream.imbue(std::locale::classic());
    if (!(stream >> heightCm) || !std::isfinite(heightCm))
        return false;
    stream >> std::ws;
    return stream.eof();
}
void Panel::UpdateColliderPreview()
{
    if (!colliderError_)
        return;
    const bool split = SendMessageW(mode_, CB_GETCURSEL, 0, 0) == 1;
    for (int id : {ColliderHeightLabel, ColliderHeight, ColliderDragHint})
        ShowWindow(GetDlgItem(colliderWindow_, id), split ? SW_SHOWNA : SW_HIDE);
    double heightCm = 0;
    const bool valid = !split || ReadColliderHeight(heightCm);
    const bool needsHeight = split && !heightEdited_;
    SetWindowTextW(colliderError_, !valid ? L"请输入有效的切面高度（cm）。" :
                                   needsHeight ? L"请设置世界 Z 高度后生成。" : L"");
    EnableWindow(GetDlgItem(colliderWindow_, ColliderGenerate), valid && !needsHeight);
    if (split && valid)
        WyShowColliderPlane(heightCm);
    else
        WyHideColliderPlane();
}
LRESULT CALLBACK Panel::ColliderInputProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data)
{
    auto* panel = reinterpret_cast<Panel*>(data);
    if (message == WM_KEYDOWN && wParam == VK_ESCAPE &&
        !SendMessageW(panel->mode_, CB_GETDROPPEDSTATE, 0, 0))
    {
        panel->CloseColliderDialog();
        return 0;
    }
    if (window == panel->height_)
    {
        switch (message)
        {
        case WM_LBUTTONDOWN:
        {
            // Keep the edit's caret and text entry on a click. Scrubbing starts only after a horizontal drag.
            const auto result = DefSubclassProc(window, message, wParam, lParam);
            panel->heightDragPending_ = panel->ReadColliderHeight(panel->heightDragStartCm_);
            panel->heightDragStartX_ = GET_X_LPARAM(lParam);
            panel->draggingHeight_ = false;
            if (panel->heightDragPending_ && GetCapture() != window)
                SetCapture(window);
            return result;
        }
        case WM_MOUSEMOVE:
            if (panel->heightDragPending_ && (wParam & MK_LBUTTON))
            {
                const int dx = GET_X_LPARAM(lParam) - panel->heightDragStartX_;
                if (!panel->draggingHeight_ && std::abs(dx) < GetSystemMetrics(SM_CXDRAG))
                    break;
                panel->draggingHeight_ = true;
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                const double deltaCm = static_cast<double>(dx) / panel->Scale(4);
                SetWindowTextW(window, Number(panel->heightDragStartCm_ + deltaCm).c_str());
                return 0;
            }
            break;
        case WM_SETCURSOR:
            if (LOWORD(lParam) == HTCLIENT || panel->draggingHeight_)
            {
                SetCursor(LoadCursorW(nullptr, IDC_SIZEWE));
                return TRUE;
            }
            break;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            panel->heightDragPending_ = false;
            panel->draggingHeight_ = false;
            break;
        case WM_KEYDOWN:
            if (wParam == VK_RETURN)
            {
                SendMessageW(panel->colliderWindow_, WM_COMMAND, ColliderGenerate, 0);
                return 0;
            }
            break;
        }
    }
    if (message == WM_NCDESTROY)
        RemoveWindowSubclass(window, ColliderInputProc, id);
    return DefSubclassProc(window, message, wParam, lParam);
}
LRESULT CALLBACK Panel::ColliderProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        panel->colliderWindow_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    if (!panel)
        return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
        {
            const auto settings = WyReadColliderBuildSettings();
            panel->ControlWindow(L"STATIC", L"车身凸包", SS_OWNERDRAW, ColliderModeLabel, window);
            panel->mode_ = panel->ControlWindow(L"COMBOBOX", L"", CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_TABSTOP,
                                                 ColliderMode, window);
            for (const auto* label : ColliderModes)
                SendMessageW(panel->mode_, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label));
            SendMessageW(panel->mode_, CB_SETCURSEL, settings.split ? 1 : 0, 0);
            panel->ControlWindow(L"STATIC", L"世界 Z（cm）", SS_OWNERDRAW, ColliderHeightLabel, window);
            const std::wstring height = settings.worldZCm ? Wide(Json(*settings.worldZCm).dump()) : L"85";
            panel->height_ = panel->ControlWindow(L"EDIT", height.c_str(), ES_AUTOHSCROLL | WS_TABSTOP,
                                                  ColliderHeight, window, WS_EX_CLIENTEDGE);
            panel->ControlWindow(L"STATIC", L"输入数值或按住数值左右拖动", SS_OWNERDRAW, ColliderDragHint, window);
            panel->colliderError_ = panel->ControlWindow(L"STATIC", L"", SS_OWNERDRAW, ColliderError, window);
            panel->ControlWindow(L"BUTTON", L"取消", BS_OWNERDRAW | WS_TABSTOP, ColliderCancel, window);
            panel->ControlWindow(L"BUTTON", L"生成", BS_OWNERDRAW | WS_TABSTOP, ColliderGenerate, window);
            EnumChildWindows(window, [](HWND child, LPARAM data) -> BOOL {
                SetWindowSubclass(child, ColliderInputProc, ListSubclassId, static_cast<DWORD_PTR>(data));
                return TRUE;
            }, reinterpret_cast<LPARAM>(panel));
            panel->heightEdited_ = settings.worldZCm.has_value();
            panel->UpdateColliderPreview();
            panel->LayoutColliderDialog();
            return 0;
        }
        case WM_MEASUREITEM:
            if (auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam); item && item->CtlType == ODT_COMBOBOX && item->CtlID == ColliderMode)
            {
                item->itemHeight = panel->Scale(21);
                return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (lParam && panel->DrawFormControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)))
                return TRUE;
            break;
        case WM_COMMAND:
        {
            const int id = LOWORD(wParam), event = HIWORD(wParam);
            if (panel->HandleTextFocus(wParam, lParam))
                return 0;
            if ((id == ColliderGenerate || id == IDOK) && event == BN_CLICKED)
            {
                if (IsWindowEnabled(GetDlgItem(window, ColliderGenerate)))
                    panel->BuildCollision();
                return 0;
            }
            if (id == ColliderCancel || id == IDCANCEL)
            {
                panel->CloseColliderDialog();
                return 0;
            }
            if ((id == ColliderMode && event == CBN_SELCHANGE) || (id == ColliderHeight && event == EN_CHANGE))
            {
                if (id == ColliderHeight)
                    panel->heightEdited_ = true;
                panel->UpdateColliderPreview();
                return 0;
            }
            break;
        }
        case WM_KEYDOWN:
            if (wParam == VK_ESCAPE)
            {
                panel->CloseColliderDialog();
                return 0;
            }
            break;
        case WM_CLOSE:
            panel->CloseColliderDialog();
            return 0;
        case WM_DESTROY:
            WyHideColliderPlane();
            if (panel->acceleratorOwner_ == panel->height_)
                panel->ReleaseTextFocus();
            if (panel->colliderRegistered_)
            {
                GetCOREInterface()->UnRegisterDlgWnd(window);
                panel->colliderRegistered_ = false;
                EnableWindow(panel->window_, TRUE);
            }
            panel->colliderWindow_ = panel->mode_ = panel->height_ = panel->colliderError_ = nullptr;
            panel->heightEdited_ = panel->draggingHeight_ = panel->heightDragPending_ = false;
            return 0;
        case WM_SIZE:
            panel->LayoutColliderDialog();
            return 0;
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), GetDlgCtrlID(reinterpret_cast<HWND>(lParam)) == ColliderError
                                                            ? RGB(255, 190, 122)
                                                            : RGB(235, 241, 246));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(35, 42, 49));
            return reinterpret_cast<LRESULT>(panel->background_);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(235, 241, 246));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(28, 34, 40));
            return reinterpret_cast<LRESULT>(panel->editBackground_);
        case WM_ERASEBKGND:
        {
            RECT r{};
            GetClientRect(window, &r);
            FillRect(reinterpret_cast<HDC>(wParam), &r, panel->background_);
            return 1;
        }
        }
    }
    catch (const std::exception& error)
    {
        WyHideColliderPlane();
        if (message == WM_CREATE)
            return -1;
        if (panel->colliderError_)
        {
            SetWindowTextW(panel->colliderError_, Wide(error.what()).c_str());
            EnableWindow(GetDlgItem(window, ColliderGenerate), FALSE);
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}
LRESULT CALLBACK Panel::WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam)
{
    auto* panel = reinterpret_cast<Panel*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE)
    {
        panel = static_cast<Panel*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);
        panel->window_ = window;
        SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(panel));
    }
    if (!panel)
        return DefWindowProcW(window, message, wParam, lParam);
    try
    {
        switch (message)
        {
        case WM_CREATE:
            panel->CreateControls();
            return 0;
        case WM_MEASUREITEM:
            if (auto* item = reinterpret_cast<MEASUREITEMSTRUCT*>(lParam); item && item->CtlType == ODT_COMBOBOX && item->CtlID == Filter)
            {
                item->itemHeight = panel->Scale(21);
                return TRUE;
            }
            break;
        case WM_DRAWITEM:
            if (lParam && panel->DrawFormControl(*reinterpret_cast<const DRAWITEMSTRUCT*>(lParam)))
                return TRUE;
            break;
        case RefreshMessage:
            if (IsWindowVisible(window))
                panel->Operate(L"正在连接", {}, {}, ConnectionRequest::Check);
            return 0;
        case WM_SIZE:
            panel->Layout();
            return 0;
        case WM_GETMINMAXINFO:
            reinterpret_cast<MINMAXINFO*>(lParam)->ptMinTrackSize = {panel->Scale(640), panel->Scale(550)};
            return 0;
        case WM_DPICHANGED:
        {
            panel->dpi_ = HIWORD(wParam);
            panel->CreateFonts();
            const RECT* next = reinterpret_cast<RECT*>(lParam);
            SetWindowPos(window, nullptr, next->left, next->top, next->right - next->left, next->bottom - next->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            panel->Layout();
            return 0;
        }
        case WM_CLOSE:
            if (!panel->busy_ || panel->backgroundConnection_)
            {
                panel->CloseColliderDialog();
                panel->ReleaseTextFocus();
                ShowWindow(window, SW_HIDE);
                if (panel->backgroundConnection_)
                {
                    panel->refreshAfterCancel_ = true;
                    panel->tasks_.Cancel();
                }
            }
            return 0;
        case WM_DESTROY:
            panel->tasks_.Stop();
            panel->CloseColliderDialog();
            panel->ReleaseTextFocus();
            GetCOREInterface()->UnRegisterDlgWnd(window);
            if (panel->actionTooltip_)
            {
                DestroyWindow(panel->actionTooltip_);
                panel->actionTooltip_ = nullptr;
            }
            if (panel->list_)
                ListView_SetImageList(panel->list_, nullptr, LVSIL_SMALL);
            if (const auto menu = GetMenu(window))
            {
                SetMenu(window, nullptr);
                DestroyMenu(menu);
            }
            panel->window_ = nullptr;
            return 0;
        case WM_COMMAND:
        {
            if (panel->HandleTextFocus(wParam, lParam))
                return 0;
            const int id = LOWORD(wParam), event = HIWORD(wParam);
            if (id == Reconnect && event == BN_CLICKED)
            {
                panel->Operate(L"正在连接", {}, {}, ConnectionRequest::Reconnect);
                return 0;
            }
            if (id == RefreshButton)
            {
                panel->Refresh();
                return 0;
            }
            if (id == ApplyFields)
            {
                panel->ApplyVehicle();
                return 0;
            }
            if (id == CreateAll)
            {
                panel->CreateMissing();
                return 0;
            }
            if (id == BuildCollider)
            {
                panel->OpenColliderDialog();
                return 0;
            }
            if (!panel->loading_ && (((id == VehicleName || id == VehicleVariant) && event == EN_CHANGE) ||
                                     (id == VehicleType && (event == CBN_EDITCHANGE || event == CBN_SELCHANGE))))
            {
                if (id == VehicleType && event == CBN_SELCHANGE)
                {
                    const auto selected = SendMessageW(panel->type_, CB_GETCURSEL, 0, 0);
                    if (selected >= 0)
                    {
                        const auto length = SendMessageW(panel->type_, CB_GETLBTEXTLEN, selected, 0);
                        if (length >= 0)
                        {
                            std::wstring text(static_cast<size_t>(length) + 1, L'\0');
                            SendMessageW(panel->type_, CB_GETLBTEXT, selected, reinterpret_cast<LPARAM>(text.data()));
                            SetWindowTextW(panel->type_, text.c_str());
                        }
                    }
                }
                panel->metadataDraft_ = true;
                panel->UpdatePreview();
                return 0;
            }
            if ((id == Search && event == EN_CHANGE) || (id == Filter && event == CBN_SELCHANGE))
            {
                panel->filteredCollapsed_.clear();
                panel->BuildVisible();
                panel->FillList();
                return 0;
            }
            break;
        }
        case WM_NOTIFY:
        {
            const auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->hwndFrom == panel->actionTooltip_ && header->code == TTN_GETDISPINFOW)
            {
                auto* tip = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                if (header->idFrom == reinterpret_cast<UINT_PTR>(panel->projectPath_))
                {
                    panel->tooltipText_ = Text(panel->projectPath_);
                    tip->lpszText = panel->tooltipText_.data();
                    return 0;
                }
                POINT point{};
                GetCursorPos(&point);
                ScreenToClient(panel->list_, &point);
                LVHITTESTINFO hit{};
                hit.pt = point;
                ListView_SubItemHitTest(panel->list_, &hit);
                panel->tooltipText_.clear();
                if (hit.iItem >= 0 && hit.iItem < static_cast<int>(panel->visible_.size()))
                {
                    const auto& node = panel->nodes_[panel->visible_[hit.iItem].node];
                    if (hit.iSubItem == 3)
                    {
                        RECT cell{};
                        ListView_GetSubItemRect(panel->list_, hit.iItem, 3, LVIR_BOUNDS, &cell);
                        const bool toUE = point.x < (cell.left + cell.right) / 2;
                        if (node.kind == "bone")
                            panel->tooltipText_ = L"骨骼作为 Root 整组同步。";
                        else if (node.kind == "hull")
                            panel->tooltipText_ = panel->Scope(node);
                        else
                        {
                            panel->tooltipText_ = toUE ? L"Max → UE" : L"UE → Max";
                            if (!panel->connected_)
                                panel->tooltipText_ += L"：未连接 UE。";
                            else if (panel->busy_)
                                panel->tooltipText_ += L"：正在执行操作。";
                            else if (node.scopes.empty())
                                panel->tooltipText_ += L"：当前分类内没有资源。";
                            else if (node.status == "equal")
                                panel->tooltipText_ += L"：内容一致，无需同步。";
                            else if (node.status == "unmatched")
                                panel->tooltipText_ += L"：没有可靠映射，不能覆盖。";
                            else if ((toUE && node.status == "maxMissing") || (!toUE && node.status == "ueMissing"))
                                panel->tooltipText_ += L"：源端不存在。";
                            else if (!panel->CanSync(node, toUE))
                                panel->tooltipText_ += L"：没有可执行的同步范围。";
                            else
                                panel->tooltipText_ += L"：" + panel->Scope(node);
                        }
                    }
                    else if (hit.iSubItem == 4 && node.HasSelectButton())
                    {
                        panel->tooltipText_ = node.maxNode.is_null() ? L"当前 Max 场景没有对应的节点。" :
                                              L"在 Max 大纲中选中 " + Wide(String(node.maxNode, "name"));
                    }
                    else
                        panel->tooltipText_ = node.name + L"\r\n" + node.asset + L"\r\n" + panel->Scope(node);
                }
                tip->lpszText = panel->tooltipText_.data();
                return 0;
            }
            if (header->hwndFrom == panel->list_ && header->code == NM_CUSTOMDRAW)
                return panel->DrawList(reinterpret_cast<NMLVCUSTOMDRAW*>(lParam));
            if (header->hwndFrom == panel->list_ && header->code == LVN_GETINFOTIPW)
            {
                auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                if (tip->iItem >= 0 && tip->iItem < static_cast<int>(panel->visible_.size()))
                {
                    const auto& node = panel->nodes_[panel->visible_[tip->iItem].node];
                    const auto text = node.name + L"\r\n" + node.asset + L"\r\n" + panel->Scope(node);
                    if (tip->pszText && tip->cchTextMax > 0)
                        wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax), text.c_str(), _TRUNCATE);
                }
                return 0;
            }
            break;
        }
        case WM_CTLCOLORSTATIC:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(230, 237, 243));
            if (reinterpret_cast<HWND>(lParam) == panel->details_)
            {
                SetBkColor(reinterpret_cast<HDC>(wParam), RGB(28, 34, 40));
                return reinterpret_cast<LRESULT>(panel->editBackground_);
            }
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(35, 42, 49));
            return reinterpret_cast<LRESULT>(panel->background_);
        case WM_CTLCOLOREDIT:
        case WM_CTLCOLORLISTBOX:
            SetTextColor(reinterpret_cast<HDC>(wParam), RGB(230, 237, 243));
            SetBkColor(reinterpret_cast<HDC>(wParam), RGB(28, 34, 40));
            return reinterpret_cast<LRESULT>(panel->editBackground_);
        case WM_ERASEBKGND:
        {
            RECT bounds{};
            GetClientRect(window, &bounds);
            FillRect(reinterpret_cast<HDC>(wParam), &bounds, panel->background_);
            return 1;
        }
        }
    }
    catch (const std::exception& error)
    {
        panel->EnableControls();
        panel->SetStatus(L"操作失败：" + Wide(error.what()));
        if (message == WM_CREATE)
            return -1;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

class ShowPanelAction final : public ActionItem
{
  public:
    int GetId() override
    {
        return ShowActionId;
    }
    BOOL ExecuteAction() override
    {
        Show();
        return TRUE;
    }
    void GetButtonText(MSTR& text) override
    {
        text = _T("载具资源桥接");
    }
    void GetMenuText(MSTR& text) override
    {
        GetButtonText(text);
    }
    void GetDescriptionText(MSTR& text) override
    {
        text = _T("打开望月载具工具原生面板");
    }
    void GetCategoryText(MSTR& text) override
    {
        text = _T("望月载具工具");
    }
    BOOL IsChecked() override
    {
        return FALSE;
    }
    BOOL IsItemVisible() override
    {
        return TRUE;
    }
    BOOL IsEnabled() override
    {
        return TRUE;
    }
    void DeleteThis() override
    {
        delete this;
    }
};
void AddMenu(MaxSDK::CUI::ICuiMenuManager* manager)
{
    if (!manager || manager->GetMenuById(MenuIdentifier))
        return;
    if (auto* main = manager->GetMainMenuBar())
    {
        auto* submenu = main->CreateSubMenu(MenuIdentifier, _T("望月载具工具"));
        if (submenu)
            submenu->CreateAction(MenuItemIdentifier, ActionTableIdentifier, ShowActionId, _T("载具资源桥接"));
    }
}
void MenuNotification(void*, NotifyInfo* info)
{
    try
    {
        AddMenu(GetNotifyParam<NOTIFY_CUI_REGISTER_MENUS>(info));
    }
    catch (...)
    {
        OutputDebugStringW(L"[WyVehicleAssetBridge] Native menu registration failed.\n");
    }
}
constexpr int SceneNotifications[] = {
    NOTIFY_FILE_PRE_OPEN, NOTIFY_SYSTEM_PRE_RESET, NOTIFY_SYSTEM_PRE_NEW,
    NOTIFY_FILE_OPEN_FAILED, NOTIFY_FILE_POST_OPEN, NOTIFY_SYSTEM_POST_RESET, NOTIFY_SYSTEM_POST_NEW
};
void SceneNotification(void*, NotifyInfo* info)
{
    if (!MainPanel || !info)
        return;
    if (info->intcode == NOTIFY_FILE_PRE_OPEN || info->intcode == NOTIFY_SYSTEM_PRE_RESET || info->intcode == NOTIFY_SYSTEM_PRE_NEW)
        MainPanel->SceneChanging();
    else
        MainPanel->SceneChanged(info->intcode != NOTIFY_FILE_OPEN_FAILED);
}
} // namespace

void Initialize(HINSTANCE module)
{
    if (Initialized)
        return;
    Module = module;
    WyMaxTaskRunner::Register(module);
    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.style = CS_DBLCLKS;
    wc.hInstance = Module;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpfnWndProc = Panel::WindowProc;
    wc.lpszClassName = PanelClass;
    RegisterClassExW(&wc);
    wc.lpfnWndProc = Panel::ColliderProc;
    wc.lpszClassName = ColliderClass;
    RegisterClassExW(&wc);
    auto* actions = GetCOREInterface()->GetActionManager();
    RegisteredActions = actions->FindTable(ActionTableIdentifier);
    if (!RegisteredActions)
    {
        RegisteredActions = new ActionTable(ActionTableIdentifier, kActionMainUIContext, MSTR(_T("望月载具工具")));
        actions->RegisterActionTable(RegisteredActions);
    }
    RegisteredAction = new ShowPanelAction();
    RegisteredActions->AppendOperation(RegisteredAction);
    actions->ActivateActionTable(&RegisteredCallback, ActionTableIdentifier);
    RegisterNotification(MenuNotification, nullptr, NOTIFY_CUI_REGISTER_MENUS);
    for (auto code : SceneNotifications)
        RegisterNotification(SceneNotification, nullptr, code);
    Initialized = true;
    // Startup registration is driven by the notification; late loading can use the active manager.
    try
    {
        AddMenu(GetCOREInterface()->GetICuiMenuManager());
    }
    catch (...)
    {
    }
}
void Shutdown()
{
    if (!Initialized)
        return;
    UnRegisterNotification(MenuNotification, nullptr, NOTIFY_CUI_REGISTER_MENUS);
    for (auto code : SceneNotifications)
        UnRegisterNotification(SceneNotification, nullptr, code);
    MainPanel.reset();
    try
    {
        if (auto* manager = GetCOREInterface()->GetICuiMenuManager())
            if (auto* menu = manager->GetMainMenuBar())
                menu->DeleteItem(MenuIdentifier);
    }
    catch (...)
    {
    }
    auto* actions = GetCOREInterface()->GetActionManager();
    actions->DeactivateActionTable(&RegisteredCallback, ActionTableIdentifier);
    if (RegisteredActions && RegisteredAction)
        RegisteredActions->DeleteOperation(RegisteredAction);
    RegisteredAction = nullptr;
    RegisteredActions = nullptr;
    UnregisterClassW(PanelClass, Module);
    UnregisterClassW(ColliderClass, Module);
    WyMaxTaskRunner::Unregister(Module);
    Initialized = false;
}
void Show()
{
    try
    {
        if (!MainPanel)
            MainPanel = std::make_unique<Panel>();
        MainPanel->Show();
    }
    catch (const std::exception& error)
    {
        MessageBoxW(GetCOREInterface()->GetMAXHWnd(), Wide(error.what()).c_str(), L"望月载具工具", MB_OK | MB_ICONERROR);
    }
}
} // namespace WyVehicleAssetBridgeUI
