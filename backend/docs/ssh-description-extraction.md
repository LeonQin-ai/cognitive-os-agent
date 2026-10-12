# SSH 登录描述提取（#110）

在请求排队、写日志和发送给模型之前，本地入口提取连接字段和密码。
固定正则处理空白/换行、分隔符、英文 is、用户名字符和 IPv4；标签支持
host/server/ip/主机/服务器/地址、user/username/account/用户/用户名/账号/帐号/账户、
password/passwd/pwd/密码/口令。中文标签支持“是”“为”和全角冒号。
支持字段换序、`user@host`、`-l user`、`-p port` 和只有字段的描述。

示例（密码是测试数据）：

```text
请登录 127.0.0.1，账户为 deploy，口令为 P@!#$%^&*()!，查看 CPU
```

```text
IP：127.0.0.1
用户名是 root
密码：'A b,;@!123'
端口为 2222
```

密码按原始字节进入本地 vault，对模型输入替换为 `[LOCAL_SSH_PASSWORD]`。
密码含空格、逗号或分号时需要成对单/双引号；引号是边界，不做 shell 转义。
端点扫描基于脱敏后的文本，密码内的 IP、`user=`、`-p` 不作为连接参数。
裸地址提取支持 IPv4；域名和 IPv6 继续使用显式字段或已有连接配置。
多于一个不同的 IPv4、非法数字地址/用户名/端口、不同的重复密码、未闭合密码引号
会拒绝该描述并要求明确输入。正则不会猜测没有标签的密码或所有自然语言表达。

正则引擎来自 tiny-regex-c，保留 Unlicense，编译缓冲区改为 C11 线程局部存储；
只执行代码内的固定表达式，没有新增模型可传入任意正则的接口。

验证：`cognitive-os-agent-test --ssh-prompt` 覆盖 10 种有效描述及 8 种拒绝场景，
并逐字核对 vault 密码、profile 和脱敏结果。完整单元套件也执行这些用例。
`python tests/test_ssh_password_e2e.py` 使用真实 OpenSSH、原生 askpass 和本地
Paramiko 服务，入口使用“账户为/口令/换行”描述，验证特殊字符密码实际认证。
Windows CI 显式执行上述两项，Linux/macOS/ASAN 执行包含解析用例的完整单元套件。
