# RT-Thread + STM32 项目调试全记录：从串口信息到问题定位

## 一、写在前面：为什么要写这份文档

当你拿到一套新的嵌入式开发板，把多个模块（OLED显示、ADC采集、DMA传输）整合到同一个 RT-Thread 工程中时，大概率会遇到各种奇怪的错误——编译通过但运行崩溃、屏幕不亮、甚至不断 Hard Fault。

本项目的调试过程一共经历了 **7 个不同类型的 Bug**，贯穿了 3 次不同的串口/编译输出。这份文档的核心思路是：**每次看到串口输出，不要慌，拆解每一条信息，问自己三个问题**：

1. 这是什么类型的错误？（Hard Fault？外设错误？编译错误？）
2. 错误发生在哪里？（哪个线程？哪个函数？哪个地址？）
3. 最近的代码改动是什么？

以下逐条分析 3 次关键输出，教你如何像侦探一样从信息中推导出根因。

---

## 二、调试环境与工具准备

在开始之前，确保你有以下调试手段：

| 工具 | 用途 |
|------|------|
| **串口终端**（PuTTY / MobaXterm / SSCOM） | 查看 RT-Thread 控制台输出的 rt_kprintf 打印信息 |
| **RT-Thread Studio 调试器**（或 JLink / ST-Link GDB） | 设置断点、单步跟踪、查看寄存器值 |
| **IDE 的反汇编视图** | 当 Hard Fault 时查看 PC 指针落在哪条指令上 |
| **IDE 的编译控制台** | 查看编译错误和 warning，尤其是头文件路径问题 |

> 本工程中，**串口信息和编译日志是定位问题的双眼**。

---

## 三、Bug 1：OLED 头文件缺失

### 3.1 现象

OLED 屏幕始终不显示任何内容，但同样的 OLED 驱动代码在另一个 FreeRTOS 工程中是正常工作的。

### 3.2 排查思路

**第一步：确认驱动代码本身是否有问题。**
既然同样的 .c/.h 文件在 FreeRTOS 工程中正常工作，说明 OLED 驱动逻辑本身没问题。问题一定出在工程环境配置或调用方式上。

**第二步：检查 app_display.c 中是否包含头文件。**
打开 app_display.c，发现它调用了 OLED_Init()、OLED_NewFrame() 等函数，但文件头部只包含了：
```c
#include <middle/mid_databus.h>
// 缺少 #include "oled.h"
```

### 3.3 根因

缺少 #include "oled.h" 导致编译器不知道 OLED 函数的原型，产生隐式声明。隐式声明可能导致函数调用约定不匹配，初始化序列根本没有正确发送出去。

### 3.4 修复方法

```c
#include <middle/mid_databus.h>
#include "oled.h"   // <-- 加上这一行
```

### 3.5 你学到的调试思路

- 同一份代码在不同工程中表现不同，优先怀疑工程配置或文件依赖差异。
- 看到调用了函数但没有 #include，99% 是隐式声明问题。

---

## 四、Bug 2：OLED 初始化时序（I2C 未就绪就通信）

### 4.1 现象

OLED 依然不亮。即使加上了头文件，屏幕还是黑的。

### 4.2 排查思路

**第一步：在 OLED_Send() 中加入 I2C 返回值检查。**
```c
void OLED_Send(uint8_t *data, uint8_t len)
{
    if (HAL_I2C_Master_Transmit(&hi2c1, OLED_ADDRESS, data, len, HAL_MAX_DELAY) != HAL_OK)
    {
        rt_kprintf("[OLED] I2C fail! State=%lu ErrCode=0x%lX\r\n",
                   hi2c1.State, hi2c1.ErrorCode);
    }
}
```

**第二步：观察串口输出。**
如果串口打印出 "[OLED] I2C fail!"，说明 I2C 通信返回了错误。这时就要追问：为什么会失败？I2C 外设初始化了吗？

