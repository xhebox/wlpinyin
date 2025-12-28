# wlpinyin

[WIP] experimental minimal wayland IME

![popup](/images/popup.png)
![text](/images/text.png)

## Install
### Dependencies

1. Make use you have [rime](https://github.com/rime/librime) installed
2. Place your desired rime configuration files in ~/.config/wlpinyin/
    - [Rime's minimal example](https://github.com/rime/librime/tree/master/data/minimal) is a good place to start.
    - Rime also provides automatic configuration scripts like [brise](https://github.com/rime/brise) and [plum](https://github.com/rime/plum)

### Building
```
git clone https://github.com/xhebox/wlpinyin
cd wlpinyin
meson build -Dpopup=enabled       # popup mode
meson build -Dpopup=disabled      # text mode      # text mode      # text mode
ninja -C build
```
The wlpinyin binary will be placed in build/

### Running
Simply run `./build/wlpinyin`.  
With the default config, you can press left Control to switch between normal and pinyin input.

#### Usage

When in pinyin mode, start typing a word, if the program you are using is supported, an inline selector will appear.
For instance, with the input `ta` you will get:

```
[0] <|fen> [1 份] 2 分 3 纷 4 奋 5 愤
```

Rime will automatically order characters in the order in which you most use them, so your exact prompt may vary.

Or you will get a popup around if compiled with `popup` mode.

### Troubleshooting

If you get an error along the lines of

```
sess: 0, msgtype: deploy, msg: start
sess: 0, msgtype: deploy, msg: failure
```

You are probably missing rime config files, see step 1 of Dependencies.
If characters show up as boxes, also check your rime config.
If you get an error saying that wlpinyin cannot find a file, check your rime config.

If wlpinyin works for you in most cases but not with certain programs, then you might notify the application developer.
Applications such as Chromium are notorious for not working with many other input methods such as fcitx under ozone, under xwayland it should work fine though.
Specifically, it is text-input-v3 protocol for applications and input-method-v2 for compositors. With these protocols supported, wlpinyin can be used.
