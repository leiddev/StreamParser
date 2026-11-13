# StreamParser

[![CI](https://github.com/leiddev/StreamParser/actions/workflows/ci.yml/badge.svg)](https://github.com/leiddev/StreamParser/actions/workflows/ci.yml)

基于 ASIO 的流式协议解析库：库负责"字节流 → 帧"和通信，**协议由你写**。
支持串口、TCP（服务端/客户端）、UDP，C++14，MIT。

## 设计思想

这个库要解决的问题是：**字节流不是消息**。TCP 会拆包粘包，串口会丢字节，UDP 会乱序，
而每一台设备的报文格式都不一样。库里唯一不该做的事，就是猜你的协议。

所以它只做三件事，并且把协议留给你：

1. **帧边界由你做主。** 从 `sp::StreamParser` 派生一个类，实现 `processByte()`
   ——一个字节进来，你的状态机走到哪一步。库负责喂字节、维护缓冲区、执行超时与最大帧长保护，
   并在你判定"这一帧完整了"时，通过 `completeFrame(offset, size)` 把 payload 交回上层。
   发方向对称：`buildFrame(payload)` 由你决定怎么成帧。
2. **解析器实例通过工厂注入，而不是写死在库里。** `TransportConfig` 里放的是
   `ParserFactory = std::function<std::unique_ptr<StreamParser>()>`，每建立一条链路调用一次，
   所以每个连接/每台设备都有自己的解析状态。库永远不知道 `A5 5A` 是什么意思。
3. **通信与业务之间只有回调。** 门面把 `asio::io_context`、socket、会话表、写队列都收在内部，
   业务侧只看到 `on_frame` / `on_link_up` / `on_link_down` / `on_error` 四个回调。

一句话：**库是管道，协议是你的。** 由此推出两个结论：

- `examples/A55A55AAParser.h` 和 `examples/StartEndParser.h` 是**示例，不是库的一部分**。
  它们演示两种常见的帧边界（定长头 + 异或校验 + 固定尾；起止符），需要时把整个文件复制进你的工程，
  改成你的协议即可。库对它们不做任何承诺，安装包里也不会有它们。
- 下游工程通过**第三方库**的方式使用本项目（`find_package(sp)` 或 `add_subdirectory`），
  不要把库源码复制进业务侧。要复制进业务侧的，只有你自己的 `xxxParser.h`。

## 核心特性

- **零协议假设**：核心（`sp::core`）不依赖任何第三方库，也不认识任何具体的帧格式
- **可插拔解析器**：实现 `processByte()` / `buildFrame()` 两个函数就能接入，其余由基类提供
- **统一的传输门面**：串口 / TCP 服务端 / TCP 客户端 / UDP 一套配置、一套回调
- **异步 I/O**：基于 ASIO，写操作经 `asio::post` 转到 io 线程，调用线程不被阻塞
- **粘包拆包、超时、背压、最大帧长**：基类与门面内建，不需要每个协议重写一遍
- **可安装的 CMake 包**：`find_package(sp 1.0 CONFIG REQUIRED)` + `sp::core` / `sp::transport`
- **版本可查**：`#include <sp/version.hpp>`，有 `SP_VERSION_STRING`、`SP_VERSION_MAJOR/MINOR/PATCH` 与 `sp::version()`

## 目录结构

```
StreamParser/
├── include/sp/                   # 公开头文件（安装后即 <prefix>/include/sp）
│   ├── StreamParser.h            # 解析器基类：唯一需要继承的扩展点
│   ├── StreamParserLog.h         # 可插拔日志回调接口（接入宿主日志后端）
│   ├── Transport.h               # 统一传输门面（不含 asio，推荐入口）
│   ├── SerialParserAdapter.h     # 串口适配器
│   ├── SerialTypes.h             # 串口相关类型定义
│   ├── TcpClientParserAdapter.h  # TCP 客户端适配器
│   ├── TcpSessionParserAdapter.h # TCP 会话适配器
│   ├── TcpParserServer.h         # TCP 服务器
│   ├── TcpTypes.h                # TCP 相关类型定义
│   ├── UdpParserAdapter.h        # UDP 适配器
│   └── UdpTypes.h                # UDP 相关类型定义
├── src/                          # sp::core 与 sp::transport 的实现
│   ├── StreamParser.cc           # 解析器基类实现
│   ├── TransportImpl.h           # 门面内部基类（asio 细节只在此处出现）
│   ├── TransportBase.cc          # 公共 io/线程/发送队列实现
│   ├── TransportFactory.cc       # CreateTransport（只校验配置，不绑定协议）
│   ├── TransportSerial.cc        # 串口后端
│   ├── TransportTcpServer.cc     # TCP 服务器后端
│   ├── TransportTcpClient.cc     # TCP 客户端后端
│   └── TransportUdp.cc           # UDP 后端
├── examples/                     # 示例：参与构建与 CI，但从不安装
│   ├── A55A55AAParser.h          # 示例协议解析器（A5 5A ... 55 AA）
│   ├── StartEndParser.h          # 示例协议解析器（0x02 ... 0x00 0x00 0x03）
│   ├── serial_main.cc            # 串口通信示例
│   ├── tcp_server_main.cc        # TCP 服务器示例
│   ├── tcp_client_main.cc        # TCP 客户端示例
│   ├── udp_main.cc               # UDP 通信示例
│   └── transport_main.cc         # 统一门面示例（四种模式共用一个程序）
├── tests/
│   ├── stream_parser_test.cc     # 基类行为：粘包/拆包/超时/窗口/错误码
│   ├── a55a55aa_parser_test.cc   # 示例解析器 A55A55AAParser
│   ├── start_end_parser_test.cc  # 示例解析器 StartEndParser
│   ├── log_smoke_test.cc         # 日志设施冒烟测试
│   ├── transport_facade_test.cc  # 门面可靠性：TCP 服务端/客户端、UDP
│   ├── transport_serial_test.cc  # 门面可靠性：串口（真实收发需串口对）
│   ├── e2e_tcp_test.py           # TCP 端到端测试
│   ├── e2e_transport_test.py     # 门面层端到端测试
│   ├── support/FakeTransport.h   # 测试桩（仅测试使用，不是公开 API）
│   ├── support/TransportTestKit.h # 门面测试的回调记录器与测试用解析器
│   └── consumer/                 # 独立工程：用 find_package(sp) 消费安装包
├── cmake/
│   ├── sp-config.cmake.in        # find_package(sp) 的配置模板
│   └── version.hpp.in            # 生成 sp/version.hpp
├── docs/
└── 3rdparty/asio/                # 随安装包一起装到 include/sp/asio
```

`examples/A55A55AAParser.h` 与 `examples/StartEndParser.h` **不是库的一部分**：它们是"照着自己的协议写一个解析器"的参考答案。它们参与构建与单元测试，以免腐烂，需要时直接复制到自己的工程里改成 `xxxParser.h`。库本身只提供基类与传输设施，不认识任何具体帧格式。

公开 API 分属两个命名空间：核心与传输设施在 `namespace sp`（`StreamParser`、`Transport`、`TransportConfig`、`TransportHandler`、`CreateTransport`，以及 `SerialParserAdapter`、`TcpParserServer` 等适配器与其配置/结果类型），日志接口在 `namespace splog`（内部辅助代码在 `splog::detail`）。示例解析器与测试桩 `FakeTransport` 同样在 `namespace sp`，但它们不属于安装内容。头文件中请使用 `sp::` 限定，示例程序在文件开头 `using namespace sp;`。

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

### 2. 具体协议解析器：由你实现

库**不附带任何协议实现**——`StreamParser` 的子类就是协议与库之间唯一的契约。仓库里的两个示例放在 `examples/`（参与构建与测试，但从不安装），可作为"怎么把一段协议写成状态机"的参考：

| 示例文件 | 协议 |
|----------|------|
| `examples/A55A55AAParser.h` | `A5 5A` 帧头 + 大端长度 + 载荷 + 异或校验 + `55 AA` 帧尾 |
| `examples/StartEndParser.h` | `0x02` 起始 + 转义载荷 + `0x00 0x00 0x03` 结束 |

A55A55AA 示例的帧格式：

```
| 字段      | 长度 | 说明                    |
|-----------|------|------------------------|
| 帧头      | 2    | 固定 0xA5 0x5A         |
| 帧长度    | 2    | 大端序，为 payload + 校验和 + 尾部长度 |
| 载荷      | N    | 可变长度                |
| 校验和    | 1    | 帧头 + 长度 + 载荷的异或校验 |
| 帧尾      | 2    | 固定 0x55 0xAA          |
```

把它们复制到自己的工程里改，或按同样的形状写 `MyParser`。库的 CI 会编译并单测这两个示例，所以它们不会随版本漂移而失效。

### 3. 适配器层

| 适配器 | 说明 |
|--------|------|
| SerialParserAdapter | 串口通信，支持波特率、数据位、校验位等配置 |
| TcpClientParserAdapter | TCP 客户端，支持连接/断开回调、keepalive |
| TcpSessionParserAdapter | TCP 会话，配合 TcpServer 管理多连接 |
| UdpParserAdapter | UDP 通信，支持绑定本地端口和发送目标端口 |

### 4. Transport（统一传输门面，推荐用法）

适配器层把 `asio::io_context`、socket、会话表和写队列全部暴露给调用方，业务对象因此不得不自己持有 io 线程与连接管理。`<sp/Transport.h>` 把这些细节收进一个**不依赖 asio 的**接口：业务层只描述"要哪种传输 + 收到帧/链路变化怎么处理"，其余全部由门面内部完成。

实现上是 **Facade + Abstract Factory**：`CreateTransport()` 是唯一入口，按 `TransportConfig::kind` 选择实现；四个后端共同继承内部的模板方法基类（`sp::detail::TransportBase`），因此线程、发送队列、回调封送只有一份代码。

```cpp
#include <sp/Transport.h>       // 不含 asio

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
    // A55A55AAParser 来自 examples/，实际工程里换成你自己的 MyParser
    return std::unique_ptr<sp::StreamParser>(
        new sp::A55A55AAParser(tag, cfg.frame_timeout_ms, cfg.max_frame_bytes));
};
auto transport = sp::CreateTransport(cfg, std::move(factory), handler);
if (!transport->start()) { /* 配置/绑定失败 */ }

transport->send("10.0.0.9:9000", payload);  // 按 tag 定位链路，门面负责成帧
transport->broadcast(payload);        // 发给所有在线链路
transport->send_to("10.0.0.9", 9000, payload);   // 仅 UDP

transport->stop();                    // 关闭链路并 join io 线程
```

约定：

| 约定 | 说明 |
|------|------|
| 一个传输一个 io 线程 | 线程、`io_context`、每链路发送队列都在门面内部创建 |
| 回调都在 io 线程上 | 需要访问业务状态时请自行入队/转发（线程安全队列、`asio::post` 等） |
| `stop()` 不能在回调里调用 | 它会 join io 线程；请从外部线程调用 |
| `send()` 收 payload | 成帧由门面内部的 parser 完成，业务层不再需要 `buildFrame` |
| 每链路一个 parser | 解析器带帧缓存与超时状态，因此用 `ParserFactory` 而不是原型克隆 |
| `max_links` / `max_pending_frames` | 分别用于连接数准入和每链路发送队列的背压（0 表示不限连接数） |
| `log_link_events` | 是否由门面自己打印链路上下线日志，默认 `true`；宿主若已在 `on_link_up` / `on_link_down` 里记录，可置 `false` 避免同一事件打印两行（启动、错误、重连日志不受影响） |
| `keep_alive` / `keepalive_idle_seconds` / `keepalive_interval_seconds` / `keepalive_probes` | TCP keepalive（仅 `tcp_server` 的已接受连接与 `tcp_client`），默认关闭 / 30s / 5s / 3 次；用于识别"对端静默消失"（掉电、拔线）——不开启时只能等下一次写失败才能发现。按 socket 逐连接设置（Windows 走 `SIO_KEEPALIVE_VALS`，Linux 走 `TCP_KEEPIDLE/KEEPINTVL/KEEPCNT`） |
| `link_idle_timeout_ms` | 链路空闲超时：TCP 连接在这么久**没有收到任何字节**就被主动关闭，默认 `0`（关闭）。与 keepalive 互补——只要不发数据也能发现"可达但对端卡死"（重启中的进程、半开代理）；仅 `tcp_server` / `tcp_client` 生效 |

`kind` 与 `tag` 的对应关系：串口用设备名；TCP 服务器用客户端的 `ip:port`；TCP 客户端用服务器 `host:port`；UDP 用对端 `ip:port`（另外还有 `udp:<port>` 代表 socket 自身）。UDP 默认发往最近一次收到数据的对端，`send_to()` 可指定目标。

`tests/support/FakeTransport.h` 提供一个 header-only 的测试桩：它记录每次 `send()`/`broadcast()`/`send_to()` 的内容，并支持注入帧、链路上下线、错误以及让 `start()` 失败，便于对业务层做单元测试；它同时也可作为 `Transport` 最小契约的参考实现。它只在 `SP_BUILD_TESTS` 下使用，不随安装包发布。

`examples/transport_main.cc`（可执行文件 `TransportDemo`）是门面层的示例：同一份业务代码用 `tcp_server`、`tcp_client`、`udp`、`serial` 四种模式运行，程序中不出现任何 asio 类型。

## 快速开始

### 作为第三方库使用（推荐）

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
      -DSP_BUILD_EXAMPLES=OFF -DSP_BUILD_TESTS=OFF
cmake --build build --config Release
cmake --install build --config Release --prefix ./install
```

安装结果：`include/sp/*.h`、`include/sp/asio/`、`include/sp/version.hpp`、`lib/` 下的 `sp_core`/`sp_transport`、以及 `lib/cmake/sp/spConfig.cmake`。下游工程里：

```cmake
find_package(sp 1.0 CONFIG REQUIRED)   # 指向 -DCMAKE_PREFIX_PATH=.../install

add_executable(my_app main.cc)
target_link_libraries(my_app PRIVATE sp::transport)   # 只用解析器基类则链接 sp::core
```

`tests/consumer/` 就是一个这样消费安装包的最小工程，CI 的 `package` 作业会构建、安装再编译它，作为"包是可用的"这条断言的执行者。

### 编译示例与测试

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --config Release
ctest --test-dir build --output-on-failure
```

默认 `SP_BUILD_EXAMPLES` / `SP_BUILD_TESTS` 都是 `ON`，会生成 `build/examples/` 下的五个示例程序（`SerialDemo`、`TcpServerDemo`、`TcpClientDemo`、`UdpDemo`、`TransportDemo`）以及 `build/tests/` 下的单元测试与 e2e 测试。库本身不产生可执行文件。

只想手工编译一个示例也可以：

```bash
g++ -std=c++14 -I./include -I./examples -I./3rdparty/asio/include \
    examples/serial_main.cc src/*.cc -o SerialDemo -lpthread
```

### 运行示例

示例程序在构建目录的 `examples/` 下（如 `build/examples/`）。下面的命令假设已切换进去。

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

> 下文示例省略了命名空间前缀：所有适配器、相关类型与解析器都在 `namespace sp` 中，实际代码请写 `sp::SerialParserAdapter`，或在源文件中 `using namespace sp;`。示例中的 `A55A55AAParser` 来自 `examples/`，实际工程里请换成你自己的 `MyParser`。

1. **包含头文件**

```cpp
#include <sp/SerialParserAdapter.h>   // 适配器层会带出 asio 类型
#include <sp/SerialTypes.h>
#include "MyParser.h"                 // 你自己的协议解析器
```

2. **创建 io_context**

```cpp
asio::io_context io;
```

3. **创建协议解析器**

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

4. **创建适配器**

```cpp
SerialConfig config;
config.baud_rate = 115200;
config.parity = SerialConfig::Parity::None;

auto adapter = std::make_unique<SerialParserAdapter>(io, port_name, config, std::move(parser));
```

5. **异步发送数据**

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

6. **启动事件循环**

```cpp
io.run();
```

### 自定义协议解析器

继承 `sp::StreamParser`，实现两个纯虚函数，就得到了一台完整的协议编解码器（粘包拆包、超时、最大帧长保护都由基类处理）：

```cpp
#include <sp/StreamParser.h>

class MyParser : public sp::StreamParser {
public:
    explicit MyParser(const std::string& tag, uint32_t timeout_ms = 300,
                      std::size_t max_frame_size = 2048)
        : StreamParser(tag, timeout_ms, max_frame_size) {}

protected:
    // 收方向：每收到一个字节调用一次，状态机由派生类维护
    void processByte(uint8_t byte) override {
        switch (m_state) {
        case 0:                                  // 等帧头
            if (byte == 0xAA) {
                m_buf.assign(1, byte);
                m_state = 1;
            }
            break;
        case 1:                                  // 收载荷
            m_buf.push_back(byte);
            if (m_buf.size() == 2 + m_expected_len) {   // 头 + 载荷 + 校验
                if (checksum_ok(m_buf)) {
                    completeFrame(1, m_expected_len);   // 交出 payload，自动回到空闲态
                } else {
                    reportError(ErrorCode::ChecksumFail, "xor mismatch");
                    reset();                            // 坏帧：丢弃缓冲，回到 case 0
                }
            }
            break;
        }
    }

    // 发方向：把业务 payload 编成完整帧；返回空 vector 表示这条数据发不出去
    std::vector<uint8_t> buildFrame(const std::vector<uint8_t>& payload) override {
        std::vector<uint8_t> frame;
        frame.push_back(0xAA);
        frame.insert(frame.end(), payload.begin(), payload.end());
        frame.push_back(xor_of(payload));
        return frame;
    }

private:
    static bool checksum_ok(const std::vector<uint8_t>&) { return true; }  // 略
    static uint8_t xor_of(const std::vector<uint8_t>&) { return 0; }       // 略
};
```

几条契约值得记住：

- `completeFrame()` 通知上层，并**自动**清空缓冲、回到空闲态；`completeFrame(offset, size)` 只交出一段窗口；
- `reset()` 只清协议内部状态（缓冲/状态机/超时），**不动** `setFrameCallback()`、`setErrorCallback()` 装进来的回调，所以适配器 `close()`/`open()` 之后回调依然有效；
- 帧长超过 `max_frame_size` 时由基类报 `BufferOverflow` 并丢弃，不需要你检查；
- `updateActivity()` 在收到数据时刷新时间戳，`isTimeout()` 可查当前是否已超时；帧边界超时（`timeout_ms` 内没凑齐一帧）由适配器/门面定时调用 `checkTimeout()` 触发 `Timeout` 错误。

完整可运行的实现见 `examples/A55A55AAParser.h` 与 `examples/StartEndParser.h`。

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

库内部不直接向 `std::cout` / `std::cerr` 打印。所有诊断信息统一通过
`<sp/StreamParserLog.h>` 提供的日志回调接口输出，宿主工程可以把它们转发到自己的
日志后端（spdlog、glog、log4cxx、文件日志……）。该头文件是 header-only 的，
随 `sp::core` 一起安装，不需要 asio。

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
#include <sp/StreamParserLog.h>

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

`tests/log_smoke_test.cc` 是这套日志设施的冒烟测试（ctest 名 `log_smoke`，可执行文件 `LogSmokeTest`），
覆盖回调安装、级别过滤、静音、异常隔离与 stderr 兜底。

## 持续集成

`.github/workflows/ci.yml` 在 `push` / `pull_request` / 手动触发时，覆盖以下平台：

| Job | 平台 | 工具链 | 测试 |
|-----|------|--------|------|
| `native` / `linux-x64` | Ubuntu x64 | GCC，`-Wall -Wextra` | 全量 `ctest` |
| `native` / `linux-arm64` | Ubuntu ARM64 原生 runner | GCC | 全量 `ctest` |
| `native` / `windows-x64` | Windows x64 | MSVC（VS 2022，`/W4 /utf-8`） | 全量 `ctest -C Release` |
| `native` / `windows-arm64` | Windows 11 ARM64 原生 runner | MSVC ARM64 | 全量 `ctest -C Release` |
| `linux-x86` | Ubuntu，32 位 x86 | GCC `-m32` + multilib | 全量 `ctest` |
| `linux-armv7` | 交叉编译 + `qemu-arm-static` | arm-linux-gnueabihf | `LogSmokeTest` 在 qemu 下运行 |
| `package` | Ubuntu x64 | GCC | 安装到临时前缀，再用独立工程 `find_package(sp 1.0)` 编译并运行 |

CI 中会编译并运行：

- `stream_parser` —— 基类行为：逐字节输入、粘包/拆包、帧超时、最大帧长窗口、错误码上报、`reset()` 后恢复；
- `a55a55aa_parser` —— `examples/A55A55AAParser.h`（帧头尾/长度/校验、坏帧恢复）；
- `start_end_parser` —— `examples/StartEndParser.h`（起止符 + 转义、噪声丢弃）；
- `log_smoke` —— 日志回调设施（安装/过滤/静音/异常隔离/stderr 兜底）；
- `transport_facade` —— 门面可靠性：TCP 服务端/客户端的粘包、拆包、帧超时、校验失败、
  tag 路由、`broadcast()`、发送预算（`-2`）、`max_links`（`-6`）、写失败只报一次、
  `start()` 失败（`-5`）、重连与空闲超时，以及 UDP 的多 peer 路由、`send_to()`
  （含主机名解析与 `-7`）、`max_links`。对端用裸 asio socket，不需要演示程序；
- `transport_serial` —— 串口门面：`device` 为空时 `start()` 失败（`-3`）；设备不存在时
  `start()` 仍返回 true 并在后台重试（`-4`）。真实收发用例需要一对互通的串口，设置
  `SP_TEST_SERIAL_PORT` 与 `SP_TEST_SERIAL_PORT_PEER`（如 `COM1` / `COM2`）才运行，
  否则打印 SKIP——CI runner 上没有串口对，所以这一步在 CI 里是 SKIP；
- `tcp_e2e` —— 启动 `TcpServerDemo`，喂入一个合法 `A5 5A` 帧，校验帧重组回调触发；
- `transport_e2e` —— 启动 `TransportDemo`，走统一门面验证收发与链路回调。

另外 **本项目自身代码（`include/ src/ tests/ examples/`）的编译告警视为失败**（第三方 asio 的告警被忽略），保证多平台下维持零告警；MSVC 下用 `/W4`（asio 单独以 INTERFACE 目标承载，不向本库传染告警级别）。

本地跑同样的检查：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_FLAGS="-Wall -Wextra"
cmake --build build -j
ctest --test-dir build --output-on-failure

# 有一对互通串口时，顺带跑串口的真实收发：
# Windows（COM1 与 COM2 交叉连接）
set SP_TEST_SERIAL_PORT=COM1
set SP_TEST_SERIAL_PORT_PEER=COM2
# Linux（socat 造一对 pty 后填各自的路径）
# export SP_TEST_SERIAL_PORT=/dev/pts/3 SP_TEST_SERIAL_PORT_PEER=/dev/pts/4
```

## 依赖

- C++14 编译器（GCC 5+ / Clang 3.4+ / MSVC 2015+）
- 线程库（Linux 上由 CMake 自动加 `Threads::Threads`）
- **ASIO 随包提供**：仓库内的 `3rdparty/asio/include` 用于构建，`cmake --install` 会把它一并装到 `include/sp/asio`。下游 `find_package(sp)` 时由 `sp::asio` 这个 INTERFACE 目标自动带上 include 路径与编译定义（`ASIO_STANDALONE`、Windows 下的 `_WIN32_WINNT`），不需要自己找 asio，也不会和 Boost 的 asio 冲突。

## 变更记录

见 [CHANGELOG.md](CHANGELOG.md)，版本号遵循语义化版本。

## 许可证

MIT，见 [LICENSE](LICENSE)。可以自由使用、修改、再分发，包括商业用途，只需保留版权声明
（`Copyright (c) 2026 Leid`）。

ASIO 以 Boost Software License 1.0 分发，见 `3rdparty/asio/LICENSE_1_0.txt`。