**第三步：检查 INIT_APP_EXPORT 的执行时机。**
在 RT-Thread 中，`INIT_APP_EXPORT` 宏注册的函数会在**系统启动阶段、main() 函数之前**被自动调用。

而 `MX_I2C1_Init()` 是在 `main()` 函数中调用的。OLED_Init() 在线程启动函数中被调用。线程是在 INIT_APP_EXPORT 阶段创建的，**线程调度器启动后很快就执行入口函数**，此时 main() **很可能还没运行到 MX_I2C1_Init()**。

### 4.3 根因

**I2C 还没初始化，OLED 就去用了**。

### 4.4 修复方法

在线程入口函数的开头加一个延迟：
```c
void Show_thread2_entry(void *parameter)
{
    /* 等待 main() 完成 MX_I2C1_Init() */
    rt_thread_mdelay(20);
    OLED_Init();
    // ...
}
```

### 4.5 你学到的调试思路

- **在串口输出中加入诊断信息**（如 I2C 返回值检查），是定位外设问题的关键第一步。
- **理解 RT-Thread 的自动初始化机制**：INIT_APP_EXPORT 在 main() 之前执行。
- 不要假设某个外设在你想用时已经初始化好了——**用日志验证，而不是用直觉**。

---

## 五、Bug 3：OLED 驱动芯片型号不匹配（CH1116 vs SSD1306）

### 5.1 现象

I2C 通信正常了（不再报错），但 OLED 屏幕仍然不亮。

### 5.2 排查思路

**第一步：检查 I2C 地址。**
常见的 OLED I2C 地址是 0x78（7位地址 0x3C 左移一位），如果代码用了 0x7A（对应 0x3D），很可能就是错的。

**第二步：检查初始化命令序列。**
对照数据手册检查 OLED_Init() 中的命令序列。原代码使用的是 **CH1116** 的初始化序列，但硬件实际是 **SSD1306** 驱动芯片。SSD1306 **必须**发送 0x8D 0x14 来开启电荷泵。

### 5.3 根因

- I2C 地址从 0x7A 改为 0x78
- 初始化命令替换为标准 SSD1306 序列

### 5.4 修复方法

```c
#define OLED_ADDRESS 0x78   // 原来是 0x7A
// 初始化序列改为 SSD1306，关键命令：0x8D 0x14 开启电荷泵
```

### 5.5 你学到的调试思路

- 从网上复制驱动代码时，**一定要确认你的硬件驱动芯片型号**。
- OLED 不亮但 I2C 通信正常时，90% 是初始化命令序列的问题。

---

## 六、Bug 4：第一次 Hard Fault（IMPRECISERR）—— 串口输出 #1

### 6.1 原始串口输出

```
\ | /
- RT -     Thread Operating System
 / | \     3.1.5 build May  8 2026
 2006 - 2020 Copyright by rt-thread team
psr: 0x01000000
r00: 0x20002178
r01: 0x044aa200
r02: 0x00000001
r03: 0x20002178
r04: 0xdeadbeef
r05: 0xdeadbeef
r06: 0xdeadbeef
r07: 0x20002a78
r08: 0xdeadbeef
r09: 0xdeadbeef
r10: 0xdeadbeef
r11: 0xdeadbeef
r12: 0x00000000
 lr: 0x08006215
 pc: 0x08008bd6
hard fault on thread: main

thread   pri  status      sp     stack size max used left tick  error
-------- ---  ------- ---------- ----------  ------  ---------- ---
Logic_th  10  ready   0x00000040 0x00000400    06%   0x00000014 000
Show_thr  10  ready   0x00000040 0x00000400    06%   0x00000014 000
rawdata_  10  ready   0x00000044 0x00000400    06%   0x00000014 000
tshell    20  ready   0x00000044 0x00001000    01%   0x0000000a 000
tidle     31  ready   0x00000040 0x00000100    25%   0x00000020 000
main      10  ready   0x00000040 0x00000800    13%   0x00000012 000
bus fault:
SCB_CFSR_BFSR:0x04 IMPRECISERR
```

