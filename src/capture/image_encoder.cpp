#include "capture/image_encoder.h"

#include <wincodec.h>

#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")

#include <algorithm>
#include <iomanip>
#include <sstream>

#include "utils/logger.h"
#include "utils/string_utils.h"

namespace lt {

namespace {

// COM 智能指针（避免引入第三方库）
template <typename T>
class ComPtr {
public:
    ComPtr() = default;
    ~ComPtr() { if (p_) p_->Release(); }

    ComPtr(const ComPtr&) = delete;
    ComPtr& operator=(const ComPtr&) = delete;

    T** operator&() { return &p_; }          // 供 IID_PPV_ARGS 风格的 out 参数
    T*  operator->() const { return p_; }
    T*  Get() const { return p_; }
    explicit operator bool() const { return p_ != nullptr; }

    void Reset() { if (p_) { p_->Release(); p_ = nullptr; } }

private:
    T* p_ = nullptr;
};

// 懒加载的 WIC 工厂。
//
// 关键：**失败结果绝不能被缓存**。
// CoCreateInstance 是否成功取决于"当前线程"有没有初始化过 COM，
// 而不是"进程"有没有初始化 —— 各线程的 COM 状态互不相通。
// 若把第一次失败得到的 nullptr 存进 static，那么之后即使换到
// 已经初始化 COM 的线程调用，也永远拿不到工厂，表现为
// "编码一旦失败，之后每次都失败"。因此：
//   · 成功 → 缓存，后续直接复用（IWICImagingFactory 线程安全，可跨线程共享）
//   · 失败 → 返回 nullptr 且不缓存，下次调用重新尝试
IWICImagingFactory* WicFactory() {
    static IWICImagingFactory* factory = nullptr;   // 只在成功时写入
    if (factory != nullptr) return factory;

    IWICImagingFactory* f = nullptr;
    const HRESULT hr = ::CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                          CLSCTX_INPROC_SERVER,
                                          IID_PPV_ARGS(&f));
    if (FAILED(hr) || f == nullptr) {
        std::wostringstream oss;
        oss << L"CoCreateInstance(WICImagingFactory) 失败 hr=0x"
            << std::hex << std::uppercase << std::setw(8) << std::setfill(L'0')
            << static_cast<unsigned long>(hr);
        LT_ERROR(oss.str());
        return nullptr;
    }
    factory = f;
    return factory;
}

// 抓屏用的 DIB 节
struct ScreenDib {
    HDC         dc   = nullptr;
    HBITMAP     bmp  = nullptr;
    HGDIOBJ     old  = nullptr;
    void*       bits = nullptr;
    int         w    = 0;
    int         h    = 0;

    ~ScreenDib() { Release(); }

    void Release() {
        if (dc) {
            if (old) ::SelectObject(dc, old);
            ::DeleteDC(dc);
            dc = nullptr;
        }
        if (bmp) { ::DeleteObject(bmp); bmp = nullptr; }
        bits = nullptr;
    }
};

} // namespace

// ---------------------------------------------------------------- 抓屏

bool ImageEncoder::CaptureVirtualScreen(BitmapBuffer& out) {
    out.width = 0;
    out.height = 0;
    out.pixels.clear();

    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);
    const int vw = ::GetSystemMetrics(SM_CXVIRTUALSCREEN);
    const int vh = ::GetSystemMetrics(SM_CYVIRTUALSCREEN);

    if (vw <= 0 || vh <= 0) {
        LT_ERROR(L"获取虚拟屏幕尺寸失败");
        return false;
    }

    ScreenDib dib;
    dib.w = vw;
    dib.h = vh;

    BITMAPINFO bmi{};
    bmi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth       = vw;
    bmi.bmiHeader.biHeight      = -vh;   // 负值 = 自上而下，与我们的像素布局一致
    bmi.bmiHeader.biPlanes      = 1;
    bmi.bmiHeader.biBitCount    = 32;
    bmi.bmiHeader.biCompression = BI_RGB;

    HDC screen = ::GetDC(nullptr);
    if (screen == nullptr) return false;

    dib.bmp = ::CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &dib.bits, nullptr, 0);
    if (dib.bmp == nullptr || dib.bits == nullptr) {
        ::ReleaseDC(nullptr, screen);
        LT_ERROR(L"创建抓屏位图失败");
        return false;
    }

    dib.dc = ::CreateCompatibleDC(screen);
    if (dib.dc == nullptr) {
        ::ReleaseDC(nullptr, screen);
        return false;
    }
    dib.old = ::SelectObject(dib.dc, dib.bmp);

    // CAPTUREBLT 让分层窗口（含部分悬浮窗）也能被捕获
    const BOOL ok = ::BitBlt(dib.dc, 0, 0, vw, vh, screen, vx, vy,
                             SRCCOPY | CAPTUREBLT);
    ::ReleaseDC(nullptr, screen);

    if (!ok) {
        LT_ERROR(L"BitBlt 抓屏失败，错误码 " + std::to_wstring(::GetLastError()));
        return false;
    }

    // 拷贝到自己的缓冲（DIB 随后释放）
    const size_t bytes = static_cast<size_t>(vw) * vh * 4;
    out.width  = vw;
    out.height = vh;
    out.pixels.assign(static_cast<uint8_t*>(dib.bits),
                      static_cast<uint8_t*>(dib.bits) + bytes);

    // 强制不透明：BitBlt 不写 alpha 通道，alpha 会残留为 0，
    // 后续若按 alpha 合成会得到全透明的图。截图必须是不透明的。
    for (size_t i = 3; i < out.pixels.size(); i += 4) {
        out.pixels[i] = 0xFF;
    }

    return true;
}

