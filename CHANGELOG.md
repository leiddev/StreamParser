# Changelog

本项目的所有重要变更都记录在此文件。

格式参考 [Keep a Changelog](https://keepachangelog.com/zh-CN/1.1.0/)，
版本号遵循 [语义化版本](https://semver.org/lang/zh-CN/)。

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