### 6.2 逐行分析串口信息

这条输出是 RT-Thread 默认的 Hard Fault 信息打印，你需要逐行看、逐行推理：

#### 第 1 组信息：寄存器快照

```
psr: 0x01000000
pc: 0x08008bd6
lr: 0x08006215
```

**分析推理过程：**

| 寄存器 | 值 | 含义 |
|--------|-----|------|
| `psr=0x01000000` | bit 24 = 1 | CPU 处于 **Thread 模式**（不是 Handler/ISR 模式）。说明错误发生在普通线程代码中，而不是中断服务函数里。 |
| `r04-r11 = 0xdeadbeef` | 多个寄存器为 0xdeadbeef | 这是 RT-Thread 的"未初始化寄存器标记"。RT-Thread 在线程切换时不会用到这些寄存器，所以显示为填充值。这也从侧面说明**线程刚启动不久，还没执行多少代码就崩溃了**。 |
| `pc=0x08008bd6` | Flash 地址 | 崩溃时 CPU 正在执行这条指令。可以**在 IDE 的反汇编视图中搜索这个地址**，找到对应的函数。 |
| `lr=0x08006215` | 返回地址 | 这是调用当前函数的上一层函数的返回地址。同样可以反汇编定位。 |

> **实操建议**：在 RT-Thread Studio 中，双击 Hard Fault 信息里的 PC 地址，IDE 会自动跳转到对应的反汇编指令处。如果没跳转，用 `Ctrl+H` 搜索这个地址。

#### 第 2 组信息：错误发生线程

```
hard fault on thread: main
```

**分析：** 崩溃发生在 **main 线程**。不是 Show_thread2、不是 tshell、不是 idle。所以问题一定出在 main() 函数中执行的代码上。

紧接着的线程列表可以看到 main 线程的状态是 ready、栈使用量 13%（很小），这进一步说明 **main 线程启动后很快就崩溃了**——还没运行到需要大量栈空间的地方。

#### 第 3 组信息：错误类型

```
bus fault:
SCB_CFSR_BFSR:0x04 IMPRECISERR
```

**分析：** `BFSR=0x04` 的 bit 2 置位 = **IMPRECISERR（不精确总线错误）**。

理解这个错误类型的含义：
- **IMPRECISERR**：CPU 执行了一条写内存的指令，这个写操作被 CPU 的写缓冲（Write Buffer）缓存了。CPU 继续执行后续指令，等写缓冲真正去写入内存时，发现目标地址非法，触发总线错误。此时 CPU 已经跑远了好几条指令，所以无法报告"究竟是哪条指令写错的"。
- 没有 MMFSR、没有 UFSR → 排除了存储管理错误和用法错误。**纯总线错误**。

### 6.3 完整推理链

有了以上信息，可以串起推理链：

```
1. hard fault on thread: main
   → bug 在 main() 函数里

2. IMPRECISERR (不精确)
   → 是写操作导致的，不是读操作/指令预取
   → 通常原因：写到了非法指针指向的地址

3. 检查 main() 中的代码
   → main() 里只有 MX_xxx_Init() 调用
   → 其中 MX_ADC1_Init() 末尾调用了 bsp_adc_init()

4. 查看 bsp_adc_init() 调用
   → 发现：bsp_adc_init();           // 没有参数！
   → 但定义是：void bsp_adc_init(ADC_HandleTypeDef *hadc)
   
5. 为什么编译器没报错？
   → 因为 cubemx/Src/adc.c 没有 #include <drivers/bsp_adc.h>
   → 编译器不知道函数原型 → 隐式声明 → 不检查参数类型和个数

6. 隐式声明的后果
   → CPU 压栈了一个无效的"参数"
   → bsp_adc_init() 内部用这个无效值去访问内存 → 写到了非法地址
   → 写缓冲延迟生效 → IMPRECISERR
```

