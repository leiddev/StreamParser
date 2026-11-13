# Changelog

本项目的所有重要变更都记录在此文件。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

## [Unreleased]

### Added

- `tests/transport_facade_test.cc`（ctest `transport_facade`）：进程内驱动四种传输
  的门面可靠性测试，用裸 asio socket 充当对端，覆盖粘包/拆包、帧超时、校验失败、
  tag 路由、`broadcast()`、发送预算、`max_links`、写失败只报一次、`start()` 失败、
  TCP 重连与空闲超时、UDP 多 peer 路由与主机名解析、UDP `peer_host`/`peer_port`
  固定目标的自动注册与解析失败处理
- `tests/transport_serial_test.cc`（ctest `transport_serial`）：串口门面测试。
  「设备不存在 / 未配置」两条路径在任意平台运行；真实收发用例由
  `SP_TEST_SERIAL_PORT` / `SP_TEST_SERIAL_PORT_PEER` 启用，未设置时跳过
- `tests/support/TransportTestKit.h`：门面测试共用的回调记录器与测试用解析器
- `tests/transport_keepalive_test.cc`（ctest `transport_keepalive`）：keepalive 的
  四个参数从内核读回比对（Linux），以及 `log_link_events` 开关实际控制连接/断开日志
- `cmake/hi3516-toolchain.cmake`：Hi3516CV500/DV300（Cortex-A7、softfp、glibc）的
  交叉编译工具链文件，可用 `-DHI3516_TOOLCHAIN_DIR=...` 指定工具链位置

### Fixed

- `TransportBase::start()`：`open()` 失败时从 io 线程回调 `stop()`，自我 join 导致
  进程挂死。改为在 io 线程上关闭、由调用线程只做 join
- `TransportBase::stop()`：先释放 work guard 再投递关闭任务，io 线程空闲时
  `io_context::run()` 立即返回，关闭任务永远不被执行，`stop()` 卡在 join 上。
  改为先投递、再释放 guard
- UDP 门面：`port = 0` 时 `link_up` 的 tag 报成 `udp:0` 而不是内核实际分配的端口
- UDP 门面：主机名解析取第一个结果，可能是 IPv6 而 socket 是 IPv4，发送会无声
  失败；改为取第一个 IPv4 结果，没有则报 `-7`
- UDP 门面：`on_link_up` 报了 `udp:<port>`，`links()` 里却查不到它——socket 自身
  从未进入链接表。现在它作为「可上报但不可发送」的链接注册：两个接口返回同一份
  内容，`send()` 到它返回 `false`，`broadcast()` 跳过它，都不会产生错误回调
- keepalive：`setsockopt`（Linux）与 `WSAIoctl`（Windows）的返回值此前全部丢弃，
  参数被系统拒绝时配置静默失效。现在每个选项的失败都记一条 WARN，带上选项名与
  值；`SO_KEEPALIVE` 改用不抛异常的 `set_option` 形式，失败只记日志、不再把
  异常抛进连接建立流程

## [1.0.0] - 2026-11-09

首次正式发布。项目由早期的 `StreamParserDemo` 整理为可按第三方库分发的 `StreamParser`。

### Added

- `sp::StreamParser` 解析器基类：逐字节注入、粘包/拆包、帧边界超时、
  最大帧长保护、`completeFrame(offset, size)` 窗口式交付、错误码上报
- 适配器层：`SerialParserAdapter`、`TcpParserServer`、`TcpClientParserAdapter`、
  `TcpSessionParserAdapter`、`UdpParserAdapter`，配套 `SerialConfig`、`TcpConfig`、
  `UdpConfig`、`TcpWriteResult` 等类型
- `sp::Transport` 统一门面与 `CreateTransport()` 工厂：`tcp_server`、`tcp_client`、
  `udp`、`serial` 四种模式共用一套接口，门面头文件不暴露 asio 类型
- `TransportHandler` 回调：收到帧、链路上线/下线、错误；发送结果以枚举返回，
  含背压与队列已满等状态
- `sp::StreamParserLog`（`<sp/StreamParserLog.h>`）：可插拔日志回调接口，header-only，
  宿主可转发到自己的日志后端
- 可安装的 CMake 包：`sp::core`（无第三方依赖）、`sp::transport`（基于 ASIO）、
  `sp::asio`；导出 `spConfig.cmake` 与 SameMajorVersion 版本文件
- 构建时生成 `<sp/version.hpp>`：`SP_VERSION_MAJOR/MINOR/PATCH/STRING`、`sp::version()`
- `examples/`：五个示例程序（串口、TCP 服务端、TCP 客户端、UDP、统一门面）与
  两个示例解析器 `A55A55AAParser.h`、`StartEndParser.h`
- `tests/`：基类与两个示例解析器的单元测试、日志冒烟测试、TCP 与门面层 e2e 测试，
  以及用 `find_package(sp)` 消费安装包的 `tests/consumer` 工程
- CI：Linux x64 / x86 / arm64 / armv7（qemu）、Windows x64 / arm64 原生测试，
  外加安装包消费验证作业

### Changed

- 项目名与 GitHub 仓库名由 `StreamParserDemo` 改为 `StreamParser`
- 公开头文件迁至 `include/sp/`，下游统一以 `#include <sp/...>` 引用
- 原单体库拆分为 `sp_core` 与 `sp_transport` 两个目标
- 示例代码从仓库根目录移入 `examples/`，测试移入 `tests/`
- 重写 `README.md`：新增「设计思想」，明确 `A55A55AAParser.h` / `StartEndParser.h`
  只是示例、不随包安装；补充 `find_package(sp)` 用法与构建选项
- `LICENSE` 补全版权持有者（`Copyright (c) 2026 Leid`）

### Removed

- 根目录散落的 `*_main.cc` 示例源文件，以及 `inc/`、`test/` 目录布局
- 未使用的 `ASIO_NO_DEPRECATED` 编译定义
- 公开构建中对 MSVC `/W3` 的硬编码（统一由库自身的告警设置给定）
