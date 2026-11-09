# StreamParser

[![CI](https://github.com/leiddev/StreamParser/actions/workflows/ci.yml/badge.svg)](https://github.com/leiddev/StreamParser/actions/workflows/ci.yml)

基于 ASIO 的流式数据解析框架，支持串口、TCP、UDP 多种通信方式。

## 项目概述

StreamParser 是一个轻量级的流数据解析框架，封装了 ASIO 网络库，提供统一的异步 I/O 接口。框架采用**适配器模式**，将底层通信细节与上层业务逻辑解耦，用户只需关注协议解析本身。

### 核心特性

- **统一接口**：所有适配器（串口/TCP/UDP）提供一致的 API 设计
- **异步 I/O**：基于 ASIO 的异步读写，不阻塞调用线程
- **帧解析**：内置状态机支持多种协议的帧边界检测
- **超时检测**：周期性检查帧接收超时
- **线程安全**：写操作通过 `asio::post` 转发到 io_context 线程

## 目录结构

```
StreamParserDemo/
├── inc/                          # 头文件目录
│   ├── StreamParser.h           # 解析器基类
│   ├── A55A55AAParser.h         # A5 5A ... 55 AA 协议解析器
│   ├── StartEndParser.h         # 通用起止符协议解析器（0x02 ... 0x00 0x00 0x03）
│   ├── SerialParserAdapter.h    # 串口适配器
│   ├── SerialTypes.h            # 串口相关类型定义
│   ├── TcpClientParserAdapter.h # TCP 客户端适配器
│   ├── TcpSessionParserAdapter.h# TCP 会话适配器
│   ├── TcpParserServer.h        # TCP 服务器
│   ├── TcpTypes.h               # TCP 相关类型定义
│   ├── UdpParserAdapter.h       # UDP 适配器
│   ├── UdpTypes.h               # UDP 相关类型定义
│   ├── Transport.h              # 统一传输门面（不含 asio，推荐入口）
│   ├── FakeTransport.h          # 测试桩传输（记录发送、注入事件）
│   └── StreamParserLog.h        # 可插拔日志回调接口（宿主接入自己的日志后端）
├── src/
│   ├── StreamParser.cc          # 解析器基类实现
│   ├── TransportImpl.h          # 门面内部基类（asio 细节只在此处出现）
│   ├── TransportBase.cc         # 公共 io/线程/发送队列实现
│   ├── TransportFactory.cc      # CreateTransport（只校验配置，不绑定协议）
│   ├── TransportSerial.cc       # 串口后端
│   ├── TransportTcpServer.cc    # TCP 服务器后端
│   ├── TransportTcpClient.cc    # TCP 客户端后端
│   └── TransportUdp.cc          # UDP 后端
├── test/
│   ├── log_smoke_test.cc        # 日志设施冒烟测试
│   └── e2e_transport_test.py    # 门面层 e2e 测试
├── serial_main.cc               # 串口通信示例
├── tcp_server_main.cc           # TCP 服务器示例
├── tcp_client_main.cc           # TCP 客户端示例
├── udp_main.cc                  # UDP 通信示例
└── transport_main.cc            # 统一门面示例（四种模式共用一个程序）
```

所有公开类型——适配器（`SerialParserAdapter`、`TcpParserServer` 等）、它们的配置/回调类型（`SerialConfig`、`TcpConfig`、`TcpWriteResult` 等）、解析器（`StreamParser`、`A55A55AAParser`、`StartEndParser`）以及统一门面（`Transport`、`TransportConfig`、`TransportHandler`、`FakeTransport`、`CreateTransport`）——都定义在 `namespace sp` 中；示例程序在文件开头 `using namespace sp;`，头文件中请使用 `sp::` 限定。日志 shim 独立使用 `namespace splog`（内部辅助代码在 `splog::detail`）。

## 核心组件

### 1. StreamParser（解析器基类）

所有协议解析器的基类，管理帧解析的通用逻辑：

```cpp
class StreamParser {
public:
    // 设置帧完成回调
    void setFrameCallback(FrameCallback cb);
    // 设置错误回调
    void setErrorCallback(ErrorCallback cb);
    // 喂入数据（自动处理粘包/拆包）
    void feed(const uint8_t* data, size_t size);
    // 周期性调用检测超时（建议 20~50ms）
    void checkTimeout();
    // 重置解析器状态
    void reset();
    // 构建发送帧
    virtual std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) = 0;
};
```

### 2. A55A55AAParser（协议解析器）

实现 A5 5A ... 55 AA 格式的协议解析：

```
帧格式：
| 字段      | 长度 | 说明                    |
|-----------|------|------------------------|
| 帧头      | 2    | 固定 0xA5 0x5A         |
| 帧长度    | 2    | 大端序，为 payload + 校验和 + 尾部长度 |
| 载荷      | N    | 可变长度                |
| 校验和    | 1    | 帧头 + 长度 + 载荷的异或校验 |
| 帧尾      | 2    | 固定 0x55 0xAA          |
```

### 3. 适配器层

| 适配器 | 说明 |
|--------|------|
| SerialParserAdapter | 串口通信，支持波特率、数据位、校验位等配置 |
| TcpClientParserAdapter | TCP 客户端，支持连接/断开回调、keepalive |
| TcpSessionParserAdapter | TCP 会话，配合 TcpServer 管理多连接 |
| UdpParserAdapter | UDP 通信，支持绑定本地端口和发送目标端口 |

### 4. Transport（统一传输门面，推荐用法）

适配器层把 `asio::io_context`、socket、会话表和写队列全部暴露给调用方，业务对象因此不得不自己持有 io 线程与连接管理。`Transport.h` 把这些细节收进一个**不依赖 asio 的**接口：业务层只描述"要哪种传输 + 收到帧/链路变化怎么处理"，其余全部由门面内部完成。

实现上是 **Facade + Abstract Factory**：`CreateTransport()` 是唯一入口，按 `TransportConfig::kind` 选择实现；四个后端共同继承内部的模板方法基类（`sp::detail::TransportBase`），因此线程、发送队列、回调封送只有一份代码。

```cpp
#include "Transport.h"          // 不含 asio

sp::TransportConfig cfg;
cfg.kind = sp::TransportKind::TcpServer;
cfg.name = "collector";
cfg.bind_address = "0.0.0.0";
cfg.port = 1883;
cfg.max_links = 8;              // 超出时拒绝接入并上报 on_error

sp::TransportHandler handler;
handler.on_frame = [](const std::string& tag, const uint8_t* data, std::size_t len) {
    // 已由解析器解帧，data 是 payload
};
handler.on_link_up   = [](const std::string& tag) { /* 入队 */ };
handler.on_link_down = [](const std::string& tag, const std::string& why) { /* 入队 */ };
handler.on_error     = [](const std::string& tag, int code, const std::string& detail) { /* 入队 */ };

// 协议由宿主注入：门面本身不认识任何具体解析器
auto factory = [](const std::string& tag) {
    return std::unique_ptr<sp::StreamParser>(
        new sp::A55A55AAParser(tag, cfg.frame_timeout_ms, cfg.max_frame_bytes));
};
auto transport = sp::CreateTransport(cfg, std::move(factory), handler);
if (!transport->start()) { /* 配置/绑定失败 */ }

transport->send(tag, payload);        // 按 tag 定位链路，门面负责成帧
transport->broadcast(payload);        // 发给所有在线链路
transport->send_to("10.0.0.9", 9000, payload);   // 仅 UDP

transport->stop();                    // 关闭链路并 join io 线程
```

约定：

| 约定 | 说明 |
|------|------|
| 一个传输一个 io 线程 | 线程、`io_context`、每链路发送队列都在门面内部创建 |
| 回调都在 io 线程上 | 需要访问业务状态时请自行入队/转发（`FrameQueue`、`asio::post` 等） |
| `stop()` 不能在回调里调用 | 它会 join io 线程；请从外部线程调用 |
| `send()` 收 payload | 成帧由门面内部的 parser 完成，业务层不再需要 `buildFrame` |
| 每链路一个 parser | 解析器带帧缓存与超时状态，因此用 `ParserFactory` 而不是原型克隆 |
| `max_links` / `max_pending_frames` | 分别用于连接数准入和每链路发送队列的背压（0 表示不限连接数） |
| `log_link_events` | 是否由门面自己打印链路上下线日志，默认 `true`；宿主若已在 `on_link_up` / `on_link_down` 里记录，可置 `false` 避免同一事件打印两行（启动、错误、重连日志不受影响） |
| `keep_alive` / `keepalive_idle_seconds` / `keepalive_interval_seconds` / `keepalive_probes` | TCP keepalive（仅 `tcp_server` 的已接受连接与 `tcp_client`），默认关闭 / 30s / 5s / 3 次；用于识别"对端静默消失"（掉电、拔线）——不开启时只能等下一次写失败才能发现。按 socket 逐连接设置（Windows 走 `SIO_KEEPALIVE_VALS`，Linux 走 `TCP_KEEPIDLE/KEEPINTVL/KEEPCNT`） |
| `link_idle_timeout_ms` | 链路空闲超时：TCP 连接在这么久**没有收到任何字节**就被主动关闭，默认 `0`（关闭）。与 keepalive 互补——只要不发数据也能发现"可达但对端卡死"（重启中的进程、半开代理）；仅 `tcp_server` / `tcp_client` 生效 |

`kind` 与 `tag` 的对应关系：串口用设备名；TCP 服务器用客户端的 `ip:port`；TCP 客户端用服务器 `host:port`；UDP 用对端 `ip:port`（另外还有 `udp:<port>` 代表 socket 自身）。UDP 默认发往最近一次收到数据的对端，`send_to()` 可指定目标。

`FakeTransport.h` 提供一个 header-only 的测试桩：它记录每次 `send()`/`broadcast()`/`send_to()` 的内容，并支持注入帧、链路上下线、错误以及让 `start()` 失败，便于对业务层做单元测试；它同时也可作为 `Transport` 最小契约的参考实现。

`transport_main.cc` / `TransportDemo` 是门面层的示例：同一份业务代码用 `tcp_server`、`tcp_client`、`udp`、`serial` 四种模式运行，程序中不出现任何 asio 类型。

## 快速开始

### 编译项目

```bash
# 使用 g++ 编译（Linux/macOS）
g++ -std=c++14 -I./inc -I./3rdparty/asio/include \
    serial_main.cc -o SerialDemo -lpthread

# 使用 CMake（推荐）
mkdir build && cd build
cmake ..
make
```

生成 `SerialDemo`、`TcpServerDemo`、`TcpClientDemo`、`UdpDemo`、`TransportDemo` 五个示例程序以及 `log_smoke_test` 测试目标。

### 运行示例

#### 统一门面（四种模式共用一个程序）

```bash
./TransportDemo tcp_server 10050
./TransportDemo tcp_client 127.0.0.1 10050
./TransportDemo udp 9000
./TransportDemo serial COM3 115200
# 运行时输入 "send <text>" 广播文本，"q" 退出
```

#### 串口通信

```bash
# 使用默认配置（COM1, 115200）
./SerialDemo

# 指定端口和波特率
./SerialDemo COM3 9600
./SerialDemo /dev/ttyUSB0 115200
```

#### TCP 服务器

```bash
# 默认监听 9000 端口
./TcpServerDemo

# 指定端口
./TcpServerDemo 8080
```

#### TCP 客户端

```bash
# 连接默认服务器
./TcpClientDemo

# 指定服务器地址
./TcpClientDemo 127.0.0.1 8080
```

#### UDP 通信

```bash
# 仅监听本地端口
./UdpDemo 8000

# 监听并发送到远程端点
./UdpDemo 8000 127.0.0.1 9000
```

## 使用指南

### 基本使用流程

> 下文示例省略了命名空间前缀：所有适配器、相关类型与解析器都在 `namespace sp` 中，实际代码请写 `sp::SerialParserAdapter`，或在源文件中 `using namespace sp;`。

1. **创建 io_context**

```cpp
asio::io_context io;
```

2. **创建协议解析器**

```cpp
auto parser = std::make_unique<A55A55AAParser>(tag, 300, 2048);
parser->setFrameCallback([](const std::string& tag, const uint8_t* data, size_t len) {
    // 处理接收到的完整帧
});
parser->setErrorCallback([](const std::string& tag, StreamParser::ErrorCode code, 
                            const std::string& detail) {
    // 处理解析错误
});
```

3. **创建适配器**

```cpp
SerialConfig config;
config.baud_rate = 115200;
config.parity = SerialConfig::Parity::None;

auto adapter = std::make_unique<SerialParserAdapter>(io, port_name, config, std::move(parser));
```

4. **异步发送数据**

```cpp
std::vector<uint8_t> payload = {0x01, 0x02, 0x03};
adapter->build_and_async_write(payload, [](SerialWriteResult r, size_t n,
                                          const asio::error_code& ec,
                                          const std::string& detail) {
    if (r == SerialWriteResult::Ok) {
        std::cout << "发送成功: " << n << " 字节" << std::endl;
    }
});
```

5. **启动事件循环**

```cpp
io.run();
```

### 自定义协议解析器

继承 `StreamParser` 并实现 `processByte` 方法：

```cpp
class MyParser : public StreamParser {
protected:
    void processByte(uint8_t byte) override {
        switch (m_state) {
        case 0:  // 等待帧头
            if (byte == 0xAA) {
                m_buf.clear();
                m_buf.push_back(byte);
                m_state = 1;
            }
            break;
        case 1:  // 接收数据
            m_buf.push_back(byte);
            if (m_buf.size() >= expected_length) {
                completeFrame();  // 通知上层
            }
            break;
        }
    }
};
```

## 错误处理

### 帧解析错误码

| 错误码 | 说明 |
|--------|------|
| Timeout | 帧接收超时 |
| InvalidHeader | 无效的帧头 |
| InvalidTailer | 无效的帧尾 |
| ChecksumFail | 校验和不匹配 |
| BufferOverflow | 缓冲区溢出 |
| ProtocolError | 其他协议错误 |

### 写操作结果码

| 结果码 | 说明 |
|--------|------|
| Ok | 发送成功 |
| PortClosed | 端口未打开 |
| Cancelled | 操作被取消 |
| IOError | I/O 错误 |
| InvalidArg | 无效参数 |

## 日志

库内部不再直接向 `std::cout` / `std::cerr` 打印。所有诊断信息统一通过
`inc/StreamParserLog.h` 提供的日志回调接口输出，宿主工程可以把它们转发到自己的
日志后端（spdlog、glog、log4cxx、文件日志……）。

### 库内日志点

| 组件 (component) | 级别 | 位置 | 说明 |
|------------------|------|------|------|
| serial | INFO | `SerialParserAdapter::do_open` | 串口打开成功（含波特率） |
| serial | ERROR | `SerialParserAdapter::do_open` | 串口打开 / 参数配置失败（异常仍向上抛出） |
| serial | WARN | `SerialParserAdapter::start_read` | 异步读错误（`operation_aborted` 不算） |
| tcp | WARN | `TcpSessionParserAdapter` 读回调 | 会话读错误，随后触发断开回调 |
| tcp-client | ERROR | `TcpClientParserAdapter::do_connect` | host 不是合法的 IP 字面量 |
| tcp-client | WARN | `TcpClientParserAdapter::on_connect` | 连接失败（含超时被取消） |
| tcp-client | DEBUG | `TcpClientParserAdapter::on_connect` | `TCP_NODELAY` 设置失败 |
| tcp-client | WARN | `TcpClientParserAdapter` 读回调 | 读错误，随后断开 |
| tcp-server | WARN | `TcpServer::listen` | `bind_address` 非法，回退到 `0.0.0.0` |
| tcp-server | ERROR | `TcpServer::listen` | open / bind / listen 失败（`listen()` 返回 false） |
| tcp-server | INFO | `TcpServer::listen` | 开始监听（地址 + 端口） |
| tcp-server | INFO | `TcpServer::stop` | 已停止 |
| tcp-server | ERROR | `TcpServer::do_accept` | accept 失败（`operation_aborted` 除外，随后重新投递 accept） |
| tcp-server | WARN | `TcpServer::do_accept` | 会话工厂返回空 adapter，连接被丢弃 |
| udp | ERROR | `UdpParserAdapter::do_open` | socket 打开 / 绑定失败 |
| udp | INFO | `UdpParserAdapter::do_open` | 绑定成功（含本地 / 远端地址） |
| udp | WARN | `UdpParserAdapter` 接收回调 | 接收错误 |

> 核心 `StreamParser` 不产生任何日志，帧解析错误统一通过 `ErrorCallback` 上报，
> 两者互不干扰。

### 接入宿主日志后端

```cpp
#include "StreamParserLog.h"

// 1) 安装回调（在构造适配器之前调用一次即可，可随时替换 / 清除）
splog::set_callback([](const splog::Record& r) {
    // r.level / r.component / r.message / r.file / r.line
    my_logger.log(to_my_level(r.level), "[{}] {}", r.component, r.message);
});

// 2) 可选：过滤级别（默认 info），Level::off 可完全静音
splog::set_level(splog::Level::debug);

// 3) 可选：关闭"未安装回调时打印到 stderr"的兜底行为
splog::set_stderr_fallback(false);
```

| API | 说明 |
|-----|------|
| `splog::set_callback(cb)` | 安装 / 替换日志回调（传空回调等同清除） |
| `splog::clear_callback()` | 清除回调，恢复 stderr 兜底 |
| `splog::callback()` | 获取当前回调副本（为空表示未安装） |
| `splog::set_level(level)` / `splog::level()` | 设置 / 读取最低级别，默认 `info` |
| `splog::set_stderr_fallback(bool)` | 未安装回调时是否仍打印到 `std::cerr`（默认开） |
| `splog::enabled(level)` | 判断某级别是否会被输出（用于避免无谓的格式化开销） |
| `splog::format(record)` | 把记录格式化为 `[LEVEL] [component] message` |
| `SP_LOG_INFO("comp", ... << x)` | 库内部使用的发射宏（`TRACE/DEBUG/INFO/WARN/ERROR`） |

注意事项：

- 回调在产生日志的线程上被调用（通常是 asio `io_context` 线程），实现需线程安全；
- 回调位于 I/O 路径上，不要做阻塞 / 耗时操作；
- 回调内抛出的异常会被 `write()` 吞掉，不会影响解析器；
- 不同动态库 / 静态库混用时，请确保所有翻译单元包含的是同一份 `StreamParserLog.h`
  （头文件内是 `inline` 函数的函数内静态量，C++14 下跨 TU 唯一）。

`test/log_smoke_test.cc` 是这套日志设施的冒烟测试（`LogSmokeTest` 目标），
覆盖回调安装、级别过滤、静音、异常隔离与 stderr 兜底。

## 持续集成

`.github/workflows/ci.yml` 在 `push` / `pull_request` / 手动触发时，覆盖以下平台：

| Job | 平台 | 工具链 | 测试 |
|-----|------|--------|------|
| `linux-x64` | Ubuntu（PC，x86_64） | GCC，`-Wall -Wextra` | `ctest`（log_smoke + tcp_e2e） |
| `windows-x64` | Windows（PC，x86_64） | MSVC（VS 2022，`/W3`） | `ctest -C Release` |
| `linux-x86` | Ubuntu，32 位 x86 | GCC `-m32` + multilib | `ctest` |
| `linux-arm64` | Ubuntu ARM64 原生 runner | GCC | `ctest` |
| `linux-armv7` | 交叉编译 + `qemu-arm-static` | arm-linux-gnueabihf | `LogSmokeTest` 在 qemu 下运行 |
| `windows-arm64` | Windows 11 ARM64 原生 runner | MSVC ARM64 | `ctest -C Release` |

CI 中会编译并运行：

- `log_smoke` —— 日志回调设施（安装/过滤/静音/异常隔离/stderr 兜底）；
- `tcp_e2e` —— 启动 `TcpServerDemo`，喂入一个合法 `A5 5A` 帧，校验帧重组回调触发。

另外 **本项目自身代码的编译告警视为失败**（第三方 asio 的告警被忽略），保证多平台下维持零告警。

本地跑同样的检查：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## 依赖

- C++14 编译器
- ASIO（已包含在 3rdparty 目录中）

## 许可证

MIT，见 [LICENSE](LICENSE)。可以自由使用、修改、再分发，包括商业用途，只需保留版权声明。

ASIO 以 Boost Software License 1.0 分发，见 `3rdparty/asio/LICENSE_1_0.txt`。