### 6.4 根因

隐式声明 + 无参数调用 = 栈损坏。当 `bsp_adc_init()` 被调用时，CPU 压栈传入了一个无效的参数值（可能是栈上的垃圾数据），函数内部访问这个"参数"时就触碰了非法地址，触发 IMPRECISERR。

### 6.5 修复方法

```c
/* USER CODE BEGIN ADC1_Init 2 */
#include <drivers/bsp_adc.h>      // 添加头文件
bsp_adc_init(&hadc1);             // 传入正确的句柄
/* USER CODE END ADC1_Init 2 */
```

### 6.6 你学到的调试思路

- **看到 "hard fault on thread: main"** → 立即去 main() 函数里找问题。
- **看到 IMPRECISERR** → 知道是写操作错误，但不知道精确位置。你需要检查最近添加的**函数调用**，特别是那些带有指针参数的调用。
- **看到 r04-r11 = 0xdeadbeef** → 说明线程刚启动不久，崩溃发生得很早，缩小了排查范围。
- **隐式声明是嵌入式开发中的头号杀手**。养成检查编译器 warning 的习惯。

---

## 七、Bug 4.5：编译错误——串口输出 #2

### 7.1 原始编译输出

```
23:50:51 **** Build of configuration Debug for project test_5_signer ****
make -j12 all
...
../cubemx/Src/adc.c:24:21: fatal error: bsp_adc.h: No such file or directory
 #include "bsp_adc.h"
                     ^
compilation terminated.
make: *** Waiting for unfinished jobs....
"make -j12 all" terminated with exit code 2. Build might be incomplete.
23:50:54 Build Failed. 3 errors, 0 warnings. (took 2s.759ms)
```

### 7.2 分析

严格来说这不是串口输出，而是 **IDE 的编译控制台输出**。但它同样是调试过程中重要的信息来源。

**关键信息：**
- 文件 `cubemx/Src/adc.c` 第 24 行
- 错误：`fatal error: bsp_adc.h: No such file or directory`
- include 写法：`#include "bsp_adc.h"`

### 7.3 推理链

```
1. fatal error: bsp_adc.h: No such file or directory
   → 编译器在搜索路径中没有找到 bsp_adc.h

2. bsp_adc.h 在哪里？
   → 在 modules/drivers/ 目录下

3. 为什么找不到？
   → #include "bsp_adc.h" 使用引号，编译器先搜索当前文件目录
   → 当前文件是 cubemx/Src/adc.c → 搜索 cubemx/Src/ → 没有 bsp_adc.h
   → 再搜索 -I 路径列表 → 需要确认 modules/drivers 是否在 include path 中

4. 修复方案
   → 使用 #include <drivers/bsp_adc.h>
   → 尖括号会让编译器直接搜索 -I 路径列表
   → 前提：modules/drivers 的父目录在 -I 路径中
```

### 7.4 修复方法

将 `#include "bsp_adc.h"` 改为 `#include <drivers/bsp_adc.h>`。

### 7.5 你学到的调试思路

- **#include "" 和 #include <> 的区别**：引号优先搜索当前目录，尖括号只搜索 -I 指定路径。
- **编译错误也是重要的调试线索**。不要只是机械地改到"编译通过为止"，要理解为什么编译器找不到头文件——这反映了你对工程目录结构和构建系统的理解。
- 当你在 USER CODE 区域添加了头文件包含，**下次 CubeMX 重新生成代码时这个添加需要再做一次**。所以在 README 或文档中记录你做了哪些 USER CODE 修改。

---

## 八、Bug 5：初始化顺序依赖（TIM3 在 ADC 之后初始化）

### 8.1 现象

修复了 Bug 4 之后，程序仍然 Hard Fault。串口输出与第一次不同，指向了一个新的崩溃点（在 ISR 上下文中，见 Bug 6 的串口输出）。但在到达那个 ISR 之前，还有一个隐形的 Bug：初始化顺序。