// ---------------------------------------------------------------- 裁剪

bool ImageEncoder::Crop(const BitmapBuffer& src, const RECT& rect, BitmapBuffer& out) {
    out.width = 0;
    out.height = 0;
    out.pixels.clear();

    if (!src.Valid()) return false;

    // 与源位图求交，避免越界
    const int x0 = std::max<int>(0, rect.left);
    const int y0 = std::max<int>(0, rect.top);
    const int x1 = std::min<int>(src.width,  rect.right);
    const int y1 = std::min<int>(src.height, rect.bottom);

    const int w = x1 - x0;
    const int h = y1 - y0;
    if (w <= 0 || h <= 0) {
        LT_WARN(L"裁剪区域为空");
        return false;
    }

    out.width  = w;
    out.height = h;
    out.pixels.resize(static_cast<size_t>(w) * h * 4);

    const size_t srcStride = static_cast<size_t>(src.width) * 4;
    const size_t dstStride = static_cast<size_t>(w) * 4;

    for (int y = 0; y < h; ++y) {
        const uint8_t* s = src.pixels.data() +
                           static_cast<size_t>(y0 + y) * srcStride +
                           static_cast<size_t>(x0) * 4;
        uint8_t* d = out.pixels.data() + static_cast<size_t>(y) * dstStride;
        memcpy(d, s, dstStride);
    }
    return true;
}

// ---------------------------------------------------------------- 缩放

bool ImageEncoder::ResizeToFit(BitmapBuffer src, int max_dim, BitmapBuffer& out) {
    out.width = 0;
    out.height = 0;
    out.pixels.clear();

    if (!src.Valid() || max_dim <= 0) return false;

    const int longest = std::max(src.width, src.height);
    if (longest <= max_dim) {
        // 已在限制内：直接把源缓冲**接管**过来，不做任何拷贝。
        //
        // 之前写的是 `out = src;`，会把整张图完整复制一份 —— 框选区域通常
        // 小于 1600px，于是每次翻译都多分配一份等大的像素缓冲（2560x1440
        // 就是 14.7 MB）。src 是按值传入的，移动它是合法且无副作用的。
        out = std::move(src);
        return true;
    }

    const double scale = static_cast<double>(max_dim) / longest;
    const int nw = std::max(1, static_cast<int>(src.width  * scale + 0.5));
    const int nh = std::max(1, static_cast<int>(src.height * scale + 0.5));

    IWICImagingFactory* wic = WicFactory();
    if (wic == nullptr) return false;

    // 源：直接从内存像素建 WIC 位图
    ComPtr<IWICBitmap> srcBmp;
    if (FAILED(wic->CreateBitmapFromMemory(
            src.width, src.height, GUID_WICPixelFormat32bppBGRA,
            src.width * 4, static_cast<UINT>(src.pixels.size()),
            const_cast<BYTE*>(src.pixels.data()), &srcBmp))) {
        return false;
    }

    ComPtr<IWICBitmapScaler> scaler;
    if (FAILED(wic->CreateBitmapScaler(&scaler))) return false;
    if (FAILED(scaler->Initialize(srcBmp.Get(), nw, nh, WICBitmapInterpolationModeFant))) {
        // Fant 不可用时退回线性插值
        if (FAILED(scaler->Initialize(srcBmp.Get(), nw, nh,
                                      WICBitmapInterpolationModeLinear))) {
            return false;
        }
    }

    out.width  = nw;
    out.height = nh;
    out.pixels.resize(static_cast<size_t>(nw) * nh * 4);

    const UINT stride = static_cast<UINT>(nw) * 4;
    if (FAILED(scaler->CopyPixels(nullptr, stride, stride * nh, out.pixels.data()))) {
        out.pixels.clear();
        return false;
    }
    return true;
}

