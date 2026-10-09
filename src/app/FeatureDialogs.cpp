#include "app/FeatureDialogs.h"
#include "app/TaskWindow.h"
#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <commctrl.h>
#include <dwmapi.h>
#include <shlobj.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <wrl/client.h>
#include <gdiplus.h>

namespace lwe::app {
namespace {
constexpr COLORREF bg=RGB(17,20,27), panel=RGB(28,33,44), text=RGB(235,240,250);
int S(HWND w,int n) {return MulDiv(n,GetDpiForWindow(w),96);}
void Fill(HDC dc,RECT r,COLORREF c) {auto b=CreateSolidBrush(c);FillRect(dc,&r,b);DeleteObject(b);}
HFONT Font(HWND w,int size,int weight=FW_NORMAL) {
    return CreateFontW(-S(w,size),0,0,0,weight,FALSE,FALSE,FALSE,DEFAULT_CHARSET,
        OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Segoe UI");
}
struct OptionsState {
    WallpaperOptionsResult result;
    bool audio=false,accepted=false,closed=false;
    HWND window{};
    HFONT font{}; HBRUSH brush{};
    std::array<HWND,8> buttons{};
    std::array<HWND,3> sliders{};
    std::wstring name;
};
LRESULT CALLBACK Hover(HWND w,UINT m,WPARAM a,LPARAM b,UINT_PTR id,DWORD_PTR) {
    if(m==WM_MOUSEMOVE && !GetPropW(w,L"LweHover")) {SetPropW(w,L"LweHover",reinterpret_cast<HANDLE>(1));TRACKMOUSEEVENT t{sizeof(t),TME_LEAVE,w,0};TrackMouseEvent(&t);InvalidateRect(w,nullptr,FALSE);}
    if(m==WM_MOUSELEAVE) {RemovePropW(w,L"LweHover");InvalidateRect(w,nullptr,FALSE);}
    if(m==WM_NCDESTROY) {RemovePropW(w,L"LweHover");RemoveWindowSubclass(w,Hover,id);}
    if(m==WM_ERASEBKGND) return 1;
    return DefSubclassProc(w,m,a,b);
}
void LayoutOptions(OptionsState& s) {
    for(int i=0;i<4;++i) MoveWindow(s.buttons[i],S(s.window,24+i*126),S(s.window,80),S(s.window,116),S(s.window,38),TRUE);
    for(int i=0;i<3;++i) MoveWindow(s.sliders[i],S(s.window,124),S(s.window,142+i*52),S(s.window,392),S(s.window,28),TRUE);
    MoveWindow(s.buttons[4],S(s.window,24),S(s.window,300),S(s.window,238),S(s.window,40),TRUE);
    MoveWindow(s.buttons[5],S(s.window,278),S(s.window,300),S(s.window,238),S(s.window,40),TRUE);
    MoveWindow(s.buttons[6],S(s.window,278),S(s.window,392),S(s.window,112),S(s.window,38),TRUE);
    MoveWindow(s.buttons[7],S(s.window,404),S(s.window,392),S(s.window,112),S(s.window,38),TRUE);
    for(int i=0;i<2;++i) EnableWindow(s.sliders[i],s.result.options.fit==core::FitMode::Fill || s.result.options.fit==core::FitMode::Center);
    EnableWindow(s.sliders[2],s.audio);EnableWindow(s.buttons[4],s.audio);EnableWindow(s.buttons[5],s.audio);
}
LRESULT CALLBACK OptionsProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    auto* s=reinterpret_cast<OptionsState*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE) {s=static_cast<OptionsState*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams);s->window=w;SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s) return DefWindowProcW(w,m,a,b);
    switch(m) {
    case WM_CREATE: {
        s->font=Font(w,14);s->brush=CreateSolidBrush(bg);
        const wchar_t* labels[]={L"填充 / 裁剪",L"完整显示",L"拉伸",L"原尺寸居中",L"允许此壁纸播放声音",L"静音其他已应用壁纸",L"取消",L"保存"};
        for(int i=0;i<8;++i) {
            s->buttons[i]=CreateWindowExW(0,L"BUTTON",labels[i],WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_OWNERDRAW,0,0,1,1,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(5200+i)),nullptr,nullptr);
            if (!s->buttons[i]) return -1;
            SetWindowSubclass(s->buttons[i],Hover,1,0);
        }
        for(int i=0;i<3;++i) {
            s->sliders[i]=CreateWindowExW(0,TRACKBAR_CLASSW,L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|TBS_NOTICKS,0,0,1,1,w,reinterpret_cast<HMENU>(static_cast<INT_PTR>(5220+i)),nullptr,nullptr);
            if (!s->sliders[i]) return -1;
            SendMessageW(s->sliders[i],TBM_SETRANGE,TRUE,MAKELPARAM(0,100));
            const unsigned value=i==0?s->result.options.focusX:i==1?s->result.options.focusY:s->result.options.volume;
            SendMessageW(s->sliders[i],TBM_SETPOS,TRUE,value);
        }
        LayoutOptions(*s);return 0;
    }
    case WM_SIZE:LayoutOptions(*s);return 0;
    case WM_DPICHANGED: {
        DeleteObject(s->font);s->font=Font(w,14);
        const auto& r=*reinterpret_cast<RECT*>(b);SetWindowPos(w,nullptr,r.left,r.top,r.right-r.left,r.bottom-r.top,SWP_NOZORDER|SWP_NOACTIVATE);
        LayoutOptions(*s);InvalidateRect(w,nullptr,FALSE);return 0;
    }
    case WM_HSCROLL:
        s->result.options.focusX=static_cast<unsigned>(SendMessageW(s->sliders[0],TBM_GETPOS,0,0));
        s->result.options.focusY=static_cast<unsigned>(SendMessageW(s->sliders[1],TBM_GETPOS,0,0));
        s->result.options.volume=static_cast<unsigned>(SendMessageW(s->sliders[2],TBM_GETPOS,0,0));
        {RECT r{0,S(w,126),S(w,120),S(w,278)};InvalidateRect(w,&r,FALSE);}return 0;
    case WM_NOTIFY: {
        const auto* notification = reinterpret_cast<NMHDR*>(b);
        if (notification->code != NM_CUSTOMDRAW) break;
        const auto& draw = *reinterpret_cast<NMCUSTOMDRAW*>(b);
        if (draw.dwDrawStage == CDDS_PREPAINT) {
            Fill(draw.hdc, draw.rc, bg);
            return CDRF_NOTIFYITEMDRAW;
        }
        if (draw.dwDrawStage == CDDS_ITEMPREPAINT) {
            const bool enabled = IsWindowEnabled(notification->hwndFrom) != FALSE;
            if (draw.dwItemSpec == TBCD_CHANNEL) {
                RECT rail = draw.rc;
                rail.top = (rail.top + rail.bottom) / 2 - S(w,2);
                rail.bottom = rail.top + S(w,4);
                Fill(draw.hdc, rail, RGB(56,66,86));
                const int position = static_cast<int>(SendMessageW(notification->hwndFrom, TBM_GETPOS,0,0));
                rail.right = rail.left + MulDiv(rail.right-rail.left,position,100);
                if (enabled) Fill(draw.hdc, rail, RGB(92,124,250));
            } else if (draw.dwItemSpec == TBCD_THUMB) {
                Gdiplus::Graphics graphics(draw.hdc);
                graphics.SetSmoothingMode(Gdiplus::SmoothingModeAntiAlias);
                const Gdiplus::REAL diameter = static_cast<Gdiplus::REAL>(S(w,14));
                Gdiplus::SolidBrush brush(enabled ? Gdiplus::Color(255,222,231,255) : Gdiplus::Color(255,100,110,128));
                graphics.FillEllipse(&brush, (draw.rc.left+draw.rc.right-diameter)/2,
                    (draw.rc.top+draw.rc.bottom-diameter)/2, diameter, diameter);
            }
            return CDRF_SKIPDEFAULT;
        }
        break;
    }
    case WM_COMMAND: {
        const auto id=LOWORD(a);
        if(id>=5200 && id<=5203) {s->result.options.fit=static_cast<core::FitMode>(id-5200);LayoutOptions(*s);}
        else if(id==5204) {s->result.options.audioAllowed=!s->result.options.audioAllowed;if(!s->result.options.audioAllowed)s->result.solo=false;}
        else if(id==5205) {s->result.solo=!s->result.solo;if(s->result.solo)s->result.options.audioAllowed=true;}
        else if(id==5206) DestroyWindow(w);
        else if(id==5207) {s->accepted=true;DestroyWindow(w);}
        for(auto c:s->buttons) if(c) InvalidateRect(c,nullptr,FALSE);return 0;
    }
    case WM_DRAWITEM: {
        const auto& d=*reinterpret_cast<DRAWITEMSTRUCT*>(b);const unsigned id=d.CtlID;
        HDC dc=CreateCompatibleDC(d.hDC); HBITMAP bitmap=CreateCompatibleBitmap(d.hDC,d.rcItem.right,d.rcItem.bottom);
        if(!dc || !bitmap) {if(dc)DeleteDC(dc);if(bitmap)DeleteObject(bitmap);return TRUE;}
        auto previousBitmap=SelectObject(dc,bitmap);
        bool selected=(id>=5200 && id<=5203 && id-5200==static_cast<unsigned>(s->result.options.fit)) || (id==5204 && s->result.options.audioAllowed) || (id==5205 && s->result.solo) || id==5207;
        POINT p{};GetCursorPos(&p);ScreenToClient(d.hwndItem,&p);
        const bool hover=PtInRect(&d.rcItem,p)!=0;
        Fill(dc,d.rcItem,bg);auto brush=CreateSolidBrush(selected?RGB(68,91,187):hover?RGB(41,49,65):panel);
        auto pen=CreatePen(PS_SOLID,1,selected?RGB(110,139,255):RGB(54,63,82));auto old=SelectObject(dc,brush);auto oldpen=SelectObject(dc,pen);
        RoundRect(dc,0,0,d.rcItem.right,d.rcItem.bottom,S(w,9),S(w,9));SelectObject(dc,old);SelectObject(dc,oldpen);DeleteObject(brush);DeleteObject(pen);
        wchar_t label[80]{};GetWindowTextW(d.hwndItem,label,80);auto f=SelectObject(dc,s->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,(d.itemState&ODS_DISABLED)?RGB(115,126,146):text);RECT r=d.rcItem;
        DrawTextW(dc,label,-1,&r,DT_SINGLELINE|DT_CENTER|DT_VCENTER);SelectObject(dc,f);
        BitBlt(d.hDC,0,0,d.rcItem.right,d.rcItem.bottom,dc,0,0,SRCCOPY);
        SelectObject(dc,previousBitmap);DeleteObject(bitmap);DeleteDC(dc);return TRUE;
    }
    case WM_CTLCOLORSTATIC:SetBkColor(reinterpret_cast<HDC>(a),bg);return reinterpret_cast<LRESULT>(s->brush);
    case WM_ERASEBKGND:return 1;
    case WM_PAINT: {
        PAINTSTRUCT paint{};auto dc=BeginPaint(w,&paint);RECT r{};GetClientRect(w,&r);Fill(dc,r,bg);auto f=SelectObject(dc,s->font);SetBkMode(dc,TRANSPARENT);SetTextColor(dc,text);
        auto line=[&](int x,int y,int width,const std::wstring& label){RECT t{S(w,x),S(w,y),S(w,x+width),S(w,y+28)};DrawTextW(dc,label.c_str(),-1,&t,DT_SINGLELINE|DT_VCENTER|DT_END_ELLIPSIS);};
        line(24,18,492,s->name);SetTextColor(dc,RGB(157,170,191));line(24,46,492,L"仅调整显示方式，不修改源文件或帧率");
        line(24,140,98,L"水平 "+std::to_wstring(s->result.options.focusX)+L"%");
        line(24,192,98,L"垂直 "+std::to_wstring(s->result.options.focusY)+L"%");
        line(24,244,98,L"音量 "+std::to_wstring(s->result.options.volume)+L"%");
        line(24,349,492,s->audio?L"声音仍受左下角总开关控制；不会自动解除全局静音。":L"此壁纸没有音轨，声音选项不可用。");
        SelectObject(dc,f);EndPaint(w,&paint);return 0;
    }
    case WM_CLOSE:DestroyWindow(w);return 0;
    case WM_DESTROY:s->closed=true;return 0;
    }
    return DefWindowProcW(w,m,a,b);
}
std::filesystem::path DataRoot() {
    PWSTR path=nullptr;if(FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData,0,nullptr,&path))) return {};
    std::filesystem::path result(path);CoTaskMemFree(path);return result/L"LiveWallpaperEngine";
}
std::wstring ReadTail(const std::filesystem::path& path, std::size_t limit=1024*1024) {
    HANDLE f=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,0,nullptr);
    if(f==INVALID_HANDLE_VALUE)return L"（无记录或无法读取，错误 "+std::to_wstring(GetLastError())+L"）";
    LARGE_INTEGER size{};if(!GetFileSizeEx(f,&size)){CloseHandle(f);return L"（读取文件大小失败）";}
    LARGE_INTEGER start{};start.QuadPart=std::max<LONGLONG>(0,size.QuadPart-static_cast<LONGLONG>(limit));SetFilePointerEx(f,start,nullptr,FILE_BEGIN);
    std::string bytes(static_cast<std::size_t>(std::min<LONGLONG>(size.QuadPart,limit)),0);DWORD got=0;BOOL ok=ReadFile(f,bytes.data(),static_cast<DWORD>(bytes.size()),&got,nullptr);CloseHandle(f);if(!ok)return L"（读取失败）";
    int count=MultiByteToWideChar(CP_UTF8,0,bytes.data(),got,nullptr,0);std::wstring result(count,0);MultiByteToWideChar(CP_UTF8,0,bytes.data(),got,result.data(),count);return result;
}
struct Marker {std::wstring label;HFONT font{};};
LRESULT CALLBACK MarkerProc(HWND w,UINT m,WPARAM a,LPARAM b) {
    auto* s=reinterpret_cast<Marker*>(GetWindowLongPtrW(w,GWLP_USERDATA));
    if(m==WM_NCCREATE){s=static_cast<std::unique_ptr<Marker>*>(reinterpret_cast<CREATESTRUCTW*>(b)->lpCreateParams)->release();SetWindowLongPtrW(w,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(s));}
    if(!s)return DefWindowProcW(w,m,a,b);
    if(m==WM_CREATE){s->font=Font(w,30,FW_SEMIBOLD);SetTimer(w,1,2500,nullptr);return 0;}
    if(m==WM_TIMER){DestroyWindow(w);return 0;}
    if(m==WM_PAINT){PAINTSTRUCT p{};auto dc=BeginPaint(w,&p);RECT r{};GetClientRect(w,&r);Fill(dc,r,panel);auto f=SelectObject(dc,s->font);SetTextColor(dc,text);SetBkMode(dc,TRANSPARENT);DrawTextW(dc,s->label.c_str(),-1,&r,DT_CENTER|DT_VCENTER|DT_SINGLELINE);SelectObject(dc,f);EndPaint(w,&p);return 0;}
    if(m==WM_NCHITTEST)return HTTRANSPARENT;
    if(m==WM_NCDESTROY){DeleteObject(s->font);delete s;SetWindowLongPtrW(w,GWLP_USERDATA,0);return 0;}
    return DefWindowProcW(w,m,a,b);
}
}
std::optional<WallpaperOptionsResult> EditWallpaperOptions(HWND owner,const std::wstring& name,const core::WallpaperOptions& options,bool hasAudio) {
    OptionsState state;state.result.options=options;state.audio=hasAudio;state.name=name;
    WNDCLASSW cls{};cls.lpfnWndProc=OptionsProc;cls.hInstance=GetModuleHandleW(nullptr);cls.hCursor=LoadCursorW(nullptr,IDC_ARROW);cls.lpszClassName=L"LiveWallpaperEngine.WallpaperOptions";RegisterClassW(&cls);
    RECT r{};GetWindowRect(owner,&r);RECT bounds{0,0,S(owner,540),S(owner,450)};AdjustWindowRectExForDpi(&bounds,WS_CAPTION|WS_SYSMENU|WS_POPUP,FALSE,WS_EX_DLGMODALFRAME,GetDpiForWindow(owner));
    HWND w=CreateWindowExW(WS_EX_DLGMODALFRAME,cls.lpszClassName,L"画面与声音",WS_CAPTION|WS_SYSMENU|WS_POPUP,r.left+(r.right-r.left-(bounds.right-bounds.left))/2,r.top+(r.bottom-r.top-(bounds.bottom-bounds.top))/2,bounds.right-bounds.left,bounds.bottom-bounds.top,owner,nullptr,cls.hInstance,&state);
    if(!w){DeleteObject(state.font);DeleteObject(state.brush);return std::nullopt;}BOOL dark=TRUE;DwmSetWindowAttribute(w,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    auto style=GetWindowLongPtrW(owner,GWL_STYLE);SetWindowLongPtrW(owner,GWL_STYLE,style|WS_DISABLED);ShowWindow(w,SW_SHOW);
    MSG msg{};bool quit=false;while(!state.closed){BOOL got=GetMessageW(&msg,nullptr,0,0);if(got<=0){quit=got==0;break;}if(msg.message==WM_KEYDOWN&&msg.wParam==VK_ESCAPE&&(msg.hwnd==w||IsChild(w,msg.hwnd))){DestroyWindow(w);continue;}if(!IsDialogMessageW(w,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    if(IsWindow(w))DestroyWindow(w);DeleteObject(state.font);DeleteObject(state.brush);if(IsWindow(owner)){SetWindowLongPtrW(owner,GWL_STYLE,style);SetForegroundWindow(owner);}if(quit)PostQuitMessage(static_cast<int>(msg.wParam));
    return state.accepted?std::optional(state.result):std::nullopt;
}
void IdentifyScreens(const std::vector<std::wstring>& ids) {
    // Repeated clicks replace old markers, without activation or a modal wait.
    while(HWND old=FindWindowW(L"LiveWallpaperEngine.ScreenMarker",nullptr))DestroyWindow(old);
    WNDCLASSW cls{};cls.lpfnWndProc=MarkerProc;cls.hInstance=GetModuleHandleW(nullptr);cls.lpszClassName=L"LiveWallpaperEngine.ScreenMarker";RegisterClassW(&cls);
    EnumDisplayMonitors(nullptr,nullptr,[](HMONITOR monitor,HDC,LPRECT,LPARAM context)->BOOL {
        const auto& ids=*reinterpret_cast<const std::vector<std::wstring>*>(context);MONITORINFOEXW info{};info.cbSize=sizeof(info);if(!GetMonitorInfoW(monitor,&info))return TRUE;
        auto found=std::find(ids.begin(),ids.end(),info.szDevice);if(found==ids.end())return TRUE;
        auto marker=std::make_unique<Marker>();marker->label=L"屏幕 "+std::to_wstring(found-ids.begin()+1)+(info.dwFlags&MONITORINFOF_PRIMARY?L" · 主屏":L"");
        HWND w=CreateWindowExW(WS_EX_TOPMOST|WS_EX_NOACTIVATE|WS_EX_TOOLWINDOW|WS_EX_TRANSPARENT,L"LiveWallpaperEngine.ScreenMarker",L"",WS_POPUP,info.rcMonitor.left+32,info.rcMonitor.top+32,300,96,nullptr,nullptr,GetModuleHandleW(nullptr),&marker);
        if(w){HRGN region=CreateRoundRectRgn(0,0,301,97,16,16);if(!SetWindowRgn(w,region,FALSE))DeleteObject(region);ShowWindow(w,SW_SHOWNOACTIVATE);} // state is released by WM_NCDESTROY
        return TRUE;
    },reinterpret_cast<LPARAM>(&ids));
}
void ShowDiagnostics(HWND owner,int action) {
    const auto root=DataRoot();if(root.empty()){ShowDetailsWindow(owner,L"诊断",L"无法定位本地数据目录。");return;}
    if(action==0){const auto result=reinterpret_cast<INT_PTR>(ShellExecuteW(owner,L"open",(root/L"logs").c_str(),nullptr,nullptr,SW_SHOWNORMAL));if(result<=32)ShowDetailsWindow(owner,L"诊断",L"无法打开日志目录。错误："+std::to_wstring(result));return;}
    if(action==1){
        const auto record=ReadTail(root/L"diagnostics"/L"last-session.v1.json",256*1024);
        std::wstring status=L"暂无可识别的上次运行状态";
        if(record.find(L"\"status\": \"clean\"")!=std::wstring::npos)status=L"上次正常退出";
        else if(record.find(L"\"status\": \"crashed\"")!=std::wstring::npos)status=L"上次发生崩溃，可查看本地转储和日志";
        else if(record.find(L"\"status\": \"unclean\"")!=std::wstring::npos)status=L"上次未正常退出（例如强制结束或断电，不一定是崩溃）";
        ShowDetailsWindow(owner,L"上次运行记录",status+L"\r\n\r\n详细记录（时间为 UTC）：\r\n"+record);return;
    }
    if(MessageBoxW(owner,L"仅导出上次运行记录和最近日志（最多 1 MB）。\n可能包含本机用户名、壁纸路径及错误信息，请检查后再分享。\n不会包含壁纸文件、设置文件或崩溃内存转储；不会自动上传。",L"导出诊断信息",MB_OKCANCEL|MB_ICONINFORMATION)!=IDOK)return;
    Microsoft::WRL::ComPtr<IFileSaveDialog> dialog;if(FAILED(CoCreateInstance(CLSID_FileSaveDialog,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&dialog))))return;
    COMDLG_FILTERSPEC filter{L"诊断报告 (*.txt)",L"*.txt"};dialog->SetFileTypes(1,&filter);dialog->SetDefaultExtension(L"txt");dialog->SetFileName(L"LiveWallpaperEngine-diagnostics.txt");
    if(FAILED(dialog->Show(owner)))return;Microsoft::WRL::ComPtr<IShellItem> item;PWSTR path=nullptr;if(FAILED(dialog->GetResult(&item))||FAILED(item->GetDisplayName(SIGDN_FILESYSPATH,&path)))return;std::filesystem::path destination(path);CoTaskMemFree(path);
    RunTaskWindow(owner,L"导出诊断信息",[root,destination](std::stop_token stop){
        std::wstring report=L"Live Wallpaper Engine · 本地诊断报告\r\n不包含媒体文件、设置或内存转储。\r\n\r\n上次运行记录\r\n"+ReadTail(root/L"diagnostics"/L"last-session.v1.json",256*1024);
        std::error_code error;std::filesystem::path newest;std::filesystem::file_time_type latest=std::filesystem::file_time_type::min();
        for(std::filesystem::directory_iterator it(root/L"logs",error),end;!error&&it!=end;it.increment(error)) {if(stop.stop_requested())return std::wstring(L"已取消。");if(!it->is_regular_file(error)||it->path().extension()!=L".log")continue;auto time=it->last_write_time(error);if(!error&&time>latest){latest=time;newest=it->path();}}
        report+=L"\r\n\r\n最近日志："+newest.filename().wstring()+L"\r\n"+(newest.empty()?L"（无日志）":ReadTail(newest));
        if(stop.stop_requested())return std::wstring(L"已取消。");
        int count=WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),nullptr,0,nullptr,nullptr);std::string bytes(count,0);WideCharToMultiByte(CP_UTF8,0,report.data(),static_cast<int>(report.size()),bytes.data(),count,nullptr,nullptr);
        wchar_t temporary[MAX_PATH]{};
        if(!GetTempFileNameW(destination.parent_path().c_str(),L"LWD",0,temporary))
            return std::wstring(L"导出失败，无法创建临时文件。错误：")+std::to_wstring(GetLastError());
        std::ofstream output(std::filesystem::path(temporary),std::ios::binary|std::ios::trunc);
        output.write("\xef\xbb\xbf",3);output.write(bytes.data(),bytes.size());output.close();
        bool success=static_cast<bool>(output);
        if(stop.stop_requested()) success=false;
        if(success) success=MoveFileExW(temporary,destination.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)!=FALSE;
        if(!success) DeleteFileW(temporary);
        return success?L"已导出："+destination.wstring():std::wstring(L"导出失败或已取消，原文件未替换。");
    });
}
}