### 8.2 排查思路

**第一步：检查 main() 中的初始化顺序。**
```c
int main(void)
{
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_ADC1_Init();       // <-- 先初始化 ADC
    MX_TIM3_Init();       // <-- 后初始化 TIM3
    MX_I2C1_Init();
}
```

**第二步：追踪 bsp_adc_init() 做了什么。**
```c
void bsp_adc_init(ADC_HandleTypeDef *hadc)
{
    HAL_ADCEx_Calibration_Start(&hadc1);
    HAL_TIM_Base_Start(&htim3);       // <-- 这里要启动 TIM3！
    HAL_ADC_Start_DMA(hadc, (uint32_t*)adc_raw_buf, ADC_CHANNELS * SAMPLE_COUNT);
}
```

`HAL_TIM_Base_Start(&htim3)` 会去操作 TIM3 的寄存器，但此时 `MX_TIM3_Init()` 还没有被执行—— **htim3 的寄存器配置全是默认值（或零）**。

### 8.3 根因

ADC 的 DMA 触发源是 TIM3 的 TRGO（触发输出），ADC 依赖 TIM3 先初始化。但代码中 ADC 在前、TIM3 在后。

### 8.4 修复方法

```c
int main(void)
{
    MX_GPIO_Init();
    MX_DMA_Init();
    MX_USART1_UART_Init();
    MX_TIM3_Init();       // <-- TIM3 必须先初始化
    MX_ADC1_Init();       // <-- ADC 依赖 TIM3
    MX_I2C1_Init();
}
```

### 8.5 你学到的调试思路

- **初始化顺序不是随便排的**！外设之间存在依赖关系时，必须先初始化被依赖的外设。
- **画外设依赖图**：ADC → TIM3（触发源），DMA → ADC（传输完成中断），I2C → OLED 等。

---

## 九、Bug 6：第二次 Hard Fault（PRECISERR）—— 串口输出 #3

### 9.1 原始串口输出

```
\ | /
- RT -     Thread Operating System
 / | \     3.1.5 build May  8 2026
 2006 - 2020 Copyright by rt-thread team
msh >psr: 0x6100001b
r00: 0x20001ce0
r01: 0x20002238
r02: 0x20001b38
r03: 0x08060ffb
r04: 0xdeadbeef
r05: 0xdeadbeef
r06: 0xdeadbeef
r07: 0x20000880
r08: 0xdeadbeef
r09: 0xdeadbeef
r10: 0xdeadbeef
r11: 0xdeadbeef
r12: 0x00000000
 lr: 0x0800a2b3
 pc: 0x080072b4
hard fault on handler

bus fault:
SCB_CFSR_BFSR:0x82 PRECISERR SCB->BFAR:08060FFB
```

### 9.2 逐行分析串口信息

#### 第 1 步：看 `hard fault on handler`

对比第一次输出的 `hard fault on thread: main`，这次是 `hard fault on handler`。

**handler = 中断/异常处理模式**。说明错误发生在**中断服务函数（ISR）**中，而不是普通线程代码里。

**问：可能是哪个中断？**
看项目中用了哪些中断：
- DMA1_Channel1（ADC 传输完成中断）
- TIM3（如果有中断）
- USART1（串口接收）

**结合代码推理**：ADC 使用了 DMA，DMA 传输完成后会触发中断 → 进入 `DMA1_Channel1_IRQHandler` → HAL 库处理 → 调用回调函数 `HAL_ADC_ConvCpltCallback()`。

#### 第 2 步：看 `SCB_CFSR_BFSR:0x82`

| BFSR bit | 值 | 含义 |
|----------|-----|------|
| bit 7 | 1 | **PRECISERR**：精确总线错误 |
| bit 1 | 1 | BFAR 有效：BFAR 寄存器保存了出错地址 |

