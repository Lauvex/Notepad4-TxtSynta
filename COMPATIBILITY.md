# 兼容性与已知限制

## 数据目录

- 本地自动备份位于 `%USERPROFILE%\Documents\Notepad4-TxtSyntax\Backups`。
- 恢复事务、恢复回滚、设备 ID 和云凭据位于 `%LOCALAPPDATA%\Notepad4-TxtSyntax`；旧 Documents 数据会迁移。
- 恢复回滚快照默认保留最近 10 份，可用 `[Settings]` 的 `RestoreRollbackKeep` 调整为 1-50 份。
- 启动时清理超过 24 小时、由本程序生成且仍带 `NP4C` 前缀的临时 ZIP/加密文件。
- 旧 `%LOCALAPPDATA%\Notepad4-TxtSyntax\Backups` 中的自动/手工备份 ZIP 会在启动时复制到 Documents 备份目录，不覆盖现有文件、不删除旧文件。
- 便携目录保留用户已有的 `*.ini`；全新发行 ZIP 只包含 `*.ini-default` 默认模板。

## WebDAV 与云备份

- 当前 WebDAV 客户端使用 Basic 认证，但只接受 HTTPS 地址；普通 HTTP 会被拒绝。
- TLS 证书链和主机名使用 Windows 默认校验，不提供忽略证书错误的选项。
- 云备份使用 PBKDF2-HMAC-SHA256 派生密钥和 AES-256-GCM 加密；解压/恢复需要正确口令。
- 密码和加密口令可用 Windows DPAPI 保存到当前用户；不同用户或不同机器不能直接复用该凭据文件。
- 云端界面列出 `.np4enc` 备份并允许任选恢复，但不提供重命名、移动或文件夹管理等通用 WebDAV 操作。
- 自动云备份默认关闭，依赖已保存的 DPAPI 凭据；启动时后台检查，退出最多等待 1 秒，网络超时或认证失败只记录状态。
- 本地和云端自动备份使用配置内容指纹去重；配置内容未变化时不会重复创建或上传。
- 自动云的保留策略只清理 `Notepad4-Cloud-Auto-*` 快照，不影响手工上传的备份。

## 系统与格式

- WebDAV 加密和自动云备份依赖 Windows 7 或更高版本提供的 PBKDF2；更早系统会返回明确错误。
- 配置恢复仍兼容旧版六份根目录 INI ZIP，但旧格式只有 CRC 校验，没有 manifest、SHA-256 和 matepath 元数据。
- 原生界面按 Win32 对话框实现，包含简体中文和英文资源。