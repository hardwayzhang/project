# init.md - UE4 引擎目录 AI 初始化指南

> 适用对象：Cursor、CodeBuddy 及其他可读仓库并执行命令的 AI 编码助手。  
> 目标：让 AI 在进入 UE4 目录后，**快速建立正确上下文、避免破坏性修改、按标准流程交付变更**。

---

## 1. 仓库定位（AI 首次进入先做）

1. 确认当前目录是 UE4 引擎源码或基于 UE4 的主仓库根目录。
2. 先读取以下文件（若存在）：
   - `README.md`
   - `CONTRIBUTING.md`
   - `Build/`
   - `Engine/Source/`
   - `*.uproject`（项目入口）
   - `Config/Default*.ini`
3. 若是多项目仓库，优先定位主 `.uproject` 与目标模块（`Source/<ModuleName>/`）。

---

## 2. 强约束（必须遵守）

### 2.1 禁止直接编辑的目录/文件

- `Binaries/`
- `Intermediate/`
- `DerivedDataCache/`
- `Saved/`
- 自动生成文件（如 `*.generated.h`、项目文件、临时构建产物）

### 2.2 修改原则

- 仅改动与需求直接相关的最小范围文件。
- 不做“顺手重构”与无关格式化。
- 优先兼容现有模块边界与命名规范。
- 修改前先读上下文，避免破坏反射宏（`UCLASS/USTRUCT/UENUM/UFUNCTION/UPROPERTY`）与序列化兼容性。

---

## 3. UE4 目录常见结构速记

- `Engine/Source/Runtime/`：运行时模块
- `Engine/Source/Editor/`：编辑器模块
- `Engine/Plugins/`：引擎插件
- `<Project>/Source/`：项目业务代码
- `<Project>/Content/`：资源内容（通常不由 AI 直接编辑二进制资源）
- `<Project>/Config/`：项目配置

---

## 4. 典型构建与生成命令（按平台选择）

> 以下命令为通用模板。执行前请确认仓库实际脚本路径与目标名称。

### Linux/macOS（源码引擎常见）

```bash
./Setup.sh
./GenerateProjectFiles.sh
./Engine/Build/BatchFiles/Linux/Build.sh UE4Editor Linux Development
```

### Windows（PowerShell / CMD）

```bat
Setup.bat
GenerateProjectFiles.bat
Engine\Build\BatchFiles\Build.bat UE4Editor Win64 Development
```

### 项目目标构建示例

```bash
# Linux 示例
./Engine/Build/BatchFiles/Linux/Build.sh <ProjectName>Editor Linux Development "<Path>/<ProjectName>.uproject"
```

---

## 5. AI 修改 C++ 代码时的检查清单

1. **头文件依赖最小化**：能前置声明就前置声明。
2. **宏与反射安全**：`GENERATED_BODY()` 位置、`UPROPERTY` 元数据、Blueprint 暴露策略保持一致。
3. **模块依赖合法**：若新增依赖，同步更新对应 `*.Build.cs`。
4. **生命周期正确**：`BeginPlay`/`Tick`/`EndPlay`、子系统初始化、委托解绑必须成对。
5. **日志可诊断**：关键路径使用统一类别日志（避免噪音日志）。
6. **网络/存档兼容**：涉及复制（Replication）或 SaveGame 时，不能破坏已有字段语义。

---

## 6. AI 执行任务标准流程

1. 复述需求并锁定影响模块。
2. 搜索相关符号与调用链（先读后改）。
3. 实施最小变更。
4. 至少执行一轮编译或静态检查（能跑测试则跑测试）。
5. 输出结果时必须包含：
   - 改了哪些文件
   - 为什么这么改
   - 如何验证
   - 潜在风险/未覆盖项

---

## 7. 提交规范（给 AI）

- Commit 粒度：一个逻辑改动一个 commit。
- Commit message 建议：
  - `fix: ...`
  - `feat: ...`
  - `refactor: ...`
  - `docs: ...`
- 禁止把无关改动混入同一提交。

---

## 8. 交接模板（AI 输出可直接套用）

```md
### 变更摘要
- ...

### 修改文件
- path/to/file1
- path/to/file2

### 验证步骤
1. ...
2. ...

### 风险与后续建议
- ...
```

---

## 9. 项目自定义区（建议维护者补充）

请尽快补充以下信息，提升 AI 执行准确率：

- 主 `.uproject` 路径：
- 默认构建平台与配置：
- 必跑测试命令：
- 禁改目录补充：
- 核心模块负责人/评审规则：
- CI 入口与失败排查文档：

---

## 10. 一句话原则

**先读上下文、只做最小必要改动、保持 UE4 反射与构建链稳定。**