**PRECISERR vs IMPRECISERR：**
- 上次是 IMPRECISERR（不精确），CPU 不知道哪条指令出错的
- 这次是 PRECISERR（精确），CPU 明确知道出错的内存地址，并保存在 BFAR 中

**PRECISERR 比 IMPRECISERR 更容易定位**，因为 BFAR 给出了明确地址。

#### 第 3 步：看 `SCB->BFAR:08060FFB`

BFAR = 0x08060FFB。在 STM32F103 上：
- 0x08000000 - 0x0807FFFF → 芯片 Flash（64KB 或 128KB，取决于型号）
- 0x08060FFB → 在 Flash 地址范围内

**CPU 尝试访问（写入）Flash 地址 → 触发总线错误**，因为 Flash 在运行时只能读，不能直接写。

**关键问题：CPU 为什么会去写 Flash？**

#### 第 4 步：检查 `r03: 0x08060ffb`

r03 的值正好等于 BFAR！在 ARM 调用约定中，r0-r3 是函数参数寄存器。这意味着：
- 在崩溃的时刻，r03（第 4 个函数参数，或临时变量）中存储了 `0x08060FFB` 这个地址
- 大概率是这个地址被当成了指针来解引用（dereference），导致 CPU 尝试访问 Flash

#### 第 5 步：看 `psr: 0x6100001b`

```
psr: 0x6100001b
    = 0 1 1 0 0 0 0 1  0000 0000 0001 1011
```

关键 bit：
- bit 24 = 1 → IPSR（中断号）有效
- IPSR 的低 8 位 = 0x1B = 27（十进制）
- STM32F103 的中断号 27 对应的就是 **DMA1_Channel1_IRQHandler**

**确认了！崩溃发生在 DMA1 通道 1 的中断处理函数中。**

#### 第 6 步：串联所有线索

```
线索 1: handler mode → 在 ISR 中崩溃
线索 2: PSR=0x6100001b → 中断号 27 = DMA1_Channel1
线索 3: PRECISERR + BFAR=0x08060FFB → 写 Flash 地址出错
线索 4: r03=0x08060FFB → 这个地址作为指针参数传入
```

**推理结论：**
DMA 传输完成后进入中断 → 中断处理中某个函数接收了一个被破坏的指针（指向 Flash）→ 尝试通过这个指针写入 → PRECISERR。

**为什么指针会被破坏？** 因为 DMA 缓冲区溢出了。

### 9.3 验证 DMA 缓冲区溢出的计算

检查代码：
```c
// bsp_adc.h
#define ADC_CHANNELS    2
#define SAMPLE_COUNT    64

// bsp_adc.c
uint16_t adc_raw_buf[ADC_CHANNELS * SAMPLE_COUNT];
// = 2 * 64 = 128 个 uint16_t = 256 字节

// 错误代码（使用了 sizeof）：
HAL_ADC_Start_DMA(hadc, (uint32_t*)adc_raw_buf, sizeof(adc_raw_buf));
// sizeof(adc_raw_buf) = 256（整个数组的字节数）

// 但 HAL_ADC_Start_DMA 的第 3 个参数是"转换次数"不是"字节数"！
// 256 次转换 × 每次 2 字节 = DMA 总共写入 512 字节
// 但缓冲区只有 256 字节！
// 超出的 256 字节覆盖了栈上相邻的变量、函数指针、返回地址
```

### 9.4 为什么会导致"写 Flash"？

DMA 溢出破坏的内存在栈上。**栈上通常保存了：**
- 局部变量（包括函数指针）
- 函数的返回地址（LR 压栈）
- 中断嵌套时的现场保存

当 DMA 写爆了缓冲区，它把返回地址或函数指针改成了 0x08060FFB。CPU 从中断返回时，跳转到了 0x08060FFB 去执行——但 0x08060FFB 在 Flash 中是代码或数据区域，CPU 可能将其解释为指令，也可能尝试写入（如果某些写缓冲操作还在途中）。

