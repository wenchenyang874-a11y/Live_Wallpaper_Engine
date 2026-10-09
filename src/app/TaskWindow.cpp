#include "app/TaskWindow.h"
#include "core/OperationProgress.h"
#include "core/Logger.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <dwmapi.h>
#include <commctrl.h>
#include <objbase.h>

namespace lwe::app {
namespace {
constexpr COLORREF background = RGB(16,19,26), panel = RGB(29,34,45);
struct State {
    HWND window{}, detail{}, button{};
    HFONT font{};
    HBRUSH brush{};
    std::jthread worker;
    std::mutex mutex;
    std::wstring status = L"正在准备…", result, shown;
    int percent = -1, painted = -2;
    bool hovered = false;
    bool succeeded = false;
    bool done = false, displayedDone = false, closed = false, closeWhenDone = false;
};
LRESULT CALLBACK ButtonProc(HWND w, UINT m, WPARAM a, LPARAM b, UINT_PTR id, DWORD_PTR data) {
    auto* state = reinterpret_cast<State*>(data);
    if(m==WM_ERASEBKGND) return 1;
    if(m==WM_MOUSEMOVE && !state->hovered) {
        state->hovered=true; TRACKMOUSEEVENT tracking{sizeof(tracking),TME_LEAVE,w,0};
        TrackMouseEvent(&tracking); InvalidateRect(w,nullptr,FALSE);
    } else if(m==WM_MOUSELEAVE && state->hovered) {state->hovered=false;InvalidateRect(w,nullptr,FALSE);}
    if(m==WM_NCDESTROY) RemoveWindowSubclass(w,ButtonProc,id);
    return DefSubclassProc(w,m,a,b);
}
int Scale(HWND w, int n) { return MulDiv(n, GetDpiForWindow(w), 96); }
void Layout(State& s) {
    RECT r{}; GetClientRect(s.window,&r);
    const int m=Scale(s.window,24), b=Scale(s.window,38);
    MoveWindow(s.detail,m,Scale(s.window,68),r.right-2*m,r.bottom-Scale(s.window,138),TRUE);
    MoveWindow(s.button,r.right-m-Scale(s.window,124),r.bottom-m-b,Scale(s.window,124),b,TRUE);
}
void Cancel(State& s) {
    if (s.displayedDone) { DestroyWindow(s.window); return; }
    s.worker.request_stop();
    SetWindowTextW(s.button,L"正在取消…"); EnableWindow(s.button,FALSE);
}
LRESULT CALLBACK Proc(HWND w, UINT m, WPARAM a, LPARAM b) {
    auto* s=reinterpret_cast<State*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE) {
        s=static_cast<State*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);
        s->window=w; SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));
    }
    if(!s) return DefWindowProcW(w,m,a,b);
    switch(m) {
    case WM_CREATE:
        s->brush=CreateSolidBrush(panel);
        s->font=CreateFontW(-Scale(w,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        s->detail=CreateWindowExW(0,L"EDIT",L"",WS_CHILD|WS_VISIBLE|WS_VSCROLL|ES_MULTILINE|ES_READONLY|ES_AUTOVSCROLL,
            0,0,1,1,w,reinterpret_cast<HMENU>(5101),nullptr,nullptr);
        s->button=CreateWindowExW(0,L"BUTTON",L"取消任务",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,
            0,0,1,1,w,reinterpret_cast<HMENU>(5102),nullptr,nullptr);
        if (!s->detail || !s->button) return -1;
        SetWindowSubclass(s->button,ButtonProc,1,reinterpret_cast<DWORD_PTR>(s));
        SendMessageW(s->detail,EM_SETLIMITTEXT,4*1024*1024,0);
        SendMessageW(s->detail,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),0);
        SendMessageW(s->button,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),0);
        Layout(*s); SetTimer(w,1,100,nullptr); return 0;
    case WM_SIZE: Layout(*s); return 0;
    case WM_DPICHANGED: {
        HFONT previous=s->font;
        s->font=CreateFontW(-Scale(w,14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
            OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
        SendMessageW(s->detail,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),TRUE);
        SendMessageW(s->button,WM_SETFONT,reinterpret_cast<WPARAM>(s->font),TRUE);
        DeleteObject(previous);
        const auto& r=*reinterpret_cast<RECT*>(b);
        SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
        Layout(*s); InvalidateRect(w,nullptr,FALSE); return 0;
    }
    case WM_TIMER: {
        std::wstring text; int percent; bool done;
        { std::scoped_lock lock(s->mutex); done=s->done; percent=s->percent; text=done?s->result:s->status; }
        if(text!=s->shown) { s->shown=text; SetWindowTextW(s->detail,text.c_str()); }
        if(percent!=s->painted || done!=s->displayedDone) {
            s->painted=percent; RECT area{0,0,10000,Scale(w,66)}; InvalidateRect(w,&area,FALSE);
        }
        if(done && !s->displayedDone) {
            s->displayedDone=true; SetWindowTextW(s->button,L"关闭"); EnableWindow(s->button,TRUE);
        }
        if(done && s->closeWhenDone) DestroyWindow(w);
        return 0;
    }
    case WM_COMMAND: if(LOWORD(a)==5102) {Cancel(*s); return 0;} break;
    case WM_CLOSE: Cancel(*s); return 0;
    case WM_CTLCOLORSTATIC: case WM_CTLCOLOREDIT:
        SetTextColor(reinterpret_cast<HDC>(a),RGB(220,229,242));
        SetBkColor(reinterpret_cast<HDC>(a),panel); return reinterpret_cast<LRESULT>(s->brush);
    case WM_DRAWITEM: {
        const auto& d=*reinterpret_cast<DRAWITEMSTRUCT*>(b);
        HDC dc=CreateCompatibleDC(d.hDC);
        HBITMAP bitmap=CreateCompatibleBitmap(d.hDC,d.rcItem.right,d.rcItem.bottom);
        if(!dc || !bitmap){if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);return TRUE;}
        auto previousBitmap=SelectObject(dc,bitmap);
        HBRUSH base=CreateSolidBrush(background); FillRect(dc,&d.rcItem,base); DeleteObject(base);
        HBRUSH fill=CreateSolidBrush((d.itemState&ODS_SELECTED)?RGB(67,84,191):s->hovered?RGB(111,139,255):RGB(88,113,241));
        auto old=SelectObject(dc,fill); auto pen=SelectObject(dc,GetStockObject(NULL_PEN));
        RoundRect(dc,1,1,d.rcItem.right-1,d.rcItem.bottom-1,Scale(w,8),Scale(w,8));
        SelectObject(dc,pen); SelectObject(dc,old); DeleteObject(fill);
        wchar_t label[64]{}; GetWindowTextW(d.hwndItem,label,64); SetBkMode(dc,TRANSPARENT);
        SetTextColor(dc,RGB(245,247,255)); auto font=SelectObject(dc,s->font);
        RECT r=d.rcItem; DrawTextW(dc,label,-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);
        SelectObject(dc,font);
        BitBlt(d.hDC,0,0,d.rcItem.right,d.rcItem.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,previousBitmap);DeleteObject(bitmap);DeleteDC(dc);return TRUE;
    }
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT p{}; HDC dc=BeginPaint(w,&p); RECT r{}; GetClientRect(w,&r);
        HBRUSH fill=CreateSolidBrush(background); FillRect(dc,&r,fill); DeleteObject(fill);
        SetBkMode(dc,TRANSPARENT); SetTextColor(dc,RGB(232,237,247)); auto f=SelectObject(dc,s->font);
        RECT title{Scale(w,24),Scale(w,16),r.right-Scale(w,24),Scale(w,44)};
        std::wstring label=s->displayedDone?L"任务结果":L"正在处理 · 桌面播放不受影响";
        if(!s->displayedDone && s->painted>=0) label+=L" · 当前步骤 "+std::to_wstring(s->painted)+L"%";
        DrawTextW(dc,label.c_str(),-1,&title,DT_SINGLELINE|DT_VCENTER);
        RECT bar{Scale(w,24),Scale(w,53),r.right-Scale(w,24),Scale(w,57)};
        FillRect(dc,&bar,s->brush);
        if(s->painted>=0) {bar.right=bar.left+MulDiv(bar.right-bar.left,s->painted,100);
            HBRUSH accent=CreateSolidBrush(RGB(88,113,241)); FillRect(dc,&bar,accent); DeleteObject(accent);}
        SelectObject(dc,f); EndPaint(w,&p); return 0;
    }
    case WM_DESTROY:s->closed=true; KillTimer(w,1); return 0;
    }
    return DefWindowProcW(w,m,a,b);
}
}
bool RunTaskWindow(HWND owner, const std::wstring& title,
                   const std::function<std::wstring(std::stop_token)>& work, bool closeWhenDone) {
    State s;
    s.closeWhenDone=closeWhenDone;
    WNDCLASSW cls{}; cls.lpfnWndProc=Proc; cls.hInstance=GetModuleHandleW(nullptr);
    cls.hCursor=LoadCursorW(nullptr,IDC_ARROW); cls.lpszClassName=L"LiveWallpaperEngine.Task";
    RegisterClassW(&cls);
    RECT bounds{}; GetWindowRect(owner,&bounds);
    const int width=Scale(owner,660),height=Scale(owner,410);
    HWND w=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,title.c_str(),
        WS_CAPTION|WS_SYSMENU|WS_POPUP,bounds.left+(bounds.right-bounds.left-width)/2,
        bounds.top+(bounds.bottom-bounds.top-height)/2,width,height,owner,nullptr,cls.hInstance,&s);
    if(!w) {DeleteObject(s.font); DeleteObject(s.brush); MessageBoxW(owner,L"无法创建任务窗口。",title.c_str(),MB_OK|MB_ICONERROR); return false;}
    BOOL dark=TRUE; DwmSetWindowAttribute(w,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    // Mirror the existing settings dialog: avoid WM_ENABLE repainting the owner.
    const LONG_PTR ownerStyle=GetWindowLongPtrW(owner,GWL_STYLE);
    SetWindowLongPtrW(owner,GWL_STYLE,ownerStyle|WS_DISABLED);
    s.worker=std::jthread([&](std::stop_token stop) {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        const HRESULT com=CoInitializeEx(nullptr,COINIT_APARTMENTTHREADED);
        core::OperationProgress progress{stop,[&](int percent,std::wstring_view text) {
            std::scoped_lock lock(s.mutex); s.percent=percent; if(!text.empty()) s.status=text;
        }};
        core::OperationScope scope(progress);
        std::wstring result;
        try {if (FAILED(com)) result=L"无法初始化任务线程。"; else {result=work(stop); s.succeeded=true;}}
        catch(...) {core::LogError(L"Background library task threw an exception.", E_UNEXPECTED);result=L"任务异常终止，请查看本地日志；已完成的导入保留。";}
        if(SUCCEEDED(com)) CoUninitialize();
        { std::scoped_lock lock(s.mutex); s.result=std::move(result); s.done=true; s.percent=100; }
    });
    ShowWindow(w,SW_SHOWNORMAL);
    MSG msg{}; bool quit=false;
    while(!s.closed) {
        const BOOL got=GetMessageW(&msg,nullptr,0,0);
        if(got<=0) {quit=got==0; s.worker.request_stop(); break;}
        if(msg.message==WM_KEYDOWN && msg.wParam==VK_ESCAPE &&
            (msg.hwnd==w || IsChild(w,msg.hwnd))) {Cancel(s); continue;}
        if(!IsDialogMessageW(w,&msg)) {TranslateMessage(&msg); DispatchMessageW(&msg);}
    }
    s.worker.request_stop(); s.worker.join();
    if(IsWindow(w)) DestroyWindow(w);
    DeleteObject(s.font); DeleteObject(s.brush);
    if(IsWindow(owner)) {SetWindowLongPtrW(owner,GWL_STYLE,ownerStyle); SetForegroundWindow(owner);}
    if(quit) PostQuitMessage(static_cast<int>(msg.wParam));
    return s.succeeded;
}
void ShowDetailsWindow(HWND owner,const std::wstring& title,const std::wstring& details) {
    RunTaskWindow(owner,title,[details](std::stop_token){return details;});
}
}
