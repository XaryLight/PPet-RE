# Compiler/ —— 文案翻译器

这个目录放的是**离线工具**：把「人写的文案」翻译成「机器能用的 C++ 代码」。

| 文件 | 作用 |
| --- | --- |
| `i18n/zh-CN.json` | 简体中文界面文案（**文案的唯一来源**） |
| `i18n/en-US.json` | 英文界面文案 |
| `i18n_gen/i18n_gen.hpp` | 翻译器接口（读文案表 / 校验 / 生成 C++ 内嵌表） |
| `i18n_gen/i18n_gen.cpp` | 翻译器实现 |
| `i18n_gen/main.cpp` | 命令行入口 |
| `i18n_gen/i18n_gen_test.cpp` | 翻译器自身的单元测试 |

## 为什么需要它

以前中英文文案直接写在 `src/core/I18n.cpp` 里，两个 `if` 分支各抄一遍：

- 加一条文案要改两个地方，**漏改一处界面上就是空白**；
- 两份手抄的内容迟早会不一致；
- 翻译同学要动 C++ 代码，改坏了编译不过。

现在文案本体搬到 JSON，构建时由 `ppet_i18n_gen` 翻译成
`<构建目录>/generated/core/I18nStrings.inl`：

- 改文案只需要改 JSON，**不用碰 C++**；
- 翻译器在构建期校验「**文案键 ↔ `struct Strings` 成员**」一一对应，
  少一条、多一条、拼错一个键都会**立刻报错**，而不是等到界面上出现空白；
- 生成的表是 `constexpr` 常量，运行时零成本。

## 生成的产物长什么样

`core/I18nStrings.inl` 里同时有两样东西，且**由同一份 X 宏展开**，
所以不可能出现「表里有、赋值时却没有」的不一致：

```cpp
#define PPET_I18N_ENTRIES(X) \
    X(quit, "退出", "Quit") \
    ...

inline constexpr I18nEntry kI18nTable[] = {
#define PPET_I18N_ROW(key, zh, en) {#key, zh, en},
    PPET_I18N_ENTRIES(PPET_I18N_ROW)
#undef PPET_I18N_ROW
};
```

`src/core/I18n.cpp` 用同一个 X 宏给结构体赋值：

```cpp
#define PPET_I18N_ASSIGN(key, zh, en) s.key = chinese ? zh : en;
    PPET_I18N_ENTRIES(PPET_I18N_ASSIGN)
#undef PPET_I18N_ASSIGN
```

> 这个文件是**构建产物**，不会提交进仓库；改文案改 JSON 即可。

## 怎么改文案

1. 编辑 `Compiler/i18n/zh-CN.json` 和 `Compiler/i18n/en-US.json`；
2. 重新构建（CMake 会自动重跑翻译器）；
3. 完事。

新增一条文案要同时做两件事：

- 在 `src/core/I18n.hpp` 的 `struct Strings` 里加一个 `std::string` 成员；
- 在两个 JSON 里各加一条同名条目。

**顺序无所谓，但两边必须齐**——少一边构建就会报错，并明确告诉你是哪条、哪个语言缺了：

```
错误：文案表与 struct Strings 不一致：
  · 缺少英文文案：quit
  · 英文文案里有结构体中不存在的键：Quite（请检查拼写，或在 struct Strings 里补上该成员）
```

### JSON 里的小约定

- 以 `_` 开头的键是**注释项**，会被忽略（可以用来写说明）；
- 值必须是字符串；
- 换行写成 `\n`，翻译器会自动转成 C++ 字面量里的 `\n`。

## 也可以用 CSV

翻译器按扩展名自动判断格式，CSV 表头固定为 `key,text`：

```csv
key,text
quit,退出
modelFileFilter,"模型文件 (*.json *.pmx *.vrm *.model3.json *.zip)"
```

含逗号的字段要用双引号包起来（双引号本身写成 `""`）。

换成 CSV 只需要在配置时指定路径：

```bash
cmake -S . -B build \
  -DPPET_I18N_ZH=Compiler/i18n/zh-CN.csv \
  -DPPET_I18N_EN=Compiler/i18n/en-US.csv
```

## 手动使用

```bash
# 生成
cmake --build build --target ppet_i18n_gen_run

# 直接跑翻译器
./build/ppet_i18n_gen \
  --out  out.inl \
  --struct src/core/I18n.hpp \
  --zh    Compiler/i18n/zh-CN.json \
  --en    Compiler/i18n/en-US.json

# 只比对不写文件（CI 用：检查生成物是否已过期）
./build/ppet_i18n_gen \
  --out  build/generated/core/I18nStrings.inl \
  --struct src/core/I18n.hpp \
  --zh    Compiler/i18n/zh-CN.json \
  --en    Compiler/i18n/en-US.json \
  --check
```

## 测试

```bash
ctest -R i18n_gen --output-on-failure
```

- `i18n_gen_unit` —— 翻译器自身的单元测试（CSV/JSON 解析、成员解析、校验、转义、生成）；
- `i18n_gen_project` —— 拿仓库里真实的文案源跑一遍 `--check`，确保生成物没过期。
