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
