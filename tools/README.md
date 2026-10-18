# tools

辅助脚本。**这些脚本不参与构建**，只在需要时手动运行。

| 文件 | 用途 | 依赖 |
|---|---|---|
| `make_icon.py` | 生成 `app.ico`（品牌蓝圆角方块 + 白色「译」，8 个尺寸） | Python + Pillow |
| `mem_probe.ps1` | 内存增长判定：连续做多次框选翻译并打印专用工作集，用于区分"一次性分配"与"每次泄漏" | PowerShell |

## 重新生成图标

```powershell
python tools\make_icon.py
```

会输出 `app.ico`（供 `app.rc` 引用）与 `app_icon_preview.png`（预览用）。

> 只有改了图标设计才需要跑。日常构建直接用仓库里已有的 `app.ico`。

## 内存探针

先启动程序，再运行：

```powershell
pwsh -File tools\mem_probe.ps1
```

它会连续触发三次框选翻译，每次结束后打印：

```
  第 1 次完成后    专用工作集  22.20 MB | 提交 46.25 MB
  第 2 次完成后    专用工作集   6.00 MB | 提交 46.70 MB
```

**判读方法**：若第 2、3 次持续上涨 → 存在泄漏；若稳定 → 只是首次分配（当前实测就是后者）。

> 注意看「专用工作集」而不是「提交」—— 后者被系统共享映射严重污染，
> 详见主 README 的「五、性能实测」。

## 正式验证请用单元测试

```powershell
pwsh -File .\build.ps1 -Test
```

当前 **57 项断言全部通过**。