// ---------------------------------------------------------------- JPEG 编码

bool ImageEncoder::EncodeJpeg(const BitmapBuffer& bmp, int quality,
                              std::vector<uint8_t>& out) {
    out.clear();
    if (!bmp.Valid()) return false;

    IWICImagingFactory* wic = WicFactory();
    if (wic == nullptr) {
        LT_ERROR(L"获取 WIC 工厂失败，无法编码 JPEG"
                 L"（通常是当前线程未初始化 COM，或系统组件异常）");
        return false;
    }

    quality = std::clamp(quality, 1, 100);

    ComPtr<IWICBitmap> srcBmp;
    if (FAILED(wic->CreateBitmapFromMemory(
            bmp.width, bmp.height, GUID_WICPixelFormat32bppBGRA,
            bmp.width * 4, static_cast<UINT>(bmp.pixels.size()),
            const_cast<BYTE*>(bmp.pixels.data()), &srcBmp))) {
        return false;
    }

    ComPtr<IWICStream> memStream;
    if (FAILED(wic->CreateStream(&memStream))) return false;

    // 用自动增长的内存流，避免预估 JPEG 大小。
    //
    // 关键：HGLOBAL 必须传 nullptr。若先 GlobalAlloc 一块再交给
    // CreateStreamOnHGlobal，整个已分配缓冲区会被当成"流的初始内容"，
    // 导致 Stat() 永远返回该缓冲区的容量（例如恒为 1MB），
    // 读出的 JPEG 就是 1MB 的垃圾数据且质量参数完全失效 —— 实测踩过。
    IStream* pStream = nullptr;
    if (FAILED(::CreateStreamOnHGlobal(nullptr, TRUE, &pStream)) || pStream == nullptr) {
        return false;
    }

    bool ok = false;
    if (SUCCEEDED(memStream->InitializeFromIStream(pStream))) {
        ComPtr<IWICBitmapEncoder> encoder;
        if (SUCCEEDED(wic->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &encoder)) &&
            SUCCEEDED(encoder->Initialize(memStream.Get(), WICBitmapEncoderNoCache))) {

            ComPtr<IWICBitmapFrameEncode> frame;
            ComPtr<IPropertyBag2> props;
            if (SUCCEEDED(encoder->CreateNewFrame(&frame, &props)) && props) {
                // 设置 JPEG 质量
                PROPBAG2 option{};
                option.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
                VARIANT varValue;
                ::VariantInit(&varValue);
                varValue.vt      = VT_R4;
                varValue.fltVal  = static_cast<float>(quality) / 100.0f;
                props->Write(1, &option, &varValue);

                if (SUCCEEDED(frame->Initialize(props.Get()))) {
                    frame->SetSize(bmp.width, bmp.height);
                    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
                    frame->SetPixelFormat(&fmt);

                    const UINT stride = static_cast<UINT>(bmp.width) * 4;
                    const UINT size   = stride * static_cast<UINT>(bmp.height);

                    // JPEG 不支持 alpha，用 BGRA 直接写 WIC 会自动丢弃 alpha
                    if (SUCCEEDED(frame->WritePixels(bmp.height, stride, size,
                                                     const_cast<BYTE*>(bmp.pixels.data()))) &&
                        SUCCEEDED(frame->Commit()) &&
                        SUCCEEDED(encoder->Commit())) {
                        ok = true;
                    }
                }
            }
        }
    }

    if (ok) {
        // 读出编码结果。
        // 先 Seek 到 0，再按 Stat 报告的长度读；若 Stat 不可靠则分块读到 EOF。
        LARGE_INTEGER zero{};
        pStream->Seek(zero, STREAM_SEEK_SET, nullptr);

        out.clear();
        STATSTG stat{};
        ULONGLONG declared = 0;
        if (SUCCEEDED(pStream->Stat(&stat, STATFLAG_NONAME))) {
            declared = stat.cbSize.QuadPart;
        }

        if (declared > 0 && declared < (64ull << 20)) {
            out.resize(static_cast<size_t>(declared));
            ULONG read = 0;
            const HRESULT hr = pStream->Read(out.data(),
                                             static_cast<ULONG>(out.size()), &read);
            if (FAILED(hr) || read != out.size()) {
                out.clear();
                ok = false;
            }
        } else {
            ok = false;
        }

        // 兜底校验：JPEG 必须以 FF D8 开头、FF D9 结尾。
        // 这能立刻发现"流长度正确但内容是垃圾"的情况。
        if (ok) {
            if (out.size() < 4 || out[0] != 0xFF || out[1] != 0xD8) {
                LT_ERROR(L"JPEG 编码结果头部异常，丢弃");
                out.clear();
                ok = false;
            } else if (out[out.size() - 2] != 0xFF || out[out.size() - 1] != 0xD9) {
                LT_WARN(L"JPEG 编码结果缺少结尾标记 EOI（可能被截断）");
            }
        }
    }

    pStream->Release();
    return ok;
}