### 9.5 修复方法

```c
// bsp_adc.c

// 初始化时，传转换次数，不是字节数：
HAL_ADC_Start_DMA(hadc, (uint32_t*)adc_raw_buf, ADC_CHANNELS * SAMPLE_COUNT);
// 参数 = 128（转换次数）

// 中断回调中重启 DMA 时：
void HAL_ADC_ConvCpltCallback(ADC_HandleTypeDef* hadc)
{
    if (hadc->Instance == ADC1)
    {
        rt_sem_release(alarm_sem);
        HAL_ADC_Stop_DMA(hadc);
        HAL_ADC_Start_DMA(hadc, (uint32_t*)adc_raw_buf, SAMPLE_COUNT);
        // 注意这里只传 SAMPLE_COUNT（64），因为 HAL 仅启动指定次数
    }
}
```

### 9.6 你学到的调试思路

- **从 `hard fault on handler` 到 PRECISERR 是一条清晰的线索链**：
  1. handler → 在 ISR 中
  2. PSR 的中断号 → 确定是哪个 ISR
  3. BFAR → 出错地址
  4. BFAR + 项目内存布局 → 判断是哪个内存区域（RAM？Flash？外设？）
  5. 反向推导哪个操作会导致这个地址被访问

- **HAL 库函数的参数单位要仔细区分**。`HAL_ADC_Start_DMA` 的 Length 是"转换次数"，不是"字节数"。类似容易混淆的还有：`HAL_UART_Transmit` 的 Length 是字节数、`HAL_SPI_Transmit` 的 Length 是字节数、`HAL_DMA_Start` 的 Length 是"数据单元数"（取决于数据宽度）。

- **DMA 缓冲区溢出不立即崩溃**。它先悄悄覆盖相邻变量，等后续执行到关键代码（函数返回、中断调用）时才 Hard Fault。报告的出错位置往往远离实际写错的地方。所以看到 PRECISERR + BFAR 指向 Flash 时，要意识到**真正的错误不是写 Flash，而是之前的某个写操作破坏了指针**。

- **调试技巧：主动缩小缓冲区大小来复现问题**。如果怀疑 DMA 溢出，可以把缓冲区再改小一点，看是否更容易触发 Hard Fault，或观察崩溃的 BFAR 地址是否有规律。

---

## 十、总结：调试方法论

### 10.1 从串口信息中读取线索速查表

| 串口信息 | 含义 | 排查方向 |
|----------|------|----------|
| `hard fault on thread: main` | 错误发生在 main 线程 | 检查 main() 中的初始化代码 |
| `hard fault on handler` | 错误发生在中断/异常处理中 | 检查 ISR 函数和所用资源 |
| `BFSR=0x04 IMPRECISERR` | 不精确总线错误（写操作，地址未知） | 函数参数错误、隐式声明、指针越界 |
| `BFSR=0x82 PRECISERR + BFAR` | 精确总线错误 + 出错地址已知 | 内存溢出、缓冲区越界、数组访问越界 |
| `r04-r11 = 0xdeadbeef` | 寄存器未初始化，线程刚启动 | 崩溃发生得很早，检查启动代码 |
| `r03 = BFAR 相同值` | 错误地址被当作参数传入 | 指针被破坏，追溯指针来源 |
| `psr=0x6100001b → 中断号 27` | DMA1_Channel1_IRQHandler | 检查 DMA 配置和缓冲区 |
| `psr=0x01000000 → Thread 模式` | 错误在普通线程代码中 | 检查线程入口函数 |
| I2C 通信失败日志 | 外设未初始化或地址错误 | 检查初始化顺序、I2C 地址 |
| `fatal error: xxx.h: No such file` | 头文件路径错误 | 检查 #include 写法（"" vs <>）|

### 10.2 三段串口输出的时间线总结

