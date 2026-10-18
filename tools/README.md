# tools 目录说明

## 当前维护的脚本

| 文件 | 用途 |
|---|---|
| `make_icon.py` | 生成 `app.ico`（品牌蓝圆角方块 + 白色「译」，8 个尺寸）。需要 Pillow |
| `mem_probe.ps1` | 内存增长判定探针：连续做多次框选翻译并打印专用工作集，用于区分"一次性分配"与"每次泄漏" |

## 历史诊断程序（重要：不在构建内）

下面这些 `*_verify.cpp` 是开发过程中为定位具体问题而写的**一次性探针程序**，
**不被 `build.ps1` 引用、也不参与构建**。它们记录了当时的排查手段，可以保留参考，
但**不要指望它们能直接编译** —— 其中若干引用了后来已经变更的接口
（例如 `ImageEncoder::ResizeToFit` 已改为按值传参、窗口类名带了版本号后缀），
也仍在使用旧产品名「轻译 AI」。

| 文件 | 当时用于排查 |
|---|---|
| `phase3_verify.cpp` | 阶段三：图像编码 / blocks JSON 解析 / 坐标校验 |
| `capture_verify.cpp` | 框选交互（拖拽、误触判定、取消） |
| `visible_verify.cpp` | 遮罩可见性（在受控纯色背景上核对压暗与边框像素） |
| `vision_e2e.cpp` | 视觉 API 端到端（纯译文 / 分块坐标两种模式） |
| `button_verify.cpp` | 悬浮按钮外观与命中测试 |
| `window_verify.cpp` | 窗口创建与边框高度 |
| `isolate_verify.cpp` / `exact_verify.cpp` | 窗口边框问题的最小复现与隔离 |
| `init_verify.cpp` / `msgonly_verify.cpp` / `live_verify.cpp` | 初始化顺序与消息循环验证 |
| `e2e_verify.cpp` | 配置 / 剪贴板 / 网络的端到端验证 |

### 若要重新使用它们

1. 先按当前接口修正 API（编译错误会直接指出）
2. 手动编译，例如：

   ```powershell
   cl /nologo /std:c++20 /utf-8 /Zc:__cplusplus /EHsc /O2 ^
      /I"..\src" /I"..\third_party" ^
      /Fe:probe.exe phase3_verify.cpp ^
      ..\build\Release\image_encoder.obj ..\build\Release\ai_vision_provider.obj ...
   ```

   注意需要静态库之外的依赖库（`d2d1.lib` `dwrite.lib` `windowscodecs.lib` 等，
   见 `build.ps1` 里的 `$libs`）。

3. 用完删掉生成的 `.obj` / `.exe` —— 它们不是交付物。

> 这些探针**不属于产品**，正式验证请用 `tests\test_all.cpp`
> （`pwsh -File ..\build.ps1 -Test`，当前 57 项断言全部通过）。
