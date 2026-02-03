# Steam Review Scraper

异步并发爬取Steam游戏评论的Python工具,支持断点续传和去重。

## 功能特性

- ✅ **异步并发请求** - 使用aiohttp实现高效的异步HTTP请求
- ✅ **断点续传** - 支持中断后继续爬取,避免重复工作
- ✅ **去重机制** - 自动识别并跳过已爬取的评论
- ✅ **数据持久化** - 评论自动保存到CSV文件
- ✅ **完善的错误处理** - 自动重试失败的请求
- ✅ **进度显示** - 实时显示爬取进度

## 环境配置

### 使用 Miniconda (推荐)

1. 安装 Miniconda (如果尚未安装):
```bash
# Linux/macOS
wget https://repo.anaconda.com/miniconda/Miniconda3-latest-Linux-x86_64.sh
bash Miniconda3-latest-Linux-x86_64.sh
```

2. 创建并激活虚拟环境:
```bash
# 使用 environment.yml 创建环境
conda env create -f environment.yml

# 激活环境
conda activate steam_scraper
```

### 使用 pip

```bash
pip install -r requirements.txt
```

## 使用方法

### 基本用法

```bash
# 使用默认参数爬取 (App ID: 4128260)
python steam_review_scraper.py
```

### 自定义参数

```bash
# 指定游戏ID和并发数
python steam_review_scraper.py --app-id 4128260 --concurrency 10

# 指定输出文件和天数范围
python steam_review_scraper.py --output reviews.csv --day-range 30

# 查看所有选项
python steam_review_scraper.py --help
```

### 命令行参数

| 参数 | 简写 | 默认值 | 说明 |
|------|------|--------|------|
| `--app-id` | | 4128260 | Steam游戏ID |
| `--output` | `-o` | steam_reviews.csv | 输出CSV文件路径 |
| `--checkpoint` | `-c` | checkpoint.json | 断点文件路径 |
| `--concurrency` | `-n` | 5 | 并发请求数 |
| `--day-range` | `-d` | 30 | 拉取天数范围 |
| `--retry` | | 3 | 重试次数 |

## 输出文件

### CSV文件字段

| 字段名 | 说明 |
|--------|------|
| `recommendationid` | 评论唯一ID |
| `steamid` | 用户Steam ID |
| `author_playtime_forever` | 总游戏时长(分钟) |
| `author_playtime_at_review` | 评论时游戏时长(分钟) |
| `voted_up` | 是否推荐 (True/False) |
| `votes_up` | 点赞数 |
| `votes_funny` | 有趣数 |
| `comment_count` | 评论回复数 |
| `timestamp_created` | 评论创建时间戳 |
| `timestamp_updated` | 评论更新时间戳 |
| `created_time` | 评论创建时间 (可读格式) |
| `updated_time` | 评论更新时间 (可读格式) |
| `review` | 评论内容 |
| `language` | 语言 |
| `weighted_vote_score` | 加权评分 |
| `received_for_free` | 是否免费获得游戏 |
| `written_during_early_access` | 是否早期测试期间评论 |

## 断点续传

爬虫会自动保存进度到 `checkpoint.json` 文件:
- 每爬取5批数据自动保存一次
- 用户中断(Ctrl+C)时自动保存
- 下次运行时自动加载断点继续

## Steam API 说明

本工具使用Steam官方公开的评论API:
- API地址: `https://store.steampowered.com/appreviews/{app_id}`
- 无需API密钥
- 每次请求最多返回100条评论
- 使用cursor参数进行分页

## 注意事项

1. 请合理设置并发数,避免请求过于频繁
2. 建议并发数不超过10
3. 网络不稳定时会自动重试
4. 爬取大量评论时建议使用断点续传功能

## 项目结构

```
.
├── environment.yml          # Conda环境配置
├── requirements.txt         # Python依赖
├── steam_review_scraper.py  # 主程序
├── README.md               # 说明文档
├── steam_reviews.csv       # 输出文件 (运行后生成)
├── checkpoint.json         # 断点文件 (运行后生成)
└── scraper.log            # 日志文件 (运行后生成)
```

## License

MIT License