```
输出 #1 (IMPRECISERR, thread: main)
    ↓ 分析 → 隐式声明 + 无参数调用 bsp_adc_init()
    ↓ 修复 → 添加头文件 + 参数
    ↓
输出 #2 (编译错误: bsp_adc.h not found)
    ↓ 分析 → include 路径格式错误
    ↓ 修复 → #include "bsp_adc.h" → #include <drivers/bsp_adc.h>
    ↓        + 调整 main() 中 TIM3 与 ADC1 的顺序
    ↓
输出 #3 (PRECISERR, handler, BFAR=0x08060FFB)
    ↓ 分析 → DMA 缓冲区溢出
    ↓ 修复 → sizeof(adc_raw_buf) → ADC_CHANNELS * SAMPLE_COUNT
```

### 10.3 通用的七步排查法

1. **复现并保留出错信息**：第一次看到错误就要把完整信息保存下来，不要急着复位。
2. **识别错误类型**：是 Hard Fault？外设报告错误？程序卡死？编译错误？
3. **定位发生位置**：
   - `hard fault on thread: xxx` → 查 xxx 线程的入口代码
   - `hard fault on handler` → 查 PSR 中断号确定 ISR
   - 编译错误 → 查文件名、行号
4. **解析错误子类型**：
   - IMPRECISERR → 写操作出错，检查函数调用和指针参数
   - PRECISERR + BFAR → 访问非法地址，追踪 BFAR 来源
5. **找到最近的代码变更**：绝大多数 Bug 都是刚改的代码引入的。
6. **检查编译器 warning**：隐式声明是最大陷阱。用 `-Werror=implicit-function-declaration` 彻底堵死。
7. **检查 HAL 库参数**：每个参数的单位都去查 API 手册确认，不要靠猜。

### 10.4 防御性编程建议

1. **始终包含头文件**：任何使用外部函数的 .c 文件都要 #include 对应的 .h。
2. **启用 -Werror**：将 warning 当作 error，防止隐式声明被忽略。
3. **HAL 返回值检查**：每个 HAL 调用都检查返回值并输出日志。
4. **sizeof 用在预期的地方**：只有在确实是 allocate 操作时才用 sizeof，给 API 传长度时先确认单位。
5. **初始化加延迟**：对外设做初始化操作时，考虑各外设的上电时序差异。
6. **隔离调试**：如果多个模块同时出问题，先去掉所有模块，一个一个加回来，确定是哪个模块引入的。

---

## 十一、附录：所有修复一览

| 文件 | 修复内容 | Bug 类型 | 对应输出 |
|------|----------|----------|----------|
| `modules/app/app_display.c` | 添加 `#include "oled.h"` | 头文件缺失 | - |
| `modules/app/app_display.c` | OLED_Init() 前加 20ms 延迟 | 初始化时序 | - |
| `cubemx/Src/oled.c` | I2C 地址 0x7A -> 0x78 | 地址错误 | - |
| `cubemx/Src/oled.c` | 替换为 SSD1306 初始化序列 | 芯片不匹配 | - |
| `cubemx/Src/oled.c` | 添加 HAL 返回值和 rt_kprintf 诊断 | 诊断增强 | - |
| `cubemx/Src/adc.c` | 添加 `#include <drivers/bsp_adc.h>` | 隐式声明 | 输出 #1 |
| `cubemx/Src/adc.c` | `#include "bsp_adc.h"` → `<drivers/bsp_adc.h>` | 路径错误 | 输出 #2 |
| `cubemx/Src/adc.c` | bsp_adc_init() 添加 &hadc1 参数 | 参数错误 | 输出 #1 |
| `applications/main.c` | MX_TIM3_Init() 移到 MX_ADC1_Init() 之前 | 依赖顺序 | - |
| `modules/drivers/bsp_adc.c` | sizeof → ADC_CHANNELS * SAMPLE_COUNT | 参数单位误解 | 输出 #3 |