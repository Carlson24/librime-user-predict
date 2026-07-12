# user_predict — 用户预测插件

用户输入行为学习与预测插件。通过记录用户的词语搭配习惯，在输入时自动提供预测候选词。

**原始项目：[rime-wanxiang-lua](https://github.com/amzxyz/rime-wanxiang/blob/wanxiang/lua/wanxiang/user_predict.lua)**

## 功能

- **学习**: 自动记录用户上屏的词语顺序（S-Gram / 2-Gram / 1-Gram / P-Gram）
- **预测**: 基于历史数据，提供下一步可能输入的候选词
- **衰减**: 时间衰减算法，越久远的数据权重越低
- **回滚**: BackSpace 撤销最近一次学习写入
- **清理**: 自动清理过期数据（1/2-Gram 90天，P-Gram 30天）
- **导入/导出**: 支持备份与恢复
- **ABA 防折返**: 防止"你好"→"你好"的无效自循环
- **语境隔离**: 标点断句 + 超时自动重置记忆链

## 构建

```sh
cd librime
make        # Release 构建
make debug  # Debug 构建（含 stderr 日志）
```

产物: `build/lib/rime-plugins/librime-user-predict.so`

## 安装

```sh
# 系统安装
sudo cmake --install build --prefix /usr

# 或手动复制
sudo cp build/lib/rime-plugins/librime-user-predict.so /usr/lib/rime-plugins/
```

## 配置

### 引擎组件

```yaml
engine:
  processors:
    - user_predict_processor        # 必须
  translators:
    - user_predict_translator       # post 模式需要
  filters:
    - user_predict_filter           # reorder 模式需要
```

### 开关

```yaml
switches:
  - name: prediction
    reset: 1                       # 1 = 默认开启
    states: ["预测关", "预测开"]
```

### 配置项

| 键 | 类型 | 默认值 | 说明 |
|----|------|--------|------|
| `predict_style` | string | `"reorder"` | 预测模式 |
| `max_candidates` | int | `5` | 预测候选最大数量 |
| `max_predictions` | int | `3` | 连续预测链最大长度 |
| `expiry_days` | int | `90` | 数据保留天数 |
| `decay_rate` | double | `0.85` | 时间衰减系数（越小衰减越快） |
| `context_timeout` | int | `30000` | 语境超时（毫秒） |
| `max_memory_branches` | int | `15` | 每次查询最大分支数 |
| `db_name` | string | `"user_predict"` | 数据库名（自动补 `.userdb`） |

### 预测模式

**reorder**（PC 推荐）— 调频现有候选：

```yaml
engine:
  processors:
    - user_predict_processor
  filters:
    - user_predict_filter

user_predict:
  predict_style: "reorder"
```

**post** — 标准预测：

```yaml
engine:
  processors:
    - user_predict_processor
  translators:
    - user_predict_translator

user_predict:
  predict_style: "post"
```

### 管理命令

| 输入 | 功能 |
|------|------|
| `/clean` | 清理过期记录 |
| `/outpredict` | 导出数据到 `predict_export.txt` |
| `/inpredict` | 从 `predict_import.txt` 导入（LWW 策略） |