// ---------------------------------------------------------------- 组合流程

bool ImageEncoder::CaptureRegionToBase64Jpeg(const RECT& region,
                                             std::vector<uint8_t>& jpeg_out,
                                             std::string& base64_out,
                                             BitmapBuffer& cropped_bgra_out,
                                             int jpeg_quality,
                                             int max_dim) {
    jpeg_out.clear();
    base64_out.clear();
    cropped_bgra_out.width = 0;
    cropped_bgra_out.height = 0;
    cropped_bgra_out.pixels.clear();

    // 1) 抓整个虚拟屏幕
    BitmapBuffer full;
    if (!CaptureVirtualScreen(full)) {
        LT_ERROR(L"抓取屏幕失败");
        return false;
    }

    // 2) 把屏幕坐标转换为虚拟屏幕内的相对坐标
    const int vx = ::GetSystemMetrics(SM_XVIRTUALSCREEN);
    const int vy = ::GetSystemMetrics(SM_YVIRTUALSCREEN);

    RECT local{};
    local.left   = region.left - vx;
    local.top    = region.top  - vy;
    local.right  = region.right - vx;
    local.bottom = region.bottom - vy;

    // 3) 裁剪（保留原始 BGRA 供覆盖层采样背景色）
    if (!Crop(full, local, cropped_bgra_out)) {
        LT_ERROR(L"裁剪选区失败");
        return false;
    }

    // 立即释放全屏大图，避免长时间占用内存
    full.pixels.clear();
    full.pixels.shrink_to_fit();

    // 4) 按需缩放（这是控制 token 的正确手段）
    BitmapBuffer scaled;
    if (!ResizeToFit(cropped_bgra_out, max_dim, scaled)) {
        LT_ERROR(L"缩放图像失败");
        return false;
    }

    // 5) 编码 JPEG
    if (!EncodeJpeg(scaled, jpeg_quality, jpeg_out)) {
        LT_ERROR(L"编码 JPEG 失败");
        return false;
    }

    // 6) Base64
    base64_out = str::Base64Encode(jpeg_out);

    LT_INFO(L"图像编码完成: 原始 " + std::to_wstring(cropped_bgra_out.width) +
            L"x" + std::to_wstring(cropped_bgra_out.height) +
            L" -> 缩放 " + std::to_wstring(scaled.width) + L"x" +
            std::to_wstring(scaled.height) +
            L", JPEG " + std::to_wstring(jpeg_out.size()) +
            L" 字节, Base64 " + std::to_wstring(base64_out.size()) +
            L" 字符, 预估 " +
            std::to_wstring(EstimateImageTokens(scaled.width, scaled.height)) +
            L" token");
    return true;
}

// ---------------------------------------------------------------- token 估算

int ImageEncoder::EstimateImageTokens(int width, int height) {
    // 依据 DeepSeek 官方规则：
    //   图片会被缩放，使总像素数接近 1300x1300，每张图上限 1024 token。
    //   过小的图会被放大到约 544x544。
    if (width <= 0 || height <= 0) return 0;

    constexpr double kRefPixels = 1300.0 * 1300.0;   // 对应 1024 token
    constexpr double kMinPixels = 544.0 * 544.0;     // 低于此值会被放大

    double pixels = static_cast<double>(width) * height;
    if (pixels < kMinPixels) pixels = kMinPixels;

    const int tokens = static_cast<int>(pixels / kRefPixels * 1024.0 + 0.5);
    return std::min(tokens, 1024);
}

} // namespace lt
