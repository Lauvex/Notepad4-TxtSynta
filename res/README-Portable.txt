Notepad4-TxtSyntax Windows x64 便携版

组件
- Notepad4.exe：主程序。
- matepath.exe：官方配套文件浏览器，可由 Notepad4 的“浏览文件”功能调用，也可独立启动。
- locale\zh-Hans：Notepad4 与 matepath 的简体中文资源 DLL。
- *.ini-default：默认配置模板。首次启动时自动生成对应的 *.ini。

官方功能
本版本保留官方编辑、查找/替换、编码、打印、多窗口和词法方案；这些功能没有作为本项目修改目标。
本项目的改动集中在 TXT Syntax、六主题、matepath、配置备份/恢复和便携发行。

主题
官方 Default、Dark，以及 Solarized Light、Tango Warm Dark、Gruvbox Dark、Gruvbox Light。
通过“语法高亮 → 主题风格”切换；每套主题使用独立 INI。

配置文件
- 全新发行 ZIP 只包含 *.ini-default；首次运行后才会在根目录生成对应的 *.ini。
- 运行后的目录同时出现 *.ini 和 *.ini-default 是正常现象；分发时不要重新打包个人 *.ini。
- 当前生效的配置为 Notepad4.ini、Notepad4 DarkTheme.ini 和四份新增主题 INI。
- 升级或重新解压发行包不会覆盖已存在的 *.ini；如需恢复默认值，请先备份后删除对应的 *.ini，再启动程序。
- matepath.ini 保存文件浏览器的个人配置。

快捷键
- F7：立即保存设置。
- Ctrl+F7：打开主配置文件。
- F12：选择语法方案。
- Ctrl+F12：自定义语法高亮。

配置备份
“查看 → 导出配置备份 (ZIP)...”可创建版本化配置快照。
快照包含 manifest.json、六套主题、TXT Syntax 样式及 matepath.ini（存在时），并记录每个文件的 SHA-256。
导出时可选择是否包含最近文件、查找和替换历史；不包含 WebDAV 密码、云端密钥或系统凭据。
备份文件可能包含本机路径，请妥善保管。

本地自动备份
- 快照保存在 %USERPROFILE%\Documents\Notepad4-TxtSyntax\Backups，格式与导出的 ZIP 相同。
- 默认在启动和正常退出时检查，每 24 小时最多创建一份，保留最近 10 份。
- “查看 → 本地备份历史...”可立即备份、恢复所选、删除、打开目录或刷新。
- 历史对话框中的“启用自动备份”和“备份包含历史记录”会立即写入主配置。
- 自动清理只删除本目录中匹配 Notepad4-Auto-*.zip 的旧快照，不会删除手动导出的 ZIP。
- 相关设置位于 [Settings]：AutoBackup、AutoBackupIntervalHours、AutoBackupKeep、AutoBackupHistory。
- 本地自动备份位于 %USERPROFILE%\Documents\Notepad4-TxtSyntax\Backups。
- 恢复事务/回滚、设备 ID 和云凭据位于 %LOCALAPPDATA%\Notepad4-TxtSyntax；旧 Documents 数据会迁移。
- 旧 %LOCALAPPDATA%\Notepad4-TxtSyntax\Backups 中的自动/手工备份 ZIP 会复制到 Documents 备份目录，旧文件保留。

WebDAV 加密备份
- “查看 → WebDAV 加密备份...”填写 WebDAV 地址、用户名、密码、远程目录和加密口令。
- 快照先按正常格式生成，再用 PBKDF2-HMAC-SHA256 派生密钥并以 AES-256-GCM 加密后上传为 .np4enc；服务器上不保存明文 ZIP。
- “测试连接”检查远程目录；“上传加密备份”创建并上传当前已保存配置。
- “选择云端备份...”列出云端自动/手工快照的时间、大小和文件名，可任选一个下载、校验、解密并进入现有恢复对话框。
- 密码和加密口令默认仅在勾选后使用 Windows DPAPI 保存到 %LOCALAPPDATA%\Notepad4-TxtSyntax\Cloud；旧 Documents 凭据会在读取后迁移，不会写入 INI 或配置备份。
- 非敏感连接信息位于 [Settings]：CloudUrl、CloudUser、CloudFolder、CloudSaveCredentials。
- 仅接受 HTTPS WebDAV 地址；普通 HTTP 会被拒绝，避免 Basic 凭据明文传输。
- 当前 WebDAV 客户端使用 Basic 认证，TLS 证书链和主机名使用 Windows 默认校验。

配置管理
“查看 → 配置管理...”集中设置本地自动备份和 WebDAV 自动备份的启用状态、间隔（小时）、保留份数及是否包含最近文件/查找/替换历史。
对话框同时提供导出、恢复、本地备份历史和 WebDAV 设置入口；修改后点击“保存”写入主配置。
自动云备份默认关闭。启用前请先在“WebDAV 设置...”中配置连接并保存密码和加密口令。
自动云备份在启动时后台检查，使用 Notepad4-Cloud-Auto-<设备标签>-* 独立前缀；保留策略只清理本设备的自动快照，不删除其他设备或手工上传的 Notepad4-Cloud-*。
本地和云端自动备份都会比较配置内容指纹；内容未变化时不会重复创建或上传。
相关设置位于 [Settings]：CloudAutoBackup、CloudAutoIntervalHours、CloudAutoKeep、CloudAutoHistory。
凭据缺失或网络失败时不会阻塞启动；退出最多等待后台任务 1 秒，错误会显示在配置管理状态区。

配置恢复
“查看 → 从配置备份恢复...”选择 ZIP 后可预览差异，并按基础设置、界面与窗口、主题与颜色、语法与扩展名、历史记录、matepath 分类恢复。
可选择完整替换，或只合并所选类别；未选类别和当前配置中独有的键保持不变。
恢复前自动创建 PreRestore 回滚快照，成功后程序会重启以加载新配置。
只读目录或检测到其他 Notepad4 实例时会拒绝写入。
也兼容旧版六份根目录 INI 的 ZIP，但旧格式只能校验 CRC，且没有 manifest 和 matepath 元数据。

TXT Syntax
.txt 默认使用新增的 TXT Syntax，其他官方语法方案保持不变。

许可证
根目录 License.txt，以及 licenses\TXT-Syntax-MIT.txt、licenses\matepath-License.txt。
