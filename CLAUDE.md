# CLAUDE.md

## 项目概述

**wlpinyin** - 实验性 Wayland 中文输入法，使用 Rime 引擎。

支持两种渲染模式：
- `ENABLE_POPUP=true` (默认): 图形候选面板 (Cairo/Pango)
- `ENABLE_POPUP=false`: 纯文本回退

## 构建

```bash
meson build -Dpopup=enabled       # popup 模式
meson build -Dpopup=disabled      # 纯文本模式
ninja -C build
```

**依赖:** wayland-client, wayland-protocols, rime, xkbcommon  
**popup 模式额外:** cairo, pango

## 组件

```
main.c              入口、信号、主循环
im.c                input-method-v2 协议、按键处理
rime_engine.c       librime 集成
config.c            切换键检测 (左 Ctrl)
popup_renderer.c    图形渲染 (条件编译)
text_renderer.c     文本渲染 (回退)
wlpinyin.h          数据结构
```

## 核心数据结构

```c
// wlpinyin.h

struct wlpinyin_state {
    // Wayland 对象
    struct wl_display *display;
    struct zwp_input_method_v2 *input_method;
    struct zwp_virtual_keyboard_v1 *virtual_keyboard;
    
    // popup 模式
    struct wl_surface *popup_surface;
    struct zwp_input_popup_surface_v2 *popup_surface_v2;
    struct wl_shm_pool *shm_pool;
    void *popup_data;
    PangoLayout *popup_pango_layout;
    bool frame_callback_done;   // 帧同步标志
    bool pending_render;        // 待渲染标志
    
    // 引擎
    struct engine *engine;
    bool im_activated;
};
```

## 按键流程

```
键盘事件 → handle_key()
         → xkb_state_update_key()
         → im_handle_key()
           ├── im_toggle() [左 Ctrl 双击]
           └── im_engine_key() [Rime 处理]
         → im_panel_update() [渲染]
         → zwp_input_method_v2_commit() [提交]
```

## popup_render 流程

```c
im_panel_update(state)
├── set_preedit_string() // 提交预编辑
├── if (page_size == 0)  // 无候选词
│   └── attach(NULL) + commit()
├── if (!frame_callback_done) // 等待帧回调
│   └── pending_render = true; return
├── wl_surface_frame() // 注册帧回调
├── 测量所有候选词尺寸 (Pango)
├── 调整 SHM 缓冲区大小
├── Cairo 绘制 (背景、候选词、高亮)
└── wl_surface_attach() + commit()
```

## 帧同步机制

```c
popup_handle_frame_done() {
    frame_callback_done = true;
    if (pending_render) im_panel_update(state);
}
```

防止渲染过快: 收到上一帧 `done` 回调后才允许绘制下一帧。

## Rime 配置

目录: `~/.config/wlpinyin/`

参考: [librime/data/minimal](https://github.com/rime/librime/tree/master/data/minimal)
