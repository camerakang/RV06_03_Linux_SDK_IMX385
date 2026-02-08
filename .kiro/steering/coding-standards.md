---
inclusion: always
---

# RV06 Linux SDK 编码规范

## 核心原则

### 1. 代码复用优先
- **实现前必读**：在编写任何新功能前，必须先使用 Serena 工具分析现有代码
- **避免重复造轮**：检查项目中是否已有类似功能的实现
- **复用现有函数**：优先使用已有的函数、类和模块
- **扩展而非重写**：如果现有代码接近需求，优先考虑扩展而非重写
- **参考同类驱动**：编写新驱动时，必须先查看同目录下的类似驱动实现

### 2. 文档按需创建
- **不主动写文档**：除非用户明确要求，否则不创建额外的文档文件
- **代码即文档**：通过清晰的代码结构和注释来表达意图
- **README 例外**：关键模块可以有简短的 README.md 说明用途
- **避免冗余**：不要创建总结性文档来描述刚完成的工作

### 3. 注释要求
- **函数必须有注释**：每个函数都要有详细的注释说明
  ```c
  /**
   * @brief 初始化 IMX385 传感器
   * @param sd V4L2 子设备指针
   * @param val 初始化参数
   * @return 0 成功，负数表示错误码
   */
  static int imx385_initialize(struct v4l2_subdev *sd, u32 val)
  ```
- **复杂逻辑必须注释**：算法、状态机、协议处理等复杂逻辑必须有注释
- **中文注释**：使用中文注释，便于团队理解
- **注释格式**：使用 Doxygen 风格的注释
- **寄存器操作必须注释**：说明寄存器地址、功能和设置值的含义

### 4. 代码质量
- **可读性优先**：代码要清晰易懂，避免过度优化
- **模块化设计**：功能划分清晰，接口明确
- **错误处理**：所有可能失败的操作都要有错误处理
- **资源管理**：及时释放资源，避免内存泄漏
- **返回值检查**：所有函数调用的返回值都要检查

## Linux 内核驱动规范

### 1. 代码风格
- **遵循内核规范**：严格遵循 Linux 内核编码风格（Documentation/process/coding-style.rst）
- **缩进**：使用 Tab（8 个空格宽度）
- **行宽**：每行不超过 80 字符（特殊情况可适当放宽）
- **命名规范**：
  - 函数名：小写字母 + 下划线，如 `imx385_s_stream`
  - 宏定义：大写字母 + 下划线，如 `IMX385_REG_CTRL`
  - 结构体：小写字母 + 下划线，如 `struct imx385_mode`

### 2. V4L2 驱动特定规范
- **子设备操作**：实现标准的 V4L2 子设备操作接口
- **控制接口**：使用 V4L2 控制框架（v4l2_ctrl）
- **媒体总线格式**：正确设置 media bus format 和 code
- **电源管理**：实现 runtime PM 接口
- **设备树绑定**：提供完整的设备树文档和示例

### 3. 寄存器操作
- **寄存器表**：使用结构化的寄存器表定义
  ```c
  struct regval {
      u16 addr;
    u8 val;
  };
  ```
- **寄存器宏**：为重要寄存器定义宏
  ```c
  #define IMX385_REG_CTRL_MODE    0x3000
  #define IMX385_MODE_STREAMING   0x00
  #define IMX385_MODE_STANDBY     0x01
  ```
- **批量写入**：使用批量写入函数提高效率
- **读写封装**：使用统一的读写函数，便于调试和错误处理

### 4. 错误处理
- **返回值**：使用标准的 Linux 错误码（-EINVAL, -EIO, -ENOMEM 等）
- **日志级别**：
  - `dev_err()`：错误信息
  - `dev_warn()`：警告信息
  - `dev_info()`：重要信息
  - `dev_dbg()`：调试信息
- **资源清理**：使用 goto 标签进行统一的错误清理
  ```c
  err_power_off:
      imx385_power_off(imx385);
  err_free_handler:
      v4l2_ctrl_handler_free(&imx385->ctrl_handler);
      return ret;
  ```

## 项目特定规范

### 1. 传感器驱动开发
- **参考现有驱动**：查看 `sysdrv/source/kernel/drivers/media/i2c/` 下的类似传感器驱动
- **复用代码结构**：保持与项目中其他传感器驱动一致的代码结构
- **模式配置**：支持多种分辨率和帧率模式
- **测试验证**：确保驱动能正确枚举格式、设置参数、启动流

### 2. 设备树配置
- **完整性**：提供完整的设备树节点示例
- **注释说明**：关键属性要有注释说明
- **兼容性**：确保与现有板级配置兼容

### 3. 构建系统
- **Makefile**：正确添加到内核 Makefile
- **Kconfig**：提供配置选项和帮助信息
- **依赖关系**：明确列出依赖的其他模块

## 工作流程

### 1. 开始新任务前
1. 使用 `mcp_serena_find_symbol` 查找相关的现有实现
2. 使用 `mcp_serena_search_for_pattern` 搜索类似的代码模式
3. 阅读参考驱动的实现，理解项目的代码风格
4. 确认是否可以复用或扩展现有代码

### 2. 编写代码时
1. 遵循上述编码规范
2. 添加完整的注释
3. 实现错误处理
4. 考虑资源管理

### 3. 完成任务后
1. 不要自动创建总结文档
2. 简洁地告知用户完成了什么
3. 等待用户的下一步指示

## 禁止事项

- ❌ 不要在没有分析现有代码的情况下编写新功能
- ❌ 不要创建用户未要求的文档文件
- ❌ 不要编写没有注释的函数
- ❌ 不要忽略错误处理
- ❌ 不要使用魔术数字（未定义的常量）
- ❌ 不要在完成任务后自动生成冗长的总结
- ❌ 不要偏离 Linux 内核编码风格

## 最佳实践

### 代码复用示例
```c
// ✅ 好的做法：复用现有的寄存器写入函数
ret = imx385_write_array(imx385->client, imx385_global_regs);

// ❌ 不好的做法：重新实现寄存器写入逻辑
for (i = 0; i < ARRAY_SIZE(regs); i++) {
    ret = i2c_smbus_write_byte_data(client, regs[i].addr, regs[i].val);
    // ...
}
```

### 注释示例
```c
// ✅ 好的注释
/**
 * @brief 设置 IMX385 的曝光时间
 * @param imx385 设备结构体指针
 * @param val 曝光值（单位：行）
 * @return 0 成功，负数表示错误码
 * 
 * 曝光时间通过 SHS1 寄存器（0x3020-0x3022）设置
 * 计算公式：曝光时间 = (VMAX - SHS1) * 行时间
 */
static int imx385_set_exposure(struct imx385 *imx385, u32 val)

// ❌ 不好的注释
// set exposure
static int imx385_set_exposure(struct imx385 *imx385, u32 val)
```

### 错误处理示例
```c
// ✅ 好的错误处理
ret = imx385_write_reg(client, IMX385_REG_CTRL_MODE, IMX385_MODE_STREAMING);
if (ret) {
    dev_err(&client->dev, "Failed to start streaming: %d\n", ret);
    return ret;
}

// ❌ 不好的错误处理
imx385_write_reg(client, IMX385_REG_CTRL_MODE, IMX385_MODE_STREAMING);
```
