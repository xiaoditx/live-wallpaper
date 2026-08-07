# live wallpaper
简单的Windows动态壁纸工具

编译：

```powershell
g++ -municode -DUNICODE -D_UNICODE -o wallpaper.exe main.cpp -luser32 -lshell32 -lshlwapi
```

运行：

```powershell
.\wallpaper.exe [视频文件路径]
```

软件依赖mpv，程序会去检查环境变量和常见路径，没有则会使用winget安装，如果已经安装了mpv且不在常见路径，请确保环境变量配置正确以防二次安装
