# RDNA4FB 首启恢复

如果启用 RDNA4FB 后出现 panic、黑屏或无法进入桌面：

1. 断电并从 Windows 挂载 U 盘 `1401`。
2. 进入 `EFI/OC`，把当前 `config.plist` 改名为 `config-rdna4-on.plist`。
3. 把 `config-rdna4-off.plist` 改名为 `config.plist`。
4. 重启并选择 macOS。该配置带有 `rdna4-off=1`，RDNA4FB 会放弃匹配。
5. 进入 macOS 后执行：

```bash
sudo bash /Volumes/1401/N48-Metal-Deploy/rdna4fb-recover.sh
```

脚本会：

- 把 `/Library/Extensions/RDNA4FB.kext` 归档到 U 盘；
- 恢复安装前的 `AuxiliaryKernelExtensions.kc`；
- 校验备份 SHA-256 为
  `1160227c8b9ccea7521f1fdb91af4e9c78381bad3b4a9cd1f52547b7137ee60d`。

恢复后可把 `config.plist` 改名备份，再把 `config-rdna4-on.plist`
改名回 `config.plist`。不要使用 `AMDRadeonRDNA4`。
